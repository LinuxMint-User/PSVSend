/* receive.c —— 接收方向实现（LocalSend v2 上传 API 的服务端会话，见 receive.h）。
 *
 * 线程模型：http.c 为每个连接开独立 worker 线程并发调用 recv_http_*；本模块
 * 用一个全互斥 g_mtx 保护会话状态。单活动会话（一次只收一个设备）由 prepare 的
 * "检查+占位原子化"保证——会话已存在时另一个 prepare 立刻拿 409，不会被静默晾在
 * TCP 队列里。
 * 会话内多文件并发：官方客户端对同一会话的多个文件是并发发起 upload 的，故每
 * 个 RFile 各自带 busy/状态标志，多个文件可同时流式收体；会话终态按"所有文件
 * 是否都到终态"判定（见 upload 收尾）。跨会话（另一台设备）仍 409。
 * UI 线程通过 *_pull/_decide/_abort 访问同一份锁内状态。
 *
 * 边界处理（用户约定）：
 *  - 大文件绝不全量入内存：upload 由 http.c 提供流读回调，边收边写临时文件
 *    （<正式名>.psvsend.tmp），收完（sha256 可选校验通过后）改名成正式文件；
 *  - 磁盘空间不足：每次开收前 devctl 查 ux0: 剩余，不够直接失败；
 *  - 中断残留：断流/校验失败/取消都立即删当前临时文件；开机 recv_init 再清扫
 *    一遍上次崩溃遗留的 *.psvsend.tmp。 */
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
#include "proto/receive.h"
#include "proto/transfer.h"       /* xfer_active：发送中拒绝接收（礼貌拒绝） */
#include "core/json_util.h"
#include "core/config.h"
#include "core/dlog.h"

#define RECV_DECIDE_TIMEOUT_US (60 * 1000000LL)  /* prepare 等 UI 决定的上限 */
#define RECV_IDLE_TIMEOUT_US   (120 * 1000000LL) /* 接受后/文件间没动静 → TIMEOUT */
#define RECV_CHUNK             65536             /* 流式写盘缓冲（每个 upload 各自一份，见下） */
/* 收体中临时文件名后缀。曾用 ".part"：落盘正式名沿用对端原名，对端若发来一个
 * 正好叫 "x.part" 的文件，它会被正常保存成正式文件，而开机清扫按后缀匹配又会
 * 把它当残留删掉——永久丢数据。改成一个正常文件名里基本不会出现的后缀。
 * 注：不再顺带清扫旧版遗留的 *.part（宁可留垃圾也不误删用户文件）。 */
#define RECV_PART_SUFFIX       ".psvsend.tmp"

/* 会话生命周期：无 / 待决定 / 活动（接受后直到 recv_clear） */
enum { PH_NONE = 0, PH_PENDING, PH_ACTIVE };

/* 一个已接受的待收文件 */
typedef struct {
    char fileid[160];     /* 对方文件 ID（upload query 用） */
    char token[72];       /* 我方签发给对方的令牌 */
    char name[192];       /* 展示用名（sanitize/排重后的落盘名，UI 与实际一致） */
    char final[800];      /* 正式路径（收完改名到这儿；目录可长，缓冲随目录放宽） */
    char part[800];       /* 临时路径（final + RECV_PART_SUFFIX） */
    SceOff size;          /* 期望字节数 */
    SceOff got;           /* 已收字节（锁保护） */
    char sha256[65];      /* 期望 sha256（准备报文里给了才校验，空串=不校验） */
    int  st;              /* 0=等待 1=完成 2=跳过 3=失败 */
    bool busy;            /* 本文件正在流式收体（同会话多文件可并发，各自独立） */
} RFile;

/* 待决定请求（PH_PENDING） */
static struct {
    char alias[64];
    char type[24];
    char ip[16];
    int  n;                  /* 列入清单的文件数（≤ RECV_MAX_FILES） */
    int  overflow;           /* 超上限被丢弃的可用文件数（确认页明示用） */
    struct { char fileid[160]; char name[192]; SceOff size; char sha256[65];
             char rname[192]; bool trunc; } f[RECV_MAX_FILES];
    bool inc[RECV_MAX_FILES];   /* UI 勾选（默认全选） */
} g_pend;

/* 活动会话（PH_ACTIVE） */
static struct {
    char session[40];     /* sessionId（hex） */
    char peer_ip[16];
    char peer_alias[64];
    int  n;               /* 参与文件数（已剔除未勾选） */
    RFile f[RECV_MAX_FILES];
    int  cur;             /* 正在收的某个文件下标，-1=无（并发时取最早遇到的，仅 UI 高亮用） */
    int  state;           /* RecvStateId */
    char err[192];
    SceOff total;         /* 期望总字节（含 size==0 的已免收文件） */
    SceOff got_total;     /* 已收总字节（锁保护） */
    uint64_t start_us;    /* 接受时刻 */
    uint64_t last_us;     /* 接受/最近一次收体开始时刻（空闲超时判定用） */
} g_sess;

static int  g_phase = PH_NONE;      /* 当前生命周期 */
/* prepare 的占位序号：每次占上 PH_PENDING 就 +1。等待 UI 决定的 prepare 只在
 * "序号仍等于自己那次" 时才回写 g_phase/g_pend_result——否则它可能把一个已经
 * 换了新会话的状态改回自己的结论（recv_clear 撤销占位、新 prepare 又占上来的
 * ABA 竞态：老的醒来会把新会话清掉，表现为新确认页点 Accept 无效、60s 后 403）。 */
static unsigned g_gen = 0;
static int  g_pend_result = 0;      /* 0=未决 1=接受 2=拒绝 3=超时 */
static volatile int g_abort = 0;    /* 用户中止请求（http 收体循环轮询） */
static SceUID g_mtx = -1;
/* 本会话保存目录：accept 前由 UI 用 recv_set_dir 设定（临时、不入 config）；
 * 启动清扫等默认场景在 recv_init 里回退 config saveDir。 */
static char g_dir[512];

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }
static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

/* 会话是否已到终态（终态后不再改变，除 recv_clear 清场） */
static bool sess_is_terminal(int state)
{
    return state == RECV_ST_DONE || state == RECV_ST_FAIL ||
           state == RECV_ST_CANCEL || state == RECV_ST_TIMEOUT;
}

/* 是否有任一文件正在流式收体（持锁调用）：同会话多文件可并发，busy 各自独立 */
static bool any_busy_locked(void)
{
    int i;
    for (i = 0; i < g_sess.n; i++)
        if (g_sess.f[i].busy) return true;
    return false;
}

/* 刷新"当前正在收的文件"下标（持锁调用）：并发时取最早遇到的一个，仅供 UI 高亮 */
static void update_cur_locked(void)
{
    int i;
    g_sess.cur = -1;
    for (i = 0; i < g_sess.n; i++)
        if (g_sess.f[i].busy) { g_sess.cur = i; break; }
}

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

/* 落盘名单个名字的字节上限：RFile / RecvPending / RecvStatus 的 name[] 都是 192。
 * ux0: 单文件名上限（exFAT 255 个 UTF-16 码元）远高于此，故这里只受自身缓冲约束。 */
#define RECV_NAME_MAX 191
/* 截断时"值得保留的后缀"最大长度（含 '.'）；比这更长的尾巴不当后缀看待 */
#define RECV_EXT_MAX  16

/* 名字里单个字节的净化：路径分隔符 / 控制字符 → '_'（其余原样，多字节序列各字节
 * 都 ≥0x80、不会被误伤） */
static unsigned char name_byte(unsigned char c)
{
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ||
        c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        return '_';
    return c;
}

/* 返回 s 中不超过 n 字节的最大"完整 UTF-8 字符"前缀长度（不切出半个字） */
static int utf8_floor(const char *s, int n)
{
    int k = n;
    while (k > 0 && ((unsigned char)s[k - 1] & 0xC0) == 0x80) k--;   /* 退到首字节 */
    if (k > 0) {
        unsigned char c = (unsigned char)s[k - 1];
        int need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (k - 1 + need > n) return k - 1;      /* 末字不完整：整个丢掉 */
    }
    return n;
}

/* 把对方给的（或 UI 改名的）名字净化成可落盘的单级文件名。落盘与 UI 改名共用
 * 本函数，保证"行上显示的即落盘名"。
 * 净化只按 ux0: 的合法性（路径分隔符、控制字符、尾点/尾空格、空名），不替
 * Windows 保留名（CON/PRN…）改任何东西——那在 ux0 上完全合法，擅自改只会让
 * 发送端和接收端对不上名。
 * 超长时按用户约定的"保完整后缀、从前往后截断主名"缩短；绝不切出半个多字节
 * 字符（非法 UTF-8 会让 sceIoOpen 直接返 EINVAL、整批接收失败）。 */
void recv_sanitize_name(const char *in, char *out, int n)
{
    int lim, o = 0, i, keep, elen = 0;
    bool any = false;
    const char *slash, *dot = NULL;
    int len;
    if (!in) in = "";
    slash = strrchr(in, '/');
    if (slash) in = slash + 1;
    while (*in == ' ') in++;                     /* 去前导空白 */
    lim = n - 1;
    if (lim > RECV_NAME_MAX) lim = RECV_NAME_MAX;
    if (lim < 1) { out[0] = 0; return; }
    len = (int)strlen(in);
    if (len <= lim) {
        keep = len;                              /* 放得下：原样保留 */
    } else {
        dot = strrchr(in, '.');
        if (dot && dot != in && (int)strlen(dot) <= RECV_EXT_MAX &&
            (int)strlen(dot) < lim)
            elen = (int)strlen(dot);             /* 像真后缀：留着，其余让给主名 */
        else
            dot = NULL;
        keep = lim - elen;
        if (keep < 0) keep = 0;
    }
    /* 边界校正（无条件做）：截断点可能落在多字节字符中间；且上游按固定缓冲拷贝
     * 名字时（name[192]）也可能已经把尾巴截成半个字——那样交出去的仍是非法
     * UTF-8，sceIoOpen 直接返 EINVAL、整批接收失败。 */
    keep = utf8_floor(in, keep);
    for (i = 0; i < keep; i++) {
        unsigned char c = name_byte((unsigned char)in[i]);
        out[o++] = (char)c;
        if (c != '_') any = true;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;  /* 去尾空白/点 */
    if (dot) {                                   /* 拼回后缀（同样净化） */
        for (i = 0; i < elen; i++) {
            unsigned char c = name_byte((unsigned char)dot[i]);
            out[o++] = (char)c;
            if (c != '_') any = true;
        }
    }
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

/* 生成唯一正式名路径 + 对应临时文件路径。同名冲突（磁盘上已存在，或本
 * 会话 prev[0..prev_n) 已分配的 final）则在扩展名前插 " (k)"。
 * prev 中 final 为空串的条目表示未落盘，跳过。
 * 下面的 sceIoGetstat 探测是在 g_mtx 内做的：调用点只有"accept 组装会话"一处，
 * 此时仍是 PH_PENDING（没有并发 upload 会被拖住，UI 的 pending_pull 也只被挡
 * 这一下），正常情况 1~2 次探测就命中，故不为此拆锁。 */
static void alloc_paths(const char *raw, char *final, int fn,
                        char *part, int pn,
                        const RFile *prev, int prev_n)
{
    char clean[192], base[192], cand[240];
    const char *ext;
    SceIoStat st;
    int k = 0, t;
    recv_sanitize_name(raw, clean, sizeof clean);
    split_ext(clean, base, sizeof base, &ext);
    for (;;) {
        if (k == 0) snprintf(cand, sizeof cand, "%s%s", base, ext);
        else        snprintf(cand, sizeof cand, "%s (%d)%s", base, k, ext);
        snprintf(final, fn, "%s/%s", g_dir, cand);
        if (sceIoGetstat(final, &st) < 0) {         /* 磁盘上不存在 */
            for (t = 0; t < prev_n; t++)            /* 本会话也没占用 */
                if (prev[t].final[0] &&
                    strcmp(final, prev[t].final) == 0) break;
            if (t == prev_n) break;                 /* 真正可用 */
        }
        if (++k > 999) {                            /* 兜底：保底名字 */
            snprintf(cand, sizeof cand, "file-%llx%s",
                     (unsigned long long)now_us(), ext);
            snprintf(final, fn, "%s/%s", g_dir, cand);
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

/* 删掉还没完成/失败的文件的临时文件（会话结束/失败清理用；持有锁时调用）。
 * busy 的文件正被另一个 worker 写盘，跳过——由该 worker 自己收尾时再清。
 * ★ 失败/取消路径的清理一律走这里（或 sess_fail_locked 内的它），worker 不要在
 *   锁外自行 sceIoRemove(f->part)：那会与新会话正在写的同名 .part 撞车（TOCTOU），
 *   而且这里本来就覆盖了同一路径。
 * 注：本函数在锁内做文件系统 IO，故只用于"失败/取消/清场"这类一次性路径；
 *     UI 每帧轮询的 recv_status_pull 不再调用它（见那里的说明）。 */
static void cleanup_parts(void)
{
    int i;
    for (i = 0; i < g_sess.n; i++)
        if (g_sess.f[i].st != 1 && !g_sess.f[i].busy)
            sceIoRemove(g_sess.f[i].part);
}

/* 结束活动会话到终态（持有锁时调用）。不碰各文件的 busy 标志：仍在并发
 * 收体的 worker 会自行收尾并清自己的 busy。 */
static void sess_terminal(int state, const char *err)
{
    g_sess.state = state;
    g_sess.cur = -1;
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

/* UI 在接受前设定"本次保存目录"：仅本次会话生效（内存态，不写 config）。
 * dir 为 NULL/空 → 回退默认（config saveDir）。目录须在 ux0: 下且存在。
 * 调用先于 recv_decide，与后端组装会话间由 g_mtx 同步，无并发风险。 */
void recv_set_dir(const char *dir)
{
    size_t sl;
    if (dir && dir[0]) {
        sl = strlen(dir);
        if (sl >= sizeof g_dir) sl = sizeof g_dir - 1;
        memcpy(g_dir, dir, sl);
        while (sl > 5 && g_dir[sl - 1] == '/') sl--;   /* 去尾斜杠（ux0:/ 根自带斜杠保留） */
        g_dir[sl] = 0;
        if (sl < 5 || strncmp(g_dir, "ux0:", 4) != 0) {
            snprintf(g_dir, sizeof g_dir, "%s", PSVSEND_DL_DIR);
            return;
        }
        dlog("recv: dir set %s", g_dir);
    } else {
        config_get_save_dir(g_dir, sizeof g_dir);
    }
}

void recv_init(void)
{
    SceUID d;
    SceIoDirent de;
    char p[512];
    size_t sl = strlen(RECV_PART_SUFFIX);
    if (g_mtx < 0)
        g_mtx = sceKernelCreateMutex("psvsend_recv", 0, 0, NULL);
    if (g_mtx < 0) return;
    if (!g_dir[0]) config_get_save_dir(g_dir, sizeof g_dir);
    lock();
    g_phase = PH_NONE;
    g_abort = 0;
    unlock();
    /* 清扫上次异常退出残留的临时文件（接收中断只会留下 <名>.psvsend.tmp，不会
     * 留下正式文件；只扫当前默认/设置目录——临时目录里的崩溃残留是孤儿，不追扫）。
     * 注：不清理旧版遗留的 *.part —— 那正是"对端发来叫 x.part 的合法文件被当
     * 残留删掉"的根源（见 RECV_PART_SUFFIX 处说明）。 */
    d = sceIoDopen(g_dir);
    if (d >= 0) {
        memset(&de, 0, sizeof de);
        while (sceIoDread(d, &de) > 0) {
            size_t l = strlen(de.d_name);
            if (l > sl &&
                strcmp(de.d_name + l - sl, RECV_PART_SUFFIX) == 0) {
                snprintf(p, sizeof p, "%s/%s", g_dir, de.d_name);
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
        out->overflow = g_pend.overflow;
        out->total = 0;
        for (i = 0; i < g_pend.n && i < RECV_MAX_FILES; i++) {
            snprintf(out->files[i].name, sizeof out->files[i].name, "%s",
                     g_pend.f[i].name);
            out->files[i].size = g_pend.f[i].size;
            out->files[i].trunc = g_pend.f[i].trunc;
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

/* UI 在接受前逐文件指定"保存名"（idx 与 pending.files 下标一致）：
 * name 为空串 = 保持对方原名；非空 = 以此名落盘（accept 时会再 sanitize +
 * 冲突排重）。仅在 PH_PENDING 生效，可多次调用覆盖。 */
void recv_set_name(int idx, const char *name)
{
    if (idx < 0 || !name) return;
    lock();
    if (g_phase == PH_PENDING && idx < g_pend.n) {
        if (name[0])
            snprintf(g_pend.f[idx].rname, sizeof g_pend.f[idx].rname, "%s", name);
        else
            g_pend.f[idx].rname[0] = 0;
    }
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
    if (g_phase == PH_ACTIVE && any_busy_locked()) {
        g_abort = 1;                       /* 收体循环下一块检测到 */
    } else if (g_phase == PH_ACTIVE && !sess_is_terminal(g_sess.state)) {
        cleanup_parts();
        sess_terminal(RECV_ST_CANCEL, "用户取消");
    }
    unlock();
}

/* http 层收体轮询用：有未决的"用户中止"请求就尽快收尾（无锁读 volatile） */
static bool recv_abort_pending(void)
{
    return g_abort != 0;
}

/* prepare-upload：解析 → 挂起等 UI（≤60s）→ 回 HTTP 码并填 resp（200 时 JSON） */
static int recv_http_prepare(const char *body, const char *ip, char *resp, int respsz)
{
    const char *infov, *filesv, *p;
    JsonIter it;
    char tmp[192];
    long long sz;
    int i, n = 0, code = 200, r;
    unsigned my_gen = 0;                     /* 本次占位的序号（见 g_gen） */
    SceLong64 dl;
    bool pend_accept;

    if (respsz > 0) resp[0] = 0;

    infov = json_get_val(body, "info");
    filesv = json_get_val(body, "files");
    if (!infov || !filesv) return 400;       /* 坏体 */

    /* 发送中拒绝接收：PSV 正在给别的设备传文件（xfer 线程 active）时，本机
     * 接收侧虽空闲，但 UI 在传输页不会弹确认（pages_tick 不打断），新请求
     * 只会干挂 60s 超时。不如立刻 409，让对端马上看到"对方正忙"，与接收
     * 忙碌时的表现一致。锁序：此处先短暂取 transfer 锁、未碰 receive 锁，
     * 无嵌套；结束后发送状态若翻转，最多多拒/多收一次，对端重试即可。 */
    if (xfer_active()) {
        dlog("recv: prepare refused, PSV sending (xfer active)");
        return 409;
    }

    /* 检查+占位必须持锁原子完成：http 现在多连接并发，若先查后占分两次
     * 加锁，两个并发 prepare 可能同时通过检查互相覆盖 g_pend。因此进锁后
     * 只要不是 PH_NONE（有待决定/活动/终态未清场）就立即 409 礼貌拒绝。 */
    lock();
    if (g_phase != PH_NONE) {
        unlock();
        return 409;
    }
    g_phase = PH_PENDING;                    /* 先占位：锁内的并发 prepare 409 */
    my_gen = ++g_gen;                        /* 本次占位的身份（见 g_gen） */
    g_pend.alias[0] = 0;
    g_pend.type[0] = 0;
    snprintf(g_pend.ip, sizeof g_pend.ip, "%s", ip ? ip : "");
    json_get_str(infov, "alias", g_pend.alias, sizeof g_pend.alias);
    json_get_str(infov, "deviceType", g_pend.type, sizeof g_pend.type);
    if (!g_pend.alias[0]) snprintf(g_pend.alias, sizeof g_pend.alias, "%s", ip);

    n = 0;
    g_pend.overflow = 0;
    if (json_iter_first(filesv, &it)) {
        do {
            char raw[512], nm[192], sh[65];
            raw[0] = nm[0] = sh[0] = 0;
            p = json_get_val(it.val, "fileName");
            if (p) json_val_str(p, raw, sizeof raw);
            /* 名字在此就规整：净化 + 超长保后缀截断（与落盘同一函数）。若拖到后面
             * 靠 192 缓冲 snprintf 拷贝，尾巴会被截成半个 UTF-8 字符，落盘时
             * sceIoOpen 直接 EINVAL（真机 d133 的失败根因）。 */
            recv_sanitize_name(raw, nm, sizeof nm);
            p = json_get_val(it.val, "size");
            sz = 0;
            if (p) json_val_int(p, &sz);
            p = json_get_val(it.val, "sha256");
            if (p && json_val_str(p, tmp, sizeof tmp) && strlen(tmp) == 64)
                snprintf(sh, sizeof sh, "%s", tmp);
            if (!nm[0] || sz < 0)
                continue;                    /* 条目不可用 → 跳过（不算超限） */
            if (n >= RECV_MAX_FILES) {       /* 超过上限：丢弃并计数，确认页明示
                                              * （协议允许只回执收下的子集） */
                g_pend.overflow++;
                continue;
            }
            snprintf(g_pend.f[n].fileid, sizeof g_pend.f[n].fileid, "%s", it.key);
            snprintf(g_pend.f[n].name, sizeof g_pend.f[n].name, "%s", nm);
            /* 名字被缩短了？（确认页据此提示"文件名过长，将自动缩短"） */
            g_pend.f[n].trunc = strlen(raw) > strlen(nm);
            g_pend.f[n].size = (SceOff)sz;
            snprintf(g_pend.f[n].sha256, sizeof g_pend.f[n].sha256, "%s", sh);
            g_pend.f[n].rname[0] = 0;    /* 默认沿用对方文件名；UI 可改（recv_set_name） */
            g_pend.inc[n] = true;
            n++;
        } while (code == 200 && json_iter_next(&it));
    }
    if (code != 200) {
        g_phase = PH_NONE;                   /* 撤销占位 */
        unlock();
        return 400;
    }
    if (n == 0) {                            /* 没有要传的文件 */
        g_phase = PH_NONE;                   /* 撤销占位 */
        unlock();
        return 204;
    }
    g_pend.n = n;
    g_pend_result = 0;
    dlog("recv: prepare %d files from %s (%s)", n, g_pend.alias, g_pend.ip);
    if (g_pend.overflow)
        dlog("recv: %d more file(s) exceed the cap %d, will be dropped",
             g_pend.overflow, RECV_MAX_FILES);
    unlock();

    /* 等 UI 决定：轮询（本连接占一个 http worker，最长 60s；期间其他连接
     * 由各自的 worker 并发处理，遇到 PH_PENDING 一律 409，不会踩这份状态） */
    dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
    for (;;) {
        lock();
        r = g_pend_result;
        if (g_gen != my_gen) r = 3;          /* 占位已被 recv_clear 撤销/被新 prepare 顶替 */
        unlock();
        if (r) break;
        if ((SceLong64)now_us() >= dl) { r = 3; break; }
        sceKernelDelayThread(50 * 1000);
    }

    lock();
    if (g_gen != my_gen) {                   /* 占位已失效：状态归别人，绝不能回写 */
        unlock();
        dlog("recv: prepare (gen %u) superseded, drop", my_gen);
        return 403;
    }
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
            /* 落盘用名：UI 指定的保存名（改名）优先，否则对方原名；alloc_paths
             * 内会再 sanitize + 冲突排重 */
            snprintf(f->name, sizeof f->name, "%s",
                     g_pend.f[i].rname[0] ? g_pend.f[i].rname : g_pend.f[i].name);
            f->size = g_pend.f[i].size;
            snprintf(f->sha256, sizeof f->sha256, "%s", g_pend.f[i].sha256);
            if (f->size == 0) {
                /* 空文件不需收体，但必须在盘上留下一个 0 字节文件：对端对空文件也
                 * 会（也可能不）发一次空体 upload。曾经的写法是"直接标完成、不建
                 * 文件"→ 界面显示收到、目录里却找不到（真机 d129 实测如此）。 */
                SceUID fd0;
                alloc_paths(f->name, f->final, sizeof f->final,
                            f->part, sizeof f->part, g_sess.f, m);
                fd0 = sceIoOpen(f->final,
                                SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
                if (fd0 < 0) {
                    /* 建不出来（磁盘满/权限）：退回普通待收文件，让对端随后的
                     * upload 走正常落盘路径；它不发的话会话以空闲超时收尾，不会静默丢。 */
                    dlog("recv: create empty file fail 0x%08X (%s)",
                         (unsigned)fd0, f->final);
                    has_pending = 1;
                } else {
                    sceIoClose(fd0);
                    f->st = 1;
                }
            } else {
                alloc_paths(f->name, f->final, sizeof f->final,
                            f->part, sizeof f->part, g_sess.f, m);
                has_pending = 1;
            }
            /* 落盘名经 sanitize + 冲突排重后可能与原名不同（对端名含 '/'、
             * 控制字符，或与本会话/磁盘既有文件重名时）。显示名回填实际
             * 落盘名，否则界面显示的和用户能在目录里找到的不是一回事。 */
            {
                const char *bn = strrchr(f->final, '/');
                snprintf(f->name, sizeof f->name, "%s", bn ? bn + 1 : f->final);
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

/* 标记会话整体失败并清理残留临时文件（持有锁时调用；fd 由调用点先关闭）。
 * 已是终态（如并发中另一个文件已先失败/被取消）则只做清理、不覆盖原终态。 */
static void sess_fail_locked(const char *err)
{
    cleanup_parts();
    if (!sess_is_terminal(g_sess.state))
        sess_terminal(RECV_ST_FAIL, err);
}

/* upload：校验 query + 来源 IP → 经 fn 流式收 total 字节写盘。
 * fn 每次返回 ≤ max；0=对端关闭/空闲超时，<0=socket 错误。 */
static int recv_http_upload(const char *query, const char *ip, int64_t total,
                            http_stream_fn fn, void *ctx)
{
    char sid[64], fid[170], tok[72];
    SceOff free_space;
    RFile *f = NULL;
    int idx = -1, i;
    SceUID fd = -1;
    bool check_sha = false, aborted = false;
    mbedtls_sha256_context hctx;
    unsigned char digest[32];
    unsigned char buf[RECV_CHUNK];   /* 本 upload 私有：同会话多文件并发时不能共用静态缓冲 */

    if (!query || !fn || !ctx) return 400;
    query_get(query, "sessionId", sid, sizeof sid);
    query_get(query, "fileId", fid, sizeof fid);
    query_get(query, "token", tok, sizeof tok);
    if (!sid[0] || !fid[0] || !tok[0]) return 400;

    lock();
    if (g_phase == PH_ACTIVE && g_sess.session[0] &&
        strcmp(g_sess.session, sid) == 0) {
        if (!g_sess.peer_ip[0] || strcmp(g_sess.peer_ip, ip) == 0)
            for (i = 0; i < g_sess.n; i++)
                if (strcmp(g_sess.f[i].fileid, fid) == 0) { idx = i; break; }
        if (idx >= 0 && strcmp(g_sess.f[idx].token, tok) != 0) idx = -2;
    }
    /* 已完成文件的重传 / 已终态会话：只在"零字节文件的幂等重传"上放行 200。
     * 终态也必须走这条判定——整批都是空文件的会话 accept 即 DONE（见 prepare
     * 收尾），对端仍会为每个空文件发一次 upload，按终态一律 403 会让对方报错。 */
    if (idx >= 0 && (sess_is_terminal(g_sess.state) || g_sess.f[idx].st == 1)) {
        int rc = (g_sess.f[idx].st == 1 && g_sess.f[idx].size == 0) ? 200 : 403;
        unlock();
        return rc;
    }
    if (idx < 0) {
        unlock();
        return 403;                          /* 会话/令牌/IP 不符 */
    }
    f = &g_sess.f[idx];
    if (f->busy) {                           /* 同一文件被重复并发上传（同会话多文件
                                              * 并发是允许的，仅同文件自身重入要拒） */
        unlock();
        return 409;
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
    f->busy = true;
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
        f->busy = false;
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
            f->busy = false;
            cleanup_parts();                 /* 含本文件的临时文件 */
            if (!sess_is_terminal(g_sess.state))     /* 别覆盖别处已判定的终态原因 */
                sess_terminal(RECV_ST_CANCEL, "用户取消");
            unlock();
            if (check_sha) mbedtls_sha256_free(&hctx);
            return 500;
        }
        if (f->got >= f->size) break;
        {
            SceOff rem = f->size - f->got;
            n = fn(ctx, buf, (int)(rem < RECV_CHUNK ? rem : RECV_CHUNK));
        }
        if (n <= 0) {                        /* 断流/空闲超时/socket 错/用户中止 */
            sceIoClose(fd); fd = -1;
            lock();
            f->busy = false;
            if (g_abort) {                   /* http 层看到中止标志提前退出 → 取消 */
                cleanup_parts();
                if (!sess_is_terminal(g_sess.state))     /* 别覆盖已判定的终态原因 */
                    sess_terminal(RECV_ST_CANCEL, "用户取消");
            } else {
                sess_fail_locked(f->got > 0 ? "传输中断" : "对方未发送数据");
            }
            unlock();
            if (check_sha) mbedtls_sha256_free(&hctx);
            return 500;
        }
        if (check_sha) mbedtls_sha256_update(&hctx, buf, (unsigned)n);
        wr = sceIoWrite(fd, buf, (unsigned)n);
        if (wr != n) {                       /* 磁盘满 / IO 错误 */
            sceIoClose(fd); fd = -1;
            lock();
            f->busy = false;
            sess_fail_locked("写入失败（磁盘空间不足？）");
            unlock();
            if (check_sha) mbedtls_sha256_free(&hctx);
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
        f->busy = false;
        sess_fail_locked("sha256 校验失败");
        unlock();
        return 422;
    }
    if (sceIoRename(f->part, f->final) < 0) {
        lock();
        f->busy = false;
        sess_fail_locked("文件落盘失败");
        unlock();
        return 500;
    }
    lock();
    f->st = 1;
    f->busy = false;
    update_cur_locked();
    if (!sess_is_terminal(g_sess.state)) {
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
            /* 还有文件没到（或正在并发收）：等下一个 upload 完成 */
            g_sess.state = any_busy_locked() ? RECV_ST_RECEIVING : RECV_ST_READY;
            g_sess.last_us = now_us();
        }
    }
    unlock();
    return 200;
}

static int recv_http_cancel(const char *query, const char *ip)
{
    char sid[64];
    (void)ip;
    query_get(query, "sessionId", sid, sizeof sid);
    lock();
    if (g_phase == PH_ACTIVE && sid[0] &&
        strcmp(g_sess.session, sid) == 0 &&
        !sess_is_terminal(g_sess.state)) {   /* 已终态就忽略：别把 DONE 改写成 CANCEL，
                                              * 更别覆盖 FAIL/TIMEOUT 的失败原因 */
        if (any_busy_locked()) {
            /* 正在流式收体（http 并发连接）：直接清场会删掉收体中那些
             * 临时文件；置中止位，让收体循环在锁内统一收尾（与 UI 取消一致） */
            g_abort = 1;
            dlog("recv: session %s cancel by sender during body -> abort",
                 g_sess.session);
        } else {
            cleanup_parts();
            sess_terminal(RECV_ST_CANCEL, "发送方取消");
            dlog("recv: session %s cancelled by sender", g_sess.session);
        }
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
        /* 空闲超时：没有文件在收体且超过 IDLE 无动静 → TIMEOUT。
         * 这里不删残留临时文件：本函数是 UI 每帧轮询的热路径，不该在锁内做
         * 文件系统 IO（慢盘掉帧 + 阻塞 worker 的收体提交）。残留交给 recv_clear
         * （用户离开结束页时的清场）或下次开机 recv_init 清扫。 */
        if (!any_busy_locked() && !sess_is_terminal(g_sess.state) &&
            now - g_sess.last_us > (uint64_t)RECV_IDLE_TIMEOUT_US) {
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
        if (any_busy_locked()) {             /* 收体进行中：别硬清，先请中止 */
            g_abort = 1;
            unlock();
            return;
        }
        cleanup_parts();
    } else if (g_phase == PH_PENDING) {
        g_pend_result = 2;                   /* http 等待循环醒来按拒绝处理 */
        g_gen++;                             /* 同时撤销占位：等待中的 prepare 万一错过
                                              * 这次结果（50ms 轮询窗口）也不会回写状态 */
    }
    g_phase = PH_NONE;
    g_abort = 0;
    unlock();
}

/* ---------- 注册给 http 层的接收侧处理集（依赖倒置，类型见 net/http.h） ---------- */
static const HttpRecvOps s_http_ops = {
    recv_http_prepare,      /* prepare：清单 → 挂起等 UI 决定 */
    recv_http_upload,       /* upload ：经 http 层流回调边收边写盘 */
    recv_http_cancel,       /* cancel ：发送方放弃会话 */
    recv_abort_pending,     /* abort_pending：用户中止查询 */
};

const HttpRecvOps *recv_http_ops(void)
{
    return &s_http_ops;
}
