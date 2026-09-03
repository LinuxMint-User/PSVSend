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
#include "net.h"
#include "dlog.h"

#define NET_POOL_SIZE (1 * 1024 * 1024)   /* SceNet 内存池 */
static char g_pool[NET_POOL_SIZE] __attribute__((aligned(64)));
static int g_state = 0;                   /* 0 未启动 / 1 就绪 / <0 失败码 */
static int g_ctl_err = 0;                 /* 最近一次 netctl 状态查询返回码 */
static int g_ctl_st  = -1;                /* 最近一次读到的网络状态 */

int net_start(void)
{
    SceNetInitParam param;
    int r;
    if (g_state > 0) return 0;
    if (g_state < 0) g_state = 0;         /* 失败过：允许上层重试 */

    r = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    if (r < 0) { g_state = r; dlog("net: load SCE_SYSMODULE_NET fail 0x%08X", (unsigned)r); return r; }

    memset(&param, 0, sizeof param);
    param.memory = g_pool;
    param.size = sizeof g_pool;
    param.flags = 0;
    r = sceNetInit(&param);
    if (r < 0) { g_state = r; dlog("net: sceNetInit fail 0x%08X", (unsigned)r); return r; }

    /* 网络状态/本机 IP 查询需要 netctl（失败不致命，net_connected 会读到错误码） */
    r = sceNetCtlInit();
    if (r < 0) dlog("net: sceNetCtlInit fail 0x%08X", (unsigned)r);
    else       dlog("net: ok (pool %d KB)", NET_POOL_SIZE / 1024);

    g_state = 1;
    return 0;
}

int net_state(void)
{
    return g_state;
}

const char *net_local_ip(void)
{
    static char buf[16] = "0.0.0.0";
    SceNetCtlInfo info;
    if (g_state <= 0) return buf;
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info) == 0 &&
        info.ip_address[0]) {
        snprintf(buf, sizeof buf, "%s", info.ip_address);
    } else {
        snprintf(buf, sizeof buf, "%s", "0.0.0.0");
    }
    return buf;
}

bool net_connected(void)
{
    int st = SCE_NETCTL_STATE_DISCONNECTED;
    int r;
    if (g_state <= 0) { g_ctl_err = -1; return false; }
    r = sceNetCtlInetGetState(&st);
    g_ctl_err = r;
    g_ctl_st  = st;
    if (r < 0) return false;
    return st == SCE_NETCTL_STATE_CONNECTED;
}

int net_ctl_err(void)  { return g_ctl_err; }
int net_ctl_state(void){ return g_ctl_st; }
