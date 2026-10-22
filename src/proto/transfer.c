/* transfer.c —— LocalSend v2 HTTP(S) 发送客户端（对应协议 §4 文件传输 HTTP）。
 * 真机事实：Vita SceNet 的 TCP connect/recv/send 在后台线程可用（http 服务器
 * 线程已验证），仅 UDP sendto 必须留在主循环。因此整段"prepare 等待 + 多文件
 * 并发上传"跑在独立线程，UI 主线程只通过 xfer_info() 取快照渲染。
 * 收发超时模型与 http.c 一致：SceNet 无数据时 recv 立刻返回 EWOULDBLOCK
 * (0x80410123)，不能当错误，必须轮询到截止时间；取消通过每轮检查标志实现。
 *
 * HTTPS 目标（对方 announce protocol=https）：走 mbedTLS（工具链自带的 Vita
 * 移植），用对方 announce 的 fingerprint（协议规定 = 证书 SHA-256 hex）做
 * pin：握手验证回调里算叶子证书 DER 的 SHA-256，与指纹不匹配即拒绝。熵源
 * 由 Vita 弱随机(时间/指针/计数)顶替，LAN 场景够用。
 *
 * 并发模型（协议 §4.2 明确写了 This route can be called in parallel.）：
 * 阶段 1 的 prepare-upload 只有一条连接，等接收方决定；阶段 2 由 N 个 worker
 * 各开一条连接、各取一个文件上传（N = config 的 maxParallel，1..6，1 即退回
 * 逐文件串行）。worker 只负责"置文件状态 / 记错"，终态（finished / ok /
 * cancelled）一律由监督者 xfer_thr 在 join 完全部 worker 之后统一写一次——
 * worker 若自己去写 finished，UI 会以为传输已结束并允许开新任务，而 g_j 会被
 * 下一次 xfer_start memset 掉，正在收尾的 worker 就在读一份被清空的 Job。 */
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
#include "proto/transfer.h"
#include "core/config.h"
#include "net/identity.h"
#include "net/http.h"
#include "core/json_util.h"
#include "core/dlog.h"

#define NET_AGAIN(r) ((r) == 0x80410123)
#define POLL_US       20000          /* 20ms 轮询间隔 */
#define TX_SOCK_BUF   8192           /* 响应收包缓冲 */
#define TX_CHUNK      32768          /* 上传分块 */
#define TX_BLOCKS     2              /* 发送流水线块数（读线程 / 发送线程各用一块） */
#define RSP_BUF       65536          /* prepare 应答缓冲上限（头+body） */
#define PREPARE_PATH  "/api/localsend/v2/prepare-upload"
#define PIN_MAX_TRY   3              /* 401 时最多尝试的 PIN 次数（含首次无 PIN） */
#define PIN_WAIT_US   (120LL * 1000000LL)  /* 等 UI 输入 PIN 的上限 */

/* prepare-upload 是"等到接收方接受/拒绝"的长请求，给足时间；
 * 期间每 20ms 检查取消标志，本地取消立即退出。 */
#define PREPARE_WAIT_US (30LL * 60 * 1000000LL)
#define OP_TIMEOUT_US   30000000LL    /* 上传响应等待上限 30s */
#define CHUNK_DEADLINE_US 10000000LL  /* 单个分块发送上限 10s */
#define TLS_HANDSHAKE_US  20000000LL  /* TLS 握手上限 20s */
/* TCP connect 截止：局域网内正常 connect <100ms（实测 thread entered→tls handshake
 * ok 约 640ms，含握手），6s 已是很宽的余量；对端 Wi-Fi 休眠唤醒也够。到点即判
 * "对端已离线"——期间每 20ms 查取消，随时可中断。 */
#define CONNECT_WAIT_US    6000000LL

typedef struct {
    XferInfo   v;
    volatile int  cancel;            /* 用户取消（UI 线程置位） */
    volatile int  abort;             /* 连接级失败 → 全局中止（对端不在） */
    volatile int  notified;          /* cancel 通知已尝试（每任务一次，锁内 test-and-set） */
    int        parallel;             /* 本次实际并发数（1=串行，决定状态行文案） */
    int        next;                 /* 下一个待派发的文件下标（worker 锁内取号） */
    char       abort_msg[160];       /* 首个连接级失败的原因（终态 err 用） */
    SceUID     th;
    char       ip[16];
    int        port;
    int        tls;                  /* 1=HTTPS(mbedTLS) 0=HTTP */
    char       pin[96];              /* TLS pin：对方证书 SHA-256（hex） */
    volatile int pin_got;            /* 0=未回填 1=已输入 2=用户取消（UI 写） */
    char       pin_in[32];           /* UI 输入的 PIN（锁内读写） */
    char       pin_code[32];         /* 本次重试实际带上的 PIN（线程私有） */
    XferFile   files[XFER_MAX_FILES];
    int        count;
    char       session[64];          /* prepare 应答的会话 ID */
    char       tokens[XFER_MAX_FILES][64];
    char       sha[XFER_MAX_FILES][65]; /* 预扫得到的 sha256（hex，空=不带） */
} Job;

static Job    g_j;
static SceUID g_mtx = -1;

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    lock();
    va_start(ap, fmt);
    vsnprintf(g_j.v.msg, sizeof g_j.v.msg, fmt, ap);
    va_end(ap);
    unlock();
}

/* 更新单文件进度，并把差值累加进总进度。
 * 并发下没有"当前文件"可言（多个 worker 各更新自己那个文件的 sent），必须
 * 每次更新即按增量累加；旧写法只在 i == v.cur 时整表重算，并发时 cur 只指
 * 向其中一个 worker 的文件，其余文件的字节数永远不计入 → 总进度条不动。 */
static void set_file_state(int i, int state, SceOff sent)
{
    if (i < 0 || i >= g_j.count) return;
    lock();
    g_j.v.total_sent += sent - g_j.v.f[i].sent;
    g_j.v.f[i].state = state;
    g_j.v.f[i].sent = sent;
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

/* 标记第 i 个文件失败（不结束整个任务）：逐文件独立成败下，单文件失败只标它
 * 自己（state=3）并记下原因，循环接着发剩下的；任务终态等循环结束时统一判定。 */
static void mark_file_failed(int i, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    if (i >= 0 && i < g_j.count)
        set_file_state(i, 3, g_j.v.f[i].sent);
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    lock();
    snprintf(g_j.v.err, sizeof g_j.v.err, "%s", tmp);
    unlock();
    dlog("xfer: file %d failed: %s", i, tmp);
}

/* 连接级失败（连不上 / TLS 握手失败 / 中途断流 / 等不到回执）：判"对端不在"，
 * 本 worker 置全局中止标志并把自己这个文件标失败——其余在途 worker 在下一个
 * 检查点看到标志即收手，还没派发的文件由监督者在 join 完全部 worker 后统一
 * 标失败。**收敛口径（2026-09-18 定案）：首个连接级失败立即全局中止**，而不是
 * 让各 worker 各自等满 6s（连接）/30s（回执）——并发 6 路下后者是 ceil(N/6)
 * 轮超时，几十个文件就是分钟级界面停摆；且对端真不在时，在途文件同样必然失败。
 * HTTP 4xx/5xx 不是连接级，走单文件失败（mark_file_failed），继续发剩下的。 */
static void abort_xfer(int i, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    lock();
    if (!g_j.abort) {                 /* 原因取第一个报出来的那个 */
        g_j.abort = 1;
        snprintf(g_j.abort_msg, sizeof g_j.abort_msg, "%s", tmp);
    }
    unlock();
    mark_file_failed(i, "%s", tmp);
    dlog("xfer: abort at file %d/%d: %s", i + 1, g_j.count, tmp);
}

/* worker 取号：返回下一个待发文件下标；-1 = 没活干了（发完 / 已取消 / 已中止） */
static int next_file(void)
{
    int i;
    lock();
    if (g_j.cancel || g_j.abort || g_j.next >= g_j.count) i = -1;
    else i = g_j.next++;
    unlock();
    return i;
}

/* 协议 v1 §3.3：cancel 由【发送方】在取消会话时调用，通知接收方清理与显示
 * "对方已取消"。若只断流不通知，接收方（尤其官方客户端）会把断流判成
 * "应收 X 实收 Y"的字节错误且不清残留——官方手机端就有这毛病。实现见 conn_open
 * 之后（依赖连接工具）；尽力通知：会话没建立（session 空）不需要；连不上/超时
 * 就算了，对方至多按断流报错。*/
static void cancel_notify_peer(void);

/* 取消收尾（终态显示 cancelled）。cancel 通知已由各传输中取消点在断流前提前发
 * （见 upload 循环），此处再调一次是兜底（guard 保证每任务最多发一次）。 */
static void finish_cancelled(void)
{
    lock();
    g_j.v.cancelled = true;
    unlock();
    fail("cancelled by user");
    cancel_notify_peer();
}

/* ---------- 连接抽象：一条 HTTP(S) 连接 ---------- */
typedef struct {
    int fd;
    int tls;                  /* 本连接是否走 TLS */
    int ignore_flags;         /* 本连接不理会 cancel/abort 标志（只有 cancel 通知
                               * 那条连接会置位：它的成败不该被它要通知的那件事
                               * 本身的标志打断，见 cancel_notify_peer） */
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

/* 发送窗口暂时没空（WANT_WRITE / EWOULDBLOCK）：先让出 CPU 空转几轮，仍不行
 * 才睡 1ms。与接收侧 http.c 的 conn_read 同口径——旧写法每次都睡 20ms，实测
 * 约四成的 32KB 块会撞一次，等于每块白扔 20ms，是发送 3.53MiB/s 的主要低效点。
 * t_wait/n_wait（可空）只用于诊断日志。 */
static void tx_backoff(int *spins, uint64_t *t_wait, int *n_wait)
{
    uint64_t t = now_us();
    if (++*spins <= 8) sceKernelDelayThread(0);
    else sceKernelDelayThread(1000);
    if (t_wait) *t_wait += now_us() - t;
    if (n_wait) ++*n_wait;
}

/* 发送 len 字节；返回 0=成功 -1=错误/超时 -2=已取消 -4=全局中止（对端不在）。
 * t_wait/n_wait（可空）累计"窗口没空"的退避次数与耗时，仅用于诊断。 */
static int conn_send_all(Conn *c, const char *data, int len,
                         uint64_t *t_wait, int *n_wait)
{
    int off = 0, spins = 0;
    SceLong64 dl = sceKernelGetSystemTimeWide() + CHUNK_DEADLINE_US;
    while (off < len) {
        if (!c->ignore_flags && g_j.cancel) return -2;
        if (!c->ignore_flags && g_j.abort) return -4;   /* 别的 worker 已判"对端不在"：别再耗发送窗口 */
        if (sceKernelGetSystemTimeWide() >= dl) return -1;   /* 发送窗口堵死 */
        if (c->tls) {
            int r = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data + off,
                                      (size_t)(len - off));
            if (r > 0) { off += r; spins = 0; continue; }
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
                tx_backoff(&spins, t_wait, n_wait);
                continue;
            }
            dlog("xfer: ssl_write err -0x%04X", (unsigned)(-r));
            return -1;
        } else {
            int r = sceNetSend(c->fd, data + off, (unsigned)(len - off), 0);
            if (r > 0) { off += r; spins = 0; continue; }
            if (r == 0) return -1;
            if (NET_AGAIN(r)) {
                tx_backoff(&spins, t_wait, n_wait);
                continue;
            }
            return -1;
        }
    }
    return 0;
}

/* 收一次包（轮询到 dl 截止）：返回 >0 字节数；0=对方关闭；-1=错误/超时；
 * -2=取消；-4=全局中止（对端不在，别的 worker 已判定） */
static int conn_recv_poll(Conn *c, char *buf, int cap, SceLong64 dl)
{
    for (;;) {
        if (!c->ignore_flags && g_j.cancel) return -2;
        if (!c->ignore_flags && g_j.abort) return -4;
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

/* 读一个 HTTP 响应。返回 body 长度（body 在 buf 里头部之后，调用方的 JSON 解析
 * 器会自己跳过头部）；-1 = 读失败/超时/缓冲装不下；-2 = 用户取消；
 * -4 = 全局中止（对端不在）。 */
static int read_resp(Conn *c, int *code, char *buf, int cap, SceLong64 wait_us)
{
    int n = 0, he = -1, cl = -1, body_off = 0, chunked = 0;
    SceLong64 dl = sceKernelGetSystemTimeWide() + wait_us;
    *code = 0;
    for (;;) {
        int i, r;
        if (!c->ignore_flags && g_j.cancel) return -2;
        if (!c->ignore_flags && g_j.abort) return -4;
        if (n >= cap - 1) {
            dlog("resp: reply does not fit in %d-byte buffer", cap);
            return -1;
        }
        r = conn_recv_poll(c, buf + n, cap - 1 - n, dl);
        if (r == -2 || r == -4) return r;
        if (r <= 0) {
            dlog("resp: incomplete reply (%d byte(s) in, r=%d)", n, r);
            return -1;              /* 读错误 / 超时 / 对端提前关闭 */
        }
        n += r;
        buf[n] = 0;
        /* 扫整个缓冲找头结束符（勿只扫末尾——历史 bug） */
        for (i = 0; i + 4 <= n && he < 0; i++)
            if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                buf[i + 2] == '\r' && buf[i + 3] == '\n') he = i;
        if (he >= 0 && cl < 0) {
            /* LocalSend 实际返回全小写头（content-length）：走 http 层的溢出安全
             * 解析（大小写不敏感）。此前用 32 位 long 裸累加，对端回超长数字会
             * 回绕成负值 → 立即 break、旧 guard 只挡上界放行负值 →
             * buf[body_off+cl]=0 负索引写越界。 */
            cl = http_hdr_content_length(buf, he);
            if (cl <= 0) {
                cl = 0;             /* 无/畸形/超范围：暂按 0 长 */
                /* 没有 Content-Length 不等于 body 为空：对端可能用 chunked
                 * （旧写法一律当 0 长，会把带 body 的回执误报成"空回执"）。 */
                if (http_hdr_chunked(buf, he)) chunked = 1;
            }
            if (strncmp(buf, "HTTP/1.", 7) == 0) {
                const char *cs = strchr(buf, ' ');
                if (cs) *code = atoi(cs + 1);
            }
            body_off = he + 4;
        }
        if (he < 0) continue;                       /* 头还没收全 */
        if (chunked) {
            int blen = http_chunked_decode(buf + body_off, n - body_off);
            if (blen >= 0) {                        /* 分块已收齐并解出 */
                buf[body_off + blen] = 0;
                return blen;
            }
            continue;                               /* 还有块没到：继续收 */
        }
        if (n >= body_off + cl) {
            if (cl > cap - 1 - body_off) {
                dlog("resp: body %d does not fit (cap %d, off %d)", cl, cap, body_off);
                return -1;
            }
            buf[body_off + cl] = 0;
            return cl;
        }
    }
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

/* TLS 握手（EWOULDBLOCK 轮询；期间可取消）；返回 0 成功 / -1 / -2 取消 / -4 中止 */
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
        if (!c->ignore_flags && g_j.cancel) return -2;
        if (!c->ignore_flags && g_j.abort) return -4;   /* 中止：别再耗 20s 握手上限 */
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
 * 返回 0 成功；-1 失败；-2 取消；-3 TLS 握手/证书校验失败；-4 全局中止
 * （对端不在，别的 worker 已判定——取消与中止都会自关连接再返回）。 */
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
    /* 建好即置非阻塞：本文件的收发本就按"无数据/无窗口 → EWOULDBLOCK → 20ms
     * 轮询到截止"模型写（conn_send_all / conn_recv_poll），connect 也一并纳入。
     * Vita socket 默认阻塞且 connect 无内置超时——挑到台刚离线的设备会一直堵在
     * 三次握手（真机实测 >45s 未返回），期间取消标志查不到，进度页按钮全失效、
     * 只能杀进程重启。置非阻塞后 connect 立即返回"在途"，改由 getpeername 轮询
     * 判定 + 截止（与 scan.c 的 net_connect_to 同一套做法）。 */
    {
        int one = 1;
        sceNetSetsockopt(c->fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                         &one, sizeof one);
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
    if (r != 0 && r != SCE_NET_ERROR_EWOULDBLOCK &&
        r != SCE_NET_ERROR_EINPROGRESS && r != SCE_NET_ERROR_EALREADY) {
        /* 立即裁决的失败：connection refused / 无路由 / 不可达 */
        dlog("xfer: connect %s:%d fail 0x%08X",
             g_j.ip, g_j.port, (unsigned)r);
        sceNetSocketClose(c->fd);
        c->fd = -1;
        return -1;
    }
    if (r != 0) {
        /* 在途：轮询到截止。未裁决时 getpeername 返回 ENOTCONN 且 SO_ERROR 为 0；
         * 建立后 getpeername 返回 0；内核裁决失败则 SO_ERROR 非 0（refused/不可达）。
         * 每轮先查取消 → 用户按取消 20ms 内生效，不再像阻塞 connect 那样只能重启。 */
        SceLong64 dl = sceKernelGetSystemTimeWide() + CONNECT_WAIT_US;
        int up = 0;
        while (!up) {
            int so = 0;
            unsigned int sl = sizeof so;
            SceNetSockaddrIn p;
            unsigned int plen = sizeof p;
            if (!c->ignore_flags && g_j.cancel) {
                dlog("xfer: connect cancelled -> %s:%d", g_j.ip, g_j.port);
                sceNetSocketClose(c->fd);
                c->fd = -1;
                return -2;
            }
            if (!c->ignore_flags && g_j.abort) {   /* 别的 worker 已判对端不在：别再等这 6s */
                sceNetSocketClose(c->fd);
                c->fd = -1;
                return -4;
            }
            if (sceNetGetsockopt(c->fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR,
                                 &so, &sl) == 0 && so != 0) {
                /* SO_ERROR 存的是裸 errno（61 = ECONNREFUSED），补上 SCE 网络错误基址
                 * 0x80410100，与 sceNetXxx 直接返回的编码（0x804101xx）统一，免得
                 * 日志里同一类错误出现两种写法、以后调试要换算。已带基址的按位或也
                 * 不变（0x8041013D | 0x80410100 == 0x8041013D）。 */
                dlog("xfer: connect %s:%d fail 0x%08X",
                     g_j.ip, g_j.port, (unsigned)so | 0x80410100u);
                break;
            }
            if (sceNetGetpeername(c->fd, (SceNetSockaddr *)&p, &plen) == 0) {
                up = 1;
                break;
            }
            if (sceKernelGetSystemTimeWide() >= dl) {
                dlog("xfer: connect %s:%d timeout (%dms) - peer offline?",
                     g_j.ip, g_j.port, (int)(CONNECT_WAIT_US / 1000));
                break;
            }
            sceKernelDelayThread(POLL_US);
        }
        if (!up) {
            sceNetSocketClose(c->fd);
            c->fd = -1;
            return -1;
        }
    }
    if (c->tls) {
        r = tls_handshake(c);
        if (r == -2) {
            conn_close(c);
            return -2;
        }
        if (r == -4) {
            conn_close(c);
            return -4;
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
    r = conn_send_all(c, hdr, hlen, NULL, NULL);
    if (r < 0) {                        /* -1 发送失败 / -2 取消 */
        conn_close(c);
        return r;
    }
    return 0;
}

/* 尽力向接收方 POST cancel（声明见上）。这条连接置 ignore_flags：它的收发轮询
 * 不能再被 cancel/abort 标志打断（它要通知的正是"已取消"这件事）。
 * **不许在这里去清 cancel/abort 全局标志**：并发下其它 worker 每轮都靠这两个标志
 * 收手，清掉等于放开它们继续上传（连接/回执各有一份超时，最坏能续跑十几秒，
 * 用户按了取消却看着进度条继续走）。
 * 并发下多个 worker 会同时走到取消路径，故用锁做一次性的 test-and-set：同一
 * 会话的 cancel 只发一次，重复发会让对端把同一会话收尾两遍。 */
static void cancel_notify_peer(void)
{
    char path[96];
    Conn c;
    char rbuf[256];
    int code = 0;
    if (!g_j.session[0]) return;     /* 会话还没建立：无可通知 */
    lock();
    if (g_j.notified) { unlock(); return; }   /* 每任务只尽力通知一次 */
    g_j.notified = 1;
    unlock();
    snprintf(path, sizeof path, "/api/localsend/v2/cancel?sessionId=%s", g_j.session);
    dlog("xfer: notify receiver cancel (session %s)", g_j.session);
    memset(&c, 0, sizeof c);
    c.fd = -1;
    c.tls = g_j.tls;
    c.ignore_flags = 1;
    if (conn_open(&c, path, 0) == 0) {
        read_resp(&c, &code, rbuf, (int)sizeof rbuf, 5000000LL);
        dlog("xfer: cancel notify -> HTTP %d", code);
        conn_close(&c);
    }
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

/* 往缓冲里追加一段 JSON：返回新的写入位置；放不下（会被截断）返回 -1。
 * 必须靠 vsnprintf 返回值判断——它返回的是"本来想写多长"，截断时远大于剩余空间，
 * 若直接累加，o 会越过 cap，随后的 b[o]=0 就是越界写（旧写法 o < cap-64 的余量
 * 判断挡不住单条最长约 344 字节的条目）。 */
static int jcat(char *b, int cap, int o, const char *fmt, ...)
{
    va_list ap;
    size_t room;
    int w;
    if (o < 0 || o >= cap - 1) return -1;
    room = (size_t)(cap - o);
    va_start(ap, fmt);
    w = vsnprintf(b + o, room, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= room) return -1;
    return o + w;
}

/* 组装 prepare-upload 请求体（成员信息 + 待发文件元数据）。
 * 任何一段装不下就返回 NULL：宁可不发，也不发一个截断的 JSON 出去。 */
static char *build_prepare_json(void)
{
    /* 单文件上限按最坏情况给足：128 字节文件名（转义后 2×）+ sha256(64) + 固定字段。
     * 早期 420 在带 sha256 后会把长名文件挤出上限，导致整份 JSON 拒发。 */
    int cap = TX_SOCK_BUF + XFER_MAX_FILES * 560;
    char al[sizeof g_cfg.alias], fp[65];
    char ae[2 * sizeof g_cfg.alias], fe[2 * 65];
    char *b = (char *)malloc((size_t)cap);
    int o, i;
    if (!b) return NULL;
    config_get_alias(al, sizeof al);         /* 锁内取快照：UI 可能正在改名 */
    /* per-device 证书指纹（与 announce / member info 同一口径） */
    if (identity_fingerprint(fp) != 0)
        config_get_fingerprint(fp, sizeof fp);
    json_escape(al, ae, sizeof ae);
    json_escape(fp, fe, sizeof fe);
    o = jcat(b, cap, 0,
             "{\"info\":{\"alias\":\"%s\",\"version\":\"2.0\","
             "\"deviceModel\":\"PlayStation Vita\",\"deviceType\":\"mobile\","
             "\"fingerprint\":\"%s\",\"port\":%d,\"protocol\":\"https\","
             "\"download\":false},\"files\":{",
             ae, fe, http_port());
    for (i = 0; o >= 0 && i < g_j.count; i++) {
        char esc[2 * 128 + 8];
        char sh[80];
        json_escape(g_j.files[i].name, esc, sizeof esc);
        /* sha256 是协议里的可空字段：算了才带（设置页开关，默认开）。
         * 带与不带的差别只是接收端是否校验，不影响传输本身。 */
        sh[0] = 0;
        if (g_j.sha[i][0])
            snprintf(sh, sizeof sh, ",\"sha256\":\"%s\"", g_j.sha[i]);
        o = jcat(b, cap, o,
                 "%s\"f%d\":{\"id\":\"f%d\",\"fileName\":\"%s\","
                 "\"size\":%lld,\"fileType\":\"application/octet-stream\"%s}",
                 i ? "," : "", i, i, esc, (long long)g_j.files[i].size, sh);
    }
    o = jcat(b, cap, o, "}}");
    if (o < 0) {                             /* 装不下：别发截断的 JSON */
        dlog("xfer: prepare json does not fit in %d bytes", cap);
        free(b);
        return NULL;
    }
    return b;
}

/* ---- 发送体双缓冲流水：读盘与加密发送各占一条线程 ----
 * 旧实现是一条线程严格串行 sceIoRead(32KB) → mbedtls_ssl_write(32KB)，两段
 * 时间直接相加（读盘约 9.6MiB/s、发送约 5.6MiB/s → 合起来 3.53MiB/s，正是
 * d180 的实测值）。改成：读线程只管读盘，worker 本线程只管 ssl_write
 * （mbedTLS 上下文只能由一条线程碰）。
 *   sem_free = 空闲块数（初值 TX_BLOCKS）、sem_full = 已读满块数；
 *   读线程：等空块 → 读 → 投 sem_full；发送侧：等满块 → 发 → 还 sem_free。
 * 读线程按声明的 size 读满（或读失败）后投一个 len=0 的终止块，发送侧见到它
 * 即收尾，不另设结束标志。每文件 2 条线程、64KB 堆缓冲；并发 N 路即 2N 条。
 * 与接收侧（receive.c 的 sha/wr 线程）同口径：起不来就判本文件失败，不做退化
 * 分支。 */
typedef struct {
    SceUID fd, thr, sem_free, sem_full;
    volatile int quit;               /* 发送侧出错/取消：让读线程尽快退出 */
    volatile int rd_err;             /* -1=读错误 -2=文件比声明的 size 短（0=正常） */
    SceOff remain;                   /* 还没读的字节数（读线程独占） */
    int    idx_rd, idx_wr;           /* 读/发各自游标（各自独占，无需锁） */
    int    len[TX_BLOCKS];
    char  *buf;                      /* TX_BLOCKS * TX_CHUNK */
    uint64_t t_read;                 /* 读盘累计（发送侧 join 之后才读） */
} TxPipe;

static int tx_reader(SceSize args, void *argp)
{
    TxPipe *p = *(TxPipe **)argp;
    (void)args;
    for (;;) {
        int s, rd, want;
        uint64_t t;
        sceKernelWaitSema(p->sem_free, 1, NULL);
        if (p->quit) { sceKernelSignalSema(p->sem_free, 1); break; }
        s = p->idx_rd % TX_BLOCKS;
        if (p->remain <= 0) {           /* 读满声明的字节数：投终止块 */
            p->len[s] = 0;
            ++p->idx_rd;
            sceKernelSignalSema(p->sem_full, 1);
            break;
        }
        want = p->remain < (SceOff)TX_CHUNK ? (int)p->remain : TX_CHUNK;
        t = now_us();
        rd = sceIoRead(p->fd, p->buf + (size_t)s * TX_CHUNK, (unsigned)want);
        p->t_read += now_us() - t;
        if (rd <= 0) {                  /* 0=文件变短 <0=读错误：同样投终止块 */
            p->rd_err = rd < 0 ? -1 : -2;
            p->len[s] = 0;
            ++p->idx_rd;
            sceKernelSignalSema(p->sem_full, 1);
            break;
        }
        p->len[s] = rd;
        p->remain -= rd;
        ++p->idx_rd;
        sceKernelSignalSema(p->sem_full, 1);
    }
    return 0;
}

/* 收掉流水线：唤停读线程 → join → 删线程/信号量 → 释放缓冲。
 * started=0 表示线程只是创建了没启动过（创建后启动失败），此时不能 join。 */
static void tx_pipe_stop(TxPipe *p, int started)
{
    if (started) {
        SceUInt tmo = 5000000;
        p->quit = 1;
        sceKernelSignalSema(p->sem_free, 1);   /* 读线程可能正等空块 */
        sceKernelWaitThreadEnd(p->thr, NULL, &tmo);
    }
    if (p->thr >= 0) sceKernelDeleteThread(p->thr);
    if (p->sem_full >= 0) sceKernelDeleteSema(p->sem_full);
    if (p->sem_free >= 0) sceKernelDeleteSema(p->sem_free);
    free(p->buf);
    p->buf = NULL;
}

/* 起"读线程 + 本线程消费"的双缓冲流水。
 * 返回 0=已起（消费方按 sem_full 取块）、1=零字节（无需流水线）、<0=建不起来。
 * 消费方负责调 tx_pipe_stop（started=0 时也能安全清理）。 */
static int tx_pipe_start(TxPipe *p, SceUID fd, SceOff size)
{
    int started = 0;
    memset(p, 0, sizeof *p);
    p->fd = fd;
    p->thr = p->sem_free = p->sem_full = -1;
    p->remain = size;
    if (size <= 0) return 1;
    p->buf = (char *)malloc((size_t)TX_CHUNK * TX_BLOCKS);
    if (p->buf)
        p->sem_free = sceKernelCreateSema("psvsend_sfree", 0, TX_BLOCKS, TX_BLOCKS, NULL);
    if (p->sem_free >= 0)
        p->sem_full = sceKernelCreateSema("psvsend_sfull", 0, 0, TX_BLOCKS, NULL);
    if (p->sem_full >= 0)
        p->thr = sceKernelCreateThread("psvsend_rd", tx_reader, 0x40, 0x20000, 0,
                                       SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (p->thr >= 0) {
        TxPipe *pp = p;
        started = sceKernelStartThread(p->thr, sizeof pp, &pp) >= 0;
    }
    if (!started) {
        dlog("xfer: pipeline setup fail (thr=%d sem=%d/%d buf=%d)", (int)p->thr,
             (int)p->sem_free, (int)p->sem_full, p->buf != NULL);
        tx_pipe_stop(p, 0);
        return -1;
    }
    return 0;
}

/* ---------- 发送前 sha256 预扫（设置项 sendSha，默认开） ----------
 * prepare-upload 的 sha256 必须随请求一起发，所以只能"先整批算完再发请求"。
 * 读盘交给 tx_reader（同一套双缓冲流水），本线程只做 sha256_update。
 * 真机实测（d183 探针）：纯哈希 8.84MiB/s、读+哈希流水 8.41、串行读+哈希 4.65
 * ——所以必须流水，串行会把等待时间翻倍。 */

/* 算第 i 个文件的 sha256，成功写 out（65 字节 hex）。返回 0=完成（含跳过），
 * 1=被取消。读失败/文件短读时不带 sha（上传阶段照旧会报该文件失败）。 */
static int hash_one(int i, char *out)
{
    TxPipe p;
    mbedtls_sha256_context ctx;
    unsigned char dig[32];
    SceUID fd;
    int st, cancelled = 0;

    out[0] = 0;
    fd = sceIoOpen(g_j.files[i].path, SCE_O_RDONLY, 0);
    if (fd < 0) return 0;
    st = tx_pipe_start(&p, fd, g_j.files[i].size);
    if (st != 0) {                       /* 零字节或起不来：不带 sha */
        sceIoClose(fd);
        return 0;
    }
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    for (;;) {
        int c, len;
        sceKernelWaitSema(p.sem_full, 1, NULL);
        c = p.idx_wr % TX_BLOCKS;
        len = p.len[c];
        ++p.idx_wr;
        if (len <= 0) {                  /* 终止块（读满 / 读失败） */
            sceKernelSignalSema(p.sem_free, 1);
            break;
        }
        mbedtls_sha256_update(&ctx, (unsigned char *)p.buf + (size_t)c * TX_CHUNK,
                              (unsigned)len);
        sceKernelSignalSema(p.sem_free, 1);
        lock();
        g_j.v.hash_done += len;
        g_j.v.hash_cur_done += len;
        unlock();
        if (g_j.cancel) { cancelled = 1; break; }
    }
    tx_pipe_stop(&p, 1);
    sceIoClose(fd);
    if (!cancelled && p.rd_err)
        dlog("xfer: hash read error on file %d (no sha)", i + 1);
    if (cancelled || p.rd_err) {
        mbedtls_sha256_free(&ctx);
        return cancelled;
    }
    mbedtls_sha256_finish(&ctx, dig);
    mbedtls_sha256_free(&ctx);
    {
        int k;
        for (k = 0; k < 32; k++) {
            out[k * 2]     = "0123456789abcdef"[dig[k] >> 4];
            out[k * 2 + 1] = "0123456789abcdef"[dig[k] & 0xF];
        }
        out[64] = 0;
    }
    return 0;
}

/* 整批预扫：更新 UI 快照（hashing/hash_done/msg），返回 0=完成 1=取消 */
static int hash_phase(void)
{
    uint64_t t0 = now_us();
    int i, n = g_j.count;

    lock();
    g_j.v.hashing = true;
    g_j.v.hash_cur = -1;
    g_j.v.hash_done = 0;
    g_j.v.hash_total = g_j.v.total;
    unlock();
    for (i = 0; i < n; i++) {
        lock();
        g_j.v.hash_cur = i;
        g_j.v.hash_cur_done = 0;
        unlock();
        set_msg("Calculating checksum (%d/%d)", i + 1, n);
        if (hash_one(i, g_j.sha[i]) != 0) {
            lock();
            g_j.v.hashing = false;
            g_j.v.hash_cur = -1;
            unlock();
            return 1;
        }
    }
    dlog("xfer: sha256 prehash %lld B in %llu ms (%.2f MiB/s)", (long long)g_j.v.total,
         (unsigned long long)((now_us() - t0) / 1000),
         (double)(g_j.v.total) / 1048576.0 /
             (((double)(now_us() - t0)) / 1000000.0 + 0.001));
    lock();
    g_j.v.hashing = false;
    g_j.v.hash_cur = -1;
    unlock();
    return 0;
}

/* ---------- 上传 worker（阶段 2 的并发执行体） ---------- */

/* 单个文件的完整上传：建连接 → 发字节 → 等回执（每文件一条独立连接）。
 * 返回 0 = 这个文件已处理完（成功，或已按"单文件失败"记下），可再取下一个；
 *      1 = 本 worker 收手（用户取消 / 全局中止）。
 * 本函数绝不写 finished / active / cancelled —— 终态只由监督者 xfer_thr 在
 * join 完全部 worker 之后写一次（理由见文件头注）。 */
static int upload_one(int i)
{
    char q[320];
    char abuf[512];                 /* 上传回执缓冲（头 + 空 body） */
    Conn c;
    TxPipe p;                       /* 读/发双缓冲流水（见 tx_reader） */
    int fd, sr = 0, blen, tx_started = 0;
    int code = 0;
    int n_wait = 0;                 /* 发送窗口没空的退避次数 */
    uint64_t t_send = 0, t_wait = 0;
    SceOff sent_file = 0;

    if (i < 0 || i >= g_j.count) return 0;
    if (g_j.tokens[i][0] == 0) {
        /* 协议 4.1 明确允许对端只回执收下的子集：没 token = 这个文件被拒 */
        mark_file_failed(i, "receiver did not accept %s", g_j.files[i].name);
        return 0;
    }
    lock();
    g_j.v.cur = i;                  /* 并发下 = 最近开工的那个（页面只拿它判状态行） */
    g_j.v.f[i].state = 1;
    unlock();
    if (g_j.parallel <= 1)          /* 串行才报"在发哪个文件"；并发时报了只会乱跳 */
        set_msg("Sending %s (%d/%d)", g_j.files[i].name, i + 1, g_j.count);

    fd = sceIoOpen(g_j.files[i].path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        mark_file_failed(i, "cannot open %s", g_j.files[i].path);
        return 0;
    }
    snprintf(q, sizeof q,
             "/api/localsend/v2/upload?sessionId=%s&fileId=f%d&token=%s",
             g_j.session, i, g_j.tokens[i]);
    memset(&c, 0, sizeof c);
    c.fd = -1;
    c.tls = g_j.tls;
    sr = conn_open(&c, q, g_j.files[i].size);
    if (sr == -2) {                 /* conn_open 取消已自关：对方在等首文件流，先通知 */
        cancel_notify_peer();
        sceIoClose(fd);
        return 1;
    }
    if (sr == -4) {                 /* 全局中止：本文件由监督者统一标失败 */
        sceIoClose(fd);
        return 1;
    }
    if (sr < 0) {
        if (sr == -3)
            abort_xfer(i, "TLS handshake with %s:%d failed", g_j.ip, g_j.port);
        else
            abort_xfer(i, "connect failed for %s", g_j.files[i].name);
        sceIoClose(fd);
        return 1;
    }
    /* 读/发双缓冲流水：读盘交给读线程，本线程只做 ssl_write。 */
    sr = tx_pipe_start(&p, fd, g_j.files[i].size);
    if (sr < 0) {
        conn_close(&c);
        sceIoClose(fd);
        mark_file_failed(i, "out of memory for %s", g_j.files[i].name);
        return 0;
    }
    tx_started = (sr == 0);          /* 零字节文件不起流水线，直接等回执 */
    sr = 0;                          /* 流水线就绪与否不参与下面的错误判定 */
    while (tx_started) {
        int s, len;
        uint64_t t;
        sceKernelWaitSema(p.sem_full, 1, NULL);
        s = p.idx_wr % TX_BLOCKS;
        len = p.len[s];
        ++p.idx_wr;
        if (len <= 0) {                 /* 读线程的终止块（读满 size / 读失败） */
            sceKernelSignalSema(p.sem_free, 1);
            break;
        }
        t = now_us();
        sr = conn_send_all(&c, p.buf + (size_t)s * TX_CHUNK, len, &t_wait, &n_wait);
        t_send += now_us() - t;
        sceKernelSignalSema(p.sem_free, 1);
        if (sr < 0) break;
        sent_file += len;
        set_file_state(i, 1, sent_file);
    }
    tx_pipe_stop(&p, tx_started);
    dlog("xfer: file %d/%d body %lld/%lld B  read %llu send %llu wait %llu ms (%d)  r=%d",
         i + 1, g_j.count, (long long)sent_file, (long long)g_j.files[i].size,
         (unsigned long long)(p.t_read / 1000), (unsigned long long)(t_send / 1000),
         (unsigned long long)(t_wait / 1000), n_wait, sr);
    if (sr < 0) {
        if (sr == -2) cancel_notify_peer(); /* 发送中被取消：断流前通知 */
        conn_close(&c);
        sceIoClose(fd);
        /* -2 取消 / -4 别人已判"对端不在"：本 worker 收手，终态交监督者 */
        if (sr == -2 || sr == -4) return 1;
        /* 上传中途断流：对端没了（连接级）→ 全局中止 */
        abort_xfer(i, "send error on %s", g_j.files[i].name);
        return 1;
    }
    if (p.rd_err) {
        /* 只算这一个文件失败，其余照发。**故意不发 cancel**：cancel 是会话级
         * （/cancel?sessionId），会把官方对端的整批一起取消；而对端若是本客户端，
         * 它已按"单文件失败"处理，不需要这条通知。关连接即让对端看到 body 截断
         * 并收尾该文件（d188 起接收侧不再因此判整个会话失败）。 */
        conn_close(&c);
        sceIoClose(fd);
        if (p.rd_err == -2)
            mark_file_failed(i, "%s shrank during send", g_j.files[i].name);
        else
            mark_file_failed(i, "read error on %s", g_j.files[i].name);
        return 0;
    }
    /* 等对方回执（2xx 即成功） */
    blen = read_resp(&c, &code, abuf, sizeof abuf, OP_TIMEOUT_US);
    conn_close(&c);
    sceIoClose(fd);
    if (blen == -2) {
        cancel_notify_peer();       /* body 已发完等回执时取消：收尾前通知 */
        return 1;
    }
    if (blen == -4) return 1;       /* 全局中止 */
    if (blen < 0) {
        /* 等不到回执（超时/连接被关）同样是连接级失败 */
        abort_xfer(i, "no reply for %s", g_j.files[i].name);
        return 1;
    }
    if (code < 200 || code >= 300) {
        /* HTTP 4xx/5xx：只算这一个文件失败，继续发别的 */
        mark_file_failed(i, "%s upload failed (HTTP %d)", g_j.files[i].name, code);
        return 0;
    }
    dlog("xfer: file %d/%d done (%lld bytes)", i + 1, g_j.count,
         (long long)g_j.files[i].size);
    set_file_state(i, 2, g_j.files[i].size);
    return 0;
}

/* 上传 worker：反复取号 → 传完一个文件，直到没活干或该收手。
 * 正常 return（不 ExitDeleteThread），监督者据此 join 后再删线程对象。 */
static int upload_worker(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;) {
        int i = next_file();        /* 发完 / 取消 / 中止 → -1 */
        if (i < 0) break;
        if (upload_one(i) != 0) break;
    }
    return 0;
}

/* ---------- 传输线程 ---------- */

/* prepare-upload 的请求路径：有 PIN 时追加 ?pin=（RFC3986 百分号编码——
 * 官方 PIN 对话框不限字符集，用户可能输入非字母数字，不编码会让请求行坏掉）。 */
static void build_prepare_path(char *out, int cap)
{
    const char *p;
    int o;
    if (!g_j.pin_code[0]) {
        snprintf(out, (size_t)cap, "%s", PREPARE_PATH);
        return;
    }
    o = snprintf(out, (size_t)cap, "%s?pin=", PREPARE_PATH);
    if (o < 0) o = 0;
    for (p = g_j.pin_code; *p && o < cap - 4; p++) {
        unsigned char ch = (unsigned char)*p;
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' ||
            ch == '_' || ch == '~')
            out[o++] = (char)ch;
        else
            o += snprintf(out + o, (size_t)(cap - o), "%%%02X", ch);
    }
    if (o > cap - 1) o = cap - 1;
    out[o] = 0;
}

/* 对方回 401：把"要 PIN"写进快照请 UI 弹键盘，然后阻塞等回填
 * （≤ PIN_WAIT_US，每 20ms 查取消）。返回 1=拿到 PIN（已写入 pin_code），
 * 0=用户取消输入 / 等超时 / 传输被取消。 */
static int wait_user_pin(int invalid)
{
    SceLong64 dl = sceKernelGetSystemTimeWide() + PIN_WAIT_US;
    lock();
    g_j.pin_got = 0;
    g_j.v.pin_need = invalid ? 2 : 1;
    unlock();
    set_msg("Waiting for PIN...");
    for (;;) {
        int got;
        lock();
        got = g_j.pin_got;
        if (got == 1) {
            snprintf(g_j.pin_code, sizeof g_j.pin_code, "%s", g_j.pin_in);
            g_j.pin_got = 0;
            g_j.v.pin_need = 0;
            unlock();
            return 1;
        }
        if (got == 2) {
            g_j.pin_got = 0;
            g_j.v.pin_need = 0;
        }
        unlock();
        if (got == 2) return 0;
        if (g_j.cancel) {
            lock();
            g_j.v.pin_need = 0;
            unlock();
            return 0;
        }
        if (sceKernelGetSystemTimeWide() >= dl) {
            lock();
            g_j.v.pin_need = 0;
            unlock();
            dlog("xfer: pin wait timeout");
            return 0;
        }
        sceKernelDelayThread(POLL_US);
    }
}

static int xfer_thr(SceSize args, void *argp)
{
    char *body = NULL;
    char *prep = NULL;              /* prepare 应答缓冲（堆上，容大文件列表） */
    int code = 0, rc, i, attempt;
    (void)args; (void)argp;
    dlog("xfer: thread entered, target %s:%d (%s)%s, %d file(s)",
         g_j.ip, g_j.port, g_j.tls ? "https" : "http",
         g_j.tls ? " tls" : "", g_j.count);

    /* 阶段 0（可选）：发送前算 sha256 —— prepare 里要随请求带上，所以只能先算完
     * 再发。代价实测约 12 s / 100MB（读+哈希流水 8.41MiB/s），大文件用户可关。 */
    if (g_cfg.send_sha && g_j.v.total > 0) {
        set_msg("Calculating checksum...");
        if (hash_phase() != 0) { finish_cancelled(); goto out; }
    }

    /* 阶段 1：prepare-upload（长等接收方决定）。401 = 对方要求 PIN：
     * 挂起等 UI 用系统键盘输入后带 ?pin= 重发（协议语义），最多 PIN_MAX_TRY 次。 */
    set_msg("Waiting for %s to accept...", g_j.ip);
    body = build_prepare_json();
    if (!body) { fail("cannot build prepare request"); goto out; }
    prep = (char *)malloc(RSP_BUF);
    if (!prep) { fail("out of memory"); goto out; }
    for (attempt = 0; ; attempt++) {
        char path[256];      /* 路径 + "?pin=" + 百分号编码后最长 ~3×32 字节 */
        Conn c;
        build_prepare_path(path, (int)sizeof path);
        memset(&c, 0, sizeof c);
        c.fd = -1;
        c.tls = g_j.tls;
        rc = conn_open(&c, path, (SceOff)strlen(body));
        if (rc == 0)
            rc = conn_send_all(&c, body, (int)strlen(body), NULL, NULL);
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
        if (code != 401) break;
        if (attempt + 1 >= PIN_MAX_TRY) {
            fail("receiver PIN incorrect (HTTP 401)");
            goto out;
        }
        if (!wait_user_pin(attempt > 0)) {
            if (g_j.cancel) { finish_cancelled(); goto out; }
            fail("receiver requires a PIN");
            goto out;
        }
        dlog("xfer: retry prepare with pin (%d/%d)", attempt + 2, PIN_MAX_TRY);
        set_msg("Waiting for %s to accept...", g_j.ip);
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
        else if (code == 429) fail("receiver is busy (HTTP 429, too many requests)");
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

    /* 阶段 2：多文件并发上传（并发数 = config 的 maxParallel，钳到文件数；
     * 1 即退回逐文件串行）。每文件一条独立连接、独立成败：
     * - 对端只接受子集（回执没给该文件 token）→ 只标这个文件失败，继续发别的；
     * - 单文件失败（打不开/读失败/文件变短、HTTP 4xx/5xx）→ 同上；
     * - 连接级失败（连不上 / 握手失败 / 中途断流 / 等不到回执）→ 判"对端不在"，
     *   置 abort 全局中止（在途 worker 顺着轮询退出），并把还没完成的文件一起
     *   标失败收尾。
     * 终态一律在 join 完全部 worker 之后由本线程写一次（worker 不碰 terminated 类
     * 字段，见文件头注）。 */
    {
        SceUID wt[PARALLEL_MAX];
        int nw = g_cfg.max_parallel, k, started = 0;
        if (nw < PARALLEL_MIN) nw = PARALLEL_MIN;
        if (nw > PARALLEL_MAX) nw = PARALLEL_MAX;
        if (nw > g_j.count) nw = g_j.count;
        g_j.parallel = nw;              /* upload_one 据此决定状态行文案（1=串行） */
        if (nw > 1)
            set_msg("Sending %d file(s) in parallel...", g_j.count);
        for (k = 0; k < nw; k++) {
            SceUID t = sceKernelCreateThread("psvsend_xferw", upload_worker,
                                             0x40, 0x20000, 0, 0, NULL);
            if (t < 0) {
                dlog("xfer: worker create fail 0x%08X (%d/%d)",
                     (unsigned)t, k + 1, nw);
                break;
            }
            if (sceKernelStartThread(t, 0, NULL) < 0) {
                dlog("xfer: worker start fail (%d/%d)", k + 1, nw);
                sceKernelDeleteThread(t);
                break;
            }
            wt[started++] = t;
        }
        if (started == 0) { fail("cannot start transfer worker"); goto out; }
        dlog("xfer: %d worker(s) started for %d file(s)", started, g_j.count);
        /* join 全部 worker 再往下走：此后没有别的线程还在动 g_j，终态写入与下一次
         * xfer_start 的 memset 才不会撞车。worker 的每个等待都有上限（连接 6s /
         * 握手 20s / 单块发送 10s / 回执 30s）且每轮都查取消与中止标志，正常秒级
         * 就到齐；这里按 250ms 轮询而不是无限等待，真出意外也不会把 xfer 线程
         * 永远挂住（谁都没结束时至少还能从日志看出卡在哪一路）。 */
        for (k = 0; k < started; k++) {
            int waited = 0;
            for (;;) {
                SceUInt to = 250 * 1000;
                if (sceKernelWaitThreadEnd(wt[k], NULL, &to) == 0) break;
                if (++waited % 20 == 0)      /* 每 5s 报一次，别刷屏 */
                    dlog("xfer: worker %d join slow (%ds)", k + 1, waited / 4);
            }
            sceKernelDeleteThread(wt[k]);
        }
    }
    dlog("xfer: all workers joined");
    if (g_j.cancel) { finish_cancelled(); goto out; }   /* 内含 cancel 通知兜底 */

    /* 连接级失败（对端不在）：把还没派发、以及在途被中止打断的文件一起标失败，
     * 再按整批失败收尾。上面已 join 完全部 worker，这里没有别的写者，直接读即可。 */
    if (g_j.abort) {
        int nfail = 0;
        for (i = 0; i < g_j.count; i++) {
            if (g_j.v.f[i].state == 0 || g_j.v.f[i].state == 1)
                set_file_state(i, 3, g_j.v.f[i].sent);
            if (g_j.v.f[i].state == 3) nfail++;
        }
        fail("%s", g_j.abort_msg[0] ? g_j.abort_msg : "peer unreachable");
        dlog("xfer: aborted, %d/%d file(s) failed", nfail, g_j.count);
        goto out;
    }

    /* 全部文件处理完：按"有没有失败文件"定终态（部分失败的展示交给页面：
     * 页面按行状态自己区分"完成但有 N 个失败"与"全部失败"）。 */
    {
        int nfail = 0;
        lock();
        for (i = 0; i < g_j.count; i++)
            if (g_j.v.f[i].state == 3) nfail++;
        g_j.v.finished = true;
        g_j.v.active = false;
        g_j.v.cur = -1;
        g_j.v.ok = (nfail == 0);
        if (nfail == 0) snprintf(g_j.v.msg, sizeof g_j.v.msg, "Complete");
        unlock();
        dlog("xfer: all files processed, %d/%d failed", nfail, g_j.count);
    }
    goto out;
out:
    free(body);
    free(prep);
    /* 线程自删：线程结束若不删，线程对象连着 128KB 栈一直挂在 DORMANT 上，
     * 而 g_j.th 下次 xfer_start 会被 memset 覆盖 → 句柄彻底丢失、再也收不回。 */
    sceKernelExitDeleteThread(0);
    return 0;
}

/* ---------- 对外接口 ---------- */
int xfer_start(const char *ip, int port, const char *proto, const char *fp,
               const XferFile *files, int n)
{
    int i;
    if (g_mtx < 0)
        g_mtx = sceKernelCreateMutex("psvsend_xfer", 0, 0, NULL);
    lock();
    if (g_j.v.active) {
        /* 已有传输在跑（含"上一次刚取消、线程还在收尾"的窗口）：只报失败返回，
         * 绝不往 g_j.v 里写——那是正在跑的那一次的共享快照，写 finished/ok 会把
         * 在途任务误标成已失败，调用方若照旧切页还会显示上一批的清单。 */
        unlock();
        dlog("xfer: refused, another transfer is still active");
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
        /* 名字按 UTF-8 边界截断：128 字节装不下的长名很常见，若切在半个汉字上，
         * prepare 的 JSON 里就是非法序列，对端解析会出错。 */
        str_copy_utf8(g_j.files[i].name, (int)sizeof g_j.files[i].name, files[i].name);
        strncpy(g_j.files[i].path, files[i].path, sizeof g_j.files[i].path - 1);
        g_j.files[i].size = files[i].size;
        str_copy_utf8(g_j.v.f[i].name, (int)sizeof g_j.v.f[i].name, files[i].name);
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

void xfer_set_pin(const char *pin)
{
    lock();
    snprintf(g_j.pin_in, sizeof g_j.pin_in, "%s", pin ? pin : "");
    g_j.pin_got = 1;
    g_j.v.pin_need = 0;     /* 立刻消掉提示：UI 每帧据此决定是否弹键盘，别重复弹 */
    unlock();
}

void xfer_pin_cancel(void)
{
    lock();
    g_j.pin_got = 2;
    g_j.v.pin_need = 0;
    unlock();
}

void xfer_info(XferInfo *out)
{
    lock();
    if (out) *out = g_j.v;
    unlock();
}

bool xfer_active(void)
{
    bool a;
    lock();
    a = g_j.v.active;
    unlock();
    return a;
}
