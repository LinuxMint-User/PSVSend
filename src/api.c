/* api 实现：把 config / net / discovery 组装成前端可调的启动入口与快照接口。
 * 线程结构（真机实测：sceKernelCreateThread 的 initPriority 不能带 0x10000000
 * 高位，否则返回 0x80028023 ILLEGAL_PRIORITY；裸 0x40 可用）：
 *   主线程          —— UI 渲染 + announce 节奏（api_tick：Vita 的 UDP sendto
 *                      只在主循环可靠，独立线程里会无限卡死）
 *   api_watch 线程  —— 每 500ms 网络巡检：net 重试 / http 拉起 / discovery 启动
 *   http 线程       —— accept 并处理对方 register/info
 * 启动策略：PSV 待机唤醒/开机后 Wi-Fi 常常还在重连，若启动瞬间网络未就绪，
 * 发现模块初始化会失败。因此 net/discovery 都不在启动瞬间判死，而是由
 * api_watch 线程周期性巡检重试，状态会自行恢复，无需重启 app。 */
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "config.h"
#include "net.h"
#include "http.h"
#include "discovery.h"
#include "receive.h"
#include "api.h"
#include "identity.h"
#include "dlog.h"

#define DISC_RETRY_MS 2000           /* 发现失败后的重试冷却 */

/* 一次巡检。由 api_start（首次同步）与 api_watch 线程调用。
 * 链路自愈（Q2）：待机唤醒/断线重连后，旧监听 socket 与 UDP 发送 socket 可能
 * 已失效（netctl 不一定报断开）。这里盯三类信号并在恢复时拆旧建新：
 *   - Wi-Fi 状态翻转（down→stop http；up→announce 重建发送 socket）；
 *   - 本机 IP 变化（announce 用本地 IP 选组播出口/算定向广播，变了要重建）；
 *   - http 探活（accept 线程因监听持续报错自尽 → 重绑）。 */
static void watch_once(void)
{
    static int l_net = 0, l_disc = 0;
    static int l_up = -1;
    static char lip[16] = "";
    static uint64_t last_disc_ms = 0;
    static uint64_t dn_since_ms = 0;   /* 本次持续断网的起始时刻（0=在线） */
    static uint64_t last_dn_ms = 0;    /* 上次"still down"日志时刻 */
    static unsigned probe = 0;
    uint64_t now;
    int r;

    if (net_state() <= 0) {              /* 网络栈没起来/失败过 → 重试 */
        r = net_start();
        if (r != l_net) {
            if (r == 0) dlog("watch: net_start ok");
            else        dlog("watch: net_start retry fail 0x%08X", (unsigned)r);
            l_net = r;
        }
    }
    if (net_state() <= 0) return;

    net_poll();                          /* 唯一的 netctl 查询点：刷新链路/IP 缓存
                                          * （本线程每 500ms 一次；UI 只读缓存） */
    {
        bool up = net_connected();
        if (up != (l_up == 1)) {
            if (up) {
                dlog("watch: link UP (ctl_st=%d) -> recycle announce tx",
                     net_ctl_state());
                disc_link_changed();   /* 断网期间建的发送 socket 一律重开 */
                lip[0] = 0;            /* 重登记 IP（必要时重绑 announce 出口） */
            } else {
                dlog("watch: link DOWN (ctl_st=%d ctl_err=0x%08X) -> stop http, "
                     "clear table", net_ctl_state(), (unsigned)net_ctl_err());
                http_stop();           /* 拆旧监听；恢复后由下面 http_start 重绑 */
                disc_link_changed();
                discovery_clear();     /* 断网瞬间旧设备即作废：唤醒后不残留 */
            }
            l_up = up ? 1 : 0;
        }
        if (up) {
            dn_since_ms = 0;             /* 在线：清断网计时 */
        } else {
            uint64_t nm = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
            if (dn_since_ms == 0) dn_since_ms = nm;
            if (nm - dn_since_ms >= 60000) {
                /* 持续断网 60s：重建 netctl 客户端（兜底解"状态查询卡死"）。
                 * 低频执行；唤醒重连靠主线程 api_tick 的活性刺激，不在这里发 */
                dn_since_ms = nm;
                net_ctl_recycle();
            }
            if (nm - last_dn_ms >= 10000) {
                last_dn_ms = nm;
                dlog("watch: still down (ctl_st=%d ctl_err=0x%08X)",
                     net_ctl_state(), (unsigned)net_ctl_err());
            }
            return;
        }

        {
            const char *ip = net_local_ip();
            if (strcmp(ip, lip) != 0) {
                if (lip[0])
                    dlog("watch: ip %s -> %s, recycle link sockets", lip, ip);
                else
                    dlog("watch: local ip=%s", ip);
                snprintf(lip, sizeof lip, "%s", ip);
                disc_link_changed();
            }
        }

        /* http 探活：accept 线程因监听 socket 持续报错自尽/从未起来 → 重建 */
        if (http_port() > 0 && (++probe % 4) == 0 && !http_alive()) {
            dlog("watch: http dead (was :%d) -> restart", http_port());
            r = http_restart();
            dlog("watch: http restart (dead) -> %d", r);
        }

        /* HTTP 服务器是"对方 register 到我们"的入口，先把它拉起来
         * （绑定的是可绑端口，不受 53317 保留限制） */
        if (http_port() == 0) {
            r = http_start();
            if (r > 0) dlog("watch: http up on :%d", r);
            else       dlog("watch: http start fail (%d)", r);
        }

        now = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
        if (discovery_state() < 0) {       /* 上次失败：冷却后再试，避免空转 */
            if (now - last_disc_ms < DISC_RETRY_MS) return;
        } else if (discovery_state() > 0) {
            return;                        /* 已就绪 */
        }
        /* 从未启动 或 冷却结束：尝试（重）启动发现 */
        last_disc_ms = now;
        {
            r = discovery_start();
            if (r != l_disc) {
                if (r == 0) dlog("watch: discovery ok");
                else        dlog("watch: discovery retry fail 0x%08X", (unsigned)r);
                l_disc = r;
            }
        }
    }
}

/* UI 主循环每帧调用（循环顶部）：500ms 节流喂一次 announce 节奏。
 * 发送 socket 与 sendto 都在主线程上——Vita SceNet 的 UDP sendto 离开主线程
 * 会无限卡死，勿移到后台线程。announce socket 已设非阻塞，此处不会阻塞渲染；
 * 断网活性刺激（会阻塞数秒的唤醒包）单独放 api_poke()，由主循环在 swap 后调用。 */
void api_tick(void)
{
    static uint64_t last_ms = 0;
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
    if (now - last_ms < 500) return;
    last_ms = now;
    disc_tick_announce();
}

/* UI 主循环每帧调用（一帧渲染完、swap 之后）：断网时的"活性刺激"。
 * 每 4s 发一包出站 UDP，让系统感知"应用仍需网络"，促使待机省电断开的热点
 * 自动重连（链路恢复后本分支自然停发）。Wi-Fi 正在重连时该 sendto 会阻塞到
 * 链路可用（真机数秒）——放在 swap 之后，停顿期间屏幕保持当前帧，恢复后继续。 */
void api_poke(void)
{
    static uint64_t last_poke_ms = 0;
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
    if (net_connected() || now - last_poke_ms < 4000) return;
    last_poke_ms = now;
    {
        dlog("net: poke begin (ctl_st=%d)", net_ctl_state());  /* 诊断：若发不出去会阻塞 */
        int pr = net_poke();              /* -2 = SceNet 栈未就绪，不发（静默） */
        if (pr != -2)
            dlog("net: poke -> 0x%08X (ctl_st=%d)",
                 (unsigned)pr, net_ctl_state());
    }
}

/* api_watch 线程：每 500ms 做一次网络巡检（announce 由 UI 的 api_tick 驱动）。
 * 网络栈/发现失败时在这里重试，状态自动恢复，UI 无需参与。 */
static int api_watch_thr(SceSize args, void *argp)
{
    int loops = 0;
    uint64_t last_tick = 0;
    (void)args; (void)argp;
    dlog("api: watch thread entered tid=%d", (int)sceKernelGetThreadId());
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        loops++;
        /* watch 心跳（诊断，与 net_poll 解耦）：循环每次 tick 前打一条节流日志。
         * 若 tick 持续出现 → 循环活着、卡点在 watch_once 内部；
         * 若 tick 中断 → watch 线程本身死了/停摆（与 net_poll 无关）。 */
        {
            uint64_t nm = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
            if (nm - last_tick >= 2000) {
                last_tick = nm;
                dlog("watch: tick %d (st=%d disc=%d)", loops,
                     net_ctl_state(), discovery_state());
            }
        }
        watch_once();
    }
    return 0;
}

void api_start(void)
{
    int r;
    config_init();                       /* 先建目录/读配置（dlog 目录依赖它） */
    dlog_init();
    dlog("== psvsend boot [TAG:d30] ==");
    {
        /* 版本标记 + 设备身份指纹：确认刷入的固件含 mTLS 客户端证书 */
        char f[65];
        if (identity_fingerprint(f) == 0)
            dlog("api: build mtls, device id fp=%s", f);
        else
            dlog("api: build mtls, identity parse FAILED");
    }

    http_set_register_cb(discovery_peer_registered);   /* 对方 register → 设备表 */
    recv_init();                         /* 接收模块：建锁 + 清扫残留 *.part */

    r = net_start();
    dlog("watch: initial net_start -> %s", r == 0 ? "ok" : "fail");
    watch_once();                        /* 立即巡检一次：连着的 Wi-Fi 不用等 500ms */
    {
        SceUID t = sceKernelCreateThread("psvsend_api_watch", api_watch_thr,
                                         0x40, 0x10000, 0, 0, NULL);
        dlog("api: watch thread create -> 0x%08X", (unsigned)t);
        if (t >= 0) {
            int sr = sceKernelStartThread(t, 0, NULL);
            dlog("api: watch thread start -> 0x%08X", (unsigned)sr);
        }
    }
}

int api_device_snapshot(Device *out, int max)
{
    return discovery_snapshot(out, max);
}

bool api_network_ready(void)
{
    return discovery_state() > 0;
}

int api_discovery_state(void)
{
    return discovery_state();
}

const char *api_discovery_fail(void)
{
    return discovery_fail_step();
}
