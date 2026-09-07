/* http.c 实现 —— 见 http.h。
 * accept 线程只做 accept + 分发：每连接开一个 worker 线程并发处理（上限
 * HTTP_MAX_CONN，满员时新连接立即关闭——宁可见效地拒绝，也不让客户端在 TCP
 * 队列里干等到超时）；worker 处理完关 fd 自清，收大文件 body 是"流式边读边
 * 转给 receive 模块"，其余请求头+体都很小读进内存即可。http_stop 关监听让
 * accept 退出后，会对仍存活的 worker 做有预算的 join，等不到就换代弃管。
 * Vita 的 SceNet socket 收/发在暂无数据时会立刻返回 EWOULDBLOCK(0x80410123)，
 * 不能当错误处理，必须轮询等数据/等窗口。 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <sys/time.h>
#include "config.h"
#include "json_util.h"
#include "http.h"
#include "receive.h"
#include "dlog.h"

#define HTTP_RECV_TIMEOUT_S 3
#define HTTP_MAX_REQ        32768   /* 请求头+体总读入上限（64 文件 prepare 清单可能 ~20KB） */
#define HTTP_MAX_BODY       2048    /* register 体上限 */
#define HTTP_MAX_PREPARE    32768   /* prepare-upload 体/回执上限（清单 JSON，64 文件会话） */
#define UPLOAD_IDLE_US      (30 * 1000000LL)  /* 收体时单块读的空闲上限 */
#define NET_AGAIN(r) ((r) == 0x80410123)
#define NET_RETRY_DELAY_US 20000    /* 20ms 轮询间隔 */
#define HTTP_MAX_CONN       8       /* 并发连接 worker 上限（满员立即关新连接） */
#define HTTP_CONN_STACK     0x40000 /* 每连接 worker 栈：与旧 accept 线程同规格
                                     * （handle_conn 内 buf+prepare 体/回执 ~100KB） */

static int    g_port = 0;           /* 实际绑定端口（0=未启动） */
static int    g_lsock = -1;
static SceUID g_thr = -1;           /* accept 线程（http_stop 要等它退） */
static volatile int g_run = 0;
static volatile int g_gen = 0;      /* 服务器代次：stop/start 递增，旧线程据此收手 */
static void (*g_cb)(const char *body, const char *src_ip) = NULL;

/* 每连接一个 worker 的登记槽：accept 线程填好 fd/ip/gen 再启动线程；
 * worker 自找本槽（tid 匹配）取出参数；http_stop 靠 tid 等仍在跑的 worker。
 * 槽空 = tid <= 0（0=静态零初始的未用槽，-1=已清/占位中；Vita 线程 UID 恒为正）；
 * worker 收尾时若代次已变（stop 后旧 worker 迟到），绝不关 fd——那可能是
 * 新监听/新连接复用走的号；宁可漏一个死 fd。 */
typedef struct {
    SceUID tid;                     /* worker 线程；<=0 = 槽空 */
    int    fd;
    int    gen;
    char   ip[16];
} ConnSlot;
static ConnSlot g_conns[HTTP_MAX_CONN];
static SceUID g_conn_mtx = -1;      /* 保护连接登记表 */
static void conn_lock(void)   { if (g_conn_mtx >= 0) sceKernelLockMutex(g_conn_mtx, 1, NULL); }
static void conn_unlock(void) { if (g_conn_mtx >= 0) sceKernelUnlockMutex(g_conn_mtx, 1); }

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
    int  gen;           /* 创建时的服务器代次：换代后轮询尽早收手 */
    const char *left;   /* 缓冲里已读但未消费的 body */
    int  llen;
    int  chunked;       /* Transfer-Encoding: chunked（dio 无 CL 流式上传） */
    int  ph;            /* chunked: 0=chunk 大小行 1=chunk 数据 2=终块 trailer */
    SceLong64 c_rem;    /* chunked: 当前 chunk 剩余数据字节 */
    unsigned char sbuf[4096];  /* chunked 解码输入暂存 */
    int  s_n, s_pos;
} UploadCtx;

/* 换代感知：http_stop/restart 后旧 worker 的轮询（头/体读取）应尽快收手，
 * 让 http_stop 的有预算 join 能等到它，而不是干耗到 recv 超时。 */
static inline int gen_stale(int my_gen) { return g_gen != my_gen; }

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
            if (gen_stale(u->gen)) return -1;    /* 换代：http_stop 已叫停 */
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
        if (gen_stale(u->gen)) return -1;    /* 换代：http_stop 已叫停 */
        int r = sceNetRecv(u->fd, buf, (unsigned)max, 0);
        if (r > 0) return r;
        if (r == 0) return 0;            /* 对端关闭 */
        if (!NET_AGAIN(r)) { dlog("http: upload recv err 0x%08X", (unsigned)r); return -1; }
        if (sceKernelGetSystemTimeWide() >= dl) { dlog("http: upload idle timeout"); return 0; }
        sceKernelDelayThread(NET_RETRY_DELAY_US);
    }
}

/* 解析 HTTP 请求并应答一个连接（在独立 worker 线程跑），返回后由 worker 关 fd */
static void handle_conn(int c, const char *rip, int my_gen)
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
            if (gen_stale(my_gen)) {           /* 换代：服务器已停，不白等 */
                dlog("http: req loop sees gen change from %s", rip);
                return;
            }
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
            if (gen_stale(my_gen)) return;   /* 换代：服务器已停，不白等 */
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
        uctx.gen = my_gen;
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

/* ---------- accept 线程 + 连接 worker ----------
 * accept 线程只做 accept + 分发（占 g_conns 槽 → 起 worker），不碰协议。
 * 每个已接受连接由一个 worker 线程独占处理到关 fd；并发连接之间、与
 * receive 模块的交互由 receive 侧互斥锁串行（见 receive.c 头注），连接
 * 本身天然互不干扰：
 *  - 槽满（HTTP_MAX_CONN 个 worker 还活着）→ 新连接立即关闭：宁可见效地
 *    拒绝，也不让客户端在 TCP 队列里干等到超时——"忙时第二个发送方立刻
 *    拿 409"就是这个门卫实现的；
 *  - worker 自找本槽（sceKernelGetThreadId 匹配）取 fd/ip/代次，处理完关
 *    fd、清槽、线程自删（ExitDeleteThread，无固定 join 者，避免泄漏）；
 *  - 换代（http_stop/restart）后旧 worker 在收头/收体轮询里感知 gen 变化
 *    尽早收手；worker 收尾时若代次已变，绝不关 fd——号可能已被新监听或
 *    新连接复用，宁可漏一个死 fd；
 *  - http_stop 关监听、join accept 线程后，对有预算地 join 仍在跑的 worker。 */
static int conn_worker(SceSize args, void *argp)
{
    SceUID me = sceKernelGetThreadId();
    int i, c = -1, my_gen = -1;
    char ip[16] = "";
    (void)args; (void)argp;

    conn_lock();
    for (i = 0; i < HTTP_MAX_CONN; i++) {
        if (g_conns[i].tid == me) {
            c = g_conns[i].fd;
            my_gen = g_conns[i].gen;
            memcpy(ip, g_conns[i].ip, sizeof ip);
            break;
        }
    }
    conn_unlock();
    if (c < 0) {                     /* 兜底：找不到自己的槽（不该发生） */
        sceKernelExitDeleteThread(0);
        return 0;
    }
    dlog("http: worker %d serves conn from %s (gen %d)",
         (int)me, ip, my_gen);
    handle_conn(c, ip, my_gen);

    conn_lock();
    for (i = 0; i < HTTP_MAX_CONN; i++) {
        if (g_conns[i].tid == me) {
            g_conns[i].tid = -1;
            g_conns[i].fd = -1;
            g_conns[i].gen = -1;
            break;
        }
    }
    conn_unlock();
    if (my_gen == g_gen)             /* 换代后绝不关：号可能已被新代复用 */
        sceNetSocketClose(c);
    dlog("http: worker %d done", (int)me);
    sceKernelExitDeleteThread(0);
    return 0;
}

/* 登记并启动一个连接 worker；返回 1=已开，0=满员/失败（调用者应关 fd） */
static int spawn_conn(int c, const char *ip)
{
    int i;
    SceUID t;

    conn_lock();
    for (i = 0; i < HTTP_MAX_CONN; i++)
        if (g_conns[i].tid <= 0) break;     /* <=0 = 空槽（含零初始的未用槽） */
    if (i >= HTTP_MAX_CONN) {        /* 满员：礼貌拒绝 */
        conn_unlock();
        dlog("http: conn from %s refused: %d workers busy", ip, HTTP_MAX_CONN);
        return 0;
    }
    g_conns[i].fd = c;
    g_conns[i].gen = g_gen;
    g_conns[i].tid = -1;             /* 先占槽（create 成功前 stop 跳过它） */
    memcpy(g_conns[i].ip, ip, 16);
    conn_unlock();

    t = sceKernelCreateThread("psvsend_httpc", conn_worker, 0x40,
                              HTTP_CONN_STACK, 0, 0, NULL);
    if (t < 0) {
        dlog("http: worker create fail 0x%08X from %s", (unsigned)t, ip);
        conn_lock();
        g_conns[i].fd = -1;
        g_conns[i].gen = -1;
        conn_unlock();
        return 0;
    }
    conn_lock();
    g_conns[i].tid = t;              /* 登记后再 start，worker 靠 tid 找槽 */
    conn_unlock();
    if (sceKernelStartThread(t, 0, NULL) < 0) {
        dlog("http: worker start fail from %s", ip);
        conn_lock();
        g_conns[i].tid = -1;
        g_conns[i].fd = -1;
        g_conns[i].gen = -1;
        conn_unlock();
        sceKernelDeleteThread(t);
        return 0;
    }
    return 1;
}

static int http_thr(SceSize args, void *argp)
{
    int my_gen = g_gen;
    int ls = g_lsock;
    int errs = 0;
    (void)args; (void)argp;
    dlog("http: accept thread gen %d on :%d", my_gen, g_port);
    while (g_run && g_gen == my_gen) {
        SceNetSockaddrIn cli;
        unsigned int clen = sizeof cli;
        char ip[16] = "";
        int c = sceNetAccept(ls, (SceNetSockaddr *)&cli, &clen);
        if (c < 0) {
            if (!g_run || g_gen != my_gen) break;      /* 停机/换代：收手 */
            if (c == 0x80410123) {                     /* 空闲空转（非阻塞 accept） */
                sceKernelDelayThread(50000);
                continue;
            }
            errs++;
            if (errs == 1 || (errs % 100) == 0)
                dlog("http: accept err x%d -> 0x%08X (gen %d)", errs,
                     (unsigned)c, my_gen);
            if (errs > 300) {   /* 持续 ~15s 报错：监听 socket 已死，自退交看门狗重启 */
                dlog("http: accept err x%d -> listener dead, self stop (gen %d)",
                     errs, my_gen);
                break;
            }
            sceKernelDelayThread(50000);
            continue;
        }
        errs = 0;
        if (sceNetInetNtop(SCE_NET_AF_INET, &cli.sin_addr.s_addr,
                           ip, sizeof ip) == NULL)
            snprintf(ip, sizeof ip, "?");
        dlog("http: conn from %s (gen %d)", ip, my_gen);
        if (!spawn_conn(c, ip))    /* 满员/起线程失败：立即关，不晾队列里 */
            sceNetSocketClose(c);
    }
    dlog("http: accept thread gen %d exit", my_gen);
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

    if (g_conn_mtx < 0)              /* 连接登记表锁：随服务器首次启动创建，
                                      * 跨 stop/start 复用（stop 不销毁） */
        g_conn_mtx = sceKernelCreateMutex("psvsend_conn", 0, 0, NULL);

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
    if (sceNetListen(s, 8) < 0) {   /* 连接交给独立 worker 并发处理，backlog 8
                                     * 兜底瞬时并发（超 HTTP_MAX_CONN 的连接会
                                     * 在 accept 时被立即关闭——礼貌拒绝） */
        sceNetSocketClose(s);
        dlog("http: listen fail");
        return -2;
    }
    g_lsock = s;
    g_port = chosen > 0 ? chosen : 0;
    g_run = 1;
    g_gen++;                              /* 新代次：旧代线程（若有）不再碰新监听 */
    {
        SceUID t = sceKernelCreateThread("psvsend_http", http_thr,
                                         0x40, 0x40000, 0, 0, NULL);
        dlog("http: thread create -> 0x%08X", (unsigned)t);
        if (t < 0) {
            dlog("http: thread create FAILED -> tear down listener");
            sceNetSocketClose(s);
            g_lsock = -1;
            g_port = 0;
            g_run = 0;
            return -3;
        }
        g_thr = t;
        {
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

/* 探活：监听 socket 还在、accept 线程还活着？0 表示服务已死需要重启。
 * 用超时 0 的 WaitThreadEnd 探测线程是否已退出（不自占线程状态：已退出时
 * 后续 http_stop 的 WaitThreadEnd 会立即返回 0，无副作用）。 */
int http_alive(void)
{
    if (g_port <= 0 || g_thr < 0) return 0;
    {
        SceUInt to = 0;
        int st = sceKernelWaitThreadEnd(g_thr, NULL, &to);
        if (st == 0) {                     /* 线程已退出（自尽/异常） */
            dlog("http: accept thread exited unexpectedly");
            g_thr = -1;
            return 0;
        }
    }
    return 1;                              /* 仍在跑（st 为超时/其他错误码） */
}

/* join 仍在跑的连接 worker：换代感知让它们在 ~20ms 轮询粒度内自退，这里给
 * 总预算 500ms 逐轮等；等不到的（如卡在 receive 内等 UI 决定）弃管——它们
 * 处理完会自删线程，收尾时因代次已变也不会去碰 fd。 */
static void http_join_conns(void)
{
    SceLong64 dl = sceKernelGetSystemTimeWide() + 500000LL;
    for (;;) {
        SceUID t = -1;
        SceLong64 rem;
        conn_lock();
        for (int i = 0; i < HTTP_MAX_CONN; i++)
            if (g_conns[i].tid > 0) { t = g_conns[i].tid; break; }  /* 真实线程才 join */
        conn_unlock();
        if (t < 0) break;
        rem = dl - sceKernelGetSystemTimeWide();
        if (rem <= 0) break;
        {
            SceUInt to = rem > 100000 ? 100000 : (SceUInt)rem;
            int st = sceKernelWaitThreadEnd(t, NULL, &to);
            if (st == 0)
                dlog("http: worker %d joined", (int)t);
            /* 超时/线程已自删：下轮再找（槽空则结束） */
        }
    }
    dlog("http: workers drained");
}

/* 停服务器：置停跑标志、换代、关监听 socket 让 accept 立刻出错退出，等 accept
 * 线程与仍存活的连接 worker 结束（有预算，等不到弃管）。旧线程回到循环头时
 * 因代次不符会自行退出，不会碰新启动的监听。 */
void http_stop(void)
{
    if (g_port <= 0 && g_thr < 0) return;
    dlog("http: stop (was :%d)", g_port);
    g_run = 0;
    g_gen++;
    if (g_lsock >= 0) {
        sceNetSocketClose(g_lsock);
        g_lsock = -1;
    }
    if (g_thr >= 0) {
        SceUInt to = 500 * 1000;      /* 最多等 500ms（关监听后 accept 立即醒） */
        int st = sceKernelWaitThreadEnd(g_thr, NULL, &to);
        if (st < 0)
            dlog("http: wait thread end -> 0x%08X", (unsigned)st);
        g_thr = -1;
    }
    http_join_conns();               /* 有预算地等仍在跑的连接 worker */
    g_port = 0;
}

/* 整机重绑（唤醒/链路变化后用）：停旧监听，再走一遍候选端口 */
int http_restart(void)
{
    http_stop();
    return http_start();
}

void http_set_register_cb(void (*cb)(const char *body, const char *src_ip))
{
    g_cb = cb;
}
