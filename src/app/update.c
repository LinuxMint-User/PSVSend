/* 客户端更新检查（见 update.h）。
 *
 * 后台一次性线程按顺序尝试一组 HTTPS 源，取首个"可用"源拿到的版本串：
 *   0) https://raw.giteeusercontent.com/epix-xhan/PSVSend/raw/main/src/core/config.h
 *      Gitee 镜像仓库里的版本真源 src/core/config.h（解析 PSVSEND_APP_VERSION
 *      宏字符串值）。镜像只同步 main 分支。不新增任何版本文件：config.h 是
 *      客户端唯一版本源，发版时本来就要改它，镜像同步自然带上，不存在多源
 *      不一致。直接请求 raw.giteeusercontent.com 免 302（gitee.com/raw 会 302
 *      跳转，本客户端不跟随）。
 *   1) https://github.com/LinuxMint-User/PSVSend/releases.atom（atom）
 *      GitHub 官方源兜底（Gitee 失败时才试；国内裸连 TLS 常被 SNI 阻断，
 *      一次尝试会吃满握手超时预算）。
 * 源给出 HTTP 2xx 且内容解析成功即视为"权威"并定论；其余情况换下一源。
 * 版本串与本地 PSVSEND_APP_VERSION 比较定状态（atom 取首个 <entry> 的 title，
 * draft 对外不可见不误报）。
 *
 * TLS 复用 scan/transfer 的 mbedTLS 1.2 出站模式（本工程无 CA 信任库，
 * 一律 VERIFY_NONE/trust-first——只读版本号不执行，MITM 影响仅提示文案，
 * 可接受）。域名解析走 sceNetResolverStartNtoa，timeout/retry 必须 0,0
 * （0=固件默认；曾传 6000ms/2 被真机固件瞬时判 EINVAL，见 resolve_host）。
 * 全程后台线程 + 每源硬预算超时，绝不阻塞 UI。 */
#include <string.h>
#include <stdio.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include "core/config.h"
#include "core/dlog.h"
#include "net/net.h"
#include "app/update.h"

#define UPD_WIN      16384        /* 响应窗口：config.h 文本极小；atom 也够首 entry */
#define UPD_STACK    0x14000      /* worker 栈：装得下 mbedtls 全套上下文 */
#define UPD_CONN_US  6000000LL    /* 单源 TCP connect 预算 */
#define UPD_HS_US    8000000LL    /* 单源 TLS 握手预算 */
#define UPD_IO_US    8000000LL    /* 单源读写预算 */
/* 非阻塞 socket 返回 EWOULDBLOCK（各网模块同款局部宏） */
#define NET_AGAIN(r) ((r) == SCE_NET_ERROR_EWOULDBLOCK)

/* 源表：want 0=GitHub atom(取首个 <entry> 的 title)，2=config.h 取版本宏 */
typedef struct {
    const char *host;
    const char *path;
    int         want;
} UpdSrc;
static const UpdSrc UPD_SRC[] = {
    { "raw.giteeusercontent.com",
      "/epix-xhan/PSVSend/raw/main/src/core/config.h", 2 },
    { "github.com",
      "/LinuxMint-User/PSVSend/releases.atom",          0 },
};
#define UPD_SRC_N ((int)(sizeof UPD_SRC / sizeof UPD_SRC[0]))

static volatile int g_st = UPD_IDLE;
static char g_latest[32];              /* UPD_NEW 时的远端版本串（含 v） */
static volatile int g_busy;            /* 一个 worker 在跑（手动/自动共用） */
static uint64_t g_tick_last_us;        /* update_tick 每秒节流 */

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }
static long now_unix(void) { return (long)(sceKernelGetSystemTimeWide() / 1000000LL); }

/* config upd_auto：0=off 1=每天 2=每周 3=每月 → 周期秒 */
static long auto_interval(void)
{
    switch (g_cfg.upd_auto) {
    case 1: return 24L * 3600;
    case 2: return 7L * 24 * 3600;
    case 3: return 30L * 24 * 3600;
    }
    return 0;
}

/* ---------- 熵源 / TLS bio / 连接（与 scan.c 同套路） ---------- */
static unsigned long s_ent_seed;
static int ent_poll(void *arg, unsigned char *out, size_t len, size_t *olen)
{
    uint64_t x = (uint64_t)sceKernelGetSystemTimeWide()
               ^ ((uint64_t)(uintptr_t)&s_ent_seed << 17)
               ^ ((uint64_t)(s_ent_seed++) * 0x9E3779B97F4A7C15ull);
    size_t i;
    (void)arg;
    for (i = 0; i < len; i += 8) {
        uint64_t z = x + 0x9E3779B97F4A7C15ull;
        int k;
        x = z;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (k = 0; k < 8; k++) {
            if (i + k >= len) break;
            out[i + k] = (unsigned char)(z >> (8 * k));
        }
    }
    *olen = len;
    return 0;
}

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int r = sceNetSend(fd, buf, (unsigned)len, 0);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (NET_AGAIN(r)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int r = sceNetRecv(fd, buf, (unsigned)len, 0);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (NET_AGAIN(r)) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* 解析 host → 网络序 IPv4；返回 0 成功，<0 为底层错误码（供诊断日志）。
 * timeout/retry 必须取 0,0——RetroArch / enet-vita 等真机可用实现全部如此
 * （0=固件默认）；曾传 6000ms/2 次被真机固件瞬时判 EINVAL（参数校验不过，
 * 与"解析不可达"的 ETIMEDOUT 类错误无关，改 0,0 后解析即通）。 */
static int resolve_host(const char *host, unsigned int *saddr)
{
    SceNetInAddr a;
    int rid, r;
    rid = sceNetResolverCreate("psvsend_upd_res", NULL, 0);
    if (rid < 0) return rid;
    r = sceNetResolverStartNtoa(rid, host, &a, 0, 0, 0);
    sceNetResolverDestroy(rid);
    if (r < 0) return r;
    *saddr = a.s_addr;
    return 0;
}

/* 非阻塞 connect + 预算内轮询（同 scan）；成功返回 fd。
 * 失败返回 -1 并经 *errc（可空）带回原因：<0 立即错误码 /
 * >0 SO_ERROR 内核裁决 / 0 预算超时 */
static int conn_to(unsigned int saddr, int port, int *errc)
{
    SceNetSockaddrIn sa;
    uint64_t dl = now_us() + UPD_CONN_US;
    int one = 1, fd, cr;
    fd = sceNetSocket("psvsend_upd", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM,
                      SCE_NET_IPPROTO_TCP);
    if (fd < 0) {
        if (errc) *errc = fd;
        return -1;
    }
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons((unsigned)port);
    sa.sin_addr.s_addr = saddr;
    cr = sceNetConnect(fd, (const SceNetSockaddr *)&sa, sizeof sa);
    if (cr != 0 &&
        cr != SCE_NET_ERROR_EWOULDBLOCK &&
        cr != SCE_NET_ERROR_EINPROGRESS &&
        cr != SCE_NET_ERROR_EALREADY) {
        sceNetSocketClose(fd);
        if (errc) *errc = cr;
        return -1;
    }
    if (cr != 0) {
        /* 非阻塞在途 connect：SO_ERROR 非 0=内核裁决失败；getpeername
         * 成功=连接建立（同 scan 轮询法，本 SDK 无 select） */
        for (;;) {
            int so = 0;
            unsigned int sl = sizeof so;
            SceNetSockaddrIn p;
            unsigned int plen = sizeof p;
            if (sceNetGetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                                 &so, &sl) == 0 && so != 0) {
                sceNetSocketClose(fd);
                if (errc) *errc = so;
                return -1;
            }
            if (sceNetGetpeername(fd, (SceNetSockaddr *)&p, &plen) == 0)
                return fd;
            if (now_us() >= dl) break;
            sceKernelDelayThread(2000);
        }
        sceNetSocketClose(fd);
        if (errc) *errc = 0;
        return -1;
    }
    return fd;
}

/* 版本比较：str 形如 "v2.0.1" / "2.0.0"（可带可不带 v、位数不等）。
 * 返回 >0 表示 a 比 b 新，<0 旧，=0 相同（非版本串按纯字典序兜底）。 */
static int cmp_version(const char *a, const char *b)
{
    const char *p = a, *q = b;
    if (*p == 'v' || *p == 'V') p++;
    if (*q == 'v' || *q == 'V') q++;
    for (;;) {
        unsigned long x = 0, y = 0;
        int xd = 0, yd = 0;
        while (*p >= '0' && *p <= '9') { x = x * 10 + (*p - '0'); p++; xd = 1; }
        while (*q >= '0' && *q <= '9') { y = y * 10 + (*q - '0'); q++; yd = 1; }
        if (x != y) return x > y ? 1 : -1;
        if (!xd && !yd) {
            /* 数字段耗尽：逐字符兜底保序即可 */
            return strcmp(p, q);
        }
        if (*p == '.') p++;
        if (*q == '.') q++;
    }
}

/* 从 release title（形如 "PSVSend v2.0.1"）抽 "vX.Y.Z" 到 out；无则返回 -1 */
static int extract_tag(const char *title, char *out, int n)
{
    const char *p = title;
    while (*p) {
        if (*p == 'v' && p[1] >= '0' && p[1] <= '9') {
            const char *q = p + 1;
            int dots = 0;
            while ((*q >= '0' && *q <= '9') || (*q == '.' && dots < 3)) {
                if (*q == '.') dots++;
                q++;
            }
            if (dots >= 2 && q - p < n) {
                int len = (int)(q - p);
                memcpy(out, p, (size_t)len);
                out[len] = 0;
                return 0;
            }
        }
        p++;
    }
    return -1;
}

/* ---- GitHub atom：取首个 <entry> 内 <title>…</title> 的版本 tag ----
 * 必须从 <entry> 之后找 title：feed 自带的 <title>（仓库名）不含版本号，
 * 若取全文档第一个 <title> 会抽空 tag。无 <entry>（如从未发布）→ -1。 */
static int parse_atom_tag(const char *b, char *out, int n)
{
    const char *e = strstr(b, "<entry>");
    const char *ts, *te;
    char tmp[192];
    int len;
    const char *p;
    if (!e) return -1;
    ts = strstr(e, "<title>");
    if (!ts) return -1;
    te = strstr(ts, "</title>");
    if (!te) return -1;
    len = (int)(te - ts - 7);
    if (len > (int)sizeof tmp - 1) len = (int)sizeof tmp - 1;
    memcpy(tmp, ts + 7, (size_t)len);
    tmp[len] = 0;
    p = tmp;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (extract_tag(p, out, n) != 0) return -1;
    return 0;
}

/* 大小写不敏感头部字段名比较：a[0..n) 是否等于字面量 lit */
static int ci_eq(const char *a, int n, const char *lit)
{
    int i;
    if (n != (int)strlen(lit)) return 0;
    for (i = 0; i < n; i++) {
        char c = a[i], d = lit[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (d >= 'A' && d <= 'Z') d = (char)(d - 'A' + 'a');
        if (c != d) return 0;
    }
    return 1;
}
static int ci_has(const char *a, int n, const char *lit)
{
    int l = (int)strlen(lit), i;
    for (i = 0; i + l <= n; i++)
        if (ci_eq(a + i, l, lit)) return 1;
    return 0;
}

/* 解析已收齐的 HTTP 头部 win[0..hn)：状态码 + Content-Length + chunked 标记 */
static void http_parse_hdr(const char *h, int hn, int *st, int *clen, int *chunked)
{
    const char *p = h, *end = h + hn;
    *st = 0;
    *clen = -1;
    *chunked = 0;
    /* 状态行：找第一个空格后的数字 */
    while (p < end && *p != '\n') {
        if (*p == ' ' && p[1] >= '0' && p[1] <= '9') {
            p++;
            while (p < end && *p >= '0' && *p <= '9') {
                *st = *st * 10 + (*p - '0');
                p++;
            }
            break;
        }
        p++;
    }
    /* 字段逐行 */
    p = h;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *ln = nl ? nl : end;
        const char *colon = memchr(p, ':', (size_t)(ln - p));
        if (colon) {
            const char *v = colon + 1;
            int vlen = (int)(ln - v);
            int nlen = (int)(colon - p);
            while (vlen > 0 && *v == ' ') { v++; vlen--; }
            if (ci_eq(p, nlen, "content-length") && vlen > 0) {
                int x = 0;
                while (vlen > 0 && *v >= '0' && *v <= '9') {
                    x = x * 10 + (*v - '0');
                    v++; vlen--;
                }
                *clen = x;
            } else if (ci_eq(p, nlen, "transfer-encoding")) {
                if (ci_has(v, vlen, "chunked")) *chunked = 1;
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* ---- 对单个源做一次完整检查 ----
 * 返回 1 = 拿到 HTTP 200 + 按源格式解析成功的响应（tag 可能为空串，
 *        如 Gitee 空 release 列表 = 权威的"无发布"），用此源定论；
 * 返回 0 = 该源不可用（解析失败/连不上/超时/非 2xx），换下一个源。
 * 内部为一次性独立连接：每源自建 fd + mbedtls 全套，出口统一清理。 */
static int fetch_one(int idx, const UpdSrc *s, char *tag, int tagn)
{
    char win[UPD_WIN];
    char req[384];
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    unsigned int saddr = 0;
    int fd = -1, r, i, bn = 0;
    int hs = 0, st = 0, clen = -1, chunked = 0, done = 0;
    uint64_t dl;
    int ch_state = 0, ch_left = 0, ch_size = 0, ch_hex = 0, ch_ext = 0;

    tag[0] = 0;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);

    /* 1) 域名解析 */
    r = resolve_host(s->host, &saddr);
    if (r != 0) {
        dlog("upd[%d]: resolve %s failed r=0x%x", idx, s->host, (unsigned)-r);
        goto fail;
    }
    {
        const unsigned char *b = (const unsigned char *)&saddr;
        dlog("upd[%d]: resolve %s ok %d.%d.%d.%d", idx, s->host,
             b[0], b[1], b[2], b[3]);
    }

    /* 2) TCP 连接 */
    {
        int ec = 0;
        fd = conn_to(saddr, 443, &ec);
        if (fd < 0) {
            dlog("upd[%d]: connect %s:443 failed ec=0x%x", idx, s->host,
                 ec < 0 ? (unsigned)-ec : (unsigned)ec);
            goto fail;
        }
    }

    /* 3) TLS 1.2 握手（VERIFY_NONE：无 CA 库，只连不校验） */
    if (mbedtls_entropy_add_source(&ent, ent_poll, NULL, 48,
                                   MBEDTLS_ENTROPY_SOURCE_STRONG) != 0)
        goto fail;
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                              (const unsigned char *)"psvsend-upd", 11) != 0)
        goto fail;
    if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT) != 0)
        goto fail;
    mbedtls_ssl_conf_min_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_set_bio(&ssl, &fd, bio_send, bio_recv, NULL);
    if (mbedtls_ssl_setup(&ssl, &conf) != 0) goto fail;
    mbedtls_ssl_set_hostname(&ssl, s->host);  /* SNI */
    dl = now_us() + UPD_HS_US;
    for (;;) {
        r = mbedtls_ssl_handshake(&ssl);
        if (r == 0) break;
        if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) {
            dlog("upd[%d]: handshake failed r=0x%x", idx, (unsigned)-r);
            goto fail;
        }
        if (now_us() >= dl) {
            dlog("upd[%d]: handshake timeout", idx);
            goto fail;
        }
        sceKernelDelayThread(2000);
    }

    /* 4) 发送 GET（请求行/头字段按本源 host/path 拼） */
    snprintf(req, sizeof req,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: PSVSend/" PSVSEND_APP_VERSION "\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n\r\n",
             s->path, s->host);
    {
        size_t off = 0, hl = strlen(req);
        dl = now_us() + UPD_IO_US;
        while (off < hl) {
            r = mbedtls_ssl_write(&ssl, (const unsigned char *)req + off,
                                  hl - off);
            if (r > 0) { off += (size_t)r; continue; }
            if (r == MBEDTLS_ERR_SSL_WANT_READ ||
                r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (now_us() >= dl) {
                    dlog("upd[%d]: GET write timeout", idx);
                    goto fail;
                }
                sceKernelDelayThread(2000);
                continue;
            }
            dlog("upd[%d]: GET write failed r=0x%x", idx, (unsigned)-r);
            goto fail;
        }
    }

    /* 5) 读响应：先收原始字节过头部，再按 framing 累积 body 到 win。
     * 完成条件：Content-Length 收齐 / chunked 0 块 / 连接 EOF / 预算到 / 窗口满。 */
    {
        unsigned char seg[512];
        dl = now_us() + UPD_IO_US;
        while (!done && bn < UPD_WIN - 256) {
            r = mbedtls_ssl_read(&ssl, seg, sizeof seg);
            if (r == MBEDTLS_ERR_SSL_WANT_READ ||
                r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (now_us() >= dl) break;
                sceKernelDelayThread(2000);
                continue;
            }
            if (r <= 0) break;        /* 连接关闭/错误：body 到此为止 */
            for (i = 0; i < r && !done; i++) {
                char c = (char)seg[i];
                if (!hs) {
                    win[bn++] = c;
                    if (bn >= 4 && win[bn - 4] == '\r' &&
                        win[bn - 3] == '\n' && win[bn - 2] == '\r' &&
                        win[bn - 1] == '\n') {
                        http_parse_hdr(win, bn, &st, &clen, &chunked);
                        if (st < 200 || st >= 300) {
                            dlog("upd[%d]: http status %d", idx, st);
                            goto fail;
                        }
                        hs = 1;
                        bn = 0;       /* 丢弃头部 */
                    }
                } else if (chunked) {
                    if (ch_state == 0) {          /* chunk-size 行 */
                        if (c == '\r') continue;
                        if (c == '\n') {
                            if (!ch_hex) { done = 1; break; }   /* 格式异常 */
                            ch_state = 1;
                            ch_left = ch_size;
                            ch_size = 0;
                            ch_hex = 0;
                            ch_ext = 0;
                            if (ch_left == 0) { done = 1; break; }  /* 尾块 */
                            continue;
                        }
                        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                            (c >= 'A' && c <= 'F')) {
                            if (!ch_ext) {
                                int v = c <= '9' ? c - '0'
                                      : (c <= 'F' ? c - 'A' + 10 : c - 'a' + 10);
                                ch_hex = 1;
                                ch_size = ch_size * 16 + v;
                            }
                        } else {
                            ch_ext = 1;           /* 扩展/空格后数字一律忽略 */
                        }
                        continue;
                    }
                    if (ch_state == 1) {          /* chunk 数据 */
                        if (ch_left > 0 && bn < UPD_WIN - 256) {
                            win[bn++] = c;
                            ch_left--;
                            if (ch_left == 0) ch_state = 2;
                        }
                        continue;
                    }
                    if (c == '\n') ch_state = 0;  /* chunk 尾 CRLF */
                } else {
                    win[bn++] = c;
                    if (clen >= 0 && bn >= clen) done = 1;
                }
            }
        }
    }
    win[bn] = 0;

    /* 6) 按源格式解析 body */
    if (s->want == 2) {
        /* config.h：取 PSVSEND_APP_VERSION 宏的字符串值。宏在文件前部
         * （#define PSVSEND_APP_VERSION "2.0.0"），首个出现即定义处；
         * 其后紧跟双引号包围的版本串。 */
        const char *m = strstr(win, "PSVSEND_APP_VERSION");
        const char *q, *e;
        if (!m) {
            dlog("upd[%d]: %s no version macro (%d bytes)", idx, s->host, bn);
            goto fail;
        }
        q = strchr(m, '"');
        if (!q) {
            dlog("upd[%d]: %s macro without value", idx, s->host);
            goto fail;
        }
        q++;
        e = strchr(q, '"');
        if (!e || e - q <= 0 || e - q >= tagn) {
            dlog("upd[%d]: %s bad macro value (%d bytes)", idx, s->host, bn);
            goto fail;
        }
        memcpy(tag, q, (size_t)(e - q));
        tag[e - q] = 0;
        dlog("upd[%d]: %s ok version=%s", idx, s->host, tag);
    } else {
        if (parse_atom_tag(win, tag, tagn) != 0) {
            dlog("upd[%d]: %s no usable entry title (%d bytes)", idx,
                 s->host, bn);
            goto fail;
        }
        dlog("upd[%d]: %s ok latest=%s", idx, s->host, tag);
    }

    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_entropy_free(&ent);
    mbedtls_ctr_drbg_free(&drbg);
    if (fd >= 0) sceNetSocketClose(fd);
    return 1;

fail:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_entropy_free(&ent);
    mbedtls_ctr_drbg_free(&drbg);
    if (fd >= 0) sceNetSocketClose(fd);
    return 0;
}

/* worker：按源表顺序试，首个可用源定论；全部失败 → FAIL。 */
static void upd_run(void)
{
    char tag[64] = "";
    char src_host[48] = "none";
    UpdState fin = UPD_FAIL;
    int i;

    for (i = 0; i < UPD_SRC_N; i++) {
        if (fetch_one(i, &UPD_SRC[i], tag, sizeof tag)) {
            snprintf(src_host, sizeof src_host, "%s", UPD_SRC[i].host);
            dlog("upd: decided by src[%d] %s tag='%s'", i, UPD_SRC[i].host, tag);
            if (tag[0]) {
                if (cmp_version(tag, PSVSEND_APP_VERSION) > 0) {
                    snprintf(g_latest, sizeof g_latest, "%s", tag);
                    fin = UPD_NEW;
                } else {
                    fin = UPD_NONE;   /* 同版/更旧都算"无新版" */
                }
            } else {
                fin = UPD_NONE;       /* 权威源确认无 release */
            }
            break;
        }
    }

    g_cfg.upd_last = (int)now_unix();    /* 成败都记：防半开网络按周期高频重试 */
    config_save();
    g_st = fin;
    g_busy = 0;
    dlog("update: done state=%d src=%s%s", fin, src_host,
         fin == UPD_NEW ? g_latest : "");
    sceKernelExitDeleteThread(0);
}

static void upd_launch(void)
{
    SceUID th;
    if (g_busy) return;
    g_busy = 1;
    g_st = UPD_WORKING;
    th = sceKernelCreateThread("psvsend_upd", (SceKernelThreadEntry)upd_run,
                               0x40, UPD_STACK, 0, 0, NULL);
    if (th < 0) {                     /* 建线程失败（极少）→ 复位 */
        g_st = UPD_FAIL;
        g_busy = 0;
        return;
    }
    sceKernelStartThread(th, 0, NULL);
}

void update_init(void)
{
    g_st = UPD_IDLE;
    g_latest[0] = 0;
    g_busy = 0;
    g_tick_last_us = 0;
}

void update_tick(void)
{
    long now, last;
    if (g_tick_last_us && now_us() - g_tick_last_us < 1000000ull) return;
    g_tick_last_us = now_us();
    if (g_busy || auto_interval() <= 0) return;
    if (!net_connected()) return;             /* Wi-Fi 未就绪：等下一拍 */
    last = (long)g_cfg.upd_last;
    if (last && now_unix() - last < auto_interval()) return;
    upd_launch();                              /* 周期到（含首次）自动查 */
}

void update_check_now(void)
{
    upd_launch();                              /* 手动：无视周期 */
}

UpdState update_state(void) { return (UpdState)g_st; }
const char *update_latest(void) { return g_latest; }
