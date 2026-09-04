/* receive.c —— 接收方向实现（LocalSend v2 上传 API 的服务端会话，见 receive.h）。
 *
 * 线程模型：所有 recv_http_* 由 http.c 的 http_thr 单线程顺序调用（prepare 阻塞
 * 等 UI 决定期间 http 线程被占用——这正是"单活动会话"约束的天然实现）。UI 线程
 * 通过 *_pull/_decide/_abort 访问同一份锁内状态。
 *
 * 边界处理（用户约定）：
 *  - 大文件绝不全量入内存：upload 由 http.c 提供流读回调，边收边写 .part 临时文件，
 *    收完（sha256 可选校验通过后）改名成正式文件；
 *  - 磁盘空间不足：每次开收前 devctl 查 ux0: 剩余，不够直接失败；
 *  - 中断残留：断流/校验失败/取消都立即删当前 .part；开机 recv_init 再清扫一遍
 *    上次崩溃遗留的 *.part。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/processmgr.h>
#include <mbedtls/sha256.h>
#include "receive.h"
#include "json_util.h"
#include "config.h"
#include "dlog.h"

#define RECV_DECIDE_TIMEOUT_US (60 * 1000000LL)  /* prepare 等 UI 决定的上限 */
#define RECV_IDLE_TIMEOUT_US   (120 * 1000000LL) /* 接受后/文件间没动静 → TIMEOUT */
#define RECV_CHUNK             65536             /* 流式写盘缓冲（静态，不占栈） */
#define RECV_PART_SUFFIX       ".part"

/* 会话生命周期：无 / 待决定 / 活动（接受后直到 recv_clear） */
enum { PH_NONE = 0, PH_PENDING, PH_ACTIVE };

/* 一个已接受的待收文件 */
typedef struct {
    char fileid[160];     /* 对方文件 ID（upload query 用） */
    char token[72];       /* 我方签发给对方的令牌 */
    char name[192];       /* 落盘用名（已 sanitize + 冲突排重） */
    char final[300];      /* 正式路径（收完改名到这儿） */
    char part[300];       /* 临时路径（.part） */
    SceOff size;          /* 期望字节数 */
    SceOff got;           /* 已收字节（锁保护） */
    char sha256[65];      /* 期望 sha256（准备报文里给了才校验，空串=不校验） */
    int  st;              /* 0=等待 1=完成 2=跳过 3=失败 */
} RFile;

/* 待决定请求（PH_PENDING） */
static struct {
    char alias[64];
    char type[24];
    char ip[16];
    int  n;
    struct { char fileid[160]; char name[192]; SceOff size; char sha256[65]; } f[RECV_MAX_FILES];
    bool inc[RECV_MAX_FILES];   /* UI 勾选（默认全选） */
} g_pend;

/* 活动会话（PH_ACTIVE） */
static struct {
    char session[40];     /* sessionId（hex） */
    char peer_ip[16];
    char peer_alias[64];
    int  n;               /* 参与文件数（已剔除未勾选） */
    RFile f[RECV_MAX_FILES];
    int  cur;             /* 正在收的下标，-1=无 */
    int  state;           /* RecvStateId */
    char err[192];
    SceOff total;         /* 期望总字节（含 size==0 的已免收文件） */
    SceOff got_total;     /* 已收总字节（锁保护） */
    uint64_t start_us;    /* 接受时刻 */
    uint64_t last_us;     /* 接受/最近一次收体开始时刻（空闲超时判定用） */
    bool busy;            /* 正在流式收体（http 线程内） */
} g_sess;

static int  g_phase = PH_NONE;      /* 当前生命周期 */
static int  g_pend_result = 0;      /* 0=未决 1=接受 2=拒绝 3=超时 */
static volatile int g_abort = 0;    /* 用户中止请求（http 收体循环轮询） */
static SceUID g_mtx = -1;
static unsigned char g_buf[RECV_CHUNK];

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }
static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

/* 随机 hex（sessionId/token） */
static void gen_hex(char *out, int bytes)
{
    int i;
    uint64_t t = (uint64_t)sceKernelGetSystemTimeWide();
    uint64_t p = (uint64_t)sceKernelGetProcessTimeWide();
    for (i = 0; i < bytes; i++) {
        if ((i % 8) == 0) t = t * 6364136223846793005ULL + (p ^ (i + 1));
        snprintf(out + i * 2, 3, "%02x",
                 (unsigned)((t >> ((unsigned)((i * 5) % 56))) & 0xFF));
    }
    out[bytes * 2] = 0;
}

/* 把对方给的原始文件名净化成可落盘的单级名字 */
static void sanitize_name(const char *in, char *out, int n)
{
    int o = 0, i;
    bool any = false;
    const char *slash;
    if (!in) in = "";
    slash = strrchr(in, '/');
    if (slash) in = slash + 1;
    while (*in == ' ') in++;                     /* 去前导空白 */
    for (i = 0; *in && o < n - 1 && i < 160; i++, in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        out[o++] = (char)c;
        if (c != '_') any = true;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;  /* 去尾空白/点 */
    out[o] = 0;
    if (!any || !out[0] || strcmp(out, ".") == 0 || strcmp(out, "..") == 0)
        snprintf(out, n, "unnamed");
}

/* 拆出 "base" 与扩展名 ".ext"（无扩展则 ext=""） */
static void split_ext(const char *name, char *base, int bn, const char **ext)
{
    const char *dot = strrchr(name, '.');
    int bl;
    if (dot && dot != name) {
        bl = (int)(dot - name);
        if (bl > bn - 1) bl = bn - 1;
        memcpy(base, name, (unsigned)bl);
        base[bl] = 0;
        *ext = dot;
    } else {
        snprintf(base, bn, "%s", name);
        *ext = "";
    }
}

/* 生成唯一正式名路径 + 对应 .part 路径。已有同名则在扩展名前插 " (k)"。 */
static void alloc_paths(const char *raw, char *final, int fn,
                        char *part, int pn)
{
    char clean[192], base[192], cand[240];
    const char *ext;
    SceIoStat st;
    int k = 0;
    sanitize_name(raw, clean, sizeof clean);
    split_ext(clean, base, sizeof base, &ext);
    for (;;) {
        if (k == 0) snprintf(cand, sizeof cand, "%s%s", base, ext);
        else        snprintf(cand, sizeof cand, "%s (%d)%s", base, k, ext);
        snprintf(final, fn, PSVSEND_DL_DIR "/%s", cand);
        if (sceIoGetstat(final, &st) < 0) break;   /* 不存在 → 可用 */
        if (++k > 999) {                            /* 兜底：保底名字 */
            snprintf(cand, sizeof cand, "file-%llx%s",
                     (unsigned long long)now_us(), ext);
            snprintf(final, fn, PSVSEND_DL_DIR "/%s", cand);
            break;
        }
    }
    snprintf(part, pn, "%s%s", final, RECV_PART_SUFFIX);
}

/* 磁盘剩余（ux0: 挂载点 devctl 0x3001 返回 SceIoDevInfo）。查不到返回 -1。 */
static int disk_free(SceOff *free_size)
{
    SceIoDevInfo di;
    memset(&di, 0, sizeof di);
    if (sceIoDevctl("ux0:", 0x3001, NULL, 0, &di, sizeof di) == 0) {
        *free_size = di.free_size;
        return 0;
    }
    return -1;
}

/* 删掉还没完成/失败的文件的 .part（会话结束清理用；持有锁时调用） */
static void cleanup_parts(void)
{
    int i;
    for (i = 0; i < g_sess.n; i++)
        if (g_sess.f[i].st != 1) sceIoRemove(g_sess.f[i].part);
}

/* 结束活动会话到终态（持有锁时调用） */
static void sess_terminal(int state, const char *err)
{
    g_sess.state = state;
    g_sess.cur = -1;
    g_sess.busy = false;
    g_sess.last_us = now_us();
    if (err) {
        snprintf(g_sess.err, sizeof g_sess.err, "%s", err);
        dlog("recv: session end st=%d err=%s", state, err);
    }
}

/* ---------- URL query 小解析 ---------- */

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void pct_decode(const char *s, char *out, int outsz)
{
    int o = 0;
    while (*s && o < outsz - 1) {
        if (s[0] == '%' && hexval(s[1]) >= 0 && hexval(s[2]) >= 0) {
            out[o++] = (char)((hexval(s[1]) << 4) | hexval(s[2]));
            s += 3;
        } else {
            out[o++] = *s++;
        }
    }
    out[o] = 0;
}

static void query_get(const char *q, const char *key, char *out, int outsz)
{
    size_t klen = strlen(key);
    out[0] = 0;
    while (q && *q) {
        const char *amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg > klen && strncmp(q, key, klen) == 0 && q[klen] == '=') {
            char val[700];
            size_t vlen = seg - klen - 1;
            if (vlen > sizeof val - 1) vlen = sizeof val - 1;
            memcpy(val, q + klen + 1, vlen);
            val[vlen] = 0;
            pct_decode(val, out, outsz);
            return;
        }
        if (!amp) break;
        q = amp + 1;
    }
}

/* ---------- 后端入口 ---------- */

void recv_init(void)
{
    SceUID d;
    SceIoDirent de;
    char p[512];
    size_t sl = strlen(RECV_PART_SUFFIX);
    if (g_mtx < 0)
        g_mtx = sceKernelCreateMutex("psvsend_recv", 0, 1, NULL);
    if (g_mtx < 0) return;
    lock();
    g_phase = PH_NONE;
    g_abort = 0;
    unlock();
    /* 清扫上次异常退出残留的 .part（接收中断只会留下 .part，不会出正式文件） */
    d = sceIoDopen(PSVSEND_DL_DIR);
    if (d >= 0) {
        memset(&de, 0, sizeof de);
        while (sceIoDread(d, &de) > 0) {
            size_t l = strlen(de.d_name);
            if (l > sl &&
                strcmp(de.d_name + l - sl, RECV_PART_SUFFIX) == 0) {
                snprintf(p, sizeof p, PSVSEND_DL_DIR "/%s", de.d_name);
                sceIoRemove(p);
                dlog("recv: init removed stale %s", de.d_name);
            }
        }
        sceIoDclose(d);
    }
}

int recv_pending_pull(RecvPending *out)
{
    int i, r = 0;
    lock();
    if (g_phase == PH_PENDING) {
        if (out) {                   /* out==NULL：纯探测，只回答"有没有" */
        snprintf(out->peer_alias, sizeof out->peer_alias, "%s", g_pend.alias);
        snprintf(out->peer_type,  sizeof out->peer_type,  "%s", g_pend.type);
        snprintf(out->peer_ip,    sizeof out->peer_ip,    "%s", g_pend.ip);
        out->count = g_pend.n;
        out->total = 0;
        for (i = 0; i < g_pend.n && i < RECV_MAX_FILES; i++) {
            snprintf(out->files[i].name, sizeof out->files[i].name, "%s",
                     g_pend.f[i].name);
            out->files[i].size = g_pend.f[i].size;
            out->total += g_pend.f[i].size;
        }
        }                        /* if (out) */
        r = 1;
    }
    unlock();
    return r;
}

void recv_set_include(const bool inc[RECV_MAX_FILES])
{
    int i;
    lock();
    if (g_phase == PH_PENDING)
        for (i = 0; i < g_pend.n; i++) g_pend.inc[i] = inc && inc[i];
    unlock();
}

void recv_decide(bool accept)
{
    lock();
    if (g_phase == PH_PENDING && g_pend_result == 0)
        g_pend_result = accept ? 1 : 2;
    unlock();
}

void recv_abort(void)
{
    lock();
    if (g_phase == PH_ACTIVE && g_sess.busy) {
        g_abort = 1;                       /* 收体循环下一块检测到 */
    } else if (g_phase == PH_ACTIVE &&
               g_sess.state != RECV_ST_DONE && g_sess.state != RECV_ST_FAIL &&
               g_sess.state != RECV_ST_CANCEL && g_sess.state != RECV_ST_TIMEOUT) {
        cleanup_parts();
        sess_terminal(RECV_ST_CANCEL, "用户取消");
    }
    unlock();
}

/* http 层收体轮询用：有未决的"用户中止"请求就尽快收尾（无锁读 volatile） */
bool recv_abort_pending(void)
{
    return g_abort != 0;
}

/* prepare-upload：解析 → 挂起等 UI（≤60s）→ 回 HTTP 码并填 resp（200 时 JSON） */
int recv_http_prepare(const char *body, const char *ip, char *resp, int respsz)
{
    const char *infov, *filesv, *p;
    JsonIter it;
    char tmp[192];
    long long sz;
    int i, n = 0, code = 200, r;
    SceLong64 dl;
    bool pend_accept;

    if (respsz > 0) resp[0] = 0;
    lock();
    r = (g_phase == PH_ACTIVE);
    unlock();
    if (r) return 409;                       /* 已有活动会话（含终态未清场） */

    infov = json_get_val(body, "info");
    filesv = json_get_val(body, "files");
    if (!infov || !filesv) return 400;       /* 坏体 */

    lock();
    g_pend.alias[0] = 0;
    g_pend.type[0] = 0;
    snprintf(g_pend.ip, sizeof g_pend.ip, "%s", ip ? ip : "");
    json_get_str(infov, "alias", g_pend.alias, sizeof g_pend.alias);
    json_get_str(infov, "deviceType", g_pend.type, sizeof g_pend.type);
    if (!g_pend.alias[0]) snprintf(g_pend.alias, sizeof g_pend.alias, "%s", ip);

    n = 0;
    if (json_iter_first(filesv, &it)) {
        do {
            if (n >= RECV_MAX_FILES) break;   /* 超过上限：忽略多出来的文件，
                                               * 回执只列前 RECV_MAX_FILES 个（协议允许只收子集） */
            snprintf(g_pend.f[n].fileid, sizeof g_pend.f[n].fileid, "%s", it.key);
            g_pend.f[n].name[0] = 0;
            g_pend.f[n].size = 0;
            g_pend.f[n].sha256[0] = 0;
            p = json_get_val(it.val, "fileName");
            if (p) json_val_str(p, g_pend.f[n].name, sizeof g_pend.f[n].name);
            p = json_get_val(it.val, "size");
            if (p && json_val_int(p, &sz)) g_pend.f[n].size = (SceOff)sz;
            p = json_get_val(it.val, "sha256");
            if (p && json_val_str(p, tmp, sizeof tmp) && strlen(tmp) == 64)
                snprintf(g_pend.f[n].sha256, sizeof g_pend.f[n].sha256, "%s", tmp);
            if (!g_pend.f[n].name[0] || g_pend.f[n].size < 0)
                continue;                    /* 条目不可用 → 跳过 */
            g_pend.inc[n] = true;
            n++;
        } while (code == 200 && json_iter_next(&it));
    }
    if (code != 200) {
        unlock();
        return 400;
    }
    if (n == 0) {                            /* 没有要传的文件 */
        g_phase = PH_NONE;
        unlock();
        return 204;
    }
    g_pend.n = n;
    g_pend_result = 0;
    g_phase = PH_PENDING;
    dlog("recv: prepare %d files from %s (%s)", n, g_pend.alias, g_pend.ip);
    unlock();

    /* 等 UI 决定：轮询（http 线程单线程，这里睡着期间不再接别的连接） */
    dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
    for (;;) {
        lock();
        r = g_pend_result;
        unlock();
        if (r) break;
        if ((SceLong64)now_us() >= dl) { r = 3; break; }
        sceKernelDelayThread(50 * 1000);
    }

    lock();
    r = g_pend_result;                       /* 醒来后再取一次，可能 UI 刚决定 */
    pend_accept = (r == 1);
    if (pend_accept) {
        /* 组装活动会话：只收 UI 勾选的文件；未勾选直接跳过 */
        char eid[512];
        int k, m = 0, has_pending = 0;
        SceOff tot = 0;
        memset(&g_sess, 0, sizeof g_sess);
        g_phase = PH_ACTIVE;
        gen_hex(g_sess.session, 16);         /* 32 hex */
        snprintf(g_sess.peer_ip, sizeof g_sess.peer_ip, "%s", g_pend.ip);
        snprintf(g_sess.peer_alias, sizeof g_sess.peer_alias, "%s", g_pend.alias);
        for (i = 0; i < g_pend.n && m < RECV_MAX_FILES; i++) {
            if (!g_pend.inc[i]) continue;
            RFile *f = &g_sess.f[m];
            memset(f, 0, sizeof *f);
            snprintf(f->fileid, sizeof f->fileid, "%s", g_pend.f[i].fileid);
            gen_hex(f->token, 16);
            snprintf(f->name, sizeof f->name, "%s", g_pend.f[i].name);
            f->size = g_pend.f[i].size;
            snprintf(f->sha256, sizeof f->sha256, "%s", g_pend.f[i].sha256);
            if (f->size == 0) {
                f->st = 1;                   /* 空文件无需收体，视为已完成 */
            } else {
                alloc_paths(f->name, f->final, sizeof f->final,
                            f->part, sizeof f->part);
                has_pending = 1;
            }
            tot += f->size;
            m++;
        }
        g_sess.n = m;
        g_sess.total = tot;
        g_sess.got_total = 0;
        g_sess.cur = -1;
        g_sess.start_us = now_us();
        g_sess.last_us = g_sess.start_us;
        g_sess.busy = false;
        g_abort = 0;
        if (m == 0) {                        /* 全被勾掉：告诉对方不用传 */
            g_phase = PH_NONE;
            code = 204;
        } else {
            g_sess.state = has_pending ? RECV_ST_READY : RECV_ST_DONE;
            if (!has_pending) g_sess.err[0] = 0;
            /* 回执 JSON：{sessionId, files{fileId:token}} */
            int o = 0;
            o += snprintf(resp + o,
                          (size_t)(respsz > o ? respsz - o : 0),
                          "{\"sessionId\":\"%s\",\"files\":{", g_sess.session);
            for (k = 0; k < m; k++) {
                json_escape(g_sess.f[k].fileid, eid, sizeof eid);
                if (k > 0)
                    o += snprintf(resp + o,
                                  (size_t)(respsz > o ? respsz - o : 0), ",");
                o += snprintf(resp + o,
                              (size_t)(respsz > o ? respsz - o : 0),
                              "\"%s\":\"%s\"", eid, g_sess.f[k].token);
            }
            snprintf(resp + o, (size_t)(respsz > o ? respsz - o : 0), "}}");
            code = 200;
        }
        dlog("recv: accept session=%s files=%d from %s",
             g_sess.session, m, g_sess.peer_alias);
    } else {
        g_phase = PH_NONE;
        code = 403;                          /* 拒绝 / 超时未决 */
        dlog("recv: prepare %s from %s", r == 2 ? "rejected" : "timeout",
             g_pend.alias);
    }
    g_pend_result = 0;
    unlock();
    return code;
}

/* 小写 hex 比较（期望值可能大写） */
static bool hex_eq_nocase(const char *a, const unsigned char *b, int n)
{
    int i;
    static const char *hexd = "0123456789abcdef";
    for (i = 0; i < n; i++) {
        int hi = (b[i] >> 4) & 0xF, lo = b[i] & 0xF;
        if (a[2 * i] != hexd[hi] && a[2 * i] != (char)(hexd[hi] - 'a' + 'A'))
            return false;
        if (a[2 * i + 1] != hexd[lo] &&
            a[2 * i + 1] != (char)(hexd[lo] - 'a' + 'A'))
            return false;
    }
    return true;
}

/* 标记会话整体失败并清理残留 .part（持有锁时调用；fd 由调用点先关闭） */
static void sess_fail_locked(const char *err)
{
    cleanup_parts();
    sess_terminal(RECV_ST_FAIL, err);
}

/* upload：校验 query + 来源 IP → 经 fn 流式收 total 字节写盘。
 * fn 每次返回 ≤ max；0=对端关闭/空闲超时，<0=socket 错误。 */
int recv_http_upload(const char *query, const char *ip, SceOff total,
                     recv_stream_fn fn, void *ctx)
{
    char sid[64], fid[170], tok[72];
    SceOff free_space;
    RFile *f = NULL;
    int idx = -1, i;
    SceUID fd = -1;
    bool check_sha = false, aborted = false;
    mbedtls_sha256_context hctx;
    unsigned char digest[32];

    if (!query || !fn || !ctx) return 400;
    query_get(query, "sessionId", sid, sizeof sid);
    query_get(query, "fileId", fid, sizeof fid);
    query_get(query, "token", tok, sizeof tok);
    if (!sid[0] || !fid[0] || !tok[0]) return 400;

    lock();
    if (g_phase == PH_ACTIVE && g_sess.session[0] &&
        strcmp(g_sess.session, sid) == 0 &&
        g_sess.state != RECV_ST_DONE && g_sess.state != RECV_ST_FAIL &&
        g_sess.state != RECV_ST_CANCEL && g_sess.state != RECV_ST_TIMEOUT) {
        if (!g_sess.peer_ip[0] || strcmp(g_sess.peer_ip, ip) == 0)
            for (i = 0; i < g_sess.n; i++)
                if (strcmp(g_sess.f[i].fileid, fid) == 0) { idx = i; break; }
        if (idx >= 0 && strcmp(g_sess.f[idx].token, tok) != 0) idx = -2;
    }
    if (idx < 0) {
        unlock();
        return 403;                          /* 会话/令牌/IP 不符 */
    }
    f = &g_sess.f[idx];
    if (f->st == 1) {                        /* 重复上传：空文件幂等成功 */
        unlock();
        return f->size == 0 ? 200 : 403;
    }
    if (total >= 0 && total != f->size) {    /* Content-Length 与清单不符（无 CL 时 total=-1，读完再验） */
        sess_fail_locked("文件大小与清单不符");
        unlock();
        return 400;
    }
    /* 磁盘预检（查不到就交给写失败兜底） */
    if (disk_free(&free_space) == 0 && free_space < f->size) {
        sess_fail_locked("存储空间不足");
        unlock();
        return 500;
    }
    g_sess.busy = true;
    g_sess.cur = idx;
    g_sess.state = RECV_ST_RECEIVING;
    g_sess.last_us = now_us();
    f->got = 0;
    check_sha = f->sha256[0] != 0;
    unlock();

    if (check_sha) {
        mbedtls_sha256_init(&hctx);
        mbedtls_sha256_starts(&hctx, 0);
    }

    fd = sceIoOpen(f->part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        dlog("recv: open part fail 0x%08X (%s)", (unsigned)fd, f->part);
        lock();
        sess_fail_locked("无法创建文件");
        unlock();
        if (check_sha) mbedtls_sha256_free(&hctx);
        return 500;
    }

    for (;;) {
        int n, wr;
        if (f->got >= f->size) break;  /* 先确认收满：完整数据绝不做取消清理 */
        lock();
        if (g_abort) aborted = true;
        unlock();
        if (aborted) {
            sceIoClose(fd); fd = -1;
            lock();
            cleanup_parts();
            sess_terminal(RECV_ST_CANCEL, "用户取消");
            unlock();
            if (check_sha) mbedtls_sha256_free(&hctx);
            sceIoRemove(f->part);
            return 500;
        }
        if (f->got >= f->size) break;
        {
            SceOff rem = f->size - f->got;
            n = fn(ctx, g_buf, (int)(rem < RECV_CHUNK ? rem : RECV_CHUNK));
        }
        if (n <= 0) {                        /* 断流/空闲超时/socket 错/用户中止 */
            sceIoClose(fd); fd = -1;
            lock();
            if (g_abort) {                   /* http 层看到中止标志提前退出 → 取消 */
                cleanup_parts();
                sess_terminal(RECV_ST_CANCEL, "用户取消");
            } else {
                sess_fail_locked(f->got > 0 ? "传输中断" : "对方未发送数据");
            }
            unlock();
            if (check_sha) mbedtls_sha256_free(&hctx);
            sceIoRemove(f->part);
            return 500;
        }
        if (check_sha) mbedtls_sha256_update(&hctx, g_buf, (unsigned)n);
        wr = sceIoWrite(fd, g_buf, (unsigned)n);
        if (wr != n) {                       /* 磁盘满 / IO 错误 */
            sceIoClose(fd); fd = -1;
            lock();
            sess_fail_locked("写入失败（磁盘空间不足？）");
            unlock();
            if (check_sha) mbedtls_sha256_free(&hctx);
            sceIoRemove(f->part);
            return 500;
        }
        lock();
        f->got += n;
        g_sess.got_total += n;
        unlock();
    }

    if (check_sha) {
        mbedtls_sha256_finish(&hctx, digest);
        mbedtls_sha256_free(&hctx);
    }
    sceIoClose(fd); fd = -1;
    if (check_sha && !hex_eq_nocase(f->sha256, digest, 32)) {
        lock();
        sess_fail_locked("sha256 校验失败");
        unlock();
        sceIoRemove(f->part);
        return 422;
    }
    if (sceIoRename(f->part, f->final) < 0) {
        lock();
        sess_fail_locked("文件落盘失败");
        unlock();
        sceIoRemove(f->part);
        return 500;
    }
    lock();
    f->st = 1;
    g_sess.cur = -1;
    {
        int all = 1;
        for (i = 0; i < g_sess.n; i++)
            if (g_sess.f[i].st == 0) { all = 0; break; }
        if (all) {
            sess_terminal(RECV_ST_DONE, NULL);
            dlog("recv: session %s done (%d files, %lld B)", g_sess.session,
                 g_sess.n, (long long)g_sess.got_total);
        } else if (g_abort) {
            /* 本文件收满后才收到取消：当前文件已完整、保留；
             * 其余未收的文件按取消收尾，会话终态 CANCEL。 */
            cleanup_parts();
            sess_terminal(RECV_ST_CANCEL, "用户取消");
            dlog("recv: session %s cancelled (file done)", g_sess.session);
        } else {
            g_sess.busy = false;
            g_sess.state = RECV_ST_READY;    /* 还有文件没到，等下一个 upload */
            g_sess.last_us = now_us();
        }
    }
    unlock();
    return 200;
}

int recv_http_cancel(const char *query, const char *ip)
{
    char sid[64];
    (void)ip;
    query_get(query, "sessionId", sid, sizeof sid);
    lock();
    if (g_phase == PH_ACTIVE && sid[0] &&
        strcmp(g_sess.session, sid) == 0) {
        cleanup_parts();
        sess_terminal(RECV_ST_CANCEL, "发送方取消");
        dlog("recv: session %s cancelled by sender", g_sess.session);
    }
    unlock();
    return 200;                              /* 协议：cancel 恒回 200 */
}

int recv_status_pull(RecvStatus *out)
{
    int i, r = 0;
    uint64_t now = now_us();
    if (!out) return 0;
    lock();
    if (g_phase == PH_ACTIVE) {
        /* 空闲超时：非收体中且超过 IDLE 无动静 → TIMEOUT（清残留） */
        if (!g_sess.busy &&
            g_sess.state != RECV_ST_DONE && g_sess.state != RECV_ST_FAIL &&
            g_sess.state != RECV_ST_CANCEL && g_sess.state != RECV_ST_TIMEOUT &&
            now - g_sess.last_us > (uint64_t)RECV_IDLE_TIMEOUT_US) {
            cleanup_parts();
            sess_terminal(RECV_ST_TIMEOUT, "等待对方传输超时");
        }
        out->state = g_sess.state;
        snprintf(out->peer_alias, sizeof out->peer_alias, "%s", g_sess.peer_alias);
        out->count = g_sess.n;
        out->cur = g_sess.cur;
        out->total = g_sess.total;
        out->got_total = g_sess.got_total;
        out->start_us = g_sess.start_us;
        snprintf(out->err, sizeof out->err, "%s", g_sess.err);
        for (i = 0; i < g_sess.n && i < RECV_MAX_FILES; i++) {
            snprintf(out->name[i], sizeof out->name[0], "%s", g_sess.f[i].name);
            out->size[i] = g_sess.f[i].size;
            out->got[i] = g_sess.f[i].got < g_sess.f[i].size
                              ? g_sess.f[i].got : g_sess.f[i].size;
        }
        r = 1;
    }
    unlock();
    return r;
}

void recv_clear(void)
{
    lock();
    if (g_phase == PH_ACTIVE) {
        if (g_sess.busy) {                   /* 收体进行中：别硬清，先请中止 */
            g_abort = 1;
            unlock();
            return;
        }
        cleanup_parts();
    } else if (g_phase == PH_PENDING) {
        g_pend_result = 2;                   /* http 等待循环醒来按拒绝处理 */
    }
    g_phase = PH_NONE;
    g_abort = 0;
    unlock();
}
