/* http.c 实现 —— 见 http.h。
 * 单线程 accept + 顺序处理；recv 带 3s 超时防挂死；每连接只处理一个请求后
 * 关闭（响应带 Connection: close）。只解析极简请求行 + Content-Length，
 * 缓冲区上限 16KB（register/info 载荷都很小），足够本阶段用。 */
#include <string.h>
#include <stdio.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <sys/time.h>
#include "config.h"
#include "json_util.h"
#include "http.h"
#include "dlog.h"

#define HTTP_RECV_TIMEOUT_S 3
#define HTTP_MAX_REQ        16384
#define HTTP_MAX_BODY       2048
/* Vita 的 SceNet socket 收/发在暂无数据时会立刻返回 EWOULDBLOCK(0x80410123)，
 * 不能当错误处理，必须轮询等数据/等窗口。 */
#define NET_AGAIN(r) ((r) == 0x80410123)
#define NET_RETRY_DELAY_US 20000   /* 20ms 轮询间隔 */

static int    g_port = 0;           /* 实际绑定端口（0=未启动） */
static int    g_lsock = -1;
static volatile int g_run = 0;
static void (*g_cb)(const char *body, const char *src_ip) = NULL;

/* ---------- 收/发工具 ---------- */
static int send_all(int fd, const char *data, int len)
{
    int off = 0;
    SceLong64 dl = sceKernelGetSystemTimeWide() + 2000000LL; /* 2s */
    while (off < len) {
        int r = sceNetSend(fd, data + off, (unsigned)(len - off), 0);
        if (r > 0) { off += r; continue; }
        if (r == 0 || sceKernelGetSystemTimeWide() >= dl) return -1;
        if (!NET_AGAIN(r)) return -1;
        sceKernelDelayThread(NET_RETRY_DELAY_US); /* 发窗口没空，等会儿再试 */
    }
    return 0;
}

static void http_respond(int fd, int code, const char *reason,
                         const char *body)
{
    char hdr[256];
    int blen = (int)strlen(body);
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: application/json\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             code, reason, blen);
    send_all(fd, hdr, (int)strlen(hdr));
    send_all(fd, body, blen);
}

static void member_info_json(char *out, int outsz)
{
    char ae[2 * sizeof g_cfg.alias];
    char fe[2 * sizeof g_cfg.fingerprint];
    json_escape(g_cfg.alias, ae, sizeof ae);
    json_escape(g_cfg.fingerprint, fe, sizeof fe);
    snprintf(out, outsz,
             "{\"alias\":\"%s\",\"version\":\"2.0\","
             "\"deviceModel\":\"PlayStation Vita\",\"deviceType\":\"mobile\","
             "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"http\","
             "\"download\":false}",
             ae, fe, g_port);
}

/* 解析 HTTP 请求并应答一个连接，返回后由调用者关 fd */
static void handle_conn(int c, const char *rip)
{
    char buf[HTTP_MAX_REQ];
    char body[HTTP_MAX_BODY];
    struct timeval tv;
    int n = 0, he = -1, i, cl = -1, need, start;
    const char *reqline;

    tv.tv_sec = HTTP_RECV_TIMEOUT_S;
    tv.tv_usec = 0;
    sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO,
                     &tv, sizeof tv);
    {
        SceLong64 dl = sceKernelGetSystemTimeWide()
                     + (SceLong64)HTTP_RECV_TIMEOUT_S * 1000000LL;

        /* 收请求直到出现空行（头结束）。注意：Vita 的 recv 无数据时立刻返回
         * EWOULDBLOCK，必须轮询到截止时间；每次收到后扫整个缓冲找结束符。 */
        while (n < HTTP_MAX_REQ - 1) {
            int r = sceNetRecv(c, buf + n, (unsigned)(HTTP_MAX_REQ - 1 - n), 0);
            if (r > 0) {
                n += r;
                buf[n] = 0;
                for (i = 0; i + 4 <= n && he < 0; i++)
                    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                        buf[i + 2] == '\r' && buf[i + 3] == '\n') he = i;
                if (he >= 0) break;
            } else if (r == 0 || sceKernelGetSystemTimeWide() >= dl) {
                dlog("http: recv stop r=0x%08X n=%d from %s",
                     (unsigned)r, n, rip);
                break;
            } else if (!NET_AGAIN(r)) {
                dlog("http: recv err r=0x%08X n=%d from %s",
                     (unsigned)r, n, rip);
                break;
            } else {
                sceKernelDelayThread(NET_RETRY_DELAY_US);
            }
        }
    }
    if (he < 0) {
        dlog("http: header incomplete n=%d from %s", n, rip);
        return;                 /* 头都没收全，直接断 */
    }

    /* Content-Length */
    {
        const char *p = strstr(buf, "Content-Length:");
        if (p && p < buf + he) {
            long v = 0;
            p += 15;
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            cl = (int)v;
        }
    }

    /* 把 body 补全（到 header_end+content-length），同样轮询等待 */
    start = he + 4;
    need = start + (cl > 0 ? cl : 0);
    if (need > HTTP_MAX_REQ) need = HTTP_MAX_REQ;
    {
        SceLong64 dl = sceKernelGetSystemTimeWide()
                     + (SceLong64)HTTP_RECV_TIMEOUT_S * 1000000LL;
        while (n < need) {
            int r = sceNetRecv(c, buf + n, (unsigned)(need - n), 0);
            if (r > 0) { n += r; continue; }
            if (r == 0 || sceKernelGetSystemTimeWide() >= dl) break;
            if (!NET_AGAIN(r)) break;
            sceKernelDelayThread(NET_RETRY_DELAY_US);
        }
    }

    reqline = buf;
    {
        char out[HTTP_MAX_BODY];
        const char *sp = strchr(reqline, ' ');
        char method[8] = "";
        char path[96] = "";
        if (sp) {
            int mlen = (int)(sp - reqline);
            if (mlen > 7) mlen = 7;
            memcpy(method, reqline, (unsigned)mlen);
            method[mlen] = 0;
            {
                const char *p2 = strchr(sp + 1, ' ');
                int plen = p2 ? (int)(p2 - (sp + 1)) : (int)strlen(sp + 1);
                if (plen > 95) plen = 95;
                memcpy(path, sp + 1, (unsigned)plen);
                path[plen] = 0;
            }
        }

        member_info_json(out, sizeof out);

        if (method[0] && path[0])
            dlog("http: %s %s from %s", method, path, rip);
        else
            dlog("http: unparsed req from %s", rip);

        if (strcmp(method, "GET") == 0 &&
            strcmp(path, "/api/localsend/v2/info") == 0) {
            http_respond(c, 200, "OK", out);
            return;
        }
        if (strcmp(method, "POST") == 0 &&
            strcmp(path, "/api/localsend/v2/register") == 0) {
            int blen = (int)(buf + n - (buf + start));
            if (cl >= 0 && cl < blen) blen = cl;
            if (cl >= 0 && cl > blen) { /* body 没收全也无妨，能解析多少算多少 */ }
            if (blen < 0) blen = 0;
            if (blen > HTTP_MAX_BODY - 1) blen = HTTP_MAX_BODY - 1;
            if (blen > 0) {
                memcpy(body, buf + start, (unsigned)blen);
                body[blen] = 0;
                dlog("http: register body %d bytes from %s", blen, rip);
                if (g_cb) g_cb(body, rip);
            } else {
                dlog("http: register with empty body from %s", rip);
            }
            http_respond(c, 200, "OK", out);
            return;
        }
        http_respond(c, 404, "Not Found", "{\"error\":\"not found\"}");
    }
}

/* ---------- accept 线程 ---------- */
static int http_thr(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (g_run) {
        SceNetSockaddrIn cli;
        unsigned int clen = sizeof cli;
        char ip[16] = "";
        int c = sceNetAccept(g_lsock, (SceNetSockaddr *)&cli, &clen);
        if (c < 0) {
            if (!g_run) break;
            sceKernelDelayThread(50000);
            continue;
        }
        if (sceNetInetNtop(SCE_NET_AF_INET, &cli.sin_addr.s_addr,
                           ip, sizeof ip) == NULL)
            snprintf(ip, sizeof ip, "?");
        dlog("http: conn from %s", ip);
        handle_conn(c, ip);
        sceNetSocketClose(c);
    }
    return 0;
}

/* ---------- 启动 ---------- */
int http_start(void)
{
    /* 真机实测：4567 可绑，53317（动态区高位）被系统保留 EACCES。
     * 候选顺序把已验证可用的低端口放前面，避免每次启动都白试高端口。 */
    static const int cand[] = { 4567, 53318, 53316, 53319, 55555, 0 };
    SceNetSockaddrIn sa;
    int one = 1, i, s = -1, chosen = 0;

    if (g_port > 0) return g_port;   /* 已在跑 */

    for (i = 0; i < (int)(sizeof cand / sizeof cand[0]); i++) {
        int r;
        s = sceNetSocket("psvsend_http", SCE_NET_AF_INET,
                         SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
        if (s < 0) { dlog("http: socket fail 0x%08X", (unsigned)s); break; }
        sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR,
                         &one, sizeof one);
        memset(&sa, 0, sizeof sa);
        sa.sin_len = sizeof sa;
        sa.sin_family = SCE_NET_AF_INET;
        sa.sin_port = sceNetHtons((unsigned)cand[i]);
        sa.sin_addr.s_addr = 0;   /* INADDR_ANY */
        r = sceNetBind(s, (const SceNetSockaddr *)&sa, sizeof sa);
        if (r == 0) { chosen = cand[i]; break; }
        dlog("http: bind :%d fail 0x%08X", cand[i], (unsigned)r);
        sceNetSocketClose(s);
        s = -1;
    }
    if (s < 0) {
        dlog("http: no bindable port");
        return -1;
    }
    if (sceNetListen(s, 4) < 0) {
        sceNetSocketClose(s);
        dlog("http: listen fail");
        return -2;
    }
    g_lsock = s;
    g_port = chosen > 0 ? chosen : 0;
    g_run = 1;
    {
        SceUID t = sceKernelCreateThread("psvsend_http", http_thr,
                                         0x40, 0x10000, 0, 0, NULL);
        dlog("http: thread create -> 0x%08X", (unsigned)t);
        if (t >= 0) {
            int sr = sceKernelStartThread(t, 0, NULL);
            dlog("http: thread start -> 0x%08X", (unsigned)sr);
        }
    }
    dlog("http: up on port %d", g_port);
    return g_port;
}

int http_port(void)
{
    return g_port;
}

void http_set_register_cb(void (*cb)(const char *body, const char *src_ip))
{
    g_cb = cb;
}
