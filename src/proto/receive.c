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
#include <unistd.h>               /* getentropy */
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/devctl.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/threadmgr/semaphore.h>
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
#define RECV_BLOCKS            3         /* 收体轮转缓冲块数（每块 RECV_CHUNK） */
#define RECV_SLOTS             8         /* 收体流水线槽位上限（= 并发 upload 上限） */

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
    bool orphan;          /* 收体流水线被遗弃：临时文件由槽位接管，cleanup_parts 跳过 */
} RFile;

/* ---- 收体流水线槽位 ------------------------------------------------
 * 每个 upload 占一个固定槽位（上限 RECV_SLOTS），槽位是静态的，消费者线程
 * 直接持有它的指针。这样主线程因存储卡死而超时返回时，仍在跑的线程不会踩到
 * 已释放的栈帧/堆内存（旧实现把 WrCtx 放在栈上，超时后照样 free/DeleteThread）。
 *   FREE → ACTIVE →（正常收尾，两线程已退）→ FREE
 *                 └→（收尾超时）→ ZOMBIE →（线程退出后 reap 回收）→ FREE
 * ZOMBIE 期间缓冲/信号量/线程句柄/fd/临时文件全部冻结，只由 reap_zombies 在
 * 确认线程退出后释放。reap 只持 g_reap_mtx，绝不反向取 g_lock。 */
enum { SLOT_FREE = 0, SLOT_ACTIVE, SLOT_ZOMBIE };

typedef struct {
    int   state;              /* SLOT_* */
    int   stuck_stage;        /* 遗弃时卡在哪：1=等空块 2=join 3=起线程 */
    int   detached;           /* 已遗弃：消费者不得再碰 g_sess / f（锁内检查） */
    char  part[800];          /* 遗弃文件的临时路径（reap 时删；正常路径为空） */
    RFile *f;                 /* 绑定的文件；detached 后不再解引用 */
    SceUID thr_sha, thr_wr, sem_free, sem_sha, sem_job, fd;
    volatile int quit;        /* 生产者收完：唤醒两条消费者退出 */
    volatile int fail;        /* 写盘线程写失败 */
    int    idx_in, idx_sha, idx_out;
    int    len[RECV_BLOCKS];
    unsigned char *buf;       /* RECV_BLOCKS * RECV_CHUNK（ACTIVE 时分配） */
    mbedtls_sha256_context sha;  /* 内嵌：不能指向主线程栈（线程会用） */
    int    check_sha;
    uint64_t t_write, t_sha;
} RecvSlot;

static RecvSlot g_slots[RECV_SLOTS];
static SceUID   g_reap_mtx = -1;   /* 保护槽位状态（g_lock 之外独立一把，锁序 g_lock → g_reap_mtx） */

/* 待决定请求（PH_PENDING） */
static struct {
    char alias[64];
    char type[24];
    char ip[16];
    char fp[65];             /* 对端叶证书 SHA-256（TLS 才有；明文为空） */
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
/* recv_clear 撞上"还有 worker 在收体"时不能当场清场：会删掉别人正在写的临时
 * 文件，且复位 phase 后新 prepare 立刻覆盖 g_sess，而旧 worker 仍持有指向
 * g_sess.f[i] 的指针。只能置中止位，等最后一个 worker 收尾时补做——否则
 * g_phase 永远停在 PH_ACTIVE，之后所有 prepare 恒 409，只能重启应用。 */
static int  g_clear_pending = 0;
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

/* 槽位内资源释放（调用者持 g_reap_mtx，且已确认两条线程都退出） */
static void slot_do_release(RecvSlot *s)
{
    if (s->thr_sha >= 0)  sceKernelDeleteThread(s->thr_sha);
    if (s->thr_wr >= 0)   sceKernelDeleteThread(s->thr_wr);
    if (s->sem_job >= 0)  sceKernelDeleteSema(s->sem_job);
    if (s->sem_sha >= 0)  sceKernelDeleteSema(s->sem_sha);
    if (s->sem_free >= 0) sceKernelDeleteSema(s->sem_free);
    if (s->check_sha) mbedtls_sha256_free(&s->sha);
    if (s->buf) { free(s->buf); s->buf = NULL; }
    if (s->fd >= 0) sceIoClose(s->fd);
    if (s->part[0] && sceIoRemove(s->part) < 0)
        dlog("recv: reap: remove %s failed", s->part);
    memset(s, 0, sizeof *s);
    s->thr_sha = s->thr_wr = s->sem_free = s->sem_sha = s->sem_job = s->fd = -1;
    s->state = SLOT_FREE;
}

/* 线程是否已退出（1=已退出）。非零/异常返回一律当"未退出"：宁可多挂一轮，
 * 不可误删一个可能还活着的线程。 */
static int thread_exited(SceUID thr)
{
    int st = 0;
    if (thr < 0) return 1;
    return sceKernelGetThreadExitStatus(thr, &st) >= 0;
}

/* 回收已退出的 ZOMBIE 槽位。调用点（prepare 开头 / upload 开头 / recv_clear
 * 开头）都在不持 g_lock 的位置，整段只持 g_reap_mtx。 */
static void reap_zombies(void)
{
    int i;
    if (g_reap_mtx < 0) return;
    sceKernelLockMutex(g_reap_mtx, 1, NULL);
    for (i = 0; i < RECV_SLOTS; i++) {
        RecvSlot *s = &g_slots[i];
        if (s->state != SLOT_ZOMBIE) continue;
        if (!thread_exited(s->thr_wr)) continue;
        if (!thread_exited(s->thr_sha)) continue;
        dlog("recv: reap slot %d (stage %d)", i, s->stuck_stage);
        slot_do_release(s);
    }
    sceKernelUnlockMutex(g_reap_mtx, 1);
}

/* 取一个空闲槽位（返回 NULL = 并发已满）。 */
static RecvSlot *slot_alloc(void)
{
    int i;
    RecvSlot *s = NULL;
    if (g_reap_mtx < 0) return NULL;
    sceKernelLockMutex(g_reap_mtx, 1, NULL);
    for (i = 0; i < RECV_SLOTS; i++)
        if (g_slots[i].state == SLOT_FREE) { s = &g_slots[i]; break; }
    if (s) {
        memset(s, 0, sizeof *s);
        s->thr_sha = s->thr_wr = s->sem_free = s->sem_sha = s->sem_job = s->fd = -1;
        s->state = SLOT_ACTIVE;
        dlog("recv: slot %d allocated", (int)(s - g_slots));
    }
    sceKernelUnlockMutex(g_reap_mtx, 1);
    return s;
}

/* 正常收尾：两条线程已确认退出 → 释放资源并归还槽位 */
static void slot_release(RecvSlot *s)
{
    sceKernelLockMutex(g_reap_mtx, 1, NULL);
    slot_do_release(s);
    sceKernelUnlockMutex(g_reap_mtx, 1);
}

/* 该临时路径是否正被某个 ZOMBIE 槽位占用（alloc_paths 避让同名 part 用）。
 * 调用点可能持 g_lock：锁序 g_lock → g_reap_mtx；reap 不取 g_lock，无环。 */
static bool part_in_zombie(const char *part)
{
    int i;
    bool hit = false;
    if (g_reap_mtx < 0) return false;
    sceKernelLockMutex(g_reap_mtx, 1, NULL);
    for (i = 0; i < RECV_SLOTS; i++)
        if (g_slots[i].state == SLOT_ZOMBIE && g_slots[i].part[0] &&
            strcmp(g_slots[i].part, part) == 0) { hit = true; break; }
    sceKernelUnlockMutex(g_reap_mtx, 1);
    return hit;
}

/* 会话是否已到终态（终态后不再改变，除 recv_clear 清场） */
static bool sess_is_terminal(int state)
{
    return state == RECV_ST_DONE || state == RECV_ST_FAIL ||
           state == RECV_ST_CANCEL || state == RECV_ST_TIMEOUT ||
           state == RECV_ST_PARTIAL;
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

/* 随机 hex（sessionId/token）。优先 libc getentropy（Vita 实测可用，与
 * identity.c 取熵同一写法）；失败再退回"系统时间 + 进程时间"混洗的 LCG。
 * 旧实现纯由时间推导，同批 sessionId/token 可被枚举——token 是 upload 的
 * 唯一凭据，可预测就等于把会话交给同网段的人（upload 另有来源 IP 校验，
 * 跨机劫持不成立，但仍是该堵的口子）。 */
static void gen_hex(char *out, int bytes)
{
    unsigned char buf[32];
    int i;
    if (bytes > (int)sizeof buf) bytes = (int)sizeof buf;
    if (getentropy(buf, (size_t)bytes) == 0) {
        for (i = 0; i < bytes; i++) snprintf(out + i * 2, 3, "%02x", buf[i]);
        out[bytes * 2] = 0;
        return;
    }
    {
        uint64_t t = (uint64_t)sceKernelGetSystemTimeWide();
        uint64_t p = (uint64_t)sceKernelGetProcessTimeWide();
        for (i = 0; i < bytes; i++) {
            if ((i % 8) == 0) t = t * 6364136223846793005ULL + (p ^ (i + 1));
            snprintf(out + i * 2, 3, "%02x",
                     (unsigned)((t >> ((unsigned)((i * 5) % 56))) & 0xFF));
        }
        out[bytes * 2] = 0;
    }
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
 * 字符（非法 UTF-8 会让 sceIoOpen 直接返 EINVAL、整批接收失败）。
 * 返回 true = 因超出长度预算被截短（确认页提示"文件名过长"就用它，不能用
 * "缩短前后字节数不等"来推——路径前缀/首尾空白点也会让名字变短）。 */
bool recv_sanitize_name(const char *in, char *out, int n)
{
    int lim, o = 0, i, keep, elen = 0;
    bool any = false, cut = false;
    const char *slash, *dot = NULL;
    int len;
    if (!in) in = "";
    slash = strrchr(in, '/');
    if (slash) in = slash + 1;
    while (*in == ' ') in++;                     /* 去前导空白 */
    lim = n - 1;
    if (lim > RECV_NAME_MAX) lim = RECV_NAME_MAX;
    if (lim < 1) { out[0] = 0; return false; }
    len = (int)strlen(in);
    if (len <= lim) {
        keep = len;                              /* 放得下：原样保留 */
    } else {
        cut = true;                              /* 装不下：这次才是"名字过长" */
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
    return cut;
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
        snprintf(part, pn, "%s%s", final, RECV_PART_SUFFIX);
        if (sceIoGetstat(final, &st) < 0) {         /* 磁盘上不存在 */
            for (t = 0; t < prev_n; t++)            /* 本会话也没占用 */
                if (prev[t].final[0] &&
                    strcmp(final, prev[t].final) == 0) break;
            /* 同名临时文件可能正被遗弃的僵尸线程写（fd 冻结、路径还占着）：
             * 绝不复用，否则两个写者写同一个 FAT 文件。 */
            if (t == prev_n && !part_in_zombie(part)) break;   /* 真正可用 */
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
        if (g_sess.f[i].st != 1 && !g_sess.f[i].busy && !g_sess.f[i].orphan)
            sceIoRemove(g_sess.f[i].part);
}

/* recv_clear 曾因收体未结束而推迟清场 → 在本 worker 收尾处补做（持锁调用）。
 * 必须等所有 busy 都清零再动手：此时 g_sess 不会再被任何 worker 访问，
 * 复位 g_phase 后新 prepare 覆盖 g_sess 才是安全的。 */
static void clear_pending_finish_locked(void)
{
    if (!g_clear_pending || any_busy_locked()) return;
    cleanup_parts();
    g_phase = PH_NONE;
    g_abort = 0;
    g_clear_pending = 0;
    dlog("recv: session %s cleared (deferred)", g_sess.session);
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
    if (g_reap_mtx < 0) {
        int i;
        g_reap_mtx = sceKernelCreateMutex("psvsend_recv_reap", 0, 0, NULL);
        for (i = 0; i < RECV_SLOTS; i++) {
            memset(&g_slots[i], 0, sizeof g_slots[i]);
            g_slots[i].thr_sha = g_slots[i].thr_wr = -1;
            g_slots[i].sem_free = g_slots[i].sem_sha = g_slots[i].sem_job = -1;
            g_slots[i].fd = -1;
        }
    }
    if (!g_dir[0]) config_get_save_dir(g_dir, sizeof g_dir);
    lock();
    g_phase = PH_NONE;
    g_abort = 0;
    g_clear_pending = 0;
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
        snprintf(out->peer_fp,    sizeof out->peer_fp,    "%s", g_pend.fp);
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
static int recv_http_prepare(const char *body, const char *ip, const char *peer_fp,
                             char *resp, int respsz)
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
    /* 必须在 alloc_paths 之前：先把旧遗留槽位回收掉，避免新会话复用正被
     * 僵尸线程写的临时文件路径。 */
    reap_zombies();

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
    /* 对端证书指纹由 TLS 层在握手时算好传进来（官方口径：注册/准备里的
     * fingerprint 在 HTTPS 模式下被忽略，一律以证书为准）。 */
    snprintf(g_pend.fp, sizeof g_pend.fp, "%s", peer_fp ? peer_fp : "");
    json_get_str(infov, "alias", g_pend.alias, sizeof g_pend.alias);
    json_get_str(infov, "deviceType", g_pend.type, sizeof g_pend.type);
    if (!g_pend.alias[0]) snprintf(g_pend.alias, sizeof g_pend.alias, "%s", ip);

    n = 0;
    g_pend.overflow = 0;
    if (json_iter_first(filesv, &it)) {
        do {
            char raw[512], nm[192], sh[65];
            bool cut;
            raw[0] = nm[0] = sh[0] = 0;
            p = json_get_val(it.val, "fileName");
            if (p) json_val_str(p, raw, sizeof raw);
            /* 名字在此就规整：净化 + 超长保后缀截断（与落盘同一函数）。若拖到后面
             * 靠 192 缓冲 snprintf 拷贝，尾巴会被截成半个 UTF-8 字符，落盘时
             * sceIoOpen 直接 EINVAL（真机 d133 的失败根因）。 */
            cut = recv_sanitize_name(raw, nm, sizeof nm);
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
            /* 名字过长被截短了？（确认页据此提示"文件名过长，将自动缩短"）。
             * 用 sanitize 的返回值，不能拿 strlen(raw) > strlen(nm) 推——名字里
             * 带路径前缀、首尾空白/点时也会变短，那样会把正常名字全报成"过长"。 */
            g_pend.f[n].trunc = cut;
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
 * 已是终态（如并发中另一个文件已先失败/被取消）则只做清理、不覆盖原终态。
 * 只用于**会话级**错误（断流 / 用户取消 / 超时）；单个文件的错误走
 * file_fail_locked——与官方一致：一个文件失败不终止其余文件的接收。 */
static void sess_fail_locked(const char *err)
{
    cleanup_parts();
    if (!sess_is_terminal(g_sess.state))
        sess_terminal(RECV_ST_FAIL, err);
}

/* 会话终态收敛（持有锁时调用）：单文件失败只置 st=3，等所有文件都收尾
 * （st != 0）才在这里汇总。失败当下不能直接判 PARTIAL——那会让还没开始
 * 上传的其余文件命中 upload 入口的终态检查、直接吃 403。
 *   g_abort 只收掉"还没结束"的会话（与旧行为一致：已收满的文件优先算完成）。 */
static void check_all_done_locked(void)
{
    int i, all = 1, failed = 0;
    if (sess_is_terminal(g_sess.state)) return;
    for (i = 0; i < g_sess.n; i++) {
        if (g_sess.f[i].st == 0) { all = 0; break; }
        if (g_sess.f[i].st == 3) failed++;
    }
    if (all) {
        if (failed) {
            sess_terminal(RECV_ST_PARTIAL,
                          g_sess.err[0] ? g_sess.err : "部分文件接收失败");
            dlog("recv: session %s partial (%d/%d files failed)",
                 g_sess.session, failed, g_sess.n);
        } else {
            sess_terminal(RECV_ST_DONE, NULL);
            dlog("recv: session %s done (%d files, %lld B)", g_sess.session,
                 g_sess.n, (long long)g_sess.got_total);
        }
    } else if (g_abort) {
        /* 还有文件没收：用户取消 → 整会话收尾（已完成的文件保留） */
        cleanup_parts();
        sess_terminal(RECV_ST_CANCEL, "用户取消");
        dlog("recv: session %s cancelled", g_sess.session);
    } else {
        /* 还有文件没到（或正在并发收）：等下一个 upload 收尾 */
        g_sess.state = any_busy_locked() ? RECV_ST_RECEIVING : RECV_ST_READY;
        g_sess.last_us = now_us();
    }
}

/* 单文件失败（持有锁时调用）：只标这个文件，不结束会话。
 * 首个失败原因留在 g_sess.err，供最后收敛成 PARTIAL 时显示。 */
static void file_fail_locked(RFile *f, const char *err)
{
    f->st = 3;
    f->busy = false;
    update_cur_locked();
    if (!g_sess.err[0])
        snprintf(g_sess.err, sizeof g_sess.err, "%s", err);
    dlog("recv: file %s failed: %s", f->name, err);
    cleanup_parts();                     /* 删掉本文件未完成的临时文件（orphan 除外） */
    check_all_done_locked();
}

/* 遗弃流水线（调用时不持 g_lock）：业务上标该文件失败、把临时文件路径交给
 * 槽位，资源一律冻结，等 reap_zombies 在两条线程都退出后回收。
 * stage: 1=等空块超时 2=join 超时 3=起线程超时 */
static void slot_abandon(RecvSlot *w, RFile *f, int stage, const char *err)
{
    lock();
    w->detached = 1;             /* 消费者此后不再碰 g_sess / f（wr_thread 锁内检查） */
    w->f = NULL;
    if (f) snprintf(w->part, sizeof w->part, "%s", f->part);
    /* 必须在放开 g_lock 之前把槽位标成 ZOMBIE：phase 一旦复位，新会话的
     * alloc_paths 会来避让同名临时文件，若此时槽位还是 ACTIVE 就避不开。 */
    sceKernelLockMutex(g_reap_mtx, 1, NULL);
    w->stuck_stage = stage;
    w->state = SLOT_ZOMBIE;
    sceKernelUnlockMutex(g_reap_mtx, 1);
    if (f) {
        f->orphan = true;        /* cleanup_parts 跳过，临时文件由槽位接管 */
        file_fail_locked(f, err);
    } else if (!sess_is_terminal(g_sess.state)) {
        sess_fail_locked(err);
    }
    clear_pending_finish_locked();
    unlock();
    dlog("recv: pipeline abandoned (stage %d): %s", stage, err);
}

/* ---- 收体三级流水：解密 / sha256 / 写盘各占一条线程 ----
 * 生产者（http worker）只做 fn()（socket 收 + TLS 解密）并攒满 64KB；sha 线程
 * 只算校验和；写盘线程只做 sceIoWrite。三块轮转缓冲 + 三个计数信号量：
 *   sem_free = 空闲块数（初值 3）、sem_sha = 待校验块数、sem_job = 待写块数。
 * 块在流水线里逐级传递：生产者填满 → sem_sha → sha 算完 → sem_job → 写盘完成
 * → sem_free。所以 sha 与写盘处理的是**不同块**，两者真正重叠（旧实现把这两
 * 步放在同一条线程里串行相加，是 4.08MiB/s 的天花板）。
 * 顺序性：三个阶段各按自己的游标轮转、天然 FIFO，sha256 输入顺序与文件内容
 * 一致；f->got/got_total 仍由写盘线程推进（= 已落盘字节），进度条不会超前。
 * 对方没给 sha256（check_sha=0）就不起 sha 线程，生产者直接投 sem_job。
 * 注意：每块 64KB，三块共 192KB/文件；多文件并发时线程与内存都按文件数倍增。 */
/* 结构体见文件前部的 RecvSlot（静态槽位，不能放栈上）。 */

static int sha_thread(SceSize args, void *argp)
{
    RecvSlot *w = *(RecvSlot **)argp;
    (void)args;
    for (;;) {
        int s, len;
        unsigned char *p;
        uint64_t t, dt;
        sceKernelWaitSema(w->sem_sha, 1, NULL);
        if (w->quit) break;
        s = w->idx_sha % RECV_BLOCKS;
        len = w->len[s];
        p = w->buf + (size_t)s * RECV_CHUNK;
        ++w->idx_sha;
        t = now_us();
        mbedtls_sha256_update(&w->sha, p, (unsigned)len);
        dt = now_us() - t;
        /* 累计值进锁：生产者每 2s 在锁内读它打诊断日志，64 位量不加锁读会撕裂 */
        lock();
        w->t_sha += dt;
        unlock();
        sceKernelSignalSema(w->sem_job, 1);
    }
    return 0;
}

static int wr_thread(SceSize args, void *argp)
{
    RecvSlot *w = *(RecvSlot **)argp;
    (void)args;
    for (;;) {
        int s, len;
        unsigned char *p;
        uint64_t t, dt;
        sceKernelWaitSema(w->sem_job, 1, NULL);
        if (w->quit) break;
        s = w->idx_out % RECV_BLOCKS;
        len = w->len[s];
        p = w->buf + (size_t)s * RECV_CHUNK;
        ++w->idx_out;
        t = now_us();
        if (sceIoWrite(w->fd, p, (unsigned)len) != len) w->fail = 1;
        dt = now_us() - t;
        lock();
        w->t_write += dt;
        /* 流水线已遗弃（主线程超时返回）时不得再碰 g_sess：临时文件可能已
         * 被新会话复用，f 也可能是别的会话的文件。 */
        if (!w->detached && w->f) {
            w->f->got += len;
            g_sess.got_total += len;
        }
        unlock();
        sceKernelSignalSema(w->sem_free, 1);
    }
    return 0;
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
    unsigned char digest[32];
    RecvSlot *w = NULL;              /* 三级流水槽位（见文件前部 RecvSlot） */
    SceOff produced = 0;             /* 已从 socket 收下的字节（写线程可能还在追） */
    int rd_fail = 0;                 /* fn 的失败返回值（0 = 正常） */
    int wr_ok = 0;                   /* 流水线线程是否都起来了 */
    int wr_started = 0;              /* 写线程已 StartThread（清理时才知道要不要收尸） */
    uint64_t t0, t_log;              /* 收体耗时/进度打点（2s 一条，用于定位掉速与截断） */
    uint64_t t_read = 0, t_mark;     /* 生产者侧（socket 收 + TLS 解密）累计耗时 */
    uint64_t t_wait = 0, w_mark;     /* 生产者等空缓冲（=写线程追不上）的累计耗时 */
    uint64_t l_read = 0, l_wait = 0, l_write = 0, l_sha = 0; /* 上一条日志时的累计值 */
    uint64_t t_write = 0, t_sha = 0; /* 槽位释放前取出：之后槽位会被 memset 归还 */
    int wr_fail = 0;                 /* 同上：写盘线程的失败标志 */
    SceOff log_got = 0;

    if (!query || !fn || !ctx) return 400;
    reap_zombies();                  /* 传输开头顺手回收已退出的僵尸槽位 */
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
        file_fail_locked(f, "文件大小与清单不符");
        unlock();
        return 400;
    }
    /* 磁盘预检（查不到就交给写失败兜底）。空间不足只判这个文件：其余文件
     * 可能更小、装得下（预检本来就是按当前文件的 size 算的）。 */
    if (disk_free(&free_space) == 0 && free_space < f->size) {
        file_fail_locked(f, "存储空间不足");
        unlock();
        return 500;
    }
    f->busy = true;
    g_sess.cur = idx;
    g_sess.state = RECV_ST_RECEIVING;
    g_sess.last_us = now_us();
    f->got = 0;
    /* 对端给了 sha256 且用户没关"接收时校验"才校验（设置项 recv_sha，默认开）。
     * 关掉只是不比对，仍然照收——用途是对端哈希实现有误时不被 422 卡死。 */
    check_sha = f->sha256[0] != 0 && g_cfg.recv_sha;
    unlock();

    fd = sceIoOpen(f->part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        dlog("recv: open part fail 0x%08X (%s)", (unsigned)fd, f->part);
        lock();
        file_fail_locked(f, "无法创建文件");
        clear_pending_finish_locked();
        unlock();
        return 500;
    }

    /* 取一个静态槽位：并发已满就拒这一条（503 = 临时性的，对端可重试）。 */
    w = slot_alloc();
    if (!w) {
        sceIoClose(fd); fd = -1;
        lock();
        file_fail_locked(f, "接收并发已满");
        clear_pending_finish_locked();
        unlock();
        return 503;
    }
    /* 起两条消费者线程（sha256 / 写盘）：解密留在本线程，三级流水。
     * 失败只判这个文件失败（内存/句柄耗尽时其余文件通常也起不来，但按
     * 单文件语义处理更接近官方：能收一个算一个），不做退化分支。 */
    w->fd = fd;
    w->f = f;
    w->check_sha = check_sha ? 1 : 0;
    if (check_sha) {
        mbedtls_sha256_init(&w->sha);
        mbedtls_sha256_starts(&w->sha, 0);
    }
    w->buf = (unsigned char *)malloc((size_t)RECV_CHUNK * RECV_BLOCKS);
    if (w->buf)
        w->sem_free = sceKernelCreateSema("psvsend_wfree", 0, RECV_BLOCKS, RECV_BLOCKS, NULL);
    if (w->sem_free >= 0)
        w->sem_sha = sceKernelCreateSema("psvsend_wsha", 0, 0, RECV_BLOCKS, NULL);
    if (w->sem_sha >= 0)
        w->sem_job = sceKernelCreateSema("psvsend_wjob", 0, 0, RECV_BLOCKS, NULL);
    if (w->sem_job >= 0)
        w->thr_wr = sceKernelCreateThread("psvsend_wr", wr_thread, 0x40, 0x20000, 0,
                                          SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    /* 对方给了 sha256 才起校验线程；没给就让生产者直接投 sem_job。 */
    if (w->thr_wr >= 0 && w->check_sha)
        w->thr_sha = sceKernelCreateThread("psvsend_sha", sha_thread, 0x40, 0x20000, 0,
                                           SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (w->thr_wr >= 0 && (!w->check_sha || w->thr_sha >= 0)) {
        /* StartThread 会把参数块按字节拷进新线程栈，传栈上临时指针安全；
         * 指针值本身指向静态槽位，线程怎么用都不会悬空。 */
        RecvSlot *wp = w;
        wr_ok = sceKernelStartThread(w->thr_wr, sizeof wp, &wp) >= 0;
        wr_started = wr_ok;
        if (wr_ok && w->check_sha)
            wr_ok = sceKernelStartThread(w->thr_sha, sizeof wp, &wp) >= 0;
    }
    if (!wr_ok) {
        dlog("recv: pipeline setup fail (wr=%d sha=%d sem=%d/%d/%d buf=%d)",
             (int)w->thr_wr, (int)w->thr_sha, (int)w->sem_free, (int)w->sem_sha,
             (int)w->sem_job, w->buf != NULL);
        /* 写线程可能已经起来并阻塞在 sem_job：置 quit 唤醒它退出 */
        if (wr_started) {
            SceUInt t = 5000000;
            w->quit = 1;
            sceKernelSignalSema(w->sem_job, 1);
            if (sceKernelWaitThreadEnd(w->thr_wr, NULL, &t) < 0) {
                /* 连收尸都等不到：整条流水线遗弃，fd/缓冲全交给槽位 */
                fd = -1;
                slot_abandon(w, f, 3, "资源不足");
                return 500;
            }
        }
        fd = -1;                     /* 所有权在槽位：由 slot_release 关闭 */
        slot_release(w);
        lock();
        file_fail_locked(f, "资源不足");
        clear_pending_finish_locked();
        unlock();
        return 500;
    }

    /* 生产者：只负责收 + 解密，攒满一块就交给 sha 线程（无校验则直接投写队列） */
    t0 = t_log = now_us();
    for (;;) {
        unsigned char *pbuf;
        int fill = 0, n;
        if (produced >= f->size) break;         /* 收满 */
        lock();
        if (g_abort) aborted = true;
        unlock();
        if (aborted) break;
        if (w->fail) break;                     /* 写盘失败：不再收 */
        w_mark = now_us();
        /* 等空块。拿不到也必须按失败收尾：裸 break 会落到"正常完成"分支，
         * 把没接满的文件改名成正式文件（st=1）。 */
        if (sceKernelWaitSema(w->sem_free, 1, NULL) < 0) { rd_fail = -1; break; }
        t_wait += now_us() - w_mark;
        pbuf = w->buf + (size_t)(w->idx_in % RECV_BLOCKS) * RECV_CHUNK;
        while (fill < RECV_CHUNK && produced + fill < f->size) {
            int want = RECV_CHUNK - fill;
            if (f->size - produced - fill < want)
                want = (int)(f->size - produced - fill);
            t_mark = now_us();
            n = fn(ctx, pbuf + fill, want);
            t_read += now_us() - t_mark;
            /* n==0 不是"没读失败"：http 层的 upload_stream 对"对端关闭"和"30s
             * 空闲超时"都返回 0。旧代码只把负数当失败 → rd_fail=0 时外层
             * if (rd_fail) 不成立，循环在 EOF 上空转烧 CPU，f->busy 也永远不清，
             * 会话卡在 RECEIVING（连空闲超时都凑不齐条件）。统一按失败收尾；
             * 记 -1 只为在日志里把"零字节"与"真错误码"区分开。 */
            if (n <= 0) { rd_fail = n ? n : -1; break; }
            fill += n;
        }
        if (fill > 0) {
            w->len[w->idx_in % RECV_BLOCKS] = fill;
            ++w->idx_in;
            produced += fill;
            sceKernelSignalSema(w->check_sha ? w->sem_sha : w->sem_job, 1);
        } else {
            sceKernelSignalSema(w->sem_free, 1); /* 一块都没收到（失败路径）：还回去 */
        }
        if (rd_fail) break;
        if (now_us() - t_log >= 2000000ull) {   /* 2s 一条：速率 + 是否还在推进 */
            uint64_t tn = now_us();
            /* wait/read 是生产者侧，write/sha 分别来自写盘线程与 sha 线程
             * （累计值在锁内读）：
             *   wait 占满窗口 → 流水线下游追不上（盘慢 / 下游线程被抢占）
             *   read 占满窗口 → 在等对端数据（网络受限；含 TLS 解密）
             *   write、sha 同时接近窗口 → 两者已各自跑满，任一慢就是瓶颈
             *   三者都不满但仍慢 → CPU 抢占（UI/握手），看三者之和 */
            uint64_t ww, ws;
            lock();
            ww = w->t_write;
            ws = w->t_sha;
            unlock();
            dlog("recv: progress %lld/%lld B  %.2f MiB/s"
                 "  [wait %llu read %llu write %llu sha %llu ms]",
                 (long long)f->got, (long long)f->size,
                 (double)(f->got - log_got) / 1048576.0
                     / ((double)(tn - t_log) / 1000000.0),
                 (unsigned long long)((t_wait - l_wait) / 1000),
                 (unsigned long long)((t_read - l_read) / 1000),
                 (unsigned long long)((ww - l_write) / 1000),
                 (unsigned long long)((ws - l_sha) / 1000));
            t_log = tn;
            log_got = f->got;
            l_wait = t_wait;
            l_read = t_read;
            l_write = ww;
            l_sha = ws;
        }
    }

    /* 收尾顺序：先等三块全部回到空闲（= sha 与写盘都把在途块处理完），再停
     * 线程。任一步等不到就**遗弃**整条流水线：不删线程、不释放资源，槽位转
     * ZOMBIE，等线程自己退出后由 reap_zombies 回收（旧代码在这里直接
     * DeleteThread + free，线程若还卡在 IO 里就会踩已释放的内存）。
     * 正常情况 ≤192KB，几百毫秒内就该回来。 */
    {
        int stuck = 0;
        SceUInt tmo = 10000000;
        if (sceKernelWaitSema(w->sem_free, RECV_BLOCKS, &tmo) < 0) {
            stuck = 1;
        } else {
            sceKernelSignalSema(w->sem_free, RECV_BLOCKS);
            w->quit = 1;
            if (w->thr_sha >= 0) sceKernelSignalSema(w->sem_sha, 1);
            sceKernelSignalSema(w->sem_job, 1);
            tmo = 10000000;                  /* 每次重置：超时值会被写回剩余量 */
            if (sceKernelWaitThreadEnd(w->thr_wr, NULL, &tmo) < 0) stuck = 2;
            if (!stuck && w->thr_sha >= 0) {
                tmo = 10000000;
                if (sceKernelWaitThreadEnd(w->thr_sha, NULL, &tmo) < 0) stuck = 2;
            }
        }
        if (stuck) {
            fd = -1;                         /* 所有权留在槽位（reap 负责关闭） */
            slot_abandon(w, f, stuck, "存储无响应");
            /* 遗弃后仍要通知两条消费者退出：stuck==1（等空块就超时）时上面还
             * 没置过 quit，写线程处理完在途块后会一直阻塞在 sem_job（生产者已
             * 返回），线程不退出 → ZOMBIE 永远没有 reap 的机会，8 次后槽位耗尽。
             * 两条线程都是 WaitSema 返回后先查 quit，故不会误写未就绪的块。 */
            w->quit = 1;
            if (w->thr_sha >= 0) sceKernelSignalSema(w->sem_sha, 1);
            sceKernelSignalSema(w->sem_job, 1);
            return 500;
        }
    }

    /* 两条消费者都已退出：先取摘要/统计，再释放槽位（关 fd、删线程、归还）。 */
    if (check_sha) mbedtls_sha256_finish(&w->sha, digest);
    wr_fail = w->fail;
    t_write = w->t_write;
    t_sha = w->t_sha;
    fd = -1;                                 /* 所有权在槽位，由 slot_release 关闭 */
    slot_release(w);

    if (aborted) {
        lock();
        f->busy = false;
        cleanup_parts();                     /* 含本文件的临时文件 */
        if (!sess_is_terminal(g_sess.state)) /* 别覆盖别处已判定的终态原因 */
            sess_terminal(RECV_ST_CANCEL, "用户取消");
        clear_pending_finish_locked();
        unlock();
        return 500;
    }
    if (rd_fail || wr_fail) {                /* 断流/空闲超时/socket 错/写盘失败 */
        lock();
        f->busy = false;
        /* 截断取证：收了多久、到多少字节停的。fn==-1 表示 fn 返回 0（对端关闭
         * 或 http 层单块空闲超时，http 层已有对应 dlog），其余负值为 socket/TLS 错。 */
        dlog("recv: upload fail got %lld/%lld B after %llu ms (fn=%d wr=%d)",
             (long long)f->got, (long long)f->size,
             (unsigned long long)((now_us() - t0) / 1000), rd_fail, wr_fail);
        if (g_abort) {                       /* http 层看到中止标志提前退出 → 取消 */
            cleanup_parts();
            if (!sess_is_terminal(g_sess.state))     /* 别覆盖已判定的终态原因 */
                sess_terminal(RECV_ST_CANCEL, "用户取消");
        } else if (wr_fail) {
            /* 写盘失败只算这个文件（其余文件可能还能写进去） */
            file_fail_locked(f, "写入失败（磁盘空间不足？）");
        } else {
            /* 断流/空闲超时是会话级：对端已不可达，剩下的文件不会再开始；
             * 按单文件处理反而要空等 120s 文件间超时才收场。 */
            sess_fail_locked(f->got > 0 ? "传输中断" : "对方未发送数据");
        }
        clear_pending_finish_locked();
        unlock();
        return 500;
    }

    if (check_sha && !hex_eq_nocase(f->sha256, digest, 32)) {
        lock();
        file_fail_locked(f, "sha256 校验失败");
        clear_pending_finish_locked();
        unlock();
        return 422;
    }
    if (sceIoRename(f->part, f->final) < 0) {
        lock();
        file_fail_locked(f, "文件落盘失败");
        clear_pending_finish_locked();
        unlock();
        return 500;
    }
    {
        uint64_t total = now_us() - t0;
        dlog("recv: file done %lld B in %llu ms (%.2f MiB/s) read=%llu ms write=%llu ms sha=%llu ms",
             (long long)f->got, (unsigned long long)(total / 1000),
             total ? (double)f->got / 1048576.0
                     / ((double)total / 1000000.0) : 0.0,
             (unsigned long long)(t_read / 1000),
             (unsigned long long)(t_write / 1000),
             (unsigned long long)(t_sha / 1000));
    }
    lock();
    f->st = 1;
    f->busy = false;
    update_cur_locked();
    check_all_done_locked();
    clear_pending_finish_locked();
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
            out->st[i] = (unsigned char)g_sess.f[i].st;
        }
        r = 1;
    }
    unlock();
    return r;
}

void recv_clear(void)
{
    reap_zombies();                      /* 会话收尾顺手回收已退出的僵尸槽位 */
    lock();
    if (g_phase == PH_ACTIVE) {
        if (any_busy_locked()) {             /* 收体进行中：别硬清，先请中止。
                                              * 清场与复位 phase 推迟到最后一个 worker
                                              * 收尾时（clear_pending_finish_locked） */
            g_abort = 1;
            g_clear_pending = 1;
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
    g_clear_pending = 0;
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
