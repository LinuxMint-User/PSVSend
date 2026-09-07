/* 网络初始化实现（见 net.h）。
 * Vita homebrew 需要先加载网络 sysmodule，再用给定内存池初始化 SceNet 协议栈，
 * 之后才能使用 sceNetInet 系列 socket / netctl 查询接口。
 * 注意：真机从待机唤醒/刚启动时 Wi-Fi 可能仍在重连，因此初始化失败不判死，
 * 允许上层（api 看门狗）稍后重试；各步错误码写 dlog 便于真机排查。 */
#include <string.h>
#include <stdio.h>
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "net/net.h"
#include "core/dlog.h"

/* 调试开关：开机后前 PSVSEND_SIM_DOWN_MS 毫秒内，net_poll 强制报 DISCONNECTED，
 * 模拟"app 启动瞬间 Wi-Fi 尚未就绪、随后自动恢复"（真实场景 = 待机唤醒后立刻
 * 进 app）。Wi-Fi 一直连着的真机上就能秒级复现，免去每次都等深度休眠再唤醒。
 * 联调结束已置 0（恢复正常；也可 cmake -DPSVSEND_SIM_DOWN_MS=6000 临时再开）。 */
#ifndef PSVSEND_SIM_DOWN_MS
#define PSVSEND_SIM_DOWN_MS 0
#endif

#define NET_POOL_SIZE (1 * 1024 * 1024)   /* SceNet 内存池 */
static char g_pool[NET_POOL_SIZE] __attribute__((aligned(64)));
static int g_state = 0;                   /* 0 未完成 / 1 就绪 / <0 上次失败码(可重试) */
static int g_step = 0;                    /* 已完成步骤位：1=sysmodule 2=net 4=netctl */
static int g_ctl_err = 0;                 /* 最近一次 netctl 状态查询返回码（net_poll 维护） */
static int g_ctl_st  = -1;                /* 最近读到的网络状态（net_poll 维护） */
static char g_ip[16] = "0.0.0.0";         /* 本机 IP 缓存（net_poll 刷新；读方无锁，
                                            最坏读到半更新串，只影响一次组播出口选择） */
static SceUID g_ctl_mtx = -1;             /* 保护 ctl 查询与 term/init 重建互斥 */
static int g_ctl_held = 0;                /* 本线程是否持有该锁（只有 watch 线程用这对函数） */

/* 有界取锁（诊断 + 防死）：理论上前述场景锁永远空闲、立即拿到。
 * d12 实测出现"expired 后 watch 线程凭空消失"——若真是锁被某个未知
 * 持有者长期占用，无限等待会让看门狗线程永久沉睡（UI/announce 全停摆）。
 * 这里单次最多等 1s：超时则大声打日志（含持有者信息）并继续（不带锁）。 */
static void ctl_lock(void)
{
    if (g_ctl_mtx < 0) { g_ctl_held = 0; return; }
    {
        unsigned int to = 1000000;        /* 1s：正常时瞬间拿到；只防永久沉睡 */
        int r = sceKernelLockMutex(g_ctl_mtx, 1, &to);
        g_ctl_held = (r == 0);
        if (r != 0) {
            /* 诊断：谁占着锁？本应无人（全工程只有本线程会用它）。
             * 内核直接报告 currentCount / 持有者线程 uid / 等待者数。 */
            SceKernelMutexInfo mi;
            memset(&mi, 0, sizeof mi);
            mi.size = sizeof mi;
            if (sceKernelGetMutexInfo(g_ctl_mtx, &mi) == 0)
                dlog("net: ctl lock 1s TO r=0x%08X tid=%d cnt=%d owner=%d nwait=%d",
                     (unsigned)r, (int)sceKernelGetThreadId(), mi.currentCount,
                     (int)mi.currentOwnerId, mi.numWaitThreads);
            else
                dlog("net: ctl lock 1s TO r=0x%08X tid=%d (mutex info n/a)",
                     (unsigned)r, (int)sceKernelGetThreadId());
        }
    }
}
static void ctl_unlock(void)
{
    if (g_ctl_mtx >= 0 && g_ctl_held) sceKernelUnlockMutex(g_ctl_mtx, 1);
    g_ctl_held = 0;
}

int net_start(void)
{
    SceNetInitParam param;
    int r;
    if (g_state > 0) return 0;            /* 全就绪 */
    if (g_ctl_mtx < 0) {                  /* 早建（只此一处）；查询与 term/init 重建互斥 */
        /* initCount 必须为 0：Vita 内核下 initCount=1 会让锁以"已由创建线程持有、
         * count=1"的状态出生。若创建者（主线程）随后未执行过 unlock（启动期
         * net_poll 常因网络未就绪提前返回，根本走不到 unlock），这把锁将永远被
         * 主线程占着——watch 线程的取锁会永久等待/超时，正是本 bug 的根源。
         * 官方 sample（debugScreen.c）对普通互斥锁一律用 initCount=0。 */
        g_ctl_mtx = sceKernelCreateMutex("psvsend_ctl", 0, 0, NULL);
    }

    /* 分步推进、只重试失败的那一步。Wi-Fi 关闭/刚开机时 netctl 常起不来
     * （sceNetCtlInit 失败）：若此处只打日志仍把 g_state 置 1，netctl 客户端
     * 永远不会补初始化，之后 sceNetCtlInetGetState 一直报错，watch 线程看不到
     * 链路恢复 → app 永久卡"网络未就绪"（必须重启）。sysmodule/net 只初始化
     * 一次，避免 wifi 恢复后重复 sceNetInit 二次初始化报错。 */
    if ((g_step & 1) == 0) {
        r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
        if (r < 0) { g_state = r; dlog("net: load SCE_SYSMODULE_NET fail 0x%08X", (unsigned)r); return r; }
        g_step |= 1;
    }
    if ((g_step & 2) == 0) {
        memset(&param, 0, sizeof param);
        param.memory = g_pool;
        param.size = sizeof g_pool;
        param.flags = 0;
        r = sceNetInit(&param);
        if (r < 0) { g_state = r; dlog("net: sceNetInit fail 0x%08X", (unsigned)r); return r; }
        g_step |= 2;
    }
    if ((g_step & 4) == 0) {
        r = sceNetCtlInit();
        if (r < 0) {                        /* 可重试步骤：watch 等 wifi 恢复后自动补跑 */
            g_state = r;
            return r;
        }
        dlog("net: ok (pool %d KB)", NET_POOL_SIZE / 1024);
        g_step |= 4;
    }
    g_state = 1;
    return 0;
}

int net_state(void)
{
    return g_state;
}

/* 后台轮询：api_watch 线程每 500ms 调用一次，刷新链路状态与本地 IP 缓存。
 * 这是全工程唯一真正调用 sceNetCtl* 查询的地方。早前版本让 UI 主线程也每帧
 * 直查 netctl，Wi-Fi 恢复瞬间的并发/高频查询会把 netctl 服务卡死，主线程永久
 * 阻塞在系统调用里 → 界面永远定格在恢复前的旧帧。收敛到单线程低频执行后，
 * 其余线程（UI、announce、scan）都只读下面的缓存，不做系统调用。 */
void net_poll(void)
{
#if PSVSEND_SIM_DOWN_MS > 0
    /* 调试模拟（见文件顶部宏说明）：窗口期内强制报 DISCONNECTED。
     * 窗口起止各打一条日志：据此可直接判断"装的是不是带模拟的 build、
     * 窗口有没有到期"，无需再猜。 */
    {
        static uint64_t sim0 = 0;
        static int sim_logged = 0;
        uint64_t nms = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
        if (sim0 == 0) sim0 = nms;
        if (nms - sim0 < (uint64_t)PSVSEND_SIM_DOWN_MS) {
            if (!sim_logged) {
                sim_logged = 1;
                dlog("net: SIM down window ON (%d ms)", PSVSEND_SIM_DOWN_MS);
            }
            g_ctl_err = 0;
            g_ctl_st  = SCE_NETCTL_STATE_DISCONNECTED;
            return;
        }
        if (sim_logged == 1) {
            sim_logged = 2;
            dlog("net: SIM down window expired, real ctl query resumes");
        }
    }
#endif
    SceNetCtlInfo info;
    int st = SCE_NETCTL_STATE_DISCONNECTED;
    int r;
    if (g_state <= 0) return;            /* 网络栈未就绪：查询无意义 */
    ctl_lock();
    {
        /* 哨兵（诊断）：对"Wi-Fi 未就绪时 init 的坏 netctl 客户端"查询可能
         * 永久卡死不返回。每 ≥4s 打一对 begin/done：只见 begin 不见 done
         * 即坐实卡点（watch 线程死于此，60s recycle 也到不了）。 */
        static uint64_t last_log_ms = 0;
        uint64_t nm = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
        int want = (nm - last_log_ms >= 4000);
        if (want) {
            last_log_ms = nm;
            dlog("net: poll begin (cached st=%d)", g_ctl_st);
        }
        r = sceNetCtlInetGetState(&st);
        if (want)
            dlog("net: poll done r=%d st=%d", r, st);
        g_ctl_err = r;
        g_ctl_st  = st;
    }
    if (r == 0 && st == SCE_NETCTL_STATE_CONNECTED &&
        sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) == 0 &&
        info.ip_address[0]) {
        snprintf(g_ip, sizeof g_ip, "%s", info.ip_address);
    }
    /* 未连上 / 信息查询暂时失败：不清 g_ip。Wi-Fi 刚恢复时 GetInfo 可能晚于
     * GetState 就绪，若此时把 IP 清成 0.0.0.0，扫描门槛（ip != 0.0.0.0）会一直
     * 卡住、announce 组播出口也选不了——保留上次有效 IP 即可，重连后下一轮
     * poll（≤500ms）会覆盖成新地址。初始默认值仍为 0.0.0.0。 */
    ctl_unlock();
}

/* 本机 IP 缓存（只读，不做系统调用）；未轮询到为 "0.0.0.0" */
const char *net_local_ip(void)
{
    return g_ip;
}

/* 链路状态缓存（只读，不做系统调用）：UI/announce/scan 每帧可调无风险 */
bool net_connected(void)
{
    return g_state > 0 && g_ctl_st == SCE_NETCTL_STATE_CONNECTED;
}

int net_ctl_err(void)  { return g_ctl_err; }
int net_ctl_state(void){ return g_ctl_st; }

/* 断网期间的"活性刺激"：向组播 239.255.255.250 发一包 UDP。
 * 目的不是连通（无关联/无 IP 时必然失败或等待链路）——而是让无线驱动层感知
 * "有应用在要网络"，促使待机省电断开的热点自动重连。
 * 只能在主线程调用（Vita 的 UDP sendto 离开主线程会无限卡死）；Wi-Fi 正在重连
 * 时本调用会阻塞到链路可用（真机数秒），调用方（api_tick）需容忍该停顿。
 * 只要求 SceNet 栈（g_step&2）就绪：netctl 客户端没起来（Wi-Fi 全关时常见）
 * 不代表不能发包唤醒，故不以 g_state 整体作闸。 */
int net_poke(void)
{
    SceNetSockaddrIn a;
    int fd, r;
    if ((g_step & 2) == 0) return -2;     /* SceNet 栈没起来：poke 无意义 */
    fd = sceNetSocket("psvsend_wake", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (fd < 0) return fd;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(53317);
    a.sin_addr.s_addr = sceNetHtonl(0xEFFFFFFA);  /* 239.255.255.250 */
    r = sceNetSendto(fd, "x", 1, 0, (SceNetSockaddr *)&a, sizeof a);
    sceNetSocketClose(fd);
    return r;
}

/* 重建 netctl 客户端（在持续断网时由 watch 周期调用）。
 * 机制：netctl 若在 Wi-Fi 全程关闭时初始化，可能从此收不到之后的状态翻转——
 * 系统侧已经连上，但 sceNetCtlInetGetState 永远返回 DISCONNECTED，app 卡在
 * "未就绪"只能重启。term + 重新 init 换一个新客户端即可恢复感知。
 * 只在 net 栈已就绪时执行；与查询接口用 g_ctl_mtx 互斥。 */
void net_ctl_recycle(void)
{
    int r;
    if (g_ctl_mtx < 0) return;            /* 从未初始化过锁：放弃（理论不发生） */
    ctl_lock();
    g_ctl_err = 0;
    g_ctl_st  = -1;
    if (g_state > 0 && (g_step & 4)) {
        sceNetCtlTerm();                  /* 先拆旧客户端 */
        g_step &= ~4;
        dlog("net: netctl term done (was init ok)");
    }
    if (g_state > 0) {
        r = sceNetCtlInit();
        if (r == 0) {
            g_step |= 4;
            dlog("net: netctl re-init ok");
        } else {
            g_state = r;                  /* 回落到失败态：watch 会走 net_start 重试 */
            dlog("net: netctl re-init fail 0x%08X", (unsigned)r);
        }
    }
    ctl_unlock();
}
