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
#include <psp2/kernel/threadmgr/mutex.h>
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
    char fp[96];             /* trust-first：本次连接 TLS 叶证书 SHA-256
                              * （每连接私有：8 路并发探测互不覆盖） */
    mbedtls_ssl_context  ssl;
    mbedtls_ssl_config   conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt     ca;
    mbedtls_x509_crt     own;
    mbedtls_pk_context   ownpk;
} SConn;

#define SCAN_WORKERS       8              /* 一轮并发探测线程数 */
#define SCAN_KNOWN_MAX     64             /* 记住的上次在线主机数（下轮优先探） */
#define SCAN_WORKER_STACK  0x14000        /* 每个 worker 栈：装得下 mbedtls 上下文 */

static SceUID    g_thr = -1;
static SceUID    g_lock = -1;   /* 保护游标/计数/已知表 */
static volatile int g_up = 0;      /* 线程已建 */
static volatile int g_active = 0;  /* 正在扫 */
static volatile int g_need = 0;    /* 空闲后顺延一轮 */
static volatile int g_done = 0;    /* 已探主机数 */
static volatile int g_found = 0;   /* 本轮发现的设备数 */
static int         g_total = 0;
static int         g_known[SCAN_KNOWN_MAX];  /* 上次在线主机号（本 /24 内） */
static int         g_known_n = 0;
static int         g_round_found[SCAN_KNOWN_MAX];
static int         g_round_found_n = 0;

static void slock(void) { if (g_lock >= 0) sceKernelLockMutex(g_lock, 1, NULL); }
static void sunlock(void) { if (g_lock >= 0) sceKernelUnlockMutex(g_lock, 1); }

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

/* trust-first：不预知对端指纹，放行任意叶子证书并把其 SHA-256 记下。
 * arg 指向本连接 SConn.fp（每连接私有，多线程并发探测互不覆盖）。 */
static int trust_verify(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    unsigned char dig[32];
    char hex[65];
    char *fp = (char *)arg;
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
    if (fp) snprintf(fp, 96, "%s", hex);
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
    {
        int cr = sceNetConnect(fd, (const SceNetSockaddr *)&sa, sizeof sa);
        /* 非阻塞 connect：立即成功（罕见）或进入在途；返回其它错误=确定失败 */
        if (cr == 0) return fd;
        if (cr != SCE_NET_ERROR_EWOULDBLOCK &&
            cr != SCE_NET_ERROR_EINPROGRESS &&
            cr != SCE_NET_ERROR_EALREADY) {
            sceNetSocketClose(fd);
            return -1;
        }
    }
    /* 在途：轮询判定完成。注意 connect 未裁决时 getpeername 返回
     * ENOTCONN（不是 EWOULDBLOCK）——老代码在这里把"在途"误判成失败
     * 秒退，导致一轮 /24 不到 100ms 就"扫完"且永远 0 台；
     * 连接建立后 getpeername 返回 0。SO_ERROR 非 0 = 内核已裁决失败
     * （refused/不可达等），读到即快速放弃。 */
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
        if (now_us() >= dl) break;          /* 预算到点，放弃 */
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
    mbedtls_ssl_conf_verify(&c->conf, trust_verify, c->fp);
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

/* 解析 register 200 响应里的对端 member info；成功返回 1 并填 out。
 * leaf_fp 为本次 TLS 连接记下的叶子证书 SHA-256（明文探测传 NULL）。 */
static int parse_member(const char *body, int tls, const char *ip, Device *out,
                        const char *leaf_fp)
{
    long long v;
    memset(out, 0, sizeof *out);
    if (!body || !body[0]) return 0;
    if (!json_get_str(body, "alias", out->alias, sizeof out->alias)) return 0;
    if (!json_get_str(body, "fingerprint", out->fingerprint,
                      sizeof out->fingerprint)) {
        if (tls && leaf_fp && leaf_fp[0])
            snprintf(out->fingerprint, sizeof out->fingerprint, "%s", leaf_fp);
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
            int ok = parse_member(resp, c.tls != 0, ip, out, c.fp);
            s_close(&c);
            return ok;
        }
        s_close(&c);
    }
    return 0;
}

/* ---------- 一轮全扫（多 worker 并发） ---------- */

/* 单 worker 探测上下文。静态全局：轮次严格串行（g_active 挡住并发轮），而
 * 被预算超时"废弃"的 worker 可能仍在跑、还会引用 ctx，故不能放栈上。
 * round 字段=本轮轮次号：worker 入口捕获，之后每次动共享统计前比对全局
 * g_round，轮次变了说明自己已被废弃，立刻收手（不再入表/计数/碰 ctx）。 */
typedef struct {
    char   prefix[16];
    int    hosts[254];    /* 候选主机号（排除本机） */
    int    n;
    int    cur;           /* 共享游标（锁保护） */
    int    round;
} ScanCtx;

static ScanCtx g_ctx;
static volatile int g_round = 0;

#define SCAN_ROUND_BUDGET_US (120 * 1000000LL)  /* 一轮硬预算：超时视为 worker 挂死 */
#define SCAN_ROUND_SLOW_US   (15 * 1000000LL)   /* 超过则周期性打慢速日志 */

static int scan_worker_thr(SceSize args, void *argp)
{
    ScanCtx *c = &g_ctx;          /* 不用 argp：StartThread(0,NULL) 传不了可靠指针 */
    int my_round = c->round;
    (void)args; (void)argp;
    dlog("scan: worker up (round %d)", my_round);
    for (;;) {
        int idx;
        Device dev;
        char hip[16];
        slock();
        if (g_round != my_round || c->cur >= c->n) { sunlock(); break; }
        idx = c->cur++;
        sunlock();
        snprintf(hip, sizeof hip, "%s%d", c->prefix, c->hosts[idx]);
        if (probe_host(hip, &dev)) {
            if (g_round != my_round) break;   /* 本轮已被废弃：不再入表/计数 */
            dlog("scan: found '%s' %s:%d (%s)", dev.alias, dev.ip, dev.port,
                 dev.protocol);
            discovery_upsert_peer(&dev);
            slock();
            if (g_round_found_n < SCAN_KNOWN_MAX)
                g_round_found[g_round_found_n++] = c->hosts[idx];
            g_found++;
            sunlock();
        }
        slock();
        g_done++;
        sunlock();
    }
    return 0;
}

static void scan_round(void)
{
    const char *lip = net_local_ip();
    unsigned a = 0, b = 0, c = 0, d = 0;
    char prefix[16];
    ScanCtx *ctx = &g_ctx;
    SceUID wt[SCAN_WORKERS];
    int alive[SCAN_WORKERS];
    int wn = 0, i;
    int my_round;
    if (!lip || sscanf(lip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255) {
        dlog("scan: no usable local ip, skip round");
        return;
    }
    if (g_lock < 0)
        g_lock = sceKernelCreateMutex("psvsend_scan", 0, 0, NULL);

    my_round = ++g_round;

    /* 候选顺序：上次在线的主机排最前（"重点先探"），其余按序补全 */
    memset(ctx, 0, sizeof *ctx);
    snprintf(prefix, sizeof prefix, "%u.%u.%u.", a, b, c);
    snprintf(ctx->prefix, sizeof ctx->prefix, "%s", prefix);
    ctx->round = my_round;
    {
        int h, k;
        for (i = 0; i < g_known_n; i++) {
            h = g_known[i];
            if (h == (int)d || h < 1 || h > 254) continue;
            ctx->hosts[ctx->n++] = h;
        }
        for (h = 1; h <= 254; h++) {
            if (h == (int)d) continue;
            for (k = 0; k < ctx->n; k++)
                if (ctx->hosts[k] == h) break;
            if (k == ctx->n) ctx->hosts[ctx->n++] = h;
        }
    }
    slock();
    g_total = 254;
    g_done = 1;                       /* 本机占 1 格，进度按 /254 走 */
    g_found = 0;
    g_round_found_n = 0;
    sunlock();
    dlog("scan: round #%d start on %s0/24 (%d candidates, %d known first)",
         my_round, prefix, ctx->n, ctx->n < g_known_n ? ctx->n : g_known_n);

    for (i = 0; i < SCAN_WORKERS && i < ctx->n; i++) {
        SceUID t = sceKernelCreateThread("psvsend_scanw", scan_worker_thr,
                                         0x40, SCAN_WORKER_STACK, 0, 0, NULL);
        if (t < 0) {
            dlog("scan: worker create fail 0x%08X", (unsigned)t);
            break;
        }
        if (sceKernelStartThread(t, 0, NULL) < 0) {
            dlog("scan: worker start fail");
            sceKernelDeleteThread(t);
            break;
        }
        alive[wn] = 1;
        wt[wn++] = t;
    }
    if (wn == 0) {                     /* 线程全没起来：退化成串行兜底 */
        dlog("scan: no worker, fallback serial");
        ctx->cur = 0;
        scan_worker_thr(0, NULL);
    } else {
        /* 等待 worker 结束：带总预算轮询，挂死的 worker 不再无限等。
         * 单 worker 单 host 的探测本身有超时预算，正常一轮几秒~几十秒；
         * 超预算说明有 worker 卡在内核调用/锁上，废弃它让 UI 恢复。 */
        SceLong64 t0 = now_us();
        int pending = wn, slow_log = 0;
        while (pending > 0) {
            if (now_us() - t0 > SCAN_ROUND_BUDGET_US) break;
            for (i = 0; i < wn; i++) {
                SceUInt to;
                if (!alive[i]) continue;
                to = 100 * 1000;              /* 每个 worker 单次最多等 100ms */
                if (sceKernelWaitThreadEnd(wt[i], NULL, &to) == 0) {
                    alive[i] = 0;
                    sceKernelDeleteThread(wt[i]);
                    pending--;
                }
            }
            {
                SceLong64 el = now_us() - t0;
                if (el > SCAN_ROUND_SLOW_US && (int)(el / 10000000) > slow_log) {
                    slow_log = (int)(el / 10000000);
                    dlog("scan: round #%d running %lld ms (pending %d, found %d)",
                         my_round, (long long)(el / 1000), pending, g_found);
                }
            }
            if (pending > 0) sceKernelDelayThread(SCAN_TICK_US);
        }
        if (pending > 0)
            dlog("scan: round #%d budget exceeded, abandon %d worker(s) (found %d)",
                 my_round, pending, g_found);
        /* 被废弃的 worker 恢复后会在下一轮（轮次号已变）到来前自己收手 */
    }

    slock();                           /* 记住本轮在线主机：下轮优先探 */
    g_known_n = g_round_found_n;
    for (i = 0; i < g_known_n; i++) g_known[i] = g_round_found[i];
    g_done = 254;
    sunlock();
    dlog("scan: round #%d done (%d hosts, %d found)", my_round, 254, g_found);
}

static int scan_thr(SceSize args, void *argp)
{
    int off_logged = 0;        /* 断网挂起只提示一次 */
    (void)args; (void)argp;
    for (;;) {
        /* 触发的轮次只在网络可用时真正执行；断网期间按三角 → 请求保持
         * g_need=1 挂起，链路恢复后本循环自动补跑，不等用户再按一次 */
        if (g_need && !g_active) {
            if (net_connected() && strcmp(net_local_ip(), "0.0.0.0") != 0) {
                g_need = 0;
                g_active = 1;
                scan_round();
                g_active = 0;
                off_logged = 0;
            } else if (!off_logged) {
                off_logged = 1;
                dlog("scan: round pending, wifi link down - wait for recovery");
            }
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
    /* 手动扫描 = 重新认识当前网络：先把旧条目清掉（含已离线的"残留"），
     * 扫到的/期间 register 回来的会立刻重新入表。给用户即时的"清空"反馈。 */
    dlog("scan: manual trigger, clear device table");
    discovery_clear();
    g_need = 1;                     /* 空闲则本轮开始；在扫则扫完顺延一轮 */
}

bool scan_up(void)   { return g_up != 0; }
int  scan_active(void){ return g_active; }
int  scan_done(void) { return g_done; }
int  scan_total(void) { return g_total; }
int  scan_found(void) { return g_found; }
