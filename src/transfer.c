/* transfer.c —— LocalSend v2 HTTP(S) 发送客户端（对应协议 §4 文件传输 HTTP）。
 * 真机事实：Vita SceNet 的 TCP connect/recv/send 在后台线程可用（http 服务器
 * 线程已验证），仅 UDP sendto 必须留在主循环。因此整段"prepare 等待 + 逐文件
 * 上传"跑在独立线程，UI 主线程只通过 xfer_info() 取快照渲染。
 * 收发超时模型与 http.c 一致：SceNet 无数据时 recv 立刻返回 EWOULDBLOCK
 * (0x80410123)，不能当错误，必须轮询到截止时间；取消通过每轮检查标志实现。
 *
 * HTTPS 目标（对方 announce protocol=https）：走 mbedTLS（工具链自带的 Vita
 * 移植），用对方 announce 的 fingerprint（协议规定 = 证书 SHA-256 hex）做
 * pin：握手验证回调里算叶子证书 DER 的 SHA-256，与指纹不匹配即拒绝。熵源
 * 由 Vita 弱随机(时间/指针/计数)顶替，LAN 场景够用。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
/* 允许按公开成员名访问 mbedtls 上下文（取对端 fatal alert 描述码，调试用） */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include "transfer.h"
#include "config.h"
#include "identity.h"
#include "http.h"
#include "json_util.h"
#include "dlog.h"

#define NET_AGAIN(r) ((r) == 0x80410123)
#define POLL_US       20000          /* 20ms 轮询间隔 */
#define TX_SOCK_BUF   8192           /* 响应收包缓冲 */
#define TX_CHUNK      32768          /* 上传分块 */
#define RSP_BUF       65536          /* prepare 应答缓冲上限（头+body） */

/* prepare-upload 是"等到接收方接受/拒绝"的长请求，给足时间；
 * 期间每 20ms 检查取消标志，本地取消立即退出。 */
#define PREPARE_WAIT_US (30LL * 60 * 1000000LL)
#define OP_TIMEOUT_US   30000000LL    /* 上传响应等待上限 30s */
#define CHUNK_DEADLINE_US 10000000LL  /* 单个分块发送上限 10s */
#define TLS_HANDSHAKE_US  20000000LL  /* TLS 握手上限 20s */

typedef struct {
    XferInfo   v;
    volatile int  cancel;
    SceUID     th;
    char       ip[16];
    int        port;
    int        tls;                  /* 1=HTTPS(mbedTLS) 0=HTTP */
    char       pin[96];              /* TLS pin：对方证书 SHA-256（hex） */
    XferFile   files[XFER_MAX_FILES];
    int        count;
    char       session[64];          /* prepare 应答的会话 ID */
    char       tokens[XFER_MAX_FILES][64];
} Job;

static Job    g_j;
static SceUID g_mtx = -1;

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    lock();
    va_start(ap, fmt);
    vsnprintf(g_j.v.msg, sizeof g_j.v.msg, fmt, ap);
    va_end(ap);
    unlock();
}

/* 更新单文件进度；若正处理该文件则重算总进度 */
static void set_file_state(int i, int state, SceOff sent)
{
    if (i < 0 || i >= g_j.count) return;
    lock();
    g_j.v.f[i].state = state;
    g_j.v.f[i].sent = sent;
    if (i == g_j.v.cur) {
        SceOff t = 0;
        int k;
        for (k = 0; k < g_j.count; k++) t += g_j.v.f[k].sent;
        g_j.v.total_sent = t;
    }
    unlock();
}

/* 整个任务失败收尾（ok=false + 失败原因） */
static void fail(const char *fmt, ...)
{
    va_list ap;
    lock();
    g_j.v.finished = true;
    g_j.v.active = false;
    g_j.v.ok = false;
    g_j.v.cur = -1;
    va_start(ap, fmt);
    vsnprintf(g_j.v.err, sizeof g_j.v.err, fmt, ap);
    va_end(ap);
    unlock();
    dlog("xfer: FAIL %s", g_j.v.err);
}

/* 标记第 i 个文件失败并整体收尾 */
static void fail_file(int i, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    if (i >= 0 && i < g_j.count)
        set_file_state(i, 3, g_j.v.f[i].sent);
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    fail("%s", tmp);
}

/* 取消收尾（终态显示 cancelled） */
static void finish_cancelled(void)
{
    lock();
    g_j.v.cancelled = true;
    unlock();
    fail("cancelled by user");
}

/* ---------- 连接抽象：一条 HTTP(S) 连接 ---------- */
typedef struct {
    int fd;
    int tls;                  /* 本连接是否走 TLS */
    int  ssl_up;              /* mbedtls 上下文已初始化（收尾要释放） */
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt     ca;  /* 占位空 CA 表：mbedTLS 要求 ca_chain 非空，
                               * 真实信任由 pin_verify 指纹回调判定 */
    mbedtls_x509_crt     own;     /* 本机身份证书：接收端强制 mTLS，客户端必须出示 */
    mbedtls_pk_context   ownpk;
} Conn;

/* TLS bio：mbedtls <-> Vita socket */
static int tls_send_cb(void *ctx, const unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int r = sceNetSend(fd, buf, (unsigned)len, 0);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (NET_AGAIN(r)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int tls_recv_cb(void *ctx, unsigned char *buf, size_t len)
{
    int fd = *(int *)ctx;
    int r = sceNetRecv(fd, buf, (unsigned)len, 0);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (NET_AGAIN(r)) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

/* 十六进制比较（大小写不敏感；LocalSend 指纹 hex 可能大写/小写混排） */
static int hexeq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'a' && x <= 'f') x -= 'a' - 'A';
        if (y >= 'a' && y <= 'f') y -= 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == 0 && *b == 0;
}

/* TLS 证书 pin：叶子证书 DER 的 SHA-256 == 对方 announce 的 fingerprint */
static int pin_verify(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    unsigned char dig[32];
    char hex[65];
    int i;
    (void)arg;
    if (depth != 0) return 0;                 /* 只核对叶子 */
    if (!crt || !crt->raw.p || crt->raw.len == 0)
        return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    if (mbedtls_sha256(crt->raw.p, crt->raw.len, dig, 0) != 0)
        return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    for (i = 0; i < 32; i++) {
        hex[i * 2]     = "0123456789ABCDEF"[dig[i] >> 4];
        hex[i * 2 + 1] = "0123456789ABCDEF"[dig[i] & 0xF];
    }
    hex[64] = 0;
    dlog("xfer: peer cert sha256=%s", hex);
    if (!g_j.pin[0] || !hexeq_ci(hex, g_j.pin)) {
        dlog("xfer: pin mismatch (want %s)", g_j.pin);
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }
    *flags = 0;                                /* 自签无 CA，忽略其它校验位 */
    return 0;
}

/* Vita 熵源：时间 + 栈/全局地址 + 计数器混洗（splitmix64）。
 * Vita 用户态没有真 RNG；LAN 场景按弱随机处理，够撑 TLS 会话密钥。 */
static unsigned long g_ent_seed;
static int vita_entropy_poll(void *arg, unsigned char *out, size_t len, size_t *olen)
{
    uint64_t x = (uint64_t)sceKernelGetSystemTimeWide()
               ^ ((uint64_t)(uintptr_t)&g_ent_seed << 17)
               ^ ((uint64_t)(g_ent_seed++) * 0x9E3779B97F4A7C15ull);
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

/* 发送 len 字节；返回 0=成功 -1=错误/超时 -2=已取消 */
static int conn_send_all(Conn *c, const char *data, int len)
{
    int off = 0;
    SceLong64 dl = sceKernelGetSystemTimeWide() + CHUNK_DEADLINE_US;
    while (off < len) {
        if (g_j.cancel) return -2;
        if (sceKernelGetSystemTimeWide() >= dl) return -1;   /* 发送窗口堵死 */
        if (c->tls) {
            int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + off,
                                      (size_t)(len - off));
            if (r > 0) { off += r; continue; }
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                sceKernelDelayThread(POLL_US);
                continue;
            }
            dlog("xfer: ssl_write err -0x%04X", (unsigned)(-r));
            return -1;
        } else {
            int r = sceNetSend(c->fd, data + off, (unsigned)(len - off), 0);
            if (r > 0) { off += r; continue; }
            if (r == 0) return -1;
            if (NET_AGAIN(r)) {
                sceKernelDelayThread(POLL_US);
                continue;
            }
            return -1;
        }
    }
    return 0;
}

/* 收一次包（轮询到 dl 截止）：返回 >0 字节数；0=对方关闭；-1=错误/超时；-2=取消 */
static int conn_recv_poll(Conn *c, char *buf, int cap, SceLong64 dl)
{
    for (;;) {
        if (g_j.cancel) return -2;
        if (sceKernelGetSystemTimeWide() >= dl) return -1;
        if (c->tls) {
            int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)cap);
            if (r > 0) return r;
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                sceKernelDelayThread(POLL_US);
                continue;
            }
            if (r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
            dlog("xfer: ssl_read err -0x%04X", (unsigned)(-r));
            return -1;
        } else {
            int r = sceNetRecv(c->fd, buf, (unsigned)cap, 0);
            if (r > 0) return r;
            if (r == 0) return 0;
            if (NET_AGAIN(r)) {
                sceKernelDelayThread(POLL_US);
                continue;
            }
            dlog("xfer: recv err 0x%08X", (unsigned)r);
            return -1;
        }
    }
}

/* 头字段名匹配（ASCII 大小写不敏感；ref 为期望的小写字段名） */
static int hdr_name_eq(const char *s, int n, const char *ref)
{
    int i;
    for (i = 0; i < n; i++) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != ref[i]) return 0;
    }
    return 1;
}

/* 读响应：把"头 + Content-Length body"整段累积进调用方 buf（cap 容量）。
 * 头结束符之后就是 body，buf[body_end]=0 收尾（解析直接对 buf 用 strstr）。
 * 返回 body 长度；-1=错误/超时/连接关/缓冲不足；-2=已取消。*code 填状态码。 */
static int read_resp(Conn *c, int *code, char *buf, int cap, SceLong64 wait_us)
{
    int n = 0, he = -1, cl = -1, body_off = 0;
    SceLong64 dl = sceKernelGetSystemTimeWide() + wait_us;
    *code = 0;
    for (;;) {
        int i, r;
        if (g_j.cancel) return -2;
        if (n >= cap - 1) return -1;
        r = conn_recv_poll(c, buf + n, cap - 1 - n, dl);
        if (r == -2) return -2;
        if (r <= 0) return -1;
        n += r;
        buf[n] = 0;
        /* 扫整个缓冲找头结束符（勿只扫末尾——历史 bug） */
        for (i = 0; i + 4 <= n && he < 0; i++)
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') he = i;
        if (he >= 0 && cl < 0) {
            long v = 0;
            int found = 0, ls = 0;
            /* LocalSend 实际返回全小写头（content-length），须大小写不敏感
             * 逐行匹配，否则把 body 长度当 0 → "empty prepare reply"。 */
            while (ls < he) {
                int le;
                const char *pn;
                for (le = ls; le < he && buf[le] != '\n'; le++) ;
                pn = memchr(buf + ls, ':', (size_t)(le - ls));
                if (pn && (pn - (buf + ls)) == 14 &&
                    hdr_name_eq(buf + ls, 14, "content-length")) {
                    const char *q = pn + 1;
                    while (*q == ' ' || *q == '\t') q++;
                    while (*q >= '0' && *q <= '9') { v = v * 10 + (*q - '0'); q++; }
                    found = 1;
                    break;
                }
                ls = le + 1;
            }
            cl = found ? (int)v : 0;
            if (strncmp(buf, "HTTP/1.", 7) == 0) {
                const char *cs = strchr(buf, ' ');
                if (cs) *code = atoi(cs + 1);
            }
            body_off = he + 4;
        }
        if (he >= 0 && n >= body_off + cl) break;
    }
    if (cl > cap - 1 - body_off) return -1;   /* body 超出缓冲 */
    buf[body_off + cl] = 0;
    return cl;
}

/* 释放 TLS 上下文并关 socket（尽力 close_notify，失败忽略） */
static void conn_close(Conn *c)
{
    if (c->tls && c->ssl_up) {
        int i;
        for (i = 0; i < 10; i++) {
            int r = mbedtls_ssl_close_notify(&c->ssl);
            if (r == 0) break;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
            sceKernelDelayThread(POLL_US);
        }
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
        mbedtls_pk_free(&c->ownpk);
        mbedtls_x509_crt_free(&c->own);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
        c->ssl_up = 0;
    }
    if (c->fd >= 0) sceNetSocketClose(c->fd);
    c->fd = -1;
}

/* TLS 握手（EWOULDBLOCK 轮询；期间可取消）；返回 0 成功 / -1 / -2 取消 */
static int tls_handshake(Conn *c)
{
    SceLong64 dl = sceKernelGetSystemTimeWide() + TLS_HANDSHAKE_US;
    int r;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);
    c->ssl_up = 1;
    if (mbedtls_entropy_add_source(&c->ent, vita_entropy_poll, NULL, 48,
                                   MBEDTLS_ENTROPY_SOURCE_STRONG) != 0)
        return -1;
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent,
                              (const unsigned char *)"psvsend-ls", 10) != 0) {
        dlog("xfer: drbg seed fail");
        return -1;
    }
    r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) { dlog("xfer: ssl defaults err -0x%04X", (unsigned)(-r)); return -1; }
    /* 锁 TLS1.2：3.x 里 TLS1.3 的证书校验不走 per-cert verify 回调，
     * 1.2 的 VERIFY_REQUIRED+自定义回调语义明确（leaf 指纹 pin 依赖它）。 */
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    /* OPTIONAL + 自定义回调是 mbedTLS 官方的"自管校验"模式：
     * - REQUIRED 强制要求配置了可验证的信任锚，否则握手报证书链错误；
     *   我们没有 CA 链（自签证书按指纹 pin），必须避开 REQUIRED；
     * - 但即便 OPTIONAL，ca_chain 也必须非空，否则仍会报 CA_CHAIN_REQUIRED
     *   (-0x7680)。故注册一个占位空 CA 表（c->ca）满足检查；
     * - 真正信任判定在 pin_verify：指纹匹配→清标志放行；不匹配→返回非零，
     *   该错误在任何 authmode 下都会立即中止握手，安全语义不变。 */
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_verify(&c->conf, pin_verify, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    /* mTLS：2026 版 LocalSend 服务器强制客户端证书（无证书即 fatal alert
     * certificate_required）。出示内嵌设备身份（自签 RSA-2048；接收端只校验
     * 证书本身有效：时间 + 自签签名，见其 client_cert_verifier）。证书/私钥
     * 随本连接析构（conn_close），conf 只在握手期引用指针。 */
    mbedtls_x509_crt_init(&c->own);
    mbedtls_pk_init(&c->ownpk);
    if (identity_cert_parse(&c->own) != 0 || identity_key_parse(&c->ownpk) != 0) {
        dlog("xfer: identity cert/key parse fail");
        return -1;
    }
    {
        char fp[65];
        if (identity_fingerprint(fp) == 0)
            dlog("xfer: client identity fp=%s", fp);
    }
    r = mbedtls_ssl_conf_own_cert(&c->conf, &c->own, &c->ownpk);
    if (r != 0) { dlog("xfer: own_cert err -0x%04X", (unsigned)(-r)); return -1; }
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) { dlog("xfer: ssl setup fail"); return -1; }
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, tls_send_cb, tls_recv_cb, NULL);
    /* mbedTLS 3.6.3+ 安全检查：客户端走证书校验路径时必须先调 set_hostname，
     * 否则直接报 -0x5D80（VERIFICATION_WITHOUT_HOSTNAME）。我们是证书指纹
     * pin 校验（不依赖主机名匹配），这里填对端 IP 即可。 */
    r = mbedtls_ssl_set_hostname(&c->ssl, g_j.ip);
    if (r != 0) { dlog("xfer: set_hostname err -0x%04X", (unsigned)(-r)); return -1; }
    for (;;) {
        if (g_j.cancel) return -2;
        if (sceKernelGetSystemTimeWide() >= dl) { dlog("xfer: tls handshake timeout"); return -1; }
        r = mbedtls_ssl_handshake(&c->ssl);
        if (r == 0) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            sceKernelDelayThread(POLL_US);
            continue;
        }
        dlog("xfer: handshake err -0x%04X", (unsigned)(-r));
        if (r == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE && c->ssl.in_msg != NULL)
            dlog("xfer: peer fatal alert lvl=%d desc=%d",
                 c->ssl.in_msg[0], c->ssl.in_msg[1]);
        return -1;
    }
}

/* 建立连接：TCP connect +（如需）TLS 握手 + 发 HTTP 头。
 * 返回 0 成功；-1 失败；-2 取消（连接已关闭）。 */
static int conn_open(Conn *c, const char *method_path, SceOff body_len)
{
    SceNetSockaddrIn sa;
    char hdr[512];
    int hlen, r;
    c->fd = sceNetSocket("psvsend_xfer", SCE_NET_AF_INET,
                         SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
    if (c->fd < 0) {
        dlog("xfer: socket fail 0x%08X", (unsigned)c->fd);
        return -1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons((unsigned)g_j.port);
    if (sceNetInetPton(SCE_NET_AF_INET, g_j.ip, &sa.sin_addr.s_addr) != 1) {
        dlog("xfer: bad ip %s", g_j.ip);
        sceNetSocketClose(c->fd);
        c->fd = -1;
        return -1;
    }
    r = sceNetConnect(c->fd, (const SceNetSockaddr *)&sa, sizeof sa);
    if (r < 0 && !NET_AGAIN(r)) {
        dlog("xfer: connect %s:%d fail 0x%08X",
             g_j.ip, g_j.port, (unsigned)r);
        sceNetSocketClose(c->fd);
        c->fd = -1;
        return -1;
    }
    if (c->tls) {
        r = tls_handshake(c);
        if (r == -2) {
            conn_close(c);
            return -2;
        }
        if (r != 0) {
            conn_close(c);
            return -3;                  /* TCP 已通但 TLS 握手/证书校验失败 */
        }
        dlog("xfer: tls handshake ok -> %s:%d", g_j.ip, g_j.port);
    }
    hlen = snprintf(hdr, sizeof hdr,
                    "POST %s HTTP/1.1\r\n"
                    "Host: %s:%d\r\n"
                    "Content-Length: %lld\r\n"
                    "Connection: close\r\n\r\n",
                    method_path, g_j.ip, g_j.port, (long long)body_len);
    r = conn_send_all(c, hdr, hlen);
    if (r < 0) {                        /* -1 发送失败 / -2 取消 */
        conn_close(c);
        return r;
    }
    return 0;
}

/* ---------- JSON 拼装/解析 ---------- */

/* 在 doc 中定位 "key":"..." 并把值拷出（值不转义，用于 session/token） */
static int find_obj_str(const char *doc, const char *key, char *out, int outsz)
{
    const char *p, *q;
    char pat[96];
    int len;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(doc, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    q = p;
    while (*q && *q != '"') {
        if (*q == '\\') q++;
        if (*q) q++;
    }
    len = (int)(q - p);
    if (len >= outsz) len = outsz - 1;
    if (len <= 0) return 0;
    memcpy(out, p, (unsigned)len);
    out[len] = 0;
    return 1;
}

/* 组装 prepare-upload 请求体（成员信息 + 待发文件元数据） */
static char *build_prepare_json(void)
{
    int cap = TX_SOCK_BUF + XFER_MAX_FILES * 420;
    char ae[2 * sizeof g_cfg.alias], fe[2 * sizeof g_cfg.fingerprint];
    char *b = (char *)malloc((size_t)cap);
    int o = 0, i;
    if (!b) return NULL;
    json_escape(g_cfg.alias, ae, sizeof ae);
    json_escape(g_cfg.fingerprint, fe, sizeof fe);
    o += snprintf(b + o, (size_t)(cap - o),
                  "{\"info\":{\"alias\":\"%s\",\"version\":\"2.0\","
                  "\"deviceModel\":\"PlayStation Vita\",\"deviceType\":\"mobile\","
                  "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"http\","
                  "\"download\":false},\"files\":{",
                  ae, fe, http_port());
    for (i = 0; i < g_j.count && o < cap - 64; i++) {
        char esc[2 * 128 + 8];
        json_escape(g_j.files[i].name, esc, sizeof esc);
        o += snprintf(b + o, (size_t)(cap - o),
                      "%s\"f%d\":{\"id\":\"f%d\",\"fileName\":\"%s\","
                      "\"size\":%lld,\"fileType\":\"application/octet-stream\"}",
                      i ? "," : "", i, i, esc, (long long)g_j.files[i].size);
    }
    if (o < cap - 4) o += snprintf(b + o, (size_t)(cap - o), "}}");
    b[o] = 0;
    return b;
}

/* ---------- 传输线程 ---------- */
static int xfer_thr(SceSize args, void *argp)
{
    char *body = NULL;
    char *prep = NULL;              /* prepare 应答缓冲（堆上，容大文件列表） */
    char chunk[TX_CHUNK];
    int code = 0, rc, i;
    (void)args; (void)argp;
    dlog("xfer: thread entered, target %s:%d (%s)%s, %d file(s)",
         g_j.ip, g_j.port, g_j.tls ? "https" : "http",
         g_j.tls ? " tls" : "", g_j.count);

    /* 阶段 1：prepare-upload（长等接收方决定） */
    set_msg("Waiting for %s to accept...", g_j.ip);
    body = build_prepare_json();
    prep = body ? (char *)malloc(RSP_BUF) : NULL;
    if (!body || !prep) { fail("out of memory"); goto out; }
    {
        Conn c;
        memset(&c, 0, sizeof c);
        c.fd = -1;
        c.tls = g_j.tls;
        rc = conn_open(&c, "/api/localsend/v2/prepare-upload", (SceOff)strlen(body));
        if (rc == 0)
            rc = conn_send_all(&c, body, (int)strlen(body));
        if (rc == 0)
            rc = read_resp(&c, &code, prep, RSP_BUF, PREPARE_WAIT_US);
        if (rc == -2) { conn_close(&c); finish_cancelled(); goto out; }
        if (rc < 0) {
            /* rc==-3: TCP 已通但 TLS 握手/证书校验失败；其余归"无应答" */
            conn_close(&c);
            if (g_j.cancel) { finish_cancelled(); goto out; }
            if (rc == -3)
                fail("TLS handshake with %s:%d failed", g_j.ip, g_j.port);
            else
                fail("no reply from receiver (HTTP %d)", code);
            goto out;
        }
        conn_close(&c);
    }
    if (code == 204) {          /* 无需传输文件：直接成功 */
        lock();
        g_j.v.finished = true;
        g_j.v.active = false;
        g_j.v.ok = true;
        g_j.v.cur = -1;
        snprintf(g_j.v.msg, sizeof g_j.v.msg, "Nothing to send");
        unlock();
        goto out;
    }
    if (code != 200) {
        if (code == 403) fail("receiver rejected the request (HTTP 403)");
        else if (code == 400) fail("receiver: bad request body (HTTP 400)");
        else fail("prepare failed (HTTP %d)", code);
        goto out;
    }
    if (rc == 0) { fail("empty prepare reply"); goto out; }

    /* 200：解析 sessionId + 每个文件的 token（body 在头结束符之后） */
    {
        char *js = strstr(prep, "\r\n\r\n");
        js = js ? js + 4 : prep;    /* 跳过 HTTP 头，指向 JSON 起点 */
        if (!json_get_str(js, "sessionId", g_j.session, sizeof g_j.session))
            find_obj_str(js, "sessionId", g_j.session, sizeof g_j.session);
        if (g_j.session[0] == 0) { fail("no sessionId in reply"); goto out; }
        for (i = 0; i < g_j.count; i++) {
            char key[16];
            snprintf(key, sizeof key, "f%d", i);
            g_j.tokens[i][0] = 0;
            find_obj_str(js, key, g_j.tokens[i], sizeof g_j.tokens[i]);
        }
    }
    dlog("xfer: prepare ok session=%s", g_j.session);
    free(body); body = NULL;
    free(prep); prep = NULL;

    /* 阶段 2：逐文件上传 */
    for (i = 0; i < g_j.count; i++) {
        int fd, rd, sr, blen;
        SceOff sent_file = 0;
        if (g_j.cancel) { finish_cancelled(); return 0; }
        if (g_j.tokens[i][0] == 0) {
            fail_file(i, "no token for %s", g_j.files[i].name);
            return 0;
        }
        lock();
        g_j.v.cur = i;
        g_j.v.f[i].state = 1;
        unlock();
        set_msg("Sending %s (%d/%d)", g_j.files[i].name, i + 1, g_j.count);

        fd = sceIoOpen(g_j.files[i].path, SCE_O_RDONLY, 0);
        if (fd < 0) {
            fail_file(i, "cannot open %s", g_j.files[i].path);
            return 0;
        }
        {
            char q[320];
            char abuf[512];         /* 上传回执缓冲（头 + 空 body） */
            Conn c;
            snprintf(q, sizeof q,
                     "/api/localsend/v2/upload?sessionId=%s&fileId=f%d&token=%s",
                     g_j.session, i, g_j.tokens[i]);
            memset(&c, 0, sizeof c);
            c.fd = -1;
            c.tls = g_j.tls;
            sr = conn_open(&c, q, g_j.files[i].size);
            if (sr == -2) {           /* conn_open 取消已自关 */
                sceIoClose(fd);
                finish_cancelled();
                return 0;
            }
            if (sr < 0) {
                sceIoClose(fd);
                if (sr == -3)
                    fail_file(i, "TLS handshake with %s:%d failed", g_j.ip, g_j.port);
                else
                    fail_file(i, "connect failed for %s", g_j.files[i].name);
                return 0;
            }
            while (sent_file < g_j.files[i].size) {
                SceOff want = g_j.files[i].size - sent_file;
                if (want > (SceOff)sizeof chunk) want = sizeof chunk;
                if (g_j.cancel) {
                    conn_close(&c);
                    sceIoClose(fd);
                    finish_cancelled();
                    return 0;
                }
                rd = sceIoRead(fd, chunk, (unsigned)want);
                if (rd < 0) {
                    conn_close(&c);
                    sceIoClose(fd);
                    fail_file(i, "read error on %s", g_j.files[i].name);
                    return 0;
                }
                sr = conn_send_all(&c, chunk, rd);
                if (sr < 0) {
                    conn_close(&c);
                    sceIoClose(fd);
                    if (sr == -2) finish_cancelled();
                    else fail_file(i, "send error on %s", g_j.files[i].name);
                    return 0;
                }
                sent_file += rd;
                set_file_state(i, 1, sent_file);
            }
            /* 等对方回执（2xx 即成功） */
            blen = read_resp(&c, &code, abuf, sizeof abuf, OP_TIMEOUT_US);
            conn_close(&c);
            if (blen == -2) {
                sceIoClose(fd);
                finish_cancelled();
                return 0;
            }
            if (blen < 0 || code < 200 || code >= 300) {
                sceIoClose(fd);
                fail_file(i, "%s upload failed (HTTP %d)", g_j.files[i].name, code);
                return 0;
            }
        }
        sceIoClose(fd);
        dlog("xfer: file %d/%d done (%lld bytes)", i + 1, g_j.count,
             (long long)g_j.files[i].size);
        set_file_state(i, 2, g_j.files[i].size);
    }
    if (g_j.cancel) { finish_cancelled(); return 0; }

    lock();
    g_j.v.finished = true;
    g_j.v.active = false;
    g_j.v.ok = true;
    g_j.v.cur = -1;
    snprintf(g_j.v.msg, sizeof g_j.v.msg, "Complete");
    unlock();
    dlog("xfer: all done");
    return 0;
out:
    free(body);
    free(prep);
    return 0;
}

/* ---------- 对外接口 ---------- */
int xfer_start(const char *ip, int port, const char *proto, const char *fp,
               const XferFile *files, int n)
{
    int i;
    if (g_mtx < 0)
        g_mtx = sceKernelCreateMutex("psvsend_xfer", 0, 1, NULL);
    lock();
    if (g_j.v.active) {
        snprintf(g_j.v.err, sizeof g_j.v.err, "transfer already active");
        g_j.v.finished = true;
        g_j.v.ok = false;
        unlock();
        return -1;
    }
    memset(&g_j, 0, sizeof g_j);
    g_j.v.cur = -1;
    g_j.th = -1;
    if (!ip || !ip[0] || n <= 0 || n > XFER_MAX_FILES) {
        snprintf(g_j.v.err, sizeof g_j.v.err, "bad target or empty file list");
        g_j.v.finished = true;
        unlock();
        return -1;
    }
    g_j.tls = (proto && strcmp(proto, "https") == 0) ? 1 : 0;
    if (fp && fp[0])
        snprintf(g_j.pin, sizeof g_j.pin, "%s", fp);
    if (g_j.tls && !g_j.pin[0]) {
        snprintf(g_j.v.err, sizeof g_j.v.err,
                 "target uses https but announced no certificate fingerprint");
        g_j.v.finished = true;
        unlock();
        return -1;
    }
    strncpy(g_j.ip, ip, sizeof g_j.ip - 1);
    g_j.port = port;
    g_j.count = n;
    for (i = 0; i < n; i++) {
        strncpy(g_j.files[i].name, files[i].name, sizeof g_j.files[i].name - 1);
        strncpy(g_j.files[i].path, files[i].path, sizeof g_j.files[i].path - 1);
        g_j.files[i].size = files[i].size;
        strncpy(g_j.v.f[i].name, files[i].name, sizeof g_j.v.f[i].name - 1);
        g_j.v.f[i].size = files[i].size;
    }
    g_j.v.count = n;
    g_j.v.start_us = (uint64_t)sceKernelGetSystemTimeWide();
    for (i = 0; i < n; i++) g_j.v.total += g_j.v.f[i].size;
    g_j.v.active = true;
    unlock();

    g_j.th = sceKernelCreateThread("psvsend_xfer", xfer_thr,
                                   0x40, 0x20000, 0, 0, NULL);
    if (g_j.th < 0) {
        dlog("xfer: thread create fail 0x%08X", (unsigned)g_j.th);
        lock();
        g_j.v.active = false;
        g_j.v.finished = true;
        g_j.v.ok = false;
        snprintf(g_j.v.err, sizeof g_j.v.err, "cannot start transfer thread");
        unlock();
        return -1;
    }
    dlog("xfer: thread create -> 0x%08X", (unsigned)g_j.th);
    sceKernelStartThread(g_j.th, 0, NULL);
    return 0;
}

void xfer_cancel(void)
{
    g_j.cancel = 1;
}

void xfer_info(XferInfo *out)
{
    lock();
    if (out) *out = g_j.v;
    unlock();
}
