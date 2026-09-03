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

/* 一次巡检。由 api_start（首次同步）与 api_watch 线程调用。 */
static void watch_once(void)
{
    static int l_net = 0, l_disc = 0, l_up = -1;
    static char lip[16] = "";
    static uint64_t last_disc_ms = 0;
    uint64_t now;

    if (net_state() <= 0) {              /* 网络栈没起来/失败过 → 重试 */
        int r = net_start();
        if (r != l_net) {
            if (r == 0) dlog("watch: net_start ok");
            else        dlog("watch: net_start retry fail 0x%08X", (unsigned)r);
            l_net = r;
        }
    }
    if (net_state() <= 0) return;

    {
        bool up = net_connected();
        if (up != (l_up == 1)) {
            if (up) dlog("watch: wifi connected (ctl_st=%d)", net_ctl_state());
            else    dlog("watch: wifi NOT connected (ctl_st=%d ctl_err=0x%08X)",
                         net_ctl_state(), (unsigned)net_ctl_err());
            l_up = up ? 1 : 0;
        }
        if (!up) return;

        const char *ip = net_local_ip();
        if (strcmp(ip, lip) != 0) {
            snprintf(lip, sizeof lip, "%s", ip);
            dlog("watch: local ip=%s", ip);
        }

        /* HTTP 服务器是"对方 register 到我们"的入口，先把它拉起来
         * （绑定的是可绑端口，不受 53317 保留限制） */
        if (http_port() == 0) {
            int hr = http_start();
            if (hr > 0) dlog("watch: http up on :%d", hr);
            else        dlog("watch: http start fail (%d)", hr);
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
            int r = discovery_start();
            if (r != l_disc) {
                if (r == 0) dlog("watch: discovery ok");
                else        dlog("watch: discovery retry fail 0x%08X", (unsigned)r);
                l_disc = r;
            }
        }
    }
}

/* UI 主循环每帧调用：500ms 节流喂一次 announce 节奏（发送 socket 与发送都在
 * 主线程上——Vita SceNet 的 UDP sendto 离开主线程会无限卡死，勿移到后台线程）。 */
void api_tick(void)
{
    static uint64_t last_ms = 0;
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
    if (now - last_ms < 500) return;
    last_ms = now;
    disc_tick_announce();
}

/* api_watch 线程：每 500ms 做一次网络巡检（announce 由 UI 的 api_tick 驱动）。
 * 网络栈/发现失败时在这里重试，状态自动恢复，UI 无需参与。 */
static int api_watch_thr(SceSize args, void *argp)
{
    int loops = 0;
    (void)args; (void)argp;
    dlog("api: watch thread entered");
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        watch_once();
    }
    return 0;
}

void api_start(void)
{
    int r;
    config_init();                       /* 先建目录/读配置（dlog 目录依赖它） */
    dlog_init();
    dlog("== psvsend boot ==");
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
