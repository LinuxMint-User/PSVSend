/* httpc.c —— 极简明文 HTTP/1.1 客户端（见 httpc.h）。
 *
 * 连接与收发的节奏沿用 scan.c 的真机经验：
 *  - 阻塞 connect 没有超时，目标不在线会把线程永久卡住 → 非阻塞 connect +
 *    getpeername 轮询；
 *  - 收数据用非阻塞 recv + 1ms 轮询，每段有独立截止时间（Vita 的 recv 没数据
 *    立刻返回 EWOULDBLOCK，睡太久等于每个 socket 缓冲周期白等）。
 * 不实现 DNS（host 必须是 IPv4 字面量）、不实现 chunked 下载（我们自己的网页
 * 共享始终带 Content-Length）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "net/httpc.h"
#include "net/http.h"
#include "core/dlog.h"

#define HC_POLL_US      1000
#define HC_BODY_CHUNK   65536
#define NET_AGAIN(r)    ((r) == 0x80410123)

static uint64_t hc_now_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

/* 非阻塞 connect + getpeername 轮询；返回 fd 或 -1（预算内没连上） */
static int hc_connect(const char *host, int port, int timeout_ms)
{
    SceNetSockaddrIn sa;
    uint64_t dl = hc_now_us() + (uint64_t)timeout_ms * 1000;
    int one = 1, fd;

    fd = sceNetSocket("psvsend_httpc", SCE_NET_AF_INET,
                      SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
    if (fd < 0) return -1;
    if (sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one,
                         sizeof one) != 0) {
        sceNetSocketClose(fd);
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons((unsigned)port);
    if (sceNetInetPton(SCE_NET_AF_INET, host, &sa.sin_addr.s_addr) != 1) {
        sceNetSocketClose(fd);
        return -1;
    }
    {
        int cr = sceNetConnect(fd, (const SceNetSockaddr *)&sa, sizeof sa);
        if (cr == 0) return fd;
        if (cr != SCE_NET_ERROR_EWOULDBLOCK &&
            cr != SCE_NET_ERROR_EINPROGRESS &&
            cr != SCE_NET_ERROR_EALREADY) {
            sceNetSocketClose(fd);
            return -1;
        }
    }
    for (;;) {
        int so = 0;
        unsigned int sl = sizeof so;
        if (sceNetGetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                             &so, &sl) == 0 && so != 0)
            break;                          /* connect 已失败 */
        {
            SceNetSockaddrIn p;
            unsigned int plen = sizeof p;
            if (sceNetGetpeername(fd, (SceNetSockaddr *)&p, &plen) == 0)
                return fd;                  /* 连接建立 */
        }
        if (hc_now_us() >= dl) break;
        sceKernelDelayThread(HC_POLL_US);
    }
    sceNetSocketClose(fd);
    return -1;
}

static int hc_send_all(int fd, const char *data, int len, uint64_t dl)
{
    int off = 0;
    while (off < len) {
        int r = sceNetSend(fd, data + off, (unsigned)(len - off), 0);
        if (r > 0) { off += r; continue; }
        if (r == 0) return -1;
        if (NET_AGAIN(r)) {
            if (hc_now_us() >= dl) return -1;
            sceKernelDelayThread(HC_POLL_US);
            continue;
        }
        return -1;
    }
    return 0;
}

/* 收一段：>0 字节数；0=对端关闭；-1=超时/错误 */
static int hc_recv(int fd, char *buf, int cap, uint64_t dl)
{
    for (;;) {
        int r = sceNetRecv(fd, buf, (unsigned)cap, 0);
        if (r > 0) return r;
        if (r == 0) return 0;
        if (NET_AGAIN(r)) {
            if (hc_now_us() >= dl) return -1;
            sceKernelDelayThread(HC_POLL_US);
            continue;
        }
        return -1;
    }
}

/* 头区里的 Content-Length（64 位；大文件不受 int 上限约束）。
 * 返回 1=找到且合法，0=没有该头，-1=畸形/溢出。 */
static int hc_hdr_len(const char *b, int he, SceLong64 *out)
{
    int i = 0, found = 0;
    SceLong64 v = 0;

    while (i < he) {
        const char *line = b + i;
        const char *eol = memchr(line, '\n', (size_t)(he - i));
        int len = eol ? (int)(eol - line) : he - i;
        if (len > 15 && strncasecmp(line, "content-length:", 15) == 0) {
            const char *p = line + 15, *end = line + len;
            while (p < end && (*p == ' ' || *p == '\t')) p++;
            if (p >= end) return -1;
            v = 0;
            found = 1;
            while (p < end && *p >= '0' && *p <= '9') {
                int d = *p - '0';
                if (v > (0x3FFFFFFFFFFFFFFFLL - d) / 10) return -1;
                v = v * 10 + d;
                p++;
            }
            if (p != end && *p != '\r') return -1;
        }
        if (!eol) break;
        i = (int)(eol - b) + 1;
    }
    if (!found) return 0;
    *out = v;
    return 1;
}

static int hc_status(const char *b)
{
    const char *sp;
    if (strncmp(b, "HTTP/1.", 7) != 0) return 0;
    sp = strchr(b, ' ');
    return sp ? atoi(sp + 1) : 0;
}

int httpc_post(const char *host, int port, const char *path,
               const char *body, char *resp, int cap, int *status,
               int timeout_ms)
{
    char hdr[512];
    int fd, hlen, blen = body ? (int)strlen(body) : 0;
    int n = 0, he = -1, code = 0, body_off = 0;
    int chunked = 0;
    SceLong64 cl = -1;
    uint64_t dl;

    if (status) *status = 0;
    if (!host || !path || !resp || cap < 2) return -1;
    fd = hc_connect(host, port, timeout_ms);
    if (fd < 0) {
        dlog("httpc: connect %s:%d fail", host, port);
        return -1;
    }
    hlen = snprintf(hdr, sizeof hdr,
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "User-Agent: PSVSend\r\n"
                    "Accept: */*\r\n"
                    "Content-Type: application/json\r\n"
                    "Content-Length: %d\r\n"
                    "Connection: close\r\n\r\n",
                    path, host, port, blen);
    dl = hc_now_us() + (uint64_t)timeout_ms * 1000;
    if (hc_send_all(fd, hdr, hlen, dl) != 0 ||
        (blen > 0 && hc_send_all(fd, body, blen, dl) != 0)) {
        sceNetSocketClose(fd);
        return -1;
    }

    resp[0] = 0;
    for (;;) {
        int r, complete = 0;
        if (n >= cap - 1) {                 /* 响应过大：按失败处理 */
            dlog("httpc: post resp exceeds %d bytes", cap);
            sceNetSocketClose(fd);
            return -1;
        }
        r = hc_recv(fd, resp + n, cap - 1 - n, dl);
        if (r < 0) { sceNetSocketClose(fd); return -1; }
        n += r;
        resp[n] = 0;
        if (he < 0) {
            char *p = strstr(resp, "\r\n\r\n");
            if (p) {
                int lr;
                he = (int)(p - resp);
                body_off = he + 4;
                code = hc_status(resp);
                lr = hc_hdr_len(resp, he, &cl);
                if (lr < 0) { sceNetSocketClose(fd); return -1; }
                if (lr == 0) cl = -1;
                chunked = http_hdr_chunked(resp, he);
            }
        }
        if (he >= 0) {
            if (cl >= 0 && (SceLong64)(n - body_off) >= cl) complete = 1;
            else if (cl < 0 && r == 0) complete = 1;   /* 无长度：读到关连接 */
        }
        if (complete) break;
        if (r == 0) {                       /* 提前关闭：长度不足 */
            sceNetSocketClose(fd);
            dlog("httpc: post %s short response", path);
            return -1;
        }
    }
    sceNetSocketClose(fd);

    if (he < 0) return -1;
    if (status) *status = code;
    {
        int bl = n - body_off;
        if (bl < 0) bl = 0;
        if (chunked) {
            int dl2 = http_chunked_decode(resp + body_off, bl);
            if (dl2 < 0) return -1;
            bl = dl2;
        } else if (cl >= 0 && (SceLong64)bl > cl) {
            bl = (int)cl;
        }
        memmove(resp, resp + body_off, (size_t)bl);
        resp[bl] = 0;
    }
    return 0;
}

int httpc_get_stream(const char *host, int port, const char *path,
                     httpc_sink_fn sink, void *ctx, int64_t *total,
                     int *status, int timeout_ms)
{
    char hdr[512], hb[4096];
    unsigned char *body;
    int fd, hlen, n = 0, he = -1, code = 0;
    SceLong64 cl = -1;
    int64_t got = 0;
    uint64_t dl;

    if (total) *total = -1;
    if (status) *status = 0;
    if (!host || !path || !sink) return -1;
    fd = hc_connect(host, port, timeout_ms);
    if (fd < 0) {
        dlog("httpc: get connect %s:%d fail", host, port);
        return -1;
    }
    hlen = snprintf(hdr, sizeof hdr,
                    "GET %s HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "User-Agent: PSVSend\r\n"
                    "Accept: */*\r\n"
                    "Connection: close\r\n\r\n",
                    path, host, port);
    dl = hc_now_us() + (uint64_t)timeout_ms * 1000;
    if (hc_send_all(fd, hdr, hlen, dl) != 0) {
        sceNetSocketClose(fd);
        return -1;
    }

    while (he < 0) {
        int r;
        if (n >= (int)sizeof hb - 1) {
            sceNetSocketClose(fd);
            return -1;                      /* 响应头异常长 */
        }
        r = hc_recv(fd, hb + n, (int)sizeof hb - 1 - n, dl);
        if (r <= 0) { sceNetSocketClose(fd); return -1; }
        n += r;
        hb[n] = 0;
        {
            char *p = strstr(hb, "\r\n\r\n");
            if (p) he = (int)(p - hb);
        }
    }
    code = hc_status(hb);
    if (status) *status = code;
    if (code != 200) {
        sceNetSocketClose(fd);
        dlog("httpc: get %s -> %d", path, code);
        return -3;
    }
    if (http_hdr_chunked(hb, he)) {
        sceNetSocketClose(fd);
        return -4;
    }
    {
        int lr = hc_hdr_len(hb, he, &cl);
        if (lr <= 0) {                      /* 无长度：本功能不支持（无进度/无法校验） */
            sceNetSocketClose(fd);
            return -4;
        }
    }
    if (total) *total = (int64_t)cl;

    /* 头区之后已经读到的 body 片段先交给 sink */
    {
        int off = he + 4;
        int avail = n - off;
        if (avail > 0 && (SceLong64)avail > cl) avail = (int)cl;
        if (avail > 0) {
            if (sink(ctx, (const unsigned char *)hb + off, avail) < 0) {
                sceNetSocketClose(fd);
                return -2;
            }
            got = avail;
        }
    }
    body = (unsigned char *)malloc(HC_BODY_CHUNK);
    if (!body) { sceNetSocketClose(fd); return -1; }
    while (got < cl) {
        int want = (cl - got) > HC_BODY_CHUNK ? HC_BODY_CHUNK : (int)(cl - got);
        int r = hc_recv(fd, (char *)body, want, hc_now_us() + (uint64_t)timeout_ms * 1000);
        if (r <= 0) {
            dlog("httpc: get %s body short %lld/%lld", path,
                 (long long)got, (long long)cl);
            free(body);
            sceNetSocketClose(fd);
            return -1;
        }
        if (sink(ctx, body, r) < 0) {
            free(body);
            sceNetSocketClose(fd);
            return -2;
        }
        got += r;
    }
    free(body);
    sceNetSocketClose(fd);
    return 0;
}
