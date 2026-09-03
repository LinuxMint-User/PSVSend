/* 发现实现（见 discovery.h）——PS Vita 特化版。
 * 真机事实：Vita 系统保留 53317 端口（TCP/UDP bind 都 EACCES），组播又收不了，
 * 标准"组播收发"在此平台不可行。本实现改为协议自带的两条仍可用的路径：
 *   1) 组播 announce 只发不收（http.c 的服务器接收对方反向 register）；
 *   2) 收到对方 HTTP POST /register 时把其信息记入设备表
 *      （discovery_peer_registered，由 http.c 调用）。
 * 由此双向互见完全不需要 bind 任何端口，绕开 53317 限制。
 * 线程模型：announce 节奏由 UI 主循环调 api_tick() 驱动（Vita SceNet 的 UDP
 * sendto 从非主线程调用会无限卡死，实测只在主循环上可靠）；http 服务器是独立
 * 线程，收到的 register 经回调记入设备表。 */
#include <string.h>
#include <stdio.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include "config.h"
#include "json_util.h"
#include "net.h"
#include "http.h"
#include "api.h"
#include "discovery.h"
#include "dlog.h"

#define DISC_ADDR    "224.0.0.167"
#define DISC_PORT    53317
#define ANNOUNCE_MS  5000          /* 广播间隔 */
#define STALE_MS     90000         /* 90s 没动静的设备移除（对方按 announce 周期回 register） */

typedef struct {
    Device   d;
    uint64_t last_ms;
} Entry;

static Entry  g_devs[API_MAX_DEVICES];
static int    g_count = 0;
static SceUID g_mtx = -1;          /* 保护设备表 */
static SceUID g_send_mtx = -1;     /* 保护发送 socket */
static int    g_tx_sock = -1;
static volatile int g_run = 0;
static int    g_state = 0;         /* 1 运行 / 0 未启动 / <0 启动失败码 */
static char   g_efail[96] = "";    /* 最近一次启动失败定位 */

static uint64_t now_ms(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide() / 1000;
}

/* ---------- 设备表 ---------- */
static void table_upsert(const Device *dev)
{
    int i, oldest = -1;
    uint64_t now = now_ms();
    if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
    for (i = 0; i < g_count; i++) {
        if (g_devs[i].d.fingerprint[0] && dev->fingerprint[0] &&
            strcmp(g_devs[i].d.fingerprint, dev->fingerprint) == 0)
            break;
        if (g_devs[i].d.fingerprint[0] == 0 &&
            strcmp(g_devs[i].d.ip, dev->ip) == 0)
            break;
    }
    if (i < g_count) {
        g_devs[i].d = *dev;
        g_devs[i].last_ms = now;
    } else {
        if (g_count < API_MAX_DEVICES) {
            g_devs[g_count].d = *dev;
            g_devs[g_count].last_ms = now;
            g_count++;
        } else {
            uint64_t oldest_t = (uint64_t)-1;
            for (i = 0; i < g_count; i++)
                if (g_devs[i].last_ms < oldest_t) { oldest_t = g_devs[i].last_ms; oldest = i; }
            if (oldest >= 0) {
                g_devs[oldest].d = *dev;
                g_devs[oldest].last_ms = now;
            }
        }
    }
    if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
}

static void table_purge(void)
{
    uint64_t now = now_ms();
    int i, w = 0;
    if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
    for (i = 0; i < g_count; i++) {
        if (now - g_devs[i].last_ms > STALE_MS) continue;
        if (w != i) g_devs[w] = g_devs[i];
        w++;
    }
    g_count = w;
    if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
}

/* 公开：http 服务器收到对方 register POST 时把对方记入设备表 */
void discovery_peer_registered(const char *body, const char *src_ip)
{
    Device dev;
    long long v;
    memset(&dev, 0, sizeof dev);
    if (!body || !body[0]) return;
    if (!json_get_str(body, "alias", dev.alias, sizeof dev.alias)) return;
    json_get_str(body, "fingerprint", dev.fingerprint, sizeof dev.fingerprint);
    if (g_cfg.fingerprint[0] && dev.fingerprint[0] &&
        strcmp(dev.fingerprint, g_cfg.fingerprint) == 0)
        return;                                /* 自己 */
    if (src_ip)
        snprintf(dev.ip, sizeof dev.ip, "%s", src_ip);
    json_get_str(body, "deviceModel", dev.model, sizeof dev.model);
    json_get_str(body, "deviceType", dev.dtype, sizeof dev.dtype);
    if (json_get_int(body, "port", &v) && v > 0 && v <= 65535)
        dev.port = (int)v;
    else
        dev.port = DISC_PORT;                  /* 对方默认协议端口 */
    if (!json_get_str(body, "protocol", dev.protocol, sizeof dev.protocol))
        snprintf(dev.protocol, sizeof dev.protocol, "%s", "http");
    dlog("disc: peer '%s' %s:%d via register", dev.alias, dev.ip, dev.port);
    table_upsert(&dev);
}

/* 公开：扫描线程发现设备后入表（带自排除：指纹与自己的设备忽略） */
void discovery_upsert_peer(const Device *dev)
{
    if (!dev || !dev->alias[0]) return;
    if (g_cfg.fingerprint[0] && dev->fingerprint[0] &&
        strcmp(dev->fingerprint, g_cfg.fingerprint) == 0)
        return;                                /* 自己 */
    table_upsert(dev);
}

/* ---------- 发送 ---------- */
/* 组播地址解析（sceNetInetPton 失败时的兜底手写解析） */
static bool ipv4_from_str(const char *str, unsigned int *netorder)
{
    unsigned a[4];
    if (sscanf(str, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4)
        return false;
    *netorder = (a[0] << 24) | (a[1] << 16) | (a[2] << 8) | a[3];
    return true;
}

static void announce_payload(char *out, int n)
{
    char ae[2 * sizeof g_cfg.alias];
    char fe[2 * sizeof g_cfg.fingerprint];
    int port = http_port();
    json_escape(g_cfg.alias, ae, sizeof ae);
    json_escape(g_cfg.fingerprint, fe, sizeof fe);
    snprintf(out, n,
             "{\"alias\":\"%s\",\"version\":\"2.0\","
             "\"deviceModel\":\"PlayStation Vita\",\"deviceType\":\"mobile\","
             "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"http\","
             "\"download\":true,\"announce\":true}",
             ae, fe, port > 0 ? port : DISC_PORT);
}

static int send_to(const SceNetSockaddrIn *dst)
{
    char body[600];
    int r = -1;
    announce_payload(body, sizeof body);
    if (g_send_mtx >= 0) sceKernelLockMutex(g_send_mtx, 1, NULL);
    if (g_tx_sock >= 0)
        r = sceNetSendto(g_tx_sock, body, (unsigned int)strlen(body), 0,
                         (const SceNetSockaddr *)dst, sizeof *dst);
    if (g_send_mtx >= 0) sceKernelUnlockMutex(g_send_mtx, 1);
    return r;
}

static int send_announce_group(void)
{
    SceNetSockaddrIn dst;
    unsigned int addr = 0;
    memset(&dst, 0, sizeof dst);
    dst.sin_len = sizeof dst;
    dst.sin_family = SCE_NET_AF_INET;
    dst.sin_port = sceNetHtons(DISC_PORT);
    if (sceNetInetPton(SCE_NET_AF_INET, DISC_ADDR, &addr) != 1)
        ipv4_from_str(DISC_ADDR, &addr);
    dst.sin_addr.s_addr = addr;
    return send_to(&dst);
}

static int send_announce_bcast(void)
{
    SceNetSockaddrIn dst;
    memset(&dst, 0, sizeof dst);
    dst.sin_len = sizeof dst;
    dst.sin_family = SCE_NET_AF_INET;
    dst.sin_port = sceNetHtons(DISC_PORT);
    dst.sin_addr.s_addr = 0xFFFFFFFFu;   /* 255.255.255.255 受限广播 */
    return send_to(&dst);
}

/* 定向广播 a.b.c.255：部分路由器会丢受限广播但放行子网定向广播 */
static int send_announce_dnet(void)
{
    const char *lip = net_local_ip();
    unsigned int ip4 = 0, bc = 0xFFFFFFFFu;
    if (sceNetInetPton(SCE_NET_AF_INET, lip, &ip4) != 1) return -1;
    bc = (ip4 & 0xFFFFFF00u) | 0xFFu;    /* 按 /24 估定向广播（Vita DHCP 常规） */
    if (bc == ip4) return -1;
    {
        SceNetSockaddrIn dst;
        memset(&dst, 0, sizeof dst);
        dst.sin_len = sizeof dst;
        dst.sin_family = SCE_NET_AF_INET;
        dst.sin_port = sceNetHtons(DISC_PORT);
        dst.sin_addr.s_addr = bc;
        return send_to(&dst);
    }
}

/* ---------- announce 发送（由 UI 主循环的 api_tick 每帧节流后调用） ---------- */
/* 数到第 10 次（5 秒）发一轮组播/广播/定向广播。 */
static int ann_ticks = 0;
static bool ann_first = true;
static int g_err = 1, b_err = 1, d_err = 1;
static int disc_open_tx(void);

void disc_tick_announce(void)
{
    if (g_state != 1 || !g_run) {
        /* 闸门挡路：记录一次，便于判断 tick 有被调用但状态被重置 */
        static int gate = 0;
        if ((++gate % 10) == 0)
            dlog("disc: tick gated (state=%d run=%d)", g_state, g_run);
        return;
    }
    if (++ann_ticks < 10) return;
    ann_ticks = 0;

    if (g_tx_sock < 0 && disc_open_tx() < 0)
        return;                     /* 发送 socket 没建起来，下一轮再试 */

    {
        int g = send_announce_group();
        int b = send_announce_bcast();
        int d = send_announce_dnet();
        if (ann_first) {
            ann_first = false;
            dlog("disc: first announce sent (group=%d bcast=%d dnet=%d)", g, b, d);
        }
        if (g < 0 && g != g_err) { g_err = g; dlog("disc: group send err 0x%08X", (unsigned)g); }
        if (b < 0 && b != b_err) { b_err = b; dlog("disc: bcast send err 0x%08X", (unsigned)b); }
        if (d < 0 && d != d_err) { d_err = d; dlog("disc: dnet send err 0x%08X", (unsigned)d); }
        table_purge();
    }
}

/* ---------- 发送 socket ----------
 * 真机坑：Vita SceNet 的 UDP sendto 从非主线程调用会无限卡死（无论 socket 归属），
 * 只有 UI 主循环能稳定发。因此 announce 节奏由 UI 调 api_tick 驱动（见 api.c），
 * 发送 socket 也在此路径上懒创建，保证"创建与使用都在主线程"。 */
static int disc_open_tx(void)
{
    int so_bcast = 1;
    int sock = sceNetSocket("psvsend_disc_tx", SCE_NET_AF_INET,
                            SCE_NET_SOCK_DGRAM, SCE_NET_IPPROTO_UDP);
    if (sock < 0) {
        dlog("disc: udp socket fail 0x%08X", (unsigned)sock);
        return sock;
    }
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST,
                     &so_bcast, sizeof so_bcast);
    /* 组播出口网卡 + TTL：不选网卡时部分协议栈默认不发/乱发组播 */
    {
        unsigned int ip4 = 0;
        const char *lip = net_local_ip();
        if (strcmp(lip, "0.0.0.0") != 0 &&
            sceNetInetPton(SCE_NET_AF_INET, lip, &ip4) == 1) {
            SceNetInAddr mif;
            unsigned char ttl = 8;
            int r;
            mif.s_addr = ip4;
            r = sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP,
                                 SCE_NET_IP_MULTICAST_IF, &mif, sizeof mif);
            dlog("disc: multicast if set -> %d", r);
            sceNetSetsockopt(sock, SCE_NET_IPPROTO_IP,
                             SCE_NET_IP_MULTICAST_TTL, &ttl, sizeof ttl);
        }
    }
    g_tx_sock = sock;
    return 0;
}

/* ---------- 启动 ---------- */
int discovery_start(void)
{
    if (g_state > 0) return 0;

    if (g_tx_sock >= 0) { sceNetSocketClose(g_tx_sock); g_tx_sock = -1; }
    g_state = 0;
    g_run = 0;

    if (g_mtx < 0)  g_mtx  = sceKernelCreateMutex("psvsend_disc_tbl", 0, 1, NULL);
    if (g_send_mtx < 0) g_send_mtx = sceKernelCreateMutex("psvsend_disc_snd", 0, 1, NULL);

    /* 发送 socket 不在这里建——由 UI 主循环的 announce 节奏懒创建（线程亲缘，见上） */

    g_run = 1;
    g_state = 1;
    g_efail[0] = 0;
    dlog("disc: announce up (http port %d)", http_port());
    return 0;
}

const char *discovery_fail_step(void)
{
    return g_efail;
}

int discovery_snapshot(Device *out, int max)
{
    int i, n;
    if (!out || max <= 0) return 0;
    if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
    n = g_count < max ? g_count : max;
    for (i = 0; i < n; i++) out[i] = g_devs[i].d;
    if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
    return n;
}

int discovery_device_count(void)
{
    int n;
    if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
    n = g_count;
    if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
    return n;
}

int discovery_state(void)
{
    return g_state;
}
