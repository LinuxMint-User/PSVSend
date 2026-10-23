/* http.c 实现 —— 见 http.h。
 * accept 线程只做 accept + 分发：每连接开一个 worker 线程并发处理（上限
 * HTTP_MAX_CONN，满员时新连接立即关闭——宁可见效地拒绝，也不让客户端在 TCP
 * 队列里干等到超时）；worker 处理完关 fd 自清，收大文件 body 是"流式边读边
 * 转给 receive 模块"，其余请求头+体都很小读进内存即可。http_stop 关监听让
 * accept 退出后，会对仍存活的 worker 做有预算的 join，等不到就换代弃管。
 * Vita 的 SceNet socket 收/发在暂无数据时会立刻返回 EWOULDBLOCK(0x80410123)，
 * 不能当错误处理，必须轮询等数据/等窗口。
 *
 * 单端口双栈：同一监听端口同时服务 TLS 与明文 HTTP。worker 先嗅探首字节
 * （0x16 = TLS handshake record）——是 TLS 就用本机身份证书现场做服务端握手，
 * 之后所有收发走 mbedTLS；否则按明文 HTTP 处理。嗅探到的首字节不能丢，
 * 存在 Conn.p 里由 TLS recv 回调/conn_read 先消费。
 * 保留明文是为了兼容旧客户端与简单调试；官方 2026 客户端走 TLS。 */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <psp2/net/net.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <sys/time.h>
#include <mbedtls/ssl.h>
#include <mbedtls/ssl_ciphersuites.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include "core/config.h"
#include "core/json_util.h"
#include "net/http.h"
#include "net/identity.h"
#include "core/dlog.h"

#define HTTP_RECV_TIMEOUT_S 3
#define HTTP_MAX_REQ        32768   /* 请求头+体总读入上限（64 文件 prepare 清单可能 ~20KB） */
#define HTTP_MAX_BODY       2048    /* register 体上限 */
#define HTTP_MAX_PREPARE    32768   /* prepare-upload 体/回执上限（清单 JSON，64 文件会话） */
#define UPLOAD_IDLE_US      (30 * 1000000LL)  /* 收体时单块读的空闲上限 */
#define NET_AGAIN(r) ((r) == 0x80410123)
#define NET_RETRY_DELAY_US 20000    /* 20ms：仅用于"长时间空闲等一个事件"的场合 */
/* 数据面/握手的轮询间隔。Vita recv 无数据立刻返回 EWOULDBLOCK，睡多久就等于
 * 每个往返/每个 socket 缓冲周期多花多久：
 *  - 握手有 4~6 个往返，20ms 一睡实测把服务端握手拖到 ~354ms，超过官方客户端
 *    discovery 的 500ms 预算 → register 被对端放弃，客户端最终降级成 http 通道；
 *  - 收体时每个周期只搬走"这 20ms 攒下的数据"（socket 缓冲就那么大），吞吐被
 *    压到 ~2MiB/s（明文与 TLS 都中招）。
 * 1ms 的开销只是少量唤醒，换来的是"有数据就立刻搬"。 */
#define NET_POLL_US         1000
#define HTTP_MAX_CONN       8       /* 并发连接 worker 上限（满员立即关新连接） */
#define HTTP_CONN_STACK     0x40000 /* 每连接 worker 栈：与旧 accept 线程同规格
                                     * （handle_conn 内 buf+prepare 体/回执 ~100KB） */

static int    g_port = 0;           /* 实际绑定端口（0=未启动） */
static int    g_lsock = -1;
static SceUID g_thr = -1;           /* accept 线程（http_stop 要等它退） */
static SceUID g_thr_zombie = -1;    /* stop 时没能等到的旧 accept 线程句柄：留着下次回收。
                                     * 线程对象在退出后仍是 DORMANT 状态，不 delete 就永久占一个
                                     * 句柄（每次断网重连泄漏一个，耗尽后 http_start 永远失败）。 */
static volatile int g_run = 0;
static volatile int g_gen = 0;      /* 服务器代次：stop/start 递增，旧线程据此收手 */
static void (*g_cb)(const char *body, const char *src_ip) = NULL;
/* 接收侧处理集（proto/receive.c 实现，app/api.c 启动时注册）：http 层不认
 * 协议语义，只按路由把请求转给它——避免 net 层反向依赖 proto 层。 */
static const HttpRecvOps *g_recv = NULL;

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

static const char *reason_of(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 409: return "Conflict";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

/* 接收侧处理集是否可用（组合层 api_start 注册前为 NULL；正常不会发生） */
static bool recv_ops_ready(void)
{
    if (g_recv && g_recv->prepare && g_recv->upload && g_recv->cancel &&
        g_recv->abort_pending)
        return true;
    dlog("http: recv ops not registered");
    return false;
}

/* 用户是否已请求中止本次上传（收体轮询用；未注册处理集时视为否） */
static bool recv_abort_q(void)
{
    return g_recv && g_recv->abort_pending && g_recv->abort_pending();
}

static void member_info_json(char *out, int outsz)
{
    char al[sizeof g_cfg.alias], fp[65];
    char ae[2 * sizeof g_cfg.alias];
    char fe[2 * 65];
    config_get_alias(al, sizeof al);         /* 锁内取快照：UI 可能正在改名 */
    /* per-device 证书指纹；生成期间 http 尚未启动，这里只做防御性回退 */
    if (identity_fingerprint(fp) != 0)
        config_get_fingerprint(fp, sizeof fp);
    json_escape(al, ae, sizeof ae);
    json_escape(fp, fe, sizeof fe);
    snprintf(out, outsz,
             "{\"alias\":\"%s\",\"version\":\"2.0\","
             "\"deviceModel\":\"PlayStation Vita\",\"deviceType\":\"mobile\","
             "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"https\","
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
            while (*q >= '0' && *q <= '9') {
                /* 溢出保护：对端可发任意长数字，累加回绕后变负会被下游当"没有此头"
                 * 或"超长 body"处理，语义不确定 → 溢出即标 -1（不可信）。 */
                if (*v > (0x7FFFFFFFFFFFFFFFLL - (*q - '0')) / 10) {
                    *v = -1;
                    return true;
                }
                *v = *v * 10 + (*q - '0');
                q++;
            }
            return true;
        }
        i = (int)(eol - buf) + 1;      /* 下一行从 \n 之后开始 */
    }
    return false;
}

/* Content-Length 安全解析（scan.c / transfer.c 的响应读取共用）。
 * 返回 >=0 长度；-1 = 无该头/溢出/超 int 范围。调用方一律 `if (cl < 0) cl = 0;`
 * 后再用上界 guard 判 `cl > cap-1-body_off`，绝不能拿负值去算索引。 */
int http_hdr_content_length(const char *buf, int he)
{
    SceLong64 v = 0;
    if (he <= 0) return -1;
    if (!hdr_value(buf, he, "content-length", &v)) return -1;
    if (v < 0 || v > 0x7FFFFFFFLL) return -1;
    return (int)v;
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

int http_hdr_chunked(const char *buf, int he)
{
    char v[64];
    if (he <= 0) return 0;
    if (!hdr_copy(buf, he, "transfer-encoding", v, (int)sizeof v)) return 0;
    return str_has_ci(v, "chunked") ? 1 : 0;
}

/* 跳过 CRLF/LF：返回下一行的起点（未到行尾就返回原位置） */
static int skip_eol(const char *s, int len, int i)
{
    while (i < len) {
        if (s[i] == '\r') { i++; continue; }
        if (s[i] == '\n') return i + 1;
        return i;
    }
    return i;
}

/* 遍历分块数据。copy=0：只校验并算长度，不动数据；copy=1：顺便把数据搬到 out。
 * 返回解出的长度；-1 = 数据还没收齐或格式畸形。 */
static int chunk_walk(char *in, int len, char *out, int copy)
{
    int p = 0, o = 0;
    while (p < len) {
        int sz = 0, digits = 0, i = p;
        while (i < len) {
            unsigned char ch = (unsigned char)in[i];
            int v;
            if (ch == ';' || ch == '\r' || ch == '\n') break;  /* 块扩展/行尾 */
            if (ch >= '0' && ch <= '9') v = ch - '0';
            else if (ch >= 'a' && ch <= 'f') v = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') v = ch - 'A' + 10;
            else return -1;                        /* 不是合法的分块数据 */
            if (sz > (1 << 20)) return -1;         /* 离谱块大小：拒绝 */
            sz = sz * 16 + v;
            digits++;
            i++;
        }
        if (!digits) return -1;
        i = skip_eol(in, len, i);
        if (sz == 0) return o;                     /* 末块 0：解出的就是全部 */
        if (i + sz > len) return -1;               /* 块数据还没收齐 */
        if (copy) memmove(out + o, in + i, (size_t)sz);
        o += sz;
        i += sz;
        p = skip_eol(in, len, i);
        if (p == len) return -1;                   /* 块尾 CRLF 未到齐：等更多数据 */
    }
    return -1;
}

int http_chunked_decode(char *body, int len)
{
    /* 两遍走：先只扫描。调用方会边收边反复调用本函数，若第一遍就逐个块就地搬运，
     * 未收齐时缓冲已被动过，下次重解会把搬过的数据当成新的块大小（分块长度是
     * 十六进制，"hello" 这种内容也能被当成合法数字）→ 解出垃圾。 */
    if (chunk_walk(body, len, NULL, 0) < 0) return -1;
    return chunk_walk(body, len, body, 1);         /* 确认收齐了再原地搬运 */
}

/* ---------- 连接读写抽象 ----------
 * 连接现在可能是明文 fd，也可能是 TLS（服务端握手）。收发统一经
 * conn_read/conn_write，由 cn->tls 分派，调用点不必关心底下是哪种。 */
typedef struct {
    int fd;
    int gen;        /* 所属服务器代次：换代后轮询尽早收手（见 gen_stale） */
    int tls;        /* 1=TLS 握手完成，收发走 mbedTLS */
    int tls_up;     /* mbedTLS 上下文已初始化（收尾要释放，含握手失败） */
    unsigned char p[16];   /* 嗅探阶段预读的字节（TLS recv 回调先消费） */
    int p_n, p_pos;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  ent;
    mbedtls_ctr_drbg_context drbg;
    char                     peer_fp[65];  /* 对端叶证书 SHA-256（握手回调里填，
                                            * 不在握手关键路径上打日志——同步写
                                            * ux0: 一次要几十毫秒，会把官方客户端
                                            * register 的 500ms 预算吃掉） */
    mbedtls_x509_crt         ca;    /* 占位空 CA 表：mbedTLS 要求 ca_chain 非空，
                                     * 客户端证书信任判定交给 srv_verify（trust-first） */
} Conn;

/* 换代感知：http_stop/restart 后旧 worker 的轮询（头/体读取）应尽快收手，
 * 让 http_stop 的有预算 join 能等到它，而不是干耗到 recv 超时。 */
static inline int gen_stale(int my_gen) { return g_gen != my_gen; }

/* 等用户决定期间探活（见 http.h）。MSG_PEEK 只"看"不取，DONTWAIT 不阻塞；
 * Vita 的非阻塞失败以 0x80410123（EAGAIN）返回，与 discovery.c 的 D_AGAIN 同口径。 */
int http_peer_gone(const HttpPeer *peer)
{
    const Conn *c = (const Conn *)peer;
    unsigned char b = 0;
    int r;
    if (!c || c->fd < 0) return -1;
    r = sceNetRecv(c->fd, &b, 1, SCE_NET_MSG_PEEK | SCE_NET_MSG_DONTWAIT);
    if (r == 0) return 1;                         /* FIN：对端已关 */
    if (r < 0) {
        if ((unsigned)r == (unsigned)SCE_NET_ERROR_EAGAIN)
            return 0;                             /* 没数据，仍连着 */
        if ((unsigned)r == (unsigned)SCE_NET_ERROR_ECONNRESET ||
            (unsigned)r == (unsigned)SCE_NET_ERROR_ENOTCONN)
            return 1;                             /* 已断 */
        return -1;                                /* 认不出的错误：不下结论 */
    }
    /* 有字节待读：这段等待里对端不该再发应用数据，正常的只有 TLS alert
     * （close_notify 首字节 0x15）——当已断；其它情况保守地认为还连着。 */
    if (c->tls && b == 0x15) return 1;
    return 0;
}

/* ---------- 服务端 TLS ---------- */

/* Vita 熵源：时间 + 栈/全局地址 + 计数器混洗（splitmix64，与客户端同一口径）。 */
static unsigned long g_srv_seed;
static int srv_entropy_poll(void *arg, unsigned char *out, size_t len,
                            size_t *olen)
{
    uint64_t x = (uint64_t)sceKernelGetSystemTimeWide()
               ^ ((uint64_t)(uintptr_t)&g_srv_seed << 17)
               ^ ((uint64_t)(g_srv_seed++) * 0x9E3779B97F4A7C15ull);
    size_t i;
    (void)arg;
    for (i = 0; i < len; i += 8) {
        uint64_t z = x + 0x9E3779B97F4A7C15ull;
        int k;
        x = z;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        for (k = 0; k < 8 && i + k < len; k++)
            out[i + k] = (unsigned char)(z >> (8 * k));
    }
    *olen = len;
    return 0;
}

/* 客户端证书校验回调（服务端 VERIFY_OPTIONAL）：现在不做信任判定
 * （trust-first 与明文时代一致），只把叶证书指纹记进 Conn->peer_fp 备查——
 * 将来接入白名单/验证码时，判定点就落在这里。
 * 注意：这里**不能**打 dlog——dlog 是同步写 ux0:，一次几十毫秒，正好插在
 * 握手中间，会直接把官方客户端 register 的 500ms 预算吃掉。 */
static int srv_verify(void *arg, mbedtls_x509_crt *crt, int depth,
                      uint32_t *flags)
{
    Conn *c = (Conn *)arg;
    if (depth == 0 && crt && crt->raw.p && crt->raw.len && c) {
        unsigned char dig[32];
        int i;
        if (mbedtls_sha256(crt->raw.p, crt->raw.len, dig, 0) == 0) {
            for (i = 0; i < 32; i++) {
                c->peer_fp[i * 2]     = "0123456789ABCDEF"[dig[i] >> 4];
                c->peer_fp[i * 2 + 1] = "0123456789ABCDEF"[dig[i] & 0xF];
            }
            c->peer_fp[64] = 0;
        }
    }
    *flags = 0;               /* 自签/无 CA 链，忽略其它校验位 */
    return 0;
}

/* mbedTLS <-> Vita socket（服务端方向）。先消费嗅探期预读的字节。 */
static int srv_tls_send(void *ctx, const unsigned char *buf, size_t len)
{
    Conn *c = (Conn *)ctx;
    int r = sceNetSend(c->fd, buf, (unsigned)len, 0);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (NET_AGAIN(r)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    /* 真错误原样打进日志：mbedTLS 拿到这里统一报成 INTERNAL_ERROR(-0x6C00)，
     * 否则日志只剩 -0x6C00，分不清对端 RST(0x80410136)、连接已断(ENOTCONN)
     * 还是别的。与 srv_tls_recv 对齐。 */
    dlog("http: tls bio send 0x%08X", (unsigned)r);
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

static int srv_tls_recv(void *ctx, unsigned char *buf, size_t len)
{
    Conn *c = (Conn *)ctx;
    int r;
    if (c->p_pos < c->p_n) {                  /* 嗅探时读走的那个字节 */
        int k = c->p_n - c->p_pos;
        if ((size_t)k > len) k = (int)len;
        memcpy(buf, c->p + c->p_pos, (size_t)k);
        c->p_pos += k;
        return k;
    }
    r = sceNetRecv(c->fd, buf, (unsigned)len, 0);
    if (r > 0) return r;
    if (r == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (NET_AGAIN(r)) return MBEDTLS_ERR_SSL_WANT_READ;
    /* 对端 RST 等真错误：原样打进日志（否则 mbedTLS 会把它统一报成
     * INTERNAL_ERROR，日志里只剩 -0x6C00，看不出到底是谁断的）。
     * 之后按"连接已结束"返回 CONN_EOF，让上层走正常收尾而不是当成协议错误。 */
    dlog("http: tls bio recv 0x%08X", (unsigned)r);
    return MBEDTLS_ERR_SSL_CONN_EOF;
}

/* 本机身份证书/私钥：进程内只解析一次。
 * 旧实现每次握手都 identity_cert_parse + identity_key_parse（两次 ux0: 读盘 +
 * PEM/DER 解析），在 333MHz 的 Vita 上是几十到上百毫秒，正好撞在官方客户端
 * register 的 500ms 预算里。解析结果常驻即可：mbedtls_ssl_conf_own_cert 只保存
 * 指针，握手期只读（mbedtls_pk_sign 形参是 const），多连接并发共用安全。 */
static mbedtls_x509_crt   g_own;
static mbedtls_pk_context g_ownpk;
static int    g_own_ready = 0;
static SceUID g_own_mtx = -1;

static int own_identity(void)
{
    int rc = 0;
    if (g_own_ready) return 0;
    /* 锁必须在 http_start 里先建好（连接线程只会在那之后才存在）；此处若还
     * 去"惰性创建"，两条连接同时首进入会各建一把锁、各锁各的，等于没有锁，
     * 进而并发对 g_own/g_ownpk 做 init/parse。锁没建好就当场失败，让握手重试。 */
    if (g_own_mtx < 0) return -1;
    sceKernelLockMutex(g_own_mtx, 1, NULL);
    if (!g_own_ready) {
        mbedtls_x509_crt_init(&g_own);
        mbedtls_pk_init(&g_ownpk);
        if (identity_cert_parse(&g_own) != 0 ||
            identity_key_parse(&g_ownpk) != 0) {
            /* 身份可能还在后台生成：清掉半成品，下次握手再试 */
            mbedtls_x509_crt_free(&g_own);
            mbedtls_pk_free(&g_ownpk);
            rc = -1;
        } else {
            g_own_ready = 1;
        }
    }
    sceKernelUnlockMutex(g_own_mtx, 1);
    return rc;
}

/* 服务端 TLS 握手（EWOULDBLOCK 轮询 + 总截止）。0=成功 <0=失败。
 * 身份证书取 per-device identity（首启生成期间 http 尚未启动）。 */
static int srv_tls_handshake(Conn *c)
{
    SceLong64 dl = sceKernelGetSystemTimeWide() + 5 * 1000000LL;
    int r;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->ent);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_x509_crt_init(&c->ca);
    c->tls_up = 1;

    if (mbedtls_entropy_add_source(&c->ent, srv_entropy_poll, NULL, 48,
                                   MBEDTLS_ENTROPY_SOURCE_STRONG) != 0)
        return -1;
    if (mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func, &c->ent,
                              (const unsigned char *)"psvsend-http", 12) != 0) {
        dlog("http: drbg seed fail");
        return -1;
    }
    r = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_SERVER,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) {
        dlog("http: ssl defaults err -0x%04X", (unsigned)(-r));
        return -1;
    }
    /* 与客户端同一口径锁 TLS1.2：3.x 的 TLS1.3 证书校验不走 per-cert 回调，
     * 1.2 的语义明确；LocalSend 客户端也支持 1.2。 */
    mbedtls_ssl_conf_min_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&c->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                 MBEDTLS_SSL_MINOR_VERSION_3);
    /* 数据面优先 ChaCha20-Poly1305。Vita 的 Cortex-A9 没有 AES/SHA 加速指令，
     * mbedTLS 的 AES-GCM 与 SHA-384 全靠查表软件实现（实测收体只有 ~1MiB/s）；
     * ChaCha20 只有加法/异或/移位，同频下快得多。官方客户端（rustls）两种都
     * 支持；万一对端只给 AES 组，这里保留了 AES-GCM 兜底，不会谈崩。
     * 顺序由服务端决定（mbedTLS 默认按自己列表取交集）。 */
    {
        static const int suites[] = {
            MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
            MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
            0
        };
        mbedtls_ssl_conf_ciphersuites(&c->conf, suites);
    }
    /* 请求客户端证书但不强制：官方客户端会出示（mTLS），旧客户端没有也能连。
     * 信任判定（白名单/验证码）以后加在 srv_verify 里。 */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_verify(&c->conf, srv_verify, c);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);

    if (own_identity() != 0) {
        dlog("http: identity cert/key parse fail");
        return -1;
    }
    r = mbedtls_ssl_conf_own_cert(&c->conf, &g_own, &g_ownpk);
    if (r != 0) {
        dlog("http: own_cert err -0x%04X", (unsigned)(-r));
        return -1;
    }
    if (mbedtls_ssl_setup(&c->ssl, &c->conf) != 0) {
        dlog("http: ssl setup fail");
        return -1;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, srv_tls_send, srv_tls_recv, NULL);

    for (;;) {
        if (gen_stale(c->gen)) return -1;
        if (sceKernelGetSystemTimeWide() >= dl) {
            dlog("http: tls handshake timeout");
            return -1;
        }
        r = mbedtls_ssl_handshake(&c->ssl);
        if (r == 0) return 0;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            sceKernelDelayThread(NET_POLL_US);   /* 握手每个往返都要快，见 NET_POLL_US */
            continue;
        }
        dlog("http: tls handshake err -0x%04X", (unsigned)(-r));
        return -1;
    }
}

/* 嗅探首字节：返回 1=TLS、0=明文、<0=无数据/出错。读到的字节留在 Conn.p，
 * 由后续读取先消费（丢掉它 TLS 握手会缺第一字节）。 */
static int conn_detect(Conn *c)
{
    SceLong64 dl = sceKernelGetSystemTimeWide()
                 + (SceLong64)HTTP_RECV_TIMEOUT_S * 1000000LL;
    for (;;) {
        int r;
        if (gen_stale(c->gen)) return -1;
        r = sceNetRecv(c->fd, c->p, 1, 0);
        if (r == 1) {
            c->p_n = 1;
            c->p_pos = 0;
            return c->p[0] == 0x16;           /* TLS handshake record */
        }
        if (r == 0) return -1;                 /* 对端连上就关 */
        if (!NET_AGAIN(r)) {
            dlog("http: sniff err 0x%08X", (unsigned)r);
            return -1;
        }
        if (sceKernelGetSystemTimeWide() >= dl) return -1;
        sceKernelDelayThread(NET_POLL_US);
    }
}

/* 关连接：尽量发 close_notify，释放 mbedTLS 上下文，最后关 fd。
 * 失败路径上（握手没完成）也安全：init/free 成对即可。 */
static void conn_close(Conn *c)
{
    if (c->tls_up) {
        if (c->tls) {
            int i;
            for (i = 0; i < 10; i++) {
                int r = mbedtls_ssl_close_notify(&c->ssl);
                if (r == 0 ||
                    (r != MBEDTLS_ERR_SSL_WANT_READ &&
                     r != MBEDTLS_ERR_SSL_WANT_WRITE))
                    break;
                sceKernelDelayThread(NET_POLL_US);
            }
        }
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
        mbedtls_ctr_drbg_free(&c->drbg);
        mbedtls_entropy_free(&c->ent);
        c->tls_up = 0;
    }
    if (c->fd >= 0) sceNetSocketClose(c->fd);
    c->fd = -1;
}

/* ---------- upload 大文件的流式读回调（receive 模块轮询调用） ---------- */
typedef struct {
    Conn *conn;         /* 连接：读经 conn_read（换代/中止/TLS 由它统一感知） */
    const char *left;   /* 缓冲里已读但未消费的 body */
    int  llen;
    int  chunked;       /* Transfer-Encoding: chunked（dio 无 CL 流式上传） */
    int  ph;            /* chunked: 0=chunk 大小行 1=chunk 数据 2=终块 trailer */
    int  n_tr;          /* chunked: 终块后已吃的 trailer 行数（上限保护，见下） */
    SceLong64 c_rem;    /* chunked: 当前 chunk 剩余数据字节 */
    unsigned char sbuf[16384]; /* chunked 解码输入暂存：一次尽量吃满一个 TLS 记录
                                * （16KB），比 4KB 少四分之一的读取/解密往返 */
    int  s_n, s_pos;
} UploadCtx;

/* ---------- 连接读写与响应工具 ---------- */

/* 全发 len 字节：20ms 轮询重试窗口，2s 内发不完即失败。0=成功 -1=失败 */
static int conn_write(Conn *c, const char *data, int len)
{
    int off = 0;
    SceLong64 dl = sceKernelGetSystemTimeWide() + 2000000LL; /* 2s */
    while (off < len) {
        int r;
        if (c->tls) {
            r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + off,
                                  (size_t)(len - off));
            if (r > 0) { off += r; continue; }
            if (r == MBEDTLS_ERR_SSL_WANT_READ ||
                r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (sceKernelGetSystemTimeWide() >= dl) return -1;
                sceKernelDelayThread(NET_POLL_US);
                continue;
            }
            return -1;
        }
        r = sceNetSend(c->fd, data + off, (unsigned)(len - off), 0);
        if (r > 0) { off += r; continue; }
        if (r == 0 || sceKernelGetSystemTimeWide() >= dl) return -1;
        if (!NET_AGAIN(r)) return -1;
        sceKernelDelayThread(NET_POLL_US);   /* 发窗口没空，等会儿再试 */
    }
    return 0;
}

/* 读最多 max 字节：Vita recv 无数据立刻返回 EWOULDBLOCK，故 20ms 轮询到
 * deadline（绝对时间，由调用者定"总截止"还是"单次空闲上限"）。
 * 返回 >0=实读字节数 0=对端关闭 -1=出错/换代 -2=空闲超时
 * flags：CONN_F_ABORTABLE → 轮询里查"用户已请求中止"（收大文件体专用） */
#define CONN_F_ABORTABLE 1u
static int conn_read(Conn *c, void *buf, int max, SceLong64 deadline,
                     unsigned flags)
{
    int spins = 0;      /* 连续"暂无数据"次数：真机实测每个 TLS 记录后面常紧跟
                         * 一次 EWOULDBLOCK，若每次都睡 1ms 就等于给每个 16KB
                         * 记录摊 1ms（148MB 收体多花约 9s）。先让出 CPU 空转几
                         * 轮（数据通常几微秒内到），仍没有才退回 1ms 长睡。 */
    for (;;) {
        int r;
        if (gen_stale(c->gen)) return -1;                   /* 换代：收手 */
        if ((flags & CONN_F_ABORTABLE) && recv_abort_q()) return -1;
        if (c->p_pos < c->p_n) {                            /* 嗅探时预读的字节 */
            int k = c->p_n - c->p_pos;
            if (k > max) k = max;
            memcpy(buf, c->p + c->p_pos, (size_t)k);
            c->p_pos += k;
            return k;
        }
        if (c->tls) {
            r = mbedtls_ssl_read(&c->ssl, buf, (size_t)max);
            if (r > 0) return r;
            if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
            if (r == MBEDTLS_ERR_SSL_WANT_READ ||
                r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                if (sceKernelGetSystemTimeWide() >= deadline) return -2;
                if (++spins <= 8) sceKernelDelayThread(0);
                else sceKernelDelayThread(NET_POLL_US);
                continue;
            }
            if (r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
            dlog("http: tls read err -0x%04X", (unsigned)(-r));
            return -1;
        }
        r = sceNetRecv(c->fd, buf, (unsigned)max, 0);
        if (r > 0) return r;
        if (r == 0) return 0;
        if (!NET_AGAIN(r)) { dlog("http: recv err 0x%08X", (unsigned)r); return -1; }
        if (sceKernelGetSystemTimeWide() >= deadline) return -2;
        if (++spins <= 8) sceKernelDelayThread(0);
        else sceKernelDelayThread(NET_POLL_US);
    }
}

static void http_respond(Conn *c, int code, const char *reason,
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
    conn_write(c, hdr, (int)strlen(hdr));
    conn_write(c, body, blen);
}

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
    int r;
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
    r = conn_read(u->conn, u->sbuf, (int)sizeof u->sbuf,
                  sceKernelGetSystemTimeWide() + UPLOAD_IDLE_US,
                  CONN_F_ABORTABLE);
    if (r > 0) { u->s_n = r; return 1; }
    return r;            /* 0=对端关闭 -1=错/换代/中止 -2=空闲超时 */
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
            if (n >= cap - 1) return -1;   /* 超长行：畸形，别无限吃数据占着 worker 槽 */
            if (ch != '\r') line[n++] = ch;
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
                while (*p && (h = hexv(*p)) >= 0) {
                    /* 位数上限：INT64_MAX/16 = 0x07FFFFFFFFFFFFFF，再乘 16 加 h 也
                     * 不会越过 INT64_MAX。对端可送 16 个 F，裸累加会回绕成负值，
                     * 之后 `c_rem` 恒负、`on` 被减成负数，状态机卡死且返回值不可信。 */
                    if (sz > 0x07FFFFFFFFFFFFFFLL) { sz = -1; break; }
                    sz = sz * 16 + h;
                    p++;
                }
            }
            if (sz == 0) { u->ph = 3; continue; } /* 终止块 */
            if (sz < 0) return -1;               /* 溢出/畸形：整条流作废 */
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
            /* 不在这里返回：对端每个 chunk 往往只有几 KB~16KB，逐块返回会让
             * receive 每收一小块就 sceIoWrite 一次，ux0: 的小块写吞吐只有
             * ~2MiB/s（明文 4MiB/s 腰斩的主因）。继续拼下一个 chunk，直到
             * out 攒满（调用者给 64KB）再返回，写盘次数降到 1/4~1/16。 */
            if (on >= max) return on;
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
            if (++u->n_tr > 32) return -1;       /* 条数上限：否则对端能一直送行占着 worker */
        }
    }
}

/* 通用上传读回调：chunked 走解码，否则直读原始字节 */
static int upload_stream(void *ctx, unsigned char *buf, int max)
{
    UploadCtx *u = ctx;
    int r;
    if (u->chunked) return chunked_stream(u, buf, max);
    if (u->llen > 0) {                   /* 先消费缓冲里的残留 body */
        int k = u->llen > max ? max : u->llen;
        memcpy(buf, u->left, (unsigned)k);
        u->left += k;
        u->llen -= k;
        return k;
    }
    r = conn_read(u->conn, buf, max,
                  sceKernelGetSystemTimeWide() + UPLOAD_IDLE_US,
                  CONN_F_ABORTABLE);
    if (r > 0) return r;
    if (r == 0) return 0;                                    /* 对端关闭 */
    if (r == -2) { dlog("http: upload idle timeout"); return 0; }
    return -1;    /* 错/换代/用户中止：让 receive 收尾为取消 */
}

/* ---------- 路由处理 ----------
 * 请求行/头/body 由 handle_conn 统一解析成 HttpReq，再按路由分派到下面的
 * handler；每个 handler 只负责自己那条路由的语义与应答。纯搬移，行为不变。 */
typedef struct {
    Conn       *cn;         /* 连接（响应经它发） */
    const char *ip;         /* 来源 IP（日志/接收侧校验用） */
    char        method[8];
    char        route[256];
    const char *query;      /* URL query（无则空串） */
    const char *buf;        /* 请求原始缓冲（诊断打点用） */
    int         he;         /* header 结束偏移（空行起点） */
    const char *left;       /* body 起始（buf + he + 4） */
    int         llen;       /* 已随头读入的 body 长度 */
    SceLong64   cl64;       /* Content-Length（无则 0） */
} HttpReq;

/* GET /api/localsend/v2/info —— 回本机 member info */
static void h_info(HttpReq *r)
{
    char out[HTTP_MAX_BODY];
    member_info_json(out, sizeof out);
    http_respond(r->cn, 200, "OK", out);
}

/* POST /api/localsend/v2/register —— 对方注册/公告，回本机 member info */
static void h_register(HttpReq *r)
{
    char body[HTTP_MAX_BODY];
    int got = r->llen;                   /* body 已随头读齐（见 handle_conn 补读） */
    if (got > (int)sizeof body - 1) got = (int)sizeof body - 1;
    memcpy(body, r->left, (unsigned)got);
    body[got] = 0;
    if (got > 0) {
        dlog("http: register body %d bytes from %s", got, r->ip);
        if (g_cb) g_cb(body, r->ip);
    } else {
        dlog("http: register with empty body from %s", r->ip);
    }
    {
        char out[HTTP_MAX_BODY];
        member_info_json(out, sizeof out);
        http_respond(r->cn, 200, "OK", out);
    }
}

/* POST /api/localsend/v2/prepare-upload —— 清单交给接收模块挂起等 UI 决定 */
static void h_prepare(HttpReq *r)
{
    char body[HTTP_MAX_PREPARE];
    char resp[HTTP_MAX_PREPARE];
    int got = r->llen;                   /* body 已随头读齐 */
    if (got > (int)sizeof body - 1) got = (int)sizeof body - 1;
    memcpy(body, r->left, (unsigned)got);
    body[got] = 0;
    if (r->cl64 <= 0) {                  /* 诊断：无 CL 客户端长啥样 */
        char dbg[256];
        int a, b = 0;
        int hl = r->he < 200 ? r->he : 200;
        for (a = 0; a < hl && b < 250; a++) {
            char ch = r->buf[a];
            if (ch == '\r' || ch == '\n') ch = '|';
            dbg[b++] = ch;
        }
        dbg[b] = 0;
        dlog("http: prepare no-cl hdr: %s", dbg);
    }
    if (r->cl64 > 0 && (SceLong64)got < r->cl64) {  /* 声称有体但没读齐 → 400 */
        dlog("http: prepare body short cl=%lld got=%d from %s",
             (long long)r->cl64, got, r->ip);
        http_respond(r->cn, 400, "Bad Request", "");
        return;
    }
    if (!recv_ops_ready()) {
        http_respond(r->cn, 500, "Internal Server Error", "");
        return;
    }
    {
        int code = g_recv->prepare(body, r->ip, r->cn->peer_fp,
                                   (const HttpPeer *)r->cn, resp, (int)sizeof resp);
        dlog("http: prepare -> %d (tls=%d)", code, r->cn->tls);
        http_respond(r->cn, code, reason_of(code), resp);
    }
}

/* POST /api/localsend/v2/upload —— 裸文件字节流，经流回调边收边交给接收模块 */
static void h_upload(HttpReq *r)
{
    UploadCtx uctx;
    int code;
    {                                    /* 诊断：看 PC 发的 upload 头（有无 CL/TE） */
        char dbg[512];
        int a, b = 0;
        int hl = r->he < 500 ? r->he : 500;
        for (a = 0; a < hl && b < 505; a++) {
            char ch = r->buf[a];
            if (ch == '\r' || ch == '\n') ch = '|';
            dbg[b++] = ch;
        }
        dbg[b] = 0;
        dlog("http: upload hdr: %s |cl=%lld", dbg, (long long)r->cl64);
    }
    memset(&uctx, 0, sizeof uctx);       /* 含 chunked 解码状态 */
    uctx.conn = r->cn;                   /* 连接句柄（TLS/代次由 conn_read 统一感知） */
    uctx.left = r->left;
    uctx.llen = r->llen;
    if (r->cl64 <= 0) {                  /* 无 CL：dio 流式上传 → chunked */
        char te[80] = "";
        hdr_copy(r->buf, r->he, "transfer-encoding", te, sizeof te);
        if (str_has_ci(te, "chunked")) uctx.chunked = 1;
    }
    if (!recv_ops_ready()) {
        http_respond(r->cn, 500, "Internal Server Error", "");
        return;
    }
    if (uctx.chunked) {
        dlog("http: upload chunked (no content-length) tls=%d", r->cn->tls);
        code = g_recv->upload(r->query, r->ip, -1, upload_stream, &uctx);
    } else {
        dlog("http: upload bytes cl=%lld tls=%d", (long long)r->cl64, r->cn->tls);
        code = g_recv->upload(r->query, r->ip, r->cl64, upload_stream, &uctx);
    }
    dlog("http: upload -> %d", code);
    http_respond(r->cn, code, reason_of(code), "");
}

/* POST /api/localsend/v2/cancel —— 发送方放弃会话 */
static void h_cancel(HttpReq *r)
{
    int code = recv_ops_ready() ? g_recv->cancel(r->query, r->ip) : 500;
    http_respond(r->cn, code, reason_of(code), "");
}

/* 解析 HTTP 请求并应答一个连接（在独立 worker 线程跑），返回后由 worker 关 fd */
static void handle_conn(Conn *cn, const char *rip, int my_gen)
{
    char buf[HTTP_MAX_REQ];
    int n = 0, he = -1, i;
    SceLong64 cl64 = -1;            /* Content-Length 原值（Vita long 仅 32 位，大文件要 64 位） */
    char method[8] = "";
    char path[768] = "";
    char route[256];
    const char *query = NULL;
    const char *left;
    int llen;

    /* 本连接 fd 由 spawn_conn 置为非阻塞（与全仓 scan/update/net 同一口径）：
     * recv 无数据立刻返回 EWOULDBLOCK，由 conn_read 20ms 轮询到各自 deadline。
     * 旧实现在这里设 SO_RCVTIMEO=3s 而 fd 是阻塞的——内核超时返回的错误码
     * 未必是 EWOULDBLOCK，conn_read 会把它当"真错误"结束会话，于是"收体空闲
     * 上限 30s"名存实亡（约 3s 就可能被切断）；取消/换代的感知也被拖到 3s。
     * 删掉它，非阻塞 + 自管 deadline 是唯一的时间语义。 */
    {
        /* 收请求头直到出现空行（头结束）：总截止 3s，认到即停。 */
        SceLong64 dl = sceKernelGetSystemTimeWide()
                     + (SceLong64)HTTP_RECV_TIMEOUT_S * 1000000LL;
        while (n < HTTP_MAX_REQ - 1) {
            int r = conn_read(cn, buf + n, HTTP_MAX_REQ - 1 - n, dl, 0);
            if (r > 0) {
                n += r;
                buf[n] = 0;
                for (i = 0; i + 4 <= n && he < 0; i++)
                    if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                        buf[i + 2] == '\r' && buf[i + 3] == '\n') he = i;
                if (he >= 0) break;
                continue;
            }
            if (r == 0 || r == -2)
                dlog("http: recv stop r=%d n=%d from %s", r, n, rip);
            else if (gen_stale(my_gen))
                dlog("http: req loop sees gen change from %s", rip);
            else
                dlog("http: recv err n=%d from %s", n, rip);
            break;
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
        /* 已收过数据后只再等 ~400ms 空窗（判定 body 完）；未收过则等到
         * 总截止（3s），但"无 CL 且还没收到任何体"时不空等，直接走。 */
        SceLong64 dl = sceKernelGetSystemTimeWide()
                     + (SceLong64)HTTP_RECV_TIMEOUT_S * 1000000LL;
        int want = cl64 > 0 ? (int)(he + 4 + cl64) : HTTP_MAX_REQ - 1;
        int had = 0;
        if (want > HTTP_MAX_REQ - 1) {
            /* 声明的 body 超出缓冲上限：正常客户端在非 upload 路由上不会发这么大
             * 的请求，出现即说明对端异常（或 Content-Length 被读错）。截断后
             * handler 多半报 400，这里点明真因，省得对着"400 bad request"倒查。 */
            dlog("http: %s req body %lld > cap %d, truncated",
                 route, (long long)cl64, HTTP_MAX_REQ - 1);
            want = HTTP_MAX_REQ - 1;
        }
        while (n < want) {
            SceLong64 lim;
            int r;
            if (!had && cl64 <= 0) break;      /* 明确无体（无 CL）不空等 */
            lim = had ? sceKernelGetSystemTimeWide() + 400000LL : dl;
            r = conn_read(cn, buf + n, want - n, lim, 0);
            if (r > 0) { n += r; buf[n] = 0; had = 1; continue; }
            break;                             /* 关闭/空窗超时/换代：补读结束 */
        }
    }

    left = buf + he + 4;
    llen = n - (he + 4);
    if (llen < 0) llen = 0;

    if (method[0] && route[0])
        dlog("http: %s %s from %s", method, route, rip);
    else
        dlog("http: unparsed req from %s", rip);

    /* 按路由分派：每个 handler 只管自己那条路由的语义与应答 */
    {
        HttpReq r;
        memset(&r, 0, sizeof r);
        r.cn = cn;
        r.ip = rip;
        memcpy(r.method, method, sizeof r.method);
        memcpy(r.route, route, sizeof r.route);
        r.query = query;
        r.buf = buf;
        r.he = he;
        r.left = left;
        r.llen = llen;
        r.cl64 = cl64;

        if (strcmp(method, "GET") == 0 &&
            strcmp(route, "/api/localsend/v2/info") == 0)
            h_info(&r);
        else if (strcmp(method, "POST") == 0 &&
                 strcmp(route, "/api/localsend/v2/register") == 0)
            h_register(&r);
        else if (strcmp(method, "POST") == 0 &&
                 strcmp(route, "/api/localsend/v2/prepare-upload") == 0)
            h_prepare(&r);
        else if (strcmp(method, "POST") == 0 &&
                 strcmp(route, "/api/localsend/v2/upload") == 0)
            h_upload(&r);
        else if (strcmp(method, "POST") == 0 &&
                 strcmp(route, "/api/localsend/v2/cancel") == 0)
            h_cancel(&r);
        else
            http_respond(cn, 404, "Not Found", "{\"error\":\"not found\"}");
    }
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
 *    尽早收手；worker 收尾时照常关自己的 fd——该 fd 在它存活期间一直由它
 *    持有，内核不会把仍打开的号分给新监听/新连接，故关闭不会误伤别人；
 *    不关才是每次断网重连泄漏一个 socket fd（join 只有 500ms 预算，等不到
 *    是常态）；
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
    {
        Conn cn;                     /* 本连接的读写句柄（明文 fd 或 TLS 上下文） */
        int ok = 0;
        int t;
        SceLong64 t_hs;              /* 握手起点：官方客户端 register 总预算只有 500ms，
                                      * 这行耗时是发现成败的直接指标 */
        memset(&cn, 0, sizeof cn);
        cn.fd = c;
        cn.gen = my_gen;
        dlog("http: worker %d serves conn from %s (gen %d)",
             (int)me, ip, my_gen);
        /* 单端口双栈：首字节 0x16=TLS。嗅探失败（对端连上不发数据）就直接
         * 关；TLS 握手失败也不进去——流已经是乱的了。 */
        t_hs = sceKernelGetSystemTimeWide();
        t = conn_detect(&cn);
        if (t < 0) {
            dlog("http: conn from %s no request byte", ip);
        } else if (t == 0) {
            ok = 1;
        } else if (srv_tls_handshake(&cn) != 0) {
            dlog("http: tls handshake failed from %s", ip);
        } else {
            const char *cs;
            cn.tls = 1;
            cs = mbedtls_ssl_get_ciphersuite(&cn.ssl);
            /* 协商结果 + 对端叶证书指纹 + 握手耗时（基线：预算 500ms，
             * 握手超过 ~350ms 就会把 register 挤掉，客户端随即降级/放弃） */
            dlog("http: tls up from %s in %d ms (%s) peer=%s", ip,
                 (int)((sceKernelGetSystemTimeWide() - t_hs) / 1000),
                 cs ? cs : "?",
                 cn.peer_fp[0] ? cn.peer_fp : "-");
            ok = 1;
        }
        if (ok) handle_conn(&cn, ip, my_gen);
        conn_close(&cn);             /* close_notify + 释放上下文 + 关 fd */
    }

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
    dlog("http: worker %d done", (int)me);
    sceKernelExitDeleteThread(0);
    return 0;
}

/* 登记并启动一个连接 worker；返回 1=已开，0=满员/失败（调用者应关 fd） */
static int spawn_conn(int c, const char *ip)
{
    int i, one = 1;
    SceUID t;

    /* 本连接的 fd 一律置非阻塞：conn_read/conn_write 的时间语义全靠
     * "EWOULDBLOCK + 20ms 轮询 + 调用方 deadline"（与 scan/update/net 一致），
     * 阻塞 fd 会让 recv 在内核里睡到超时才返回（见 handle_conn 头注）。
     * 置位失败就不接手这条连接：让调用方关掉它（返回 0 = 未开）。 */
    if (sceNetSetsockopt(c, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one,
                         sizeof one) != 0) {
        dlog("http: conn from %s set NBIO fail", ip);
        return 0;
    }

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

    /* 允许跑在所有用户核上：TLS 的 RSA/ECDHE 与收体解密都是纯 CPU 活，
     * 钉在一个核上会和 UI 渲染互抢（真机表现为握手 400ms+、吞吐腰斩）。 */
    t = sceKernelCreateThread("psvsend_httpc", conn_worker, 0x40,
                              HTTP_CONN_STACK, 0, SCE_KERNEL_CPU_MASK_USER_ALL,
                              NULL);
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
                /* 2ms：官方客户端 register 的总预算只有 500ms
                 * （DEFAULT_DISCOVERY_TIMEOUT），这里每多睡 50ms 就直接从
                 * 握手预算里扣。空转唤醒的开销可以忽略。 */
                sceKernelDelayThread(2000);
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

/* 回收上一次 stop 没能等到的 accept 线程对象（见 g_thr_zombie 注释）。
 * 线程已退出才删得掉；还在跑就留着句柄，下次启动再来一次。 */
static void http_reap_zombie(void)
{
    SceUInt to = 0;
    if (g_thr_zombie < 0) return;
    if (sceKernelWaitThreadEnd(g_thr_zombie, NULL, &to) == 0) {
        dlog("http: stale accept thread %d reclaimed", (int)g_thr_zombie);
        sceKernelDeleteThread(g_thr_zombie);
        g_thr_zombie = -1;
    }
}

/* ---------- 启动 ---------- */
int http_start(void)
{
    /* 真机实测：4567 可绑，53317（动态区高位）被系统保留 EACCES。
     * 候选顺序把已验证可用的低端口放前面，避免每次启动都白试高端口。
     * 末尾不要放 0（临时端口）：bind(0) 必成功，但实际端口要靠 getsockname
     * 才知道，而公告里带的是本函数返回的端口 → 对端会拿到 0 连不上；
     * 且 g_port 为 0 会让"已在跑"判断与 watch 的重启条件同时失灵（反复新建
     * listener）。全候选失败就老实返回失败，交给 watch 下一轮重试。 */
    static const int cand[] = { 4567, 53318, 53316, 53319, 55555 };
    SceNetSockaddrIn sa;
    int one = 1, i, s = -1, chosen = 0;

    if (g_port > 0) return g_port;   /* 已在跑 */

    http_reap_zombie();              /* 顺手回收上次没删掉的旧 accept 线程对象 */

    if (g_conn_mtx < 0)              /* 连接登记表锁：随服务器首次启动创建，
                                      * 跨 stop/start 复用（stop 不销毁） */
        g_conn_mtx = sceKernelCreateMutex("psvsend_conn", 0, 0, NULL);
    if (g_own_mtx < 0)               /* 本机身份解析锁：必须早于任何连接线程
                                      * 创建（own_identity 不再惰性建锁） */
        g_own_mtx = sceKernelCreateMutex("psvsend_tlsown", 0, 0, NULL);

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
    /* 监听 socket 置非阻塞：accept 空转立即返回 EWOULDBLOCK（http_thr 里那条
     * 分支以前是死代码，因为 socket 一直阻塞着），于是停机/换代的响应不再
     * 卡在一次 accept 上，http_stop 的 join 预算更容易等到它。置位失败只是
     * 退回"阻塞 accept"的老行为（功能不变，仅换代会慢一拍），不值得拆服务。 */
    if (sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one,
                         sizeof one) != 0)
        dlog("http: listener set NBIO fail, keep blocking accept");
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
            if (sr < 0) {   /* 起不来：端口已 bind 却没人 accept。此时不能只记日志——
                             * 未启动的线程 http_alive 判不出死，看门狗永不重启，
                             * 服务就这么静默死了。就地拆干净，让 watch 下次重试。 */
                dlog("http: thread start FAILED -> tear down listener");
                sceKernelDeleteThread(t);
                g_thr = -1;
                sceNetSocketClose(s);
                g_lsock = -1;
                g_port = 0;
                g_run = 0;
                return -4;
            }
        }
    }
    /* 预热身份解析（证书 DER + RSA 私钥）：mbedtls_pk_parse_key 要先解 DER 再
     * 补算 RSA 的 CRT 参数，在 333MHz 上要几十到上百毫秒。官方客户端的第一次
     * 探测往往就在开机后几秒内，这段成本若留在握手关键路径上会直接吃掉
     * register 的 500ms 预算（对端放弃 → 设备从列表消失）。首启身份还在后台
     * 生成时跳过，让握手自己按 identity_wait_ready 等。 */
    if (!identity_pending() && own_identity() != 0)
        dlog("http: identity warm failed (retry on first handshake)");
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
            sceKernelDeleteThread(g_thr);  /* 回收线程对象：不删则在 DORMANT 上永久占句柄 */
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
        if (st == 0) {
            sceKernelDeleteThread(g_thr);   /* 已退出：回收线程对象（不删就每次重连泄漏一个） */
        } else {
            dlog("http: wait thread end -> 0x%08X", (unsigned)st);
            if (g_thr_zombie < 0) g_thr_zombie = g_thr;    /* 等不到：留句柄下次 start 再收 */
            else dlog("http: unreclaimed accept thread %d dropped", (int)g_thr);
        }
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

void http_set_recv_ops(const HttpRecvOps *ops)
{
    g_recv = ops;
    dlog("http: recv ops %s", ops ? "registered" : "cleared");
}
