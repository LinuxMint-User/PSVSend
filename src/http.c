/* http.c 实现 —— 见 http.h。
 * 单线程 accept + 顺序处理；recv 带超时防挂死；每连接只处理一个请求后关闭
 * （响应带 Connection: close）。除接收大文件 body 是"流式边读边转给 receive 模块"
 * 外，其余请求（info/register/prepare-upload/cancel）头+体都很小，读进内存即可。
 * Vita 的 SceNet socket 收/发在暂无数据时会立刻返回 EWOULDBLOCK(0x80410123)，
 * 不能当错误处理，必须轮询等数据/等窗口。 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <sys/time.h>
#include "config.h"
#include "json_util.h"
#include "http.h"
#include "receive.h"
#include "dlog.h"

#define HTTP_RECV_TIMEOUT_S 3
#define HTTP_MAX_REQ        16384
#define HTTP_MAX_BODY       2048    /* register 体上限 */
#define HTTP_MAX_PREPARE    8192    /* prepare-upload 体上限（清单 JSON） */
#define UPLOAD_IDLE_US      (30 * 1000000LL)  /* 收体时单块读的空闲上限 */
#define NET_AGAIN(r) ((r) == 0x80410123)
#define NET_RETRY_DELAY_US 20000    /* 20ms 轮询间隔 */

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

static const char *reason_of(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
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
             "\"download\":true}",
             ae, fe, g_port);
}

/* 在请求头区（buf[0..he]，不含 body）逐行找 "name: <数字>"；HTTP 头名
 * 大小写不敏感（Dart/Go 客户端发小写 "content-length"，不能用大写匹配）。
 * 找到返回 true 并填 v；找不到返回 false。 */
static bool hdr_value(const char *buf, int he, const char *name, SceLong64 *v)
{
    int nl = (int)strlen(name);
    int end = he + 3;                  /* header 区含结尾 \r\n\r\n */
    int i = 0;
    while (i + nl <= he) {
        const char *ln = buf + i;
        const char *eol = memchr(ln, '\n', (size_t)(end - i));
        int le;
        if (!eol) break;
        le = (int)(eol - ln);
        if (le > 0 && ln[le - 1] == '\r') le--;   /* 去掉行尾 \r，le=内容长 */
        if (le == 0) break;                        /* 空行：header 区结束 */
        if (le >= nl && strncasecmp(ln, name, (size_t)nl) == 0 &&
            ln[nl] == ':') {
            const char *q = ln + nl + 1;
            while (*q == ' ' || *q == '\t') q++;
            *v = 0;
            while (*q >= '0' && *q <= '9')
                *v = *v * 10 + (*q - '0'), q++;
            return true;
        }
        i = (int)(eol - buf) + 1;      /* 下一行从 \n 之后开始 */
    }
    return false;
}

/* 同上，但把头的值拷成字符串（用于 transfer-encoding 等） */
static bool hdr_copy(const char *buf, int he, const char *name, char *out, int cap)
{
    int nl = (int)strlen(name);
    int end = he + 3;
    int i = 0;
    while (i + nl <= he) {
        const char *ln = buf + i;
        const char *eol = memchr(ln, '\n', (size_t)(end - i));
        const char *q;
        int le, vl;
        if (!eol) break;
        le = (int)(eol - ln);
        if (le > 0 && ln[le - 1] == '\r') le--;
        if (le == 0) break;
        if (le >= nl && strncasecmp(ln, name, (size_t)nl) == 0 && ln[nl] == ':') {
            q = ln + nl + 1;
            while (*q == ' ' || *q == '\t') q++;
            vl = (int)(eol - q);
            if (vl > 0 && q[vl - 1] == '\r') vl--;
            if (vl >= cap) vl = cap - 1;
            memcpy(out, q, (unsigned)vl);
            out[vl] = 0;
            return true;
        }
        i = (int)(eol - buf) + 1;
    }
    out[0] = 0;
    return false;
}

/* 大小写不敏感子串查找（newlib 未必有 strcasestr） */
static bool str_has_ci(const char *s, const char *sub)
{
    int nl = (int)strlen(sub);
    for (; *s; s++)
        if (strncasecmp(s, sub, (size_t)nl) == 0) return true;
    return false;
}

/* ---------- upload 大文件的流式读回调（receive 模块轮询调用） ---------- */
typedef struct {
    int  fd;
    const char *left;   /* 缓冲里已读但未消费的 body */
    int  llen;
    int  chunked;       /* Transfer-Encoding: chunked（dio 无 CL 流式上传） */
    int  ph;            /* chunked: 0=chunk 大小行 1=chunk 数据 2=终块 trailer */
    SceLong64 c_rem;    /* chunked: 当前 chunk 剩余数据字节 */
    unsigned char sbuf[4096];  /* chunked 解码输入暂存 */
    int  s_n, s_pos;
} UploadCtx;

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* chunked 解码输入：先消费头缓冲残留，其次从 fd 读，暂存 sbuf。
 * 返回 1=有新数据 0=对端关闭 -1=错 -2=空闲超时 */
static int u_refill(UploadCtx *u)
{
    if (u->s_pos < u->s_n) return 1;
    u->s_pos = u->s_n = 0;
    if (u->llen > 0) {
        int k = u->llen < (int)sizeof u->sbuf ? u->llen : (int)sizeof u->sbuf;
        memcpy(u->sbuf, u->left, (unsigned)k);
        u->left += k;
        u->llen -= k;
        u->s_n = k;
        return 1;
    }
    {
        SceLong64 dl = sceKernelGetSystemTimeWide() + UPLOAD_IDLE_US;
        for (;;) {
            int r;
            if (recv_abort_pending()) return -1; /* 用户中止 */
            r = sceNetRecv(u->fd, u->sbuf, (unsigned)sizeof u->sbuf, 0);
            if (r > 0) { u->s_n = r; return 1; }
            if (r == 0) return 0;
            if (!NET_AGAIN(r)) return -1;
            if (sceKernelGetSystemTimeWide() >= dl) return -2;
            sceKernelDelayThread(NET_RETRY_DELAY_US);
        }
    }
}

/* chunked: 读一行（去 \r\n）。返回 1=成功 0=EOF -1=错 */
static int u_readline(UploadCtx *u, char *line, int cap)
{
    int n = 0;
    for (;;) {
        if (u->s_pos >= u->s_n) {
            int r = u_refill(u);
            if (r <= 0) return r < 0 ? -1 : 0;
        }
        while (u->s_pos < u->s_n) {
            char ch = (char)u->sbuf[u->s_pos++];
            if (ch == '\n') { line[n] = 0; return 1; }
            if (ch != '\r' && n < cap - 1) line[n++] = ch;
        }
    }
}

/* chunked 解码流：把帧解码成原始文件字节喂给 receive。
 * 帧：<hex>\r\n <data 正好 hex 字节> \r\n <hex>... 0\r\n [trailer]\r\n
 * ph: 0=大小行 1=chunk 数据 2=数据后块尾空行 3=终块后 trailer */
static int chunked_stream(void *ctx, unsigned char *out, int max)
{
    UploadCtx *u = ctx;
    int on = 0;
    for (;;) {
        if (u->ph == 0) {                        /* chunk 大小行 */
            char line[64];
            SceLong64 sz = 0;
            int r = u_readline(u, line, sizeof line);
            if (r <= 0) return r < 0 ? -1 : (on > 0 ? on : -1);
            {
                const char *p = line;
                int h;
                while (*p && (h = hexv(*p)) >= 0) { sz = sz * 16 + h; p++; }
            }
            if (sz == 0) { u->ph = 3; continue; } /* 终止块 */
            u->c_rem = sz;
            u->ph = 1;
        } else if (u->ph == 1) {                 /* chunk 数据 */
            int need, got = 0;
            if (u->c_rem == 0) { u->ph = 2; continue; }
            need = (int)(u->c_rem < (SceLong64)(max - on)
                             ? u->c_rem : (SceLong64)(max - on));
            while (got < need) {
                int k;
                if (u->s_pos >= u->s_n) {
                    int r = u_refill(u);
                    if (r <= 0) return on > 0 ? on : -1;
                }
                k = u->s_n - u->s_pos;
                if (k > need - got) k = need - got;
                memcpy(out + on + got, u->sbuf + u->s_pos, (unsigned)k);
                u->s_pos += k;
                u->c_rem -= k;
                got += k;
            }
            on += need;
            if (u->c_rem == 0) u->ph = 2;        /* 块完：下一轮先吃块尾 */
            return on;
        } else if (u->ph == 2) {                 /* 数据块后的块尾 "\r\n" */
            char line[16];
            int r = u_readline(u, line, sizeof line);
            if (r <= 0) return on > 0 ? on : -1;
            u->ph = 0;                           /* 应为空行，丢弃 */
        } else {                                 /* ph==3：终块后的 trailer */
            char line[128];
            int r = u_readline(u, line, sizeof line);
            if (r <= 0) return on > 0 ? on : 0;  /* 流结束 */
            if (line[0] == 0) return 0;          /* 空行：收尾完成 */
        }
    }
}

/* 通用上传读回调：chunked 走解码，否则直读原始字节 */
static int upload_stream(void *ctx, unsigned char *buf, int max)
{
    UploadCtx *u = ctx;
    SceLong64 dl;
    if (u->chunked) return chunked_stream(u, buf, max);
    if (u->llen > 0) {                   /* 先消费缓冲里的残留 body */
        int k = u->llen > max ? max : u->llen;
        memcpy(buf, u->left, (unsigned)k);
        u->left += k;
        u->llen -= k;
        return k;
    }
    dl = sceKernelGetSystemTimeWide() + UPLOAD_IDLE_US;
    for (;;) {
        if (recv_abort_pending()) return -1; /* 用户中止：让 receive 收尾为取消 */
        int r = sceNetRecv(u->fd, buf, (unsigned)max, 0);
        if (r > 0) return r;
        if (r == 0) return 0;            /* 对端关闭 */
        if (!NET_AGAIN(r)) { dlog("http: upload recv err 0x%08X", (unsigned)r); return -1; }
        if (sceKernelGetSystemTimeWide() >= dl) { dlog("http: upload idle timeout"); return 0; }
        sceKernelDelayThread(NET_RETRY_DELAY_US);
    }
}

/* 解析 HTTP 请求并应答一个连接，返回后由调用者关 fd */
static void handle_conn(int c, const char *rip)
{
    char buf[HTTP_MAX_REQ];
    struct timeval tv;
    int n = 0, he = -1, i;
    SceLong64 cl64 = -1;            /* Content-Length 原值（Vita long 仅 32 位，大文件要 64 位） */
    char method[8] = "";
    char path[768] = "";
    char route[256];
    const char *query = NULL;
    const char *left;
    int llen;

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

    /* 请求行：method path（可能带 ?query） */
    {
        const char *sp = strchr(buf, ' ');
        if (sp) {
            int mlen = (int)(sp - buf);
            const char *p2 = strchr(sp + 1, ' ');
            int plen = p2 ? (int)(p2 - (sp + 1)) : (int)strlen(sp + 1);
            if (mlen > 7) mlen = 7;
            memcpy(method, buf, (unsigned)mlen);
            method[mlen] = 0;
            if (plen > (int)sizeof path - 1) plen = (int)sizeof path - 1;
            memcpy(path, sp + 1, (unsigned)plen);
            path[plen] = 0;
        }
    }
    {
        const char *qm = strchr(path, '?');
        int rl = qm ? (int)(qm - path) : (int)strlen(path);
        if (rl > (int)sizeof route - 1) rl = (int)sizeof route - 1;
        memcpy(route, path, (unsigned)rl);
        route[rl] = 0;
        query = qm ? qm + 1 : "";
    }

    /* Content-Length：头名大小写不敏感（Dart 客户端发小写 "content-length"，
     * 用大写 strstr 匹配会漏掉 → body 全读到空） */
    cl64 = -1;
    hdr_value(buf, he, "content-length", &cl64);
    if (cl64 < 0) cl64 = 0;

    /* 小请求体（register/prepare/info/cancel）头齐后把 body 尽量读齐进 buf：
     * - 客户端常把 body 与头分开发送，头循环一收齐就处理会拿到空 body；
     * - 有的客户端不送 Content-Length（EOF/keep-alive 式），只能靠"收到过
     *   数据后再出现空窗"来判定 body 结束，不能因为无 CL 就判 400；
     * - upload 是大文件流，绝不进 buf，走下面的流式回调。 */
    if (strcmp(route, "/api/localsend/v2/upload") != 0) {
        SceLong64 dl = sceKernelGetSystemTimeWide()
                     + (SceLong64)HTTP_RECV_TIMEOUT_S * 1000000LL;
        int want = cl64 > 0 ? (int)(he + 4 + cl64) : HTTP_MAX_REQ - 1;
        int had = 0, idle = 0;
        if (want > HTTP_MAX_REQ - 1) want = HTTP_MAX_REQ - 1;
        while (n < want) {
            int r = sceNetRecv(c, buf + n, (unsigned)(want - n), 0);
            if (r > 0) { n += r; buf[n] = 0; had = 1; idle = 0; continue; }
            if (r == 0 || sceKernelGetSystemTimeWide() >= dl) break;
            if (!NET_AGAIN(r)) break;
            if (had && ++idle > 20) break;    /* 收过数据后 ~100ms 没再来 → 完 */
            if (!had && cl64 <= 0) break;      /* 明确无体（无 CL）不空等 */
            sceKernelDelayThread(NET_RETRY_DELAY_US);
        }
    }

    left = buf + he + 4;
    llen = n - (he + 4);
    if (llen < 0) llen = 0;

    if (method[0] && route[0])
        dlog("http: %s %s from %s", method, route, rip);
    else
        dlog("http: unparsed req from %s", rip);

    if (strcmp(method, "GET") == 0 &&
        strcmp(route, "/api/localsend/v2/info") == 0) {
        char out[HTTP_MAX_BODY];
        member_info_json(out, sizeof out);
        http_respond(c, 200, "OK", out);
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        strcmp(route, "/api/localsend/v2/register") == 0) {
        char body[HTTP_MAX_BODY];
        int got = llen;                      /* body 已随头读齐（见上补读） */
        if (got > (int)sizeof body - 1) got = (int)sizeof body - 1;
        memcpy(body, left, (unsigned)got);
        body[got] = 0;
        if (got > 0) {
            dlog("http: register body %d bytes from %s", got, rip);
            if (g_cb) g_cb(body, rip);
        } else {
            dlog("http: register with empty body from %s", rip);
        }
        {
            char out[HTTP_MAX_BODY];
            member_info_json(out, sizeof out);
            http_respond(c, 200, "OK", out);
        }
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        strcmp(route, "/api/localsend/v2/prepare-upload") == 0) {
        char body[HTTP_MAX_PREPARE];
        char resp[HTTP_MAX_PREPARE];
        int got = llen;                      /* body 已随头读齐 */
        if (got > (int)sizeof body - 1) got = (int)sizeof body - 1;
        memcpy(body, left, (unsigned)got);
        body[got] = 0;
        if (cl64 <= 0) {                   /* 诊断：无 CL 客户端长啥样 */
            char dbg[256];
            int a, b = 0;
            int hl = he < 200 ? he : 200;
            for (a = 0; a < hl && b < 250; a++) {
                char ch = buf[a];
                if (ch == '\r' || ch == '\n') ch = '|';
                dbg[b++] = ch;
            }
            dbg[b] = 0;
            dlog("http: prepare no-cl hdr: %s", dbg);
        }
        if (cl64 > 0 && (SceLong64)got < cl64) {  /* 声称有体但没读齐 → 400 */
            dlog("http: prepare body short cl=%lld got=%d from %s",
                 (long long)cl64, got, rip);
            http_respond(c, 400, "Bad Request", "");
            return;
        }
        {
            int code = recv_http_prepare(body, rip, resp, (int)sizeof resp);
            dlog("http: prepare -> %d", code);
            http_respond(c, code, reason_of(code), resp);
        }
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        strcmp(route, "/api/localsend/v2/upload") == 0) {
        UploadCtx uctx;
        int code;
        {                            /* 诊断：看 PC 发的 upload 头（有无 CL/TE） */
            char dbg[512];
            int a, b = 0;
            int hl = he < 500 ? he : 500;
            for (a = 0; a < hl && b < 505; a++) {
                char ch = buf[a];
                if (ch == '\r' || ch == '\n') ch = '|';
                dbg[b++] = ch;
            }
            dbg[b] = 0;
            dlog("http: upload hdr: %s |cl=%lld", dbg, (long long)cl64);
        }
        if (cl64 < 0) cl64 = 0;
        memset(&uctx, 0, sizeof uctx);       /* 含 chunked 解码状态 */
        uctx.fd = c;
        uctx.left = left;
        uctx.llen = llen;
        if (cl64 <= 0) {                     /* 无 CL：dio 流式上传 → chunked */
            char te[80] = "";
            hdr_copy(buf, he, "transfer-encoding", te, sizeof te);
            if (str_has_ci(te, "chunked")) uctx.chunked = 1;
        }
        if (uctx.chunked) {
            dlog("http: upload chunked (no content-length)");
            code = recv_http_upload(query, rip, -1, upload_stream, &uctx);
        } else {
            code = recv_http_upload(query, rip, cl64, upload_stream, &uctx);
        }
        dlog("http: upload -> %d", code);
        http_respond(c, code, reason_of(code), "");
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        strcmp(route, "/api/localsend/v2/cancel") == 0) {
        int code = recv_http_cancel(query, rip);
        http_respond(c, code, reason_of(code), "");
        return;
    }
    http_respond(c, 404, "Not Found", "{\"error\":\"not found\"}");
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
    if (sceNetListen(s, 8) < 0) {   /* 收体是逐连接顺序处理的，backlog 留宽点 */
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
