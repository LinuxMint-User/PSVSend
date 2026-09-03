/* scan.c 实现 —— 见 scan.h。
 * 探测顺序：对每个候选 IP 先发明文 HTTP register（快：https 服务器收到明文
 * 会立刻关连接），失败再试 TLS（mbedTLS 1.2，trust-first 信任任意叶子证书并
 * 记录其 SHA-256 供后续发送 pin；mTLS 出示内嵌设备身份证书，LocalSend 服务器
 * 强制客户端证书）。拿到 200 + member info 即 discovery_upsert_peer 入表。
 * Vita 的 socket 默认阻塞，connect 无内置超时，扫不存在的主机会卡死整个线程，
 * 故 connect 前把 fd 置 SCE_NET_SO_NBIO 非阻塞，再用 getpeername 轮询判定。 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include "config.h"
#include "json_util.h"
#include "net.h"
#include "http.h"
#include "api.h"
#include "discovery.h"
#include "identity.h"
#include "dlog.h"

#define SCAN_PORT       53317          /* 协议默认 HTTP 端口 */
#define SCAN_CONNECT_US 180000LL       /* 单 IP connect 预算 */
#define SCAN_RESP_US    500000LL       /* 收响应预算 */
#define SCAN_HS_US      3000000LL      /* 单次 TLS 握手预算 */
#define SCAN_POLL_US    10000          /* 轮询间隔（10ms） */
#define SCAN_TICK_US    100000         /* 扫描线程主循环间隔 */
#define NET_AGAIN(r) ((r) == 0x80410123)

/* 一条探测连接（简化版 transfer.c Conn；只服务本模块） */
typedef struct {
    int fd;
    int tls;                 /* 1=走了 TLS 握手 */
    int  ssl_up;             /* mbedtls 上下文已初始化 */
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt     ca;
    mbedtls_x509_crt     own;
    mbedtls_pk_context   ownpk;
} SConn;

static SceUID    g_thr = -1;
static volatile int g_up = 0;      /* 线程已建 */
static volatile int g_active = 0;  /* 正在扫 */
static volatile int g_need = 0;    /* 空闲后顺延一轮 */
static volatile int g_done = 0;    /* 已探主机数 */
static int         g_total = 0;
static char        g_peer_fp[96];  /* trust-first：最后一次 TLS 叶证书 SHA-256 */

static uint64_t now_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

/* ---------- 熵 / TLS bio / trust-first 校验（同 transfer.c 思路） ---------- */
static unsigned long s_ent_seed;
static int vita_entropy_poll(void *arg, unsigned char *out, size_t len, size_t *olen)
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

/* trust-first：不预知对端指纹，放行任意叶子证书并把其 SHA-256 记下 */
static int trust_verify(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    unsigned char dig[32];
    char hex[65];
    int i;
    (void)arg;
    if (depth != 0) return 0;
    if (!crt || !crt->raw.p || crt->raw.len == 0)
        return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    if (mbedtls_sha256(crt->raw.p, crt->raw.len, dig, 0) != 0)
        return MBEDTLS_ERR_X509_BAD_INPUT_DATA;
    for (i = 0; i < 32; i++) {
        hex[i * 2]     = "0123456789ABCDEF"[dig[i] >> 4];
        hex[i * 2 + 1] = "0123456789ABCDEF"[dig[i] & 0xF];
    }
    hex[64] = 0;
    snprintf(g_peer_fp, sizeof g_peer_fp, "%s", hex);
    *flags = 0;                              /* 自签无 CA：清掉其它校验位 */
    return 0;
}

/* ---------- 连接工具 ---------- */

/* 非阻塞 connect + getpeername 轮询；返回 fd 或 -1（预算内没连上） */
static int net_connect_to(const char *ip, int port, SceLong64 budget_us)
{
    SceNetSockaddrIn sa;
    SceLong64 dl = now_us() + budget_us;
    int one = 1, fd;
    fd = sceNetSocket("psvsend_scan", SCE_NET_AF_INET,
                      SCE_NET_SOCK_STREAM, SCE_NET_IPPROTO_TCP);
    if (fd < 0) return -1;
    sceNetSetsockopt(fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one);
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons((unsigned)port);
    if (sceNetInetPton(SCE_NET_AF_INET, ip, &sa.sin_addr.s_addr) != 1) {
        sceNetSocketClose(fd);
        return -1;
    }
    sceNetConnect(fd, (const SceNetSockaddr *)&sa, sizeof sa);
    for (;;) {
        SceNetSockaddrIn p;
        unsigned int plen = sizeof p;
        int r = sceNetGetpeername(fd, (SceNetSockaddr *)&p, &plen);
        if (r == 0) return fd;               /* 连接建立 */
        if (!NET_AGAIN(r)) break;            /* 拒绝/不可达等明确失败 */
        if (now_us() >= dl) break;           /* 预算到点，放弃 */
        sceKernelDelayThread(SCAN_POLL_US);
    }
    sceNetSocketClose(fd);
    return -1;
}

static int s_send_all(SConn *c, const char *data, int len)
{
    int off = 0;
    SceLong64 dl = now_us() + SCAN_RESP_US;
    while (off < len) {
        if (now_us() >= dl) return -1;
        if (c->tls) {
            int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + off,
                                      (size_t)(len - off));
            if (r > 0) { off += r; continue; }
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                sceKernelDelayThread(SCAN_POLL_US);
                continue;
            }
            return -1;
        } else {
            int r = sceNetSend(c->fd, data + off, (unsigned)(len - off), 0);
            if (r > 0) { off += r; continue; }
            if (r == 0) return -1;
            if (NET_AGAIN(r)) { sceKernelDelayThread(SCAN_POLL_US); continue; }
            return -1;
        }
    }
    return 0;
}

static int s_recv_poll(SConn *c, char *buf, int cap, SceLong64 dl)
{
    for (;;) {
        if (now_us() >= dl) return -1;
        if (c->tls) {
            int r = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)cap);
            if (r > 0) return r;
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                sceKernelDelayThread(SCAN_POLL_US);
                continue;
            }
            if (r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
            return -1;
        } else {
            int r = sceNetRecv(c->fd, buf, (unsigned)cap, 0);
            if (r > 0) return r;
            if (r == 0) return 0;
            if (NET_AGAIN(r)) { sceKernelDelayThread(SCAN_POLL_US); continue; }
            return -1;
        }
    }
}

/* 读完整响应（头 + Content-Length body）进 buf；返回 body 长，-1 失败。*code 填状态码 */
static int s_read_resp(SConn *c, int *code, char *buf, int cap, SceLong64 wait_us)
{
    int n = 0, he = -1, cl = -1, body_off = 0;
    SceLong64 dl = now_us() + wait_us;
    *code = 0;
    for (;;) {
        int i, r;
        if (n >= cap - 1) return -1;
        r = s_recv_poll(c, buf + n, cap - 1 - n, dl);
        if (r <= 0) return -1;
        n += r;
        buf[n] = 0;
        for (i = 0; i + 4 <= n && he < 0; i++)
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') he = i;
        if (he >= 0 && cl < 0) {
            int ls = 0;
            long v = 0;
            int found = 0;
            while (ls < he) {
                int le;
                const char *pn;
                for (le = ls; le < he && buf[le] != '\n'; le++) ;
                pn = memchr(buf + ls, ':', (size_t)(le - ls));
                if (pn && (int)(pn - (buf + ls)) == 14 &&
                    strncasecmp(buf + ls, "content-length", 14) == 0) {
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
    if (cl > cap - 1 - body_off) return -1;
    buf[body_off + cl] = 0;
    return cl;
}

/* TLS 握手（EWOULDBLOCK 轮询）；返回 0 成功 / -1 失败 */
static int s_handshake(SConn *c, const char *ip)
{
    SceLong64 dl = now_us() + SCAN_HS_US;
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
                              (const unsigned char *)"psvsend-scan", 11) != 0)
        return -1;
    r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) return -1;
    /* 锁 TLS1.2（3.x 的 TLS1.3 不走 per-cert verify 回调，无法 trust-first 记录） */
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_verify(&c->conf, trust_verify, NULL);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    /* mTLS：出示内嵌设备身份证书（接收端强制客户端证书） */
    mbedtls_x509_crt_init(&c->own);
    mbedtls_pk_init(&c->ownpk);
    if (identity_cert_parse(&c->own) != 0 || identity_key_parse(&c->ownpk) != 0)
        return -1;
    r = mbedtls_ssl_conf_own_cert(&c->conf, &c->own, &c->ownpk);
    if (r != 0) return -1;
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) return -1;
    mbedtls_ssl_set_bio(&c->ssl, &c->fd, tls_send_cb, tls_recv_cb, NULL);
    r = mbedtls_ssl_set_hostname(&c->ssl, ip);
    if (r != 0) return -1;
    for (;;) {
        if (now_us() >= dl) return -1;
        r = mbedtls_ssl_handshake(&c->ssl);
        if (r == 0) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            sceKernelDelayThread(SCAN_POLL_US);
            continue;
        }
        return -1;
    }
}

static void s_close(SConn *c)
{
    if (c->tls && c->ssl_up) {
        int i;
        for (i = 0; i < 5; i++) {
            int r = mbedtls_ssl_close_notify(&c->ssl);
            if (r == 0) break;
            if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE) break;
            sceKernelDelayThread(SCAN_POLL_US);
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

/* ---------- 探测一个 IP ---------- */

/* 拼本机 member info（register 请求体 + 我们入对方表的依据） */
static void self_info_json(char *out, int outsz)
{
    char ae[2 * sizeof g_cfg.alias];
    char fe[2 * sizeof g_cfg.fingerprint];
    int port = http_port();
    json_escape(g_cfg.alias, ae, sizeof ae);
    json_escape(g_cfg.fingerprint, fe, sizeof fe);
    snprintf(out, outsz,
             "{\"alias\":\"%s\",\"version\":\"2.0\","
             "\"deviceModel\":\"PlayStation Vita\",\"deviceType\":\"mobile\","
             "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"http\","
             "\"download\":true}",
             ae, fe, port > 0 ? port : SCAN_PORT);
}

/* 解析 register 200 响应里的对端 member info；成功返回 1 并填 out */
static int parse_member(const char *body, int tls, const char *ip, Device *out)
{
    long long v;
    memset(out, 0, sizeof *out);
    if (!body || !body[0]) return 0;
    if (!json_get_str(body, "alias", out->alias, sizeof out->alias)) return 0;
    if (!json_get_str(body, "fingerprint", out->fingerprint,
                      sizeof out->fingerprint)) {
        if (tls && g_peer_fp[0])
            snprintf(out->fingerprint, sizeof out->fingerprint, "%s", g_peer_fp);
    }
    if (out->fingerprint[0] &&
        strcmp(out->fingerprint, g_cfg.fingerprint) == 0)
        return 0;                            /* 自己 */
    snprintf(out->ip, sizeof out->ip, "%s", ip);
    if (!json_get_str(body, "protocol", out->protocol, sizeof out->protocol))
        snprintf(out->protocol, sizeof out->protocol, "%s", tls ? "https" : "http");
    out->port = SCAN_PORT;
    if (json_get_int(body, "port", &v) && v > 0 && v <= 65535)
        out->port = (int)v;
    json_get_str(body, "deviceModel", out->model, sizeof out->model);
    json_get_str(body, "deviceType", out->dtype, sizeof out->dtype);
    return 1;
}

/* 对 ip 的 53317 发 register：明文先试（对 https 服务器会秒关），失败再 TLS */
static int probe_host(const char *ip, Device *out)
{
    char body[600], hdr[420], resp[2048];
    int blen = 0, i;
    self_info_json(body, sizeof body);
    blen = (int)strlen(body);
    for (i = 0; i < 2; i++) {
        SConn c;
        int code = 0, r, hlen;
        memset(&c, 0, sizeof c);
        c.fd = net_connect_to(ip, SCAN_PORT, SCAN_CONNECT_US);
        if (c.fd < 0) return 0;
        c.tls = (i == 1);
        if (c.tls && s_handshake(&c, ip) != 0) {
            s_close(&c);
            continue;                        /* TLS 不通；可能不是 https */
        }
        hlen = snprintf(hdr, sizeof hdr,
                        "POST /api/localsend/v2/register HTTP/1.1\r\n"
                        "Host: %s:%d\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n",
                        ip, SCAN_PORT, blen);
        r = s_send_all(&c, hdr, hlen);
        if (r == 0) r = s_send_all(&c, body, blen);
        if (r == 0) r = s_read_resp(&c, &code, resp, (int)sizeof resp, SCAN_RESP_US);
        else r = -1;
        if (r > 0 && code == 200) {
            int ok = parse_member(resp, c.tls != 0, ip, out);
            s_close(&c);
            return ok;
        }
        s_close(&c);
    }
    return 0;
}

/* ---------- 一轮全扫 ---------- */
static void scan_round(void)
{
    const char *lip = net_local_ip();
    unsigned a = 0, b = 0, c = 0, d = 0;
    char prefix[16];
    int host, self_host = -1;
    if (!lip || sscanf(lip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) {
        dlog("scan: no usable local ip, skip round");
        return;
    }
    self_host = (int)d;
    snprintf(prefix, sizeof prefix, "%u.%u.%u.", a, b, c);
    g_done = 0;
    g_total = 254;
    dlog("scan: round start on %u.%u.%u.0/24", a, b, c);
    for (host = 1; host <= 254; host++) {
        char hip[16];
        Device dev;
        if (host == self_host) { g_done = host; continue; }
        snprintf(hip, sizeof hip, "%s%d", prefix, host);
        if (probe_host(hip, &dev)) {
            dlog("scan: found '%s' %s:%d (%s)", dev.alias, dev.ip, dev.port,
                 dev.protocol);
            discovery_upsert_peer(&dev);
        }
        g_done = host;
    }
    dlog("scan: round done (%d hosts)", g_total);
}

static int scan_thr(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        if (g_need || g_active) {
            g_need = 0;
            g_active = 1;
            scan_round();
            g_active = 0;
        }
        sceKernelDelayThread(SCAN_TICK_US);
    }
    return 0;
}

/* ---------- 公开 ---------- */
void scan_trigger(void)
{
    if (!g_up) {
        SceUID t = sceKernelCreateThread("psvsend_scan", scan_thr,
                                         0x40, 0x20000, 0, 0, NULL);
        if (t < 0) {
            dlog("scan: thread create fail 0x%08X", (unsigned)t);
            return;
        }
        if (sceKernelStartThread(t, 0, NULL) < 0) {
            dlog("scan: thread start fail");
            return;
        }
        g_thr = t;
        g_up = 1;
        dlog("scan: thread up");
    }
    g_need = 1;                     /* 空闲则本轮开始；在扫则扫完顺延一轮 */
}

bool scan_up(void)   { return g_up != 0; }
int  scan_active(void){ return g_active; }
int  scan_done(void) { return g_done; }
int  scan_total(void) { return g_total; }
