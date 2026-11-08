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
#include "net/httpc.h"            /* 扫码链接：明文 HTTP 客户端（prepare/download） */
#include "core/json_util.h"
#include "core/config.h"
#include "core/dlog.h"

#define RECV_DECIDE_TIMEOUT_US (60 * 1000000LL)  /* prepare 等 UI 决定的上限 */
#define RECV_IDLE_TIMEOUT_US   (120 * 1000000LL) /* 接受后/文件间没动静 → TIMEOUT */
#define RECV_CHUNK             65536             /* 流式写盘缓冲（每个 upload 各自一份，见下） */
/* 收体中临时文件名后缀见 recv_path.h 的 RP_PART_SUFFIX。曾用 ".part"：落盘
 * 正式名沿用对端原名，对端若发来一个正好叫 "x.part" 的文件，它会被正常保存成
 * 正式文件，而开机清扫按后缀匹配又会把它当残留删掉——永久丢数据。改成一个
 * 正常文件名里基本不会出现的后缀。
 * 注：不再顺带清扫旧版遗留的 *.part（宁可留垃圾也不误删用户文件）。 */
#define RECV_BLOCKS            3         /* 收体轮转缓冲块数（每块 RECV_CHUNK） */
#define RECV_SLOTS             8         /* 收体流水线槽位上限（= 并发 upload 上限） */

/* 会话生命周期：无 / 待决定 / 活动（接受后直到 recv_clear） */
enum { PH_NONE = 0, PH_PENDING, PH_ACTIVE };

/* 待决定请求里的一条文件（按实际数量动态分配） */
typedef struct {
    char fileid[160];              /* 对方文件 ID（upload query 用） */
    char name[RP_REL_MAX + 1];     /* 规整后的相对路径（落盘口径） */
    char rname[RP_COMP_MAX + 1];   /* UI 改名：只替换最后一级；空=沿用原名 */
    char sha256[65];               /* 期望 sha256（空=不校验） */
    SceOff size;                   /* 期望字节数 */
    bool trunc;                    /* 名字过长被缩短过 */
    bool path_ok;                  /* 按当前保存目录算，路径预算内 */
} PendFile;

/* 一个已接受的待收文件（按参与文件数动态分配） */
typedef struct {
    char fileid[160];              /* 对方文件 ID（upload query 用） */
    char token[72];                /* 我方签发给对方的令牌 */
    char name[192];                /* 展示用 basename（兼容旧 UI） */
    char rel[RP_REL_MAX + 1];      /* 实际落盘相对路径（可能含 '/'） */
    char final[RP_PATH_MAX + 1];   /* 正式路径 */
    char part[RP_PATH_MAX + 1];    /* 临时路径（final + RP_PART_SUFFIX） */
    SceOff size;                   /* 期望字节数 */
    SceOff got;                    /* 已收字节（锁保护） */
    char sha256[65];               /* 期望 sha256（准备报文里给了才校验，空串=不校验） */
    int  st;                       /* 0=等待 1=完成 2=跳过 3=失败 */
    bool busy;                     /* 本文件正在流式收体（同会话多文件可并发，各自独立） */
    bool orphan;                   /* 收体流水线被遗弃：临时文件由槽位接管，cleanup_parts 跳过 */
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
    char  part[RP_PATH_MAX + 1]; /* 遗弃文件的临时路径（与 RFile.part 同宽） */
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

/* 待决定请求（PH_PENDING）：f/inc 按实际条目数动态分配，以下标访问。
 * inc 是 bitset（默认全选），count 变化时同步扩容。 */
static struct {
    char alias[64];
    char type[24];
    char ip[16];
    char fp[65];             /* 对端叶证书 SHA-256（TLS 才有；明文为空） */
    int  n;                  /* 已解析的条目数（≤ RECV_FILES_MAX） */
    int  cap;                /* f/inc 的容量 */
    int  overflow;           /* 超上限被丢弃的可用文件数（M3 起改为整份拒绝，恒为 0，
                              * 字段保留给旧 UI 快照兼容） */
    int  skipped;            /* 非法路径/目录条目等跳过数 */
    int  path_skipped;       /* 按当前保存目录超预算、接受时会跳过的条目数 */
    int  cfg_limit;           /* 设置里的接收文件上限（M3 固定 64，M5 接 config） */
    int  eff_limit;           /* 本次实际生效档位（文件数超过 cfg 时临时提升） */
    PendFile *f;
    unsigned char *inc;      /* bitset：bit i = 第 i 条勾选 */
} g_pend;

/* ---- M2 暂存根 ----
 * 每个接收顶层目录一个暂存根 <save_dir>/<top>.psvsend-part[/inner]，收完后由
 * finalize 线程整体 rename（目标不存在）或逐项合并（目标已存在）。平铺文件
 * 没有暂存根，直接写 <save_dir>/<name>.psvsend.tmp 后 rename。
 * roots 只登记目录型顶层；每项单独 malloc，保证 PathSet 的键指针在数组扩容时
 * 仍然有效（键指向 r->top）。 */
typedef struct {
    char top[RP_COMP_MAX + 1];        /* 最终顶层目录名 */
    char stage_name[RP_COMP_MAX + 1]; /* 暂存根名（同名已存在时自动 "top (k)"） */
    int  nfiles;                      /* 归入该根的文件数 */
} SessRoot;

typedef struct {
    int fidx;                         /* g_sess.f 下标 */
    int ridx;                         /* g_sess.roots 下标；-1=顶层平铺文件 */
} SessZero;

/* 活动会话（PH_ACTIVE）：f 按实际参与文件数动态分配 */
static struct {
    char session[40];     /* sessionId（hex） */
    char peer_ip[16];
    char peer_alias[64];
    int  n;               /* 参与文件数（已剔除未勾选） */
    RFile *f;
    int  skipped;         /* 接受时跳过（非法/超预算/冲突）的文件数 */
    int  cur;             /* 正在收的某个文件下标，-1=无（并发时取最早遇到的，仅 UI 高亮用） */
    int  state;           /* RecvStateId */
    char err[192];
    SceOff total;         /* 期望总字节（含 size==0 的已免收文件） */
    SceOff got_total;     /* 已收总字节（锁保护） */
    uint64_t start_us;    /* 接受时刻 */
    uint64_t last_us;     /* 接受/最近一次收体开始时刻（空闲超时判定用） */
    SessRoot **roots;     /* 目录型顶层的暂存根（按首次出现顺序） */
    int  nroots, roots_cap;
    SessZero *zeros;      /* 0 字节文件：finalize 时统一创建 */
    int  nzeros, zeros_cap;
    int  fin_attempted;   /* 已进入最终化：recv_clear 不再递归删暂存根 */
} g_sess;

static int  g_phase = PH_NONE;      /* 当前生命周期 */
/* 本次请求/会话是否来自"网页共享"（prepare 的 query 带了有效 PIN）。只用于
 * 停止共享时把网页侧的待决定请求拒掉、进行中的上传中止；普通 LocalSend 发送方
 * 不带 PIN，永远为 0，不受此影响。 */
static int  g_pend_web = 0;
static int  g_sess_web = 0;
/* 拉取会话（扫码链接）的下载线程还在跑：与推送会话的 f[].busy 同义——告诉
 * recv_clear/recv_status_pull"还有 worker 持有 g_sess"，别在文件间空档清场。
 * 只有扫码链路会置 1，推送会话恒为 0。 */
static int  g_sess_pull = 0;
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
static int  g_peer_gone_evt = 0;    /* 上一份 pending 因"发送方断开"消失（UI 一次性取走） */
static int  g_reject_evt = 0;       /* 最近一次 prepare 被拒原因（HTTP_REJ_*，UI 一次性取走） */
static volatile int g_abort = 0;    /* 用户中止请求（http 收体循环轮询） */
static SceUID g_mtx = -1;
/* 本会话保存目录：accept 前由 UI 用 recv_set_dir 设定（临时、不入 config）；
 * 启动清扫等默认场景在 recv_init 里回退 config saveDir。 */
static char g_dir[512];

/* 决定窗口的暂停/重开状态（UI 驱动，见 recv_decide_hold）：
 * hold=1 = 用户正在接收设置页/验证页里操作，不计时；
 * g_pend_dl = 当前截止时刻（绝对 us），暂停期间每轮被推到"现在 + 满窗"。 */
static volatile int g_pend_hold;
static volatile SceLong64 g_pend_dl;

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

/* 接收文件数档位：设置值默认 64（M5 才接 config），实际生效档位在文件数超过
 * 设置值时临时提升到最小可容纳档（64/128/256/512/1024/2048），不改设置本身。 */
#define RECV_FILES_LIMIT_CFG 64
static int files_limit_cfg(void) { return RECV_FILES_LIMIT_CFG; }
static int files_limit_raise(int need)
{
    static const int tier[] = { 64, 128, 256, 512, 1024, 2048 };
    int i;
    for (i = 0; i < (int)(sizeof tier / sizeof tier[0]); i++)
        if (need <= tier[i]) return tier[i];
    return RECV_FILES_MAX;
}

/* 拒绝原因事件：http 层在无锁上下文回调，这里补锁；锁内代码直接写 g_reject_evt。 */
static void reject_note_http(int reason)
{
    lock();
    g_reject_evt = reason;
    unlock();
}
static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

/* ---------- pending / session 动态数组生命周期（均需持 g_mtx） ---------- */

static void pend_free_locked(void)
{
    free(g_pend.f);
    free(g_pend.inc);
    g_pend.f = NULL;
    g_pend.inc = NULL;
    g_pend.n = g_pend.cap = 0;
    g_pend.overflow = g_pend.skipped = g_pend.path_skipped = 0;
}

static bool pend_inc_get(int i)
{
    if (i < 0 || i >= g_pend.n || !g_pend.inc) return false;
    return ((g_pend.inc[i >> 3] >> (i & 7)) & 1) != 0;
}

static void pend_inc_set(int i, bool on)
{
    if (i < 0 || i >= g_pend.cap || !g_pend.inc) return;
    if (on) g_pend.inc[i >> 3] |= (unsigned char)(1u << (i & 7));
    else    g_pend.inc[i >> 3] &= (unsigned char)~(1u << (i & 7));
}

/* 扩到至少 need 条；新条目清零、新勾选位默认 1。失败返回 false。 */
static bool pend_reserve_locked(int need)
{
    int oldcap = g_pend.cap, cap = oldcap ? oldcap : 16;
    PendFile *nf;
    unsigned char *ni;
    int oldbytes, newbytes;

    if (need <= oldcap) return true;
    if (need > RECV_FILES_MAX) return false;
    while (cap < need) {
        if (cap >= RECV_FILES_MAX) { cap = RECV_FILES_MAX; break; }
        cap *= 2;
        if (cap > RECV_FILES_MAX) cap = RECV_FILES_MAX;
    }
    nf = (PendFile *)realloc(g_pend.f, (size_t)cap * sizeof *nf);
    if (!nf) return false;
    g_pend.f = nf;
    ni = (unsigned char *)realloc(g_pend.inc, (size_t)((cap + 7) / 8));
    if (!ni) return false;
    g_pend.inc = ni;
    oldbytes = (oldcap + 7) / 8;
    newbytes = (cap + 7) / 8;
    if (newbytes > oldbytes)
        memset(g_pend.inc + oldbytes, 0xFF, (size_t)(newbytes - oldbytes));
    memset(g_pend.f + oldcap, 0, (size_t)(cap - oldcap) * sizeof *nf);
    g_pend.cap = cap;
    return true;
}

static void sess_free_locked(void)
{
    int i;
    for (i = 0; i < g_sess.nroots; i++) free(g_sess.roots[i]);
    free(g_sess.roots);
    g_sess.roots = NULL;
    g_sess.nroots = g_sess.roots_cap = 0;
    free(g_sess.zeros);
    g_sess.zeros = NULL;
    g_sess.nzeros = g_sess.zeros_cap = 0;
    g_sess.fin_attempted = 0;
    free(g_sess.f);
    g_sess.f = NULL;
    g_sess.n = 0;
}

/* ---------- 路径小工具（相对路径 → 最终/临时路径；全部可检测溢出） ---------- */

static void split_top_inner(const char *rel, char *top, size_t topn,
                            const char **inner)
{
    const char *slash = strchr(rel, '/');
    size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
    if (n >= topn) n = topn - 1;
    memcpy(top, rel, n);
    top[n] = 0;
    *inner = slash ? slash + 1 : "";
}

static const char *path_base(const char *rel)
{
    const char *slash = strrchr(rel, '/');
    return slash ? slash + 1 : rel;
}

/* rel → 暂存内最终路径 / 实际写入路径。
 * inner 为空 = 顶层平铺文件（没有暂存根）；否则 stage_name 是该顶层的暂存根名
 * （可能带 " (k)"，见 SessRoot）。所有拼接失败都返回 false，绝不静默截断。 */
static bool compute_paths(const char *rel, const char *stage_name,
                          char *final, size_t fsz, char *part, size_t psz)
{
    char top[RP_COMP_MAX + 1];
    const char *inner;

    split_top_inner(rel, top, sizeof top, &inner);
    if (!inner[0]) {
        return rp_final_path(g_dir, top, NULL, final, fsz) &&
               rp_flat_part_path(g_dir, top, part, psz);
    }
    return rp_stage_file_path(g_dir, stage_name, inner, final, fsz) &&
           rp_stage_part_path(g_dir, stage_name, inner, part, psz);
}

/* 暂存根下是否有被遗弃槽位（ZOMBIE）仍持有的临时文件。这类根不能删、也不能
 * 复用：槽位线程退出后会按记录路径 sceIoRemove，误删新会话的同名临时文件。 */
static bool stage_has_zombie(const char *root)
{
    int i;
    size_t n = strlen(root);
    bool hit = false;

    if (g_reap_mtx < 0) return false;
    sceKernelLockMutex(g_reap_mtx, 1, NULL);
    for (i = 0; i < RECV_SLOTS; i++) {
        RecvSlot *s = &g_slots[i];
        if (s->state != SLOT_ZOMBIE || !s->part[0]) continue;
        if (strncmp(s->part, root, n) == 0 && s->part[n] == '/') {
            hit = true;
            break;
        }
    }
    sceKernelUnlockMutex(g_reap_mtx, 1);
    return hit;
}

/* 为顶层目录挑一个可用的暂存根名并建目录：
 * 优先 "<top>.psvsend-part"；同名已存在（上次残留/用户自己的同名目录）时
 * 自动改用 "<top> (k).psvsend-part"，绝不删除未知数据。 */
static bool stage_pick_name(const char *top, char *out, size_t outsz)
{
    char cand[RP_COMP_MAX + 1];
    char path[RP_PATH_MAX + 1];
    SceIoStat st;
    int k;

    if (out && outsz) out[0] = 0;
    /* 保存目录不存在时不要空转 1000 次 mkdir */
    if (sceIoGetstat(g_dir, &st) < 0 || !(st.st_mode & SCE_S_IFDIR))
        return false;
    for (k = 0; k <= 999; k++) {
        if (k == 0) snprintf(cand, sizeof cand, "%s", top);
        else if (!rp_unique_name(top, k, cand, sizeof cand)) return false;
        if (!rp_stage_root_path(g_dir, cand, path, sizeof path)) continue;
        if (sceIoGetstat(path, &st) >= 0) continue;      /* 已存在：避让 */
        if (stage_has_zombie(path)) continue;            /* 遗弃槽位仍持有其中文件 */
        if (sceIoMkdir(path, 0777) >= 0) {
            snprintf(out, outsz, "%s", cand);
            return true;
        }
        /* mkdir 失败：父目录不存在（保存目录无效）或同名竞态，继续试 */
    }
    return false;
}

static bool sess_zero_add_locked(int fidx, int ridx)
{
    SessZero *nz;
    int nc;
    if (g_sess.nzeros >= g_sess.zeros_cap) {
        nc = g_sess.zeros_cap ? g_sess.zeros_cap * 2 : 16;
        nz = (SessZero *)realloc(g_sess.zeros, (size_t)nc * sizeof *nz);
        if (!nz) return false;
        g_sess.zeros = nz;
        g_sess.zeros_cap = nc;
    }
    g_sess.zeros[g_sess.nzeros].fidx = fidx;
    g_sess.zeros[g_sess.nzeros].ridx = ridx;
    g_sess.nzeros++;
    return true;
}

/* 预算按 M2 暂存内的 part 实际路径算（比最终路径长），超了跳过并计数。 */
static bool path_budget_ok(const char *rel)
{
    char top[RP_COMP_MAX + 1];
    char tmp[RP_PATH_MAX + 2];
    const char *inner;

    split_top_inner(rel, top, sizeof top, &inner);
    if (!inner[0]) return rp_flat_part_path(g_dir, top, tmp, sizeof tmp);
    return rp_stage_part_path(g_dir, top, inner, tmp, sizeof tmp);
}

static bool rel_replace_last(const char *rel, const char *newname,
                             char *out, size_t outsz);

/* 待确认条目的实际落盘路径（UI 改名只替换最后一级）。失败=超预算/超长。 */
static bool pend_item_rel_locked(int i, char *rel, size_t relsz)
{
    if (g_pend.f[i].rname[0])
        return rel_replace_last(g_pend.f[i].name, g_pend.f[i].rname,
                                rel, relsz);
    snprintf(rel, relsz, "%s", g_pend.f[i].name);
    return true;
}

/* 保存目录/UI 改名变化后：重算每条路径是否在预算内。 */
static void pend_recheck_paths_locked(void)
{
    int i, bad = 0;
    for (i = 0; i < g_pend.n; i++) {
        char rel[RP_REL_MAX + 1];
        char top[RP_COMP_MAX + 1];
        const char *inner;
        bool ok = pend_item_rel_locked(i, rel, sizeof rel);

        if (ok) {
            split_top_inner(rel, top, sizeof top, &inner);
            /* 顶层名以保留后缀结尾：会与暂存根/开机清扫撞名。M5 提供改名
             * 提示；在那之前一律跳过（不覆盖、不误删）。嵌套层安全。 */
            if (rp_reserved_name(top)) ok = false;
        }
        g_pend.f[i].path_ok = ok && path_budget_ok(rel);
        if (!g_pend.f[i].path_ok) bad++;
    }
    g_pend.path_skipped = bad;
}

/* 把 rel 的最后一级换成 newname；结果必须仍满足相对路径预算。 */
static bool rel_replace_last(const char *rel, const char *newname,
                             char *out, size_t outsz)
{
    const char *slash = strrchr(rel, '/');
    size_t pl = slash ? (size_t)(slash - rel + 1) : 0;
    size_t nl = strlen(newname);
    if (pl + nl > RP_REL_MAX || pl + nl >= outsz) return false;
    if (pl) memcpy(out, rel, pl);
    memcpy(out + pl, newname, nl + 1);
    return true;
}

/* 在 rel 的基础上生成 "base (k).ext" 形式的最后一级。 */
static bool rel_with_unique(const char *rel, int k, char *out, size_t outsz)
{
    char nn[RP_COMP_MAX + 2];
    const char *base = path_base(rel);
    if (!rp_unique_name(base, k, nn, sizeof nn)) return false;
    return rel_replace_last(rel, nn, out, outsz);
}

/* ---------- 大小写不敏感字符串集合（入站冲突/目录合并用） ----------
 * 键只存"指针 + 长度"，指向稳定的 RFile.rel；不复制字符串（2048 文件 ×
 * 最多 16 级目录时，复制键会多吃十几 MB）。调用者保证键内存在 set 生命周期
 * 内有效：build_session_locked 在 realloc 收缩 RFile 数组之前就释放 set。 */

typedef struct { const char *p; int len; } PKey;
typedef struct {
    PKey *keys;
    int n, cap;
    int *tab;                /* 存 keyidx+1；0=空 */
    int tcap;
} PathSet;

static unsigned ci_hash_n(const char *s, int len)
{
    unsigned h = 2166136261u;
    int i;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

static unsigned ci_hash(const char *s)
{
    return ci_hash_n(s, (int)strlen(s));
}

static bool ci_eq_n(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i];
        unsigned char y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

static bool pathset_init(PathSet *ps)
{
    memset(ps, 0, sizeof *ps);
    ps->tcap = 32;
    ps->tab = (int *)calloc((size_t)ps->tcap, sizeof *ps->tab);
    return ps->tab != NULL;
}

static void pathset_free(PathSet *ps)
{
    free(ps->keys);
    free(ps->tab);
    memset(ps, 0, sizeof *ps);
}

static bool pathset_rehash(PathSet *ps)
{
    int nt = ps->tcap * 2, i;
    int *ntab = (int *)calloc((size_t)nt, sizeof *ntab);
    if (!ntab) return false;
    for (i = 0; i < ps->n; i++) {
        unsigned h = ci_hash_n(ps->keys[i].p, ps->keys[i].len) &
                     (unsigned)(nt - 1);
        while (ntab[h]) h = (h + 1) & (unsigned)(nt - 1);
        ntab[h] = i + 1;
    }
    free(ps->tab);
    ps->tab = ntab;
    ps->tcap = nt;
    return true;
}

/* 返回 keys 下标；不存在 -1 */
static int pathset_find_n(const PathSet *ps, const char *s, int len)
{
    unsigned h;
    if (!ps->tab || ps->n == 0) return -1;
    h = ci_hash_n(s, len) & (unsigned)(ps->tcap - 1);
    while (ps->tab[h]) {
        int idx = ps->tab[h] - 1;
        if (ps->keys[idx].len == len && ci_eq_n(ps->keys[idx].p, s, len))
            return idx;
        h = (h + 1) & (unsigned)(ps->tcap - 1);
    }
    return -1;
}

static int pathset_find(const PathSet *ps, const char *s)
{
    return pathset_find_n(ps, s, (int)strlen(s));
}

/* 键指针必须长期有效（见上面注释） */
static bool pathset_add_n(PathSet *ps, const char *s, int len)
{
    unsigned h;
    int idx;
    if (pathset_find_n(ps, s, len) >= 0) return true;
    if ((ps->n + 1) * 10 > ps->tcap * 7 && !pathset_rehash(ps)) return false;
    if (ps->n >= ps->cap) {
        int nc = ps->cap ? ps->cap * 2 : 64;
        PKey *nk = (PKey *)realloc(ps->keys, (size_t)nc * sizeof *nk);
        if (!nk) return false;
        ps->keys = nk;
        ps->cap = nc;
    }
    idx = ps->n++;
    ps->keys[idx].p = s;
    ps->keys[idx].len = len;
    h = ci_hash_n(s, len) & (unsigned)(ps->tcap - 1);
    while (ps->tab[h]) h = (h + 1) & (unsigned)(ps->tcap - 1);
    ps->tab[h] = idx + 1;
    return true;
}

static bool pathset_add(PathSet *ps, const char *s)
{
    return pathset_add_n(ps, s, (int)strlen(s));
}

/* 取得/创建顶层目录的暂存根，返回 roots 下标。
 * 返回 -1 = 内存不足（*oom=1）；-2 = 建不了暂存根（跳过该顶层）。 */
static int root_get_or_create_locked(PathSet *tops, const char *top, bool *oom)
{
    int idx = pathset_find(tops, top), nc;
    SessRoot *r;
    SessRoot **nr;

    *oom = false;
    if (idx >= 0) return idx;
    if (g_sess.nroots >= g_sess.roots_cap) {
        nc = g_sess.roots_cap ? g_sess.roots_cap * 2 : 8;
        nr = (SessRoot **)realloc(g_sess.roots, (size_t)nc * sizeof *nr);
        if (!nr) { *oom = true; return -1; }
        g_sess.roots = nr;
        g_sess.roots_cap = nc;
    }
    r = (SessRoot *)calloc(1, sizeof *r);
    if (!r) { *oom = true; return -1; }
    snprintf(r->top, sizeof r->top, "%s", top);
    if (!stage_pick_name(top, r->stage_name, sizeof r->stage_name)) {
        dlog("recv: cannot create stage root for %s", top);
        free(r);
        return -2;
    }
    idx = g_sess.nroots;
    g_sess.roots[idx] = r;
    g_sess.nroots++;
    if (!pathset_add(tops, r->top)) {          /* 键指向 r->top，扩容不影响 */
        g_sess.nroots--;
        free(r);
        *oom = true;
        return -1;
    }
    dlog("recv: stage root %s (top %s)", r->stage_name, r->top);
    return idx;
}

/* rel 的任一父目录是文件（入站"文件挡目录"） */
static bool has_file_ancestor(const PathSet *files, const char *rel)
{
    size_t i;
    for (i = 0; rel[i]; i++) {
        if (rel[i] == '/' && i > 0) {
            if (i > RP_REL_MAX) return true;
            if (pathset_find_n(files, rel, (int)i) >= 0) return true;
        }
    }
    return false;
}

/* 把 rel 的所有父目录登记进 dirs（目录合并：保留首次拼写）。
 * 键指向 rel 本身，调用者必须传稳定内存（build 里传 RFile.rel）。 */
static bool add_ancestors(PathSet *dirs, const char *rel)
{
    size_t i;
    for (i = 0; rel[i]; i++) {
        if (rel[i] == '/' && i > 0) {
            if (i > RP_REL_MAX) return false;
            if (!pathset_add_n(dirs, rel, (int)i)) return false;
        }
    }
    return true;
}

/* ---------- 惰性建目录（逐级 mkdir + 小缓存） ----------
 * 缓存只做"最近建过"的加速，命中失败就重新 mkdir，不会改变正确性。
 * 调用点都在释放 g_mtx 之后；缓存自身用 g_mtx 短暂保护。 */
#define RECV_DIR_CACHE 256
static char g_dir_cache[RECV_DIR_CACHE][RP_PATH_MAX + 1];
static unsigned char g_dir_cache_ok[RECV_DIR_CACHE];

static void dir_cache_reset_locked(void)
{
    memset(g_dir_cache_ok, 0, sizeof g_dir_cache_ok);
}

static bool dir_cache_hit(const char *path)
{
    unsigned h = ci_hash(path) % RECV_DIR_CACHE;
    bool hit;
    lock();
    hit = g_dir_cache_ok[h] && rp_name_eq(g_dir_cache[h], path);
    unlock();
    return hit;
}

static void dir_cache_put(const char *path)
{
    unsigned h = ci_hash(path) % RECV_DIR_CACHE;
    lock();
    snprintf(g_dir_cache[h], sizeof g_dir_cache[h], "%s", path);
    g_dir_cache_ok[h] = 1;
    unlock();
}

static bool ensure_dir(const char *path)
{
    SceIoStat st;
    if (dir_cache_hit(path)) return true;
    if (sceIoGetstat(path, &st) == 0) {
        if (!(st.st_mode & SCE_S_IFDIR)) return false;
        dir_cache_put(path);
        return true;
    }
    if (sceIoMkdir(path, 0777) < 0) {
        if (sceIoGetstat(path, &st) != 0 || !(st.st_mode & SCE_S_IFDIR))
            return false;
    }
    dir_cache_put(path);
    return true;
}

/* 为最终文件路径逐级建父目录（g_dir 必须已是 ux0: 绝对路径） */
static bool mk_parents(const char *filepath)
{
    char tmp[RP_PATH_MAX + 1];
    char *p;
    size_t n = strlen(filepath);
    if (n >= sizeof tmp) return false;
    memcpy(tmp, filepath, n + 1);
    p = strrchr(tmp, '/');
    if (!p) return true;
    *p = 0;
    if (strncmp(tmp, "ux0:", 4) != 0) return false;
    for (p = tmp + 4; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (strlen(tmp) > 4 && !ensure_dir(tmp)) { *p = '/'; return false; }
            *p = '/';
        }
    }
    return strlen(tmp) <= 4 || ensure_dir(tmp);
}

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

/* 该临时路径是否正被某个 ZOMBIE 槽位占用（resolve_disk_conflict 避让同名 part 用）。
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
    if (g_sess_pull) return true;        /* 拉取线程可能在文件间空档，仍持有 g_sess */
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

/* 把对方给的（或 UI 改名的）名字净化成可落盘的单级文件名。落盘与 UI 改名共用
 * 本函数，保证"行上显示的即落盘名"。实现委托给 M0 的 rp_sanitize_component：
 * 同一套 ux0: 合法性、UTF-8 边界截断、保后缀规则。
 * 返回 true = 因超出长度预算被截短（确认页提示"文件名过长"就用它）。 */
bool recv_sanitize_name(const char *in, char *out, int n)
{
    const char *base = in ? in : "";
    const char *slash;
    bool cut = false;

    if (!out || n <= 0) return false;
    out[0] = 0;
    slash = strrchr(base, '/');
    if (slash) base = slash + 1;
    if (!rp_sanitize_component(base, out, (size_t)n, &cut)) {
        if (n >= 8) memcpy(out, "unnamed", 8);
        return false;
    }
    return cut;
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

static void rm_stage_roots_locked(void);      /* 见下：finalize 辅助区 */

/* recv_clear 曾因收体未结束而推迟清场 → 在本 worker 收尾处补做（持锁调用）。
 * 必须等所有 busy 都清零再动手：此时 g_sess 不会再被任何 worker 访问，
 * 复位 g_phase 后新 prepare 覆盖 g_sess 才是安全的。
 * 最终化（FINALIZING）期间同样推迟：finalize 线程还在用 g_sess/暂存根。 */
static void clear_pending_finish_locked(void)
{
    if (!g_clear_pending || any_busy_locked()) return;
    if (g_sess.state == RECV_ST_FINALIZING) return;
    cleanup_parts();
    if (!g_sess.fin_attempted) rm_stage_roots_locked();
    sess_free_locked();
    g_phase = PH_NONE;
    g_pend_hold = 0;
    g_abort = 0;
    g_clear_pending = 0;
    dlog("recv: session cleared (deferred)");
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

/* ---------------- M2 最终化（暂存根 → 最终位置） ----------------
 * 全部文件收尾后由独立线程做 IO，不持 g_lock；finalize 期间 recv_abort /
 * recv_http_cancel / recv_clear / 空闲超时全部绕开（见各入口的 FINALIZING 判断）。
 * 目标不存在 → 整个暂存根 rename（原子）；已存在 → 逐项合并，文件/类型冲突加
 * " (k)"。多顶层各自独立，成功的不回滚；失败的暂存根保留给下次启动清扫。 */

static bool str_ends_ci(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lf = strlen(suffix);
    return ls >= lf && rp_name_cmp(s + ls - lf, suffix) == 0;
}

/* 递归删除（跳过 . / ..）。用于取消/超时的暂存根清理与开机清扫。 */
static void rm_tree(const char *path)
{
    SceIoStat st;
    SceUID d;
    SceIoDirent de;

    if (sceIoGetstat(path, &st) < 0) return;
    if (!(st.st_mode & SCE_S_IFDIR)) { sceIoRemove(path); return; }
    d = sceIoDopen(path);
    if (d >= 0) {
        memset(&de, 0, sizeof de);
        while (sceIoDread(d, &de) > 0) {
            char child[RP_PATH_MAX + 1];
            if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0)
                continue;
            if (snprintf(child, sizeof child, "%s/%s", path, de.d_name) >=
                (int)sizeof child)
                continue;
            rm_tree(child);
        }
        sceIoDclose(d);
    }
    sceIoRmdir(path);
}

#define FIN_MAX_DEPTH (RP_MAX_DEPTH + 2)

/* 把 src 目录内容逐项并入 dst（dst 已存在）。同名文件/类型冲突用 " (k)"；
 * 返回 false = 有项没能移动（src 保留未移动的部分）。 */
static bool merge_dir(const char *src, const char *dst, int depth)
{
    SceUID d;
    SceIoDirent de;
    bool ok = true;

    if (depth > FIN_MAX_DEPTH) return false;
    d = sceIoDopen(src);
    if (d < 0) return false;
    memset(&de, 0, sizeof de);
    while (sceIoDread(d, &de) > 0) {
        char sp[RP_PATH_MAX + 1], dp[RP_PATH_MAX + 1];
        SceIoStat dt, nt;
        bool sdir, ddir;

        if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0)
            continue;
        if (snprintf(sp, sizeof sp, "%s/%s", src, de.d_name) >= (int)sizeof sp ||
            snprintf(dp, sizeof dp, "%s/%s", dst, de.d_name) >= (int)sizeof dp) {
            ok = false;
            continue;
        }
        if (sceIoGetstat(dp, &dt) < 0) {
            if (sceIoRename(sp, dp) < 0) ok = false;
            continue;
        }
        sdir = (de.d_stat.st_mode & SCE_S_IFDIR) != 0;
        ddir = (dt.st_mode & SCE_S_IFDIR) != 0;
        if (sdir && ddir) {
            if (!merge_dir(sp, dp, depth + 1)) ok = false;
        } else {
            char nn[RP_COMP_MAX + 1], np[RP_PATH_MAX + 1];
            int k;
            bool moved = false;
            for (k = 1; k <= 999; k++) {
                if (!rp_unique_name(de.d_name, k, nn, sizeof nn)) break;
                if (snprintf(np, sizeof np, "%s/%s", dst, nn) >= (int)sizeof np)
                    continue;
                if (sceIoGetstat(np, &nt) < 0) {
                    if (sceIoRename(sp, np) == 0) moved = true;
                    break;
                }
            }
            if (!moved) ok = false;
        }
    }
    sceIoDclose(d);
    sceIoRmdir(src);        /* 合并完应为空；失败时非空，rmdir 失败即保留 */
    return ok;
}

/* 单个目录型顶层的收尾：目标不存在 → 整体 rename；已存在 → 逐项合并。
 * 返回 false = 该根失败（暂存根保留，交给下次启动清扫）。 */
static bool finish_root(const SessRoot *r)
{
    char stage[RP_PATH_MAX + 1], target[RP_PATH_MAX + 1];
    SceIoStat st;

    if (!r->stage_name[0]) return true;
    if (!rp_stage_root_path(g_dir, r->stage_name, stage, sizeof stage) ||
        !rp_final_path(g_dir, r->top, NULL, target, sizeof target))
        return false;
    if (sceIoGetstat(stage, &st) < 0)
        return true;                       /* 没建过/已处理：无文件可搬 */
    if (stage_has_zombie(stage))
        return false;                      /* 有遗弃槽位仍在写：整根保留，别搬半成品 */
    if (r->nfiles == 0) {                  /* 空根：直接删掉 */
        sceIoRmdir(stage);
        return true;
    }
    if (sceIoGetstat(target, &st) < 0)
        return sceIoRename(stage, target) == 0;
    if (!(st.st_mode & SCE_S_IFDIR))
        return false;                      /* 目标被同名文件占用：留给 M5 重命名流程 */
    return merge_dir(stage, target, 0);
}

static bool create_zero_file(const char *path)
{
    SceUID fd;
    if (!mk_parents(path)) return false;
    fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) return false;
    sceIoClose(fd);
    return true;
}

/* 顶层平铺 0 字节文件：在保存目录里选一个不冲突的名字并创建。 */
static bool create_zero_flat(const char *name)
{
    char cand[RP_COMP_MAX + 1], path[RP_PATH_MAX + 1];
    SceIoStat st;
    int k;

    for (k = 0; k <= 999; k++) {
        if (k == 0) snprintf(cand, sizeof cand, "%s", name);
        else if (!rp_unique_name(name, k, cand, sizeof cand)) return false;
        if (!rp_final_path(g_dir, cand, NULL, path, sizeof path)) return false;
        if (sceIoGetstat(path, &st) >= 0) continue;
        return create_zero_file(path);
    }
    return false;
}

/* 0 字节文件统一在 finalize 创建：目录内的写进暂存根（随后随根搬走），
 * 平铺的直接落保存目录。 */
static bool create_zero_staged(const char *rel, const char *stage_name)
{
    char top[RP_COMP_MAX + 1], path[RP_PATH_MAX + 1];
    const char *inner;

    split_top_inner(rel, top, sizeof top, &inner);
    if (!inner[0]) return create_zero_flat(top);
    if (!rp_stage_file_path(g_dir, stage_name, inner, path, sizeof path))
        return false;
    return create_zero_file(path);
}

static SceUID g_fin_thr = -1;

static void fin_reap(void)
{
    if (g_fin_thr >= 0 && thread_exited(g_fin_thr)) {
        sceKernelDeleteThread(g_fin_thr);
        g_fin_thr = -1;
    }
}

static int fin_thread(SceSize args, void *argp)
{
    int i, roots_ok = 0, roots_bad = 0, zero_bad = 0;
    unsigned char *zok = NULL;

    (void)args; (void)argp;
    if (g_sess.nzeros > 0)
        zok = (unsigned char *)calloc((size_t)g_sess.nzeros, 1);

    for (i = 0; i < g_sess.nzeros; i++) {
        int fidx = g_sess.zeros[i].fidx;
        int ridx = g_sess.zeros[i].ridx;
        const char *stage = (ridx >= 0 && ridx < g_sess.nroots)
                                ? g_sess.roots[ridx]->stage_name : "";
        bool ok = create_zero_staged(g_sess.f[fidx].rel, stage);
        if (zok) zok[i] = ok ? 1 : 0;
        if (!ok) zero_bad++;
    }
    for (i = 0; i < g_sess.nroots; i++) {
        if (finish_root(g_sess.roots[i])) roots_ok++;
        else roots_bad++;
    }

    lock();
    if (g_phase == PH_ACTIVE && g_sess.state == RECV_ST_FINALIZING) {
        int failed = 0, done = 0;
        for (i = 0; i < g_sess.nzeros && zok; i++)
            if (!zok[i]) g_sess.f[g_sess.zeros[i].fidx].st = 3;
        for (i = 0; i < g_sess.n; i++) {
            if (g_sess.f[i].st == 3) failed++;
            else if (g_sess.f[i].st == 1) done++;
        }
        g_sess.cur = -1;
        g_sess.err[0] = 0;
        if (roots_bad > 0 && roots_ok == 0)
            sess_terminal(RECV_ST_FAIL, "文件整理失败（暂存已保留）");
        else if (roots_bad > 0)
            sess_terminal(RECV_ST_PARTIAL, "部分文件夹整理失败（暂存已保留）");
        else if (failed > 0 && done == 0)
            sess_terminal(RECV_ST_FAIL, "文件接收失败");
        else if (failed > 0 || zero_bad > 0)
            sess_terminal(RECV_ST_PARTIAL, "部分文件接收失败");
        else
            sess_terminal(RECV_ST_DONE, NULL);
        dlog("recv: finalize done roots ok=%d bad=%d zero_bad=%d done=%d failed=%d",
             roots_ok, roots_bad, zero_bad, done, failed);
        clear_pending_finish_locked();     /* 最终化期间被推迟的清场在这里补做 */
    }
    unlock();
    free(zok);
    return 0;
}

/* 进入最终化（持锁调用）。线程启动失败也必须收尾成 FAIL，不能卡在 FINALIZING。 */
static void finalize_start_locked(void)
{
    SceUID t;

    if (g_sess.fin_attempted) return;
    g_sess.fin_attempted = 1;
    if (g_sess.nroots == 0 && g_sess.nzeros == 0) {
        int i, failed = 0;
        for (i = 0; i < g_sess.n; i++)
            if (g_sess.f[i].st == 3) failed++;
        g_sess.cur = -1;
        g_sess.err[0] = 0;
        if (failed) sess_terminal(RECV_ST_PARTIAL, "部分文件接收失败");
        else        sess_terminal(RECV_ST_DONE, NULL);
        return;
    }
    g_sess.state = RECV_ST_FINALIZING;
    g_sess.cur = -1;
    g_sess.last_us = now_us();
    fin_reap();
    /* 合并是递归目录遍历，栈留足（深度 ≤18，每层约 4KB） */
    t = sceKernelCreateThread("psvsend_fin", fin_thread, 0x40, 0x40000, 0,
                              SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (t < 0) {
        dlog("recv: finalize thread create fail 0x%08X", (unsigned)t);
        sess_terminal(RECV_ST_FAIL, "无法启动文件整理");
        return;
    }
    if (sceKernelStartThread(t, 0, NULL) < 0) {
        sceKernelDeleteThread(t);
        dlog("recv: finalize thread start fail");
        sess_terminal(RECV_ST_FAIL, "无法启动文件整理");
        return;
    }
    g_fin_thr = t;
    dlog("recv: finalizing %d root(s), %d zero file(s)", g_sess.nroots,
         g_sess.nzeros);
}

/* 取消/超时/文件失败时清场：递归删掉本次建的暂存根（含已落盘的半成品）。
 * finalize 已尝试过则不删（成功已搬走；失败按计划保留给下次启动清扫）。 */
static void rm_stage_roots_locked(void)
{
    int i;
    char path[RP_PATH_MAX + 1];

    for (i = 0; i < g_sess.nroots; i++) {
        SessRoot *r = g_sess.roots[i];
        if (!r->stage_name[0]) continue;
        if (!rp_stage_root_path(g_dir, r->stage_name, path, sizeof path))
            continue;
        if (stage_has_zombie(path)) {
            /* 遗弃槽位还持有其中的临时文件：删掉会让它退出时误删新会话的
             * 同名文件。保留整根，交给下次启动清扫（那时已无 zombie）。 */
            dlog("recv: keep stage %s (zombie holds it)", r->stage_name);
            continue;
        }
        rm_tree(path);
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
    char nd[512];
    size_t sl;

    if (dir && dir[0]) {
        sl = strlen(dir);
        if (sl >= sizeof nd) sl = sizeof nd - 1;
        memcpy(nd, dir, sl);
        while (sl > 5 && nd[sl - 1] == '/') sl--;   /* 去尾斜杠（ux0:/ 根自带斜杠保留） */
        nd[sl] = 0;
        if (sl < 5 || strncmp(nd, "ux0:", 4) != 0)
            snprintf(nd, sizeof nd, "%s", PSVSEND_DL_DIR);
    } else {
        config_get_save_dir(nd, sizeof nd);
    }
    lock();
    snprintf(g_dir, sizeof g_dir, "%s", nd);
    if (g_phase == PH_PENDING) pend_recheck_paths_locked();
    unlock();
    dlog("recv: dir set %s", g_dir);
}

void recv_init(void)
{
    SceUID d;
    SceIoDirent de;
    char p[RP_PATH_MAX + 1];
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
    g_sess_pull = 0;
    pend_free_locked();
    sess_free_locked();
    g_sess.skipped = 0;
    dir_cache_reset_locked();
    unlock();
    /* 清扫上次异常退出残留（只扫当前默认/设置目录顶层）：
     *   - *.psvsend-part 目录（M2 暂存根，可能带 " (k)"）→ 递归删除；
     *   - *.psvsend.tmp 文件 / .psvsend-tmp-* → 删除。
     * 正式文件不带这些后缀，所以不会误删已收完的内容。用户自己正好有同名
     * 文件/目录的取舍见 TODO「后缀即保留名」：视为残留，这是既定设计。
     * 注：不清理旧版遗留的 *.part —— 那正是"对端发来叫 x.part 的合法文件被当
     * 残留删掉"的根源（见 RECV_PART_SUFFIX 处说明）。 */
    d = sceIoDopen(g_dir);
    if (d >= 0) {
        memset(&de, 0, sizeof de);
        while (sceIoDread(d, &de) > 0) {
            bool isdir;
            if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0)
                continue;
            if (snprintf(p, sizeof p, "%s/%s", g_dir, de.d_name) >=
                (int)sizeof p)
                continue;
            isdir = (de.d_stat.st_mode & SCE_S_IFDIR) != 0;
            if (isdir && str_ends_ci(de.d_name, RP_STAGE_SUFFIX)) {
                rm_tree(p);
                dlog("recv: init removed stale stage %s", de.d_name);
            } else if (!isdir &&
                       (str_ends_ci(de.d_name, RP_PART_SUFFIX) ||
                        strncmp(de.d_name, ".psvsend-tmp-", 13) == 0)) {
                sceIoRemove(p);
                dlog("recv: init removed stale %s", de.d_name);
            }
        }
        sceIoDclose(d);
    }
}

int recv_pending_summary(RecvPendingSummary *out)
{
    int i, r = 0;
    lock();
    if (g_phase == PH_PENDING) {
        if (out) {
            int sel = 0, trunc = 0;
            SceOff tot = 0, ssz = 0;
            for (i = 0; i < g_pend.n; i++) {
                SceOff z = g_pend.f[i].size;
                tot += z;
                if (g_pend.f[i].trunc) trunc++;
                if (g_pend.f[i].path_ok && pend_inc_get(i)) {
                    sel++;
                    ssz += z;
                }
            }
            snprintf(out->peer_alias, sizeof out->peer_alias, "%s", g_pend.alias);
            snprintf(out->peer_type, sizeof out->peer_type, "%s", g_pend.type);
            snprintf(out->peer_ip, sizeof out->peer_ip, "%s", g_pend.ip);
            snprintf(out->peer_fp, sizeof out->peer_fp, "%s", g_pend.fp);
            out->count = g_pend.n;
            out->selected = sel;
            out->overflow = g_pend.overflow;
            out->skipped = g_pend.skipped + g_pend.path_skipped;
            out->trunc = trunc;
            out->total = tot;
            out->selected_size = ssz;
            out->cfg_limit = g_pend.cfg_limit;
            out->eff_limit = g_pend.eff_limit;
        }
        r = 1;
    }
    unlock();
    return r;
}

int recv_pending_window(int base, int max, RecvPendingRow *out)
{
    int i, n = 0;
    if (!out || max <= 0) return 0;
    if (base < 0) base = 0;
    lock();
    if (g_phase == PH_PENDING) {
        for (i = base; i < g_pend.n && n < max; i++, n++) {
            snprintf(out[n].name, sizeof out[n].name, "%s", g_pend.f[i].name);
            out[n].size = g_pend.f[i].size;
            out[n].trunc = g_pend.f[i].trunc;
            out[n].selected = pend_inc_get(i);
            out[n].ok = g_pend.f[i].path_ok;
        }
    }
    unlock();
    return n;
}

void recv_pending_toggle(int idx, bool on)
{
    lock();
    if (g_phase == PH_PENDING && idx >= 0 && idx < g_pend.n)
        pend_inc_set(idx, on);
    unlock();
}

void recv_pending_rename(int idx, const char *name)
{
    char comp[RP_COMP_MAX + 1];
    bool cut = false;
    if (idx < 0 || !name) return;
    lock();
    if (g_phase == PH_PENDING && idx < g_pend.n) {
        if (!name[0]) {
            g_pend.f[idx].rname[0] = 0;
        } else if (rp_sanitize_component(name, comp, sizeof comp, &cut)) {
            snprintf(g_pend.f[idx].rname, sizeof g_pend.f[idx].rname, "%s", comp);
        }
        pend_recheck_paths_locked();      /* 改名可能超出路径预算 */
    }
    unlock();
}

/* ---- 旧全量快照（兼容包装；M4 下线） ---- */
int recv_pending_pull(RecvPending *out)
{
    int i, r = 0;
    lock();
    if (g_phase == PH_PENDING) {
        if (out) {                   /* out==NULL：纯探测，只回答"有没有" */
            snprintf(out->peer_alias, sizeof out->peer_alias, "%s", g_pend.alias);
            snprintf(out->peer_type, sizeof out->peer_type, "%s", g_pend.type);
            snprintf(out->peer_ip, sizeof out->peer_ip, "%s", g_pend.ip);
            snprintf(out->peer_fp, sizeof out->peer_fp, "%s", g_pend.fp);
            out->count = g_pend.n;
            out->overflow = g_pend.overflow;
            out->total = 0;
            for (i = 0; i < g_pend.n; i++) out->total += g_pend.f[i].size;
            for (i = 0; i < g_pend.n && i < RECV_MAX_FILES; i++) {
                snprintf(out->files[i].name, sizeof out->files[i].name, "%s",
                         path_base(g_pend.f[i].name));
                out->files[i].size = g_pend.f[i].size;
                out->files[i].trunc = g_pend.f[i].trunc;
            }
        }
        r = 1;
    }
    unlock();
    return r;
}

int recv_pending_peer_gone_take(void)
{
    int r;
    lock();
    r = g_peer_gone_evt;
    g_peer_gone_evt = 0;
    unlock();
    return r;
}

int recv_reject_take(void)
{
    int r;
    lock();
    r = g_reject_evt;
    g_reject_evt = 0;
    unlock();
    return r;
}

void recv_set_include(const bool inc[RECV_MAX_FILES])
{
    int i, n;
    lock();
    if (g_phase == PH_PENDING) {
        n = g_pend.n < RECV_MAX_FILES ? g_pend.n : RECV_MAX_FILES;
        for (i = 0; i < n; i++) pend_inc_set(i, inc && inc[i]);
    }
    unlock();
}

/* 兼容包装：等价 recv_pending_rename（idx 为全局下标）。 */
void recv_set_name(int idx, const char *name)
{
    recv_pending_rename(idx, name);
}

void recv_decide(bool accept)
{
    lock();
    if (g_phase == PH_PENDING && g_pend_result == 0)
        g_pend_result = accept ? 1 : 2;
    unlock();
}

/* 决定窗口的语义 = "确认页上没有操作满 60s"（不是"从弹窗开始 60s"）：
 *   - 用户在接收设置页 / 验证页（含设置页里再打开的目录选择页）里时由 UI
 *     置 hold=1，暂停计时；回到确认页置 0，窗口重开成满 60s。回来时是"重开"
 *     而不是"接着走"——接着走会出现"看完验证页回来只剩 5s"，比不暂停更糟。
 *   - 确认页上任何按键/触摸走 recv_decide_touch，同样重开满窗。
 * 暂停不阻断 d199 的探活：对方撤销照样 200ms 内收场，回来看到的是"对方已取消"。
 * 一律在锁内判定 PH_PENDING：请求已结束（接受/拒绝/对方撤销/到点）时这些调用
 * 都是空操作，不会把状态留给下一份请求。 */
void recv_decide_hold(int on)
{
    lock();
    if (g_phase == PH_PENDING) {
        g_pend_hold = on ? 1 : 0;
        if (!on) g_pend_dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
    }
    unlock();
}

void recv_decide_touch(void)
{
    lock();
    if (g_phase == PH_PENDING && !g_pend_hold)
        g_pend_dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
    unlock();
}

/* 确认页倒计时用：剩余整秒。-1 = 没有待决定请求或正在暂停（不该显示数字）。
 * 注意 now_us() 与 RECV_DECIDE_TIMEOUT_US 都是微秒，这里要除 1000000。 */
int recv_decide_left_s(void)
{
    SceLong64 left;
    lock();
    if (g_phase != PH_PENDING || g_pend_hold) {
        unlock();
        return -1;
    }
    left = g_pend_dl - (SceLong64)now_us();
    unlock();
    if (left < 0) left = 0;
    return (int)(left / 1000000);
}

void recv_abort(void)
{
    lock();
    if (g_phase != PH_ACTIVE || g_sess.state == RECV_ST_FINALIZING) {
        /* 最终化期间不可取消：暂存根正在被 rename/合并，打断会留下半成品 */
        unlock();
        return;
    }
    if (any_busy_locked()) {
        g_abort = 1;                       /* 收体循环下一块检测到 */
    } else if (!sess_is_terminal(g_sess.state)) {
        cleanup_parts();
        sess_terminal(RECV_ST_CANCEL, "用户取消");
    }
    unlock();
}

/* 停止网页共享时调用：网页侧的待决定请求按"拒绝"立刻收尾（等决定的 prepare
 * 线程 50ms 内返回 403，浏览器看到失败），已接受的网页上传会话中止。
 * 普通 LocalSend 发送方（prepare 不带 PIN）不受影响。 */
void recv_cancel_web(void)
{
    int abort_now = 0;
    lock();
    if (g_phase == PH_PENDING && g_pend_web && g_pend_result == 0)
        g_pend_result = 2;                  /* 走拒绝分支：清占位、回 403 */
    else if (g_phase == PH_ACTIVE && g_sess_web)
        abort_now = 1;
    unlock();
    if (abort_now)
        recv_abort();
}

/* http 层收体轮询用：有未决的"用户中止"请求就尽快收尾（无锁读 volatile） */
static bool recv_abort_pending(void)
{
    return g_abort != 0;
}

/* 由 g_pend 组装活动会话（推送会话与扫码拉取会话共用）。
 *  - 只收 UI 勾选且路径预算可用的文件；
 *  - 目录型顶层在锁内建一次暂存根（每个顶层一次 stat/mkdir，不是每文件）；
 *    同名已存在则自动 " (k).psvsend-part"，绝不删未知数据；
 *  - 0 字节文件只登记、不建文件（finalize 统一建），避免 2048 次锁内 IO；
 *  - 平铺文件的磁盘同名冲突留到 upload 真正写盘时再解析；
 *  - session_id 为空串 = 本地生成（推送会话），否则用对方给的（拉取会话）；
 *  - via_web 仅决定"停止网页共享时是否连带中止"。
 * 返回参与文件数；0 = 全被勾掉/全部跳过，调用方自行把 phase 复位。调用者持锁。 */
static int build_session_locked(const char *session_id, int via_web)
{
    int i, m = 0, has_pending = 0, skipped = 0, oom = 0;
    SceOff tot = 0;
    RFile *arr;
    PathSet files, dirs, tops;

    sess_free_locked();
    memset(&g_sess, 0, sizeof g_sess);
    g_phase = PH_ACTIVE;
    g_sess_web = via_web ? 1 : 0;
    g_sess_pull = 0;
    g_pend_hold = 0;
    dir_cache_reset_locked();
    if (session_id && session_id[0])
        snprintf(g_sess.session, sizeof g_sess.session, "%s", session_id);
    else
        gen_hex(g_sess.session, 16);         /* 32 hex */
    snprintf(g_sess.peer_ip, sizeof g_sess.peer_ip, "%s", g_pend.ip);
    snprintf(g_sess.peer_alias, sizeof g_sess.peer_alias, "%s", g_pend.alias);
    if (g_pend.n <= 0 || !g_pend.f) return 0;

    arr = (RFile *)calloc((size_t)g_pend.n, sizeof *arr);
    if (!arr) {
        dlog("recv: session alloc failed (%d files)", g_pend.n);
        return 0;
    }
    if (!pathset_init(&files)) {
        free(arr);
        dlog("recv: pathset alloc failed");
        return 0;
    }
    if (!pathset_init(&dirs) || !pathset_init(&tops)) {
        pathset_free(&files);
        pathset_free(&dirs);
        free(arr);
        dlog("recv: pathset alloc failed");
        return 0;
    }

    for (i = 0; i < g_pend.n; i++) {
        RFile *f;
        char rel[RP_REL_MAX + 1];
        char cand[RP_REL_MAX + 1];
        char base_rel[RP_REL_MAX + 1];
        char ctop[RP_COMP_MAX + 1];
        const char *cinner;
        char final[RP_PATH_MAX + 1];
        char part[RP_PATH_MAX + 1];
        int ridx = -1, k = 0;
        bool ok = false, oomr = false;

        if (!pend_inc_get(i)) continue;
        if (!g_pend.f[i].path_ok) { skipped++; continue; }

        if (g_pend.f[i].rname[0]) {
            if (!rel_replace_last(g_pend.f[i].name, g_pend.f[i].rname,
                                  rel, sizeof rel)) {
                skipped++;
                continue;
            }
        } else {
            snprintf(rel, sizeof rel, "%s", g_pend.f[i].name);
        }
        if (!path_budget_ok(rel)) { skipped++; continue; }

        snprintf(base_rel, sizeof base_rel, "%s", rel);
        snprintf(cand, sizeof cand, "%s", rel);
        for (;;) {
            if (has_file_ancestor(&files, cand)) break;  /* 文件挡目录：跳过 */
            if (pathset_find(&dirs, cand) >= 0) break;   /* 文件挡目录：跳过 */
            if (pathset_find(&files, cand) < 0) { ok = true; break; }
            if (++k > 999) break;
            if (!rel_with_unique(base_rel, k, cand, sizeof cand)) break;
            if (!path_budget_ok(cand)) break;
        }
        if (!ok) { skipped++; continue; }

        split_top_inner(cand, ctop, sizeof ctop, &cinner);
        if (cinner[0]) {                         /* 目录型：先拿到暂存根 */
            ridx = root_get_or_create_locked(&tops, ctop, &oomr);
            if (ridx == -1) { oom = 1; break; }
            if (ridx < 0) { skipped++; continue; }
        }
        if (!compute_paths(cand,
                           ridx >= 0 ? g_sess.roots[ridx]->stage_name : "",
                           final, sizeof final, part, sizeof part)) {
            skipped++;
            continue;
        }

        f = &arr[m];
        memset(f, 0, sizeof *f);
        snprintf(f->fileid, sizeof f->fileid, "%s", g_pend.f[i].fileid);
        gen_hex(f->token, 16);
        snprintf(f->rel, sizeof f->rel, "%s", cand);
        snprintf(f->name, sizeof f->name, "%s", path_base(cand));
        snprintf(f->final, sizeof f->final, "%s", final);
        snprintf(f->part, sizeof f->part, "%s", part);
        f->size = g_pend.f[i].size;
        snprintf(f->sha256, sizeof f->sha256, "%s", g_pend.f[i].sha256);
        /* 0 字节文件：只登记给 finalize 统一创建；先标完成，避免会话永远等
         * 不到 upload。对端若仍发空体 upload，入口按"已完成 + size==0"回 200。 */
        f->st = (f->size == 0) ? 1 : 0;
        if (f->st == 0) {
            has_pending = 1;
        } else if (!sess_zero_add_locked(m, ridx)) {
            oom = 1;
            break;
        }
        if (ridx >= 0) g_sess.roots[ridx]->nfiles++;

        /* 键指向 f->rel（数组在循环内不移动，且 set 在 realloc 收缩前释放） */
        if (!add_ancestors(&dirs, f->rel) || !pathset_add(&files, f->rel)) {
            oom = 1;
            break;
        }
        tot += f->size;
        m++;
    }
    pathset_free(&files);
    pathset_free(&dirs);
    pathset_free(&tops);

    if (oom || m == 0) {
        /* 回滚本次新建的暂存根（都还没写入任何文件）后释放会话 */
        rm_stage_roots_locked();
        sess_free_locked();
        g_sess.skipped = skipped;
        if (oom) dlog("recv: session build aborted (alloc failed)");
        return 0;
    }
    if (m < g_pend.n) {
        RFile *na = (RFile *)realloc(arr, (size_t)m * sizeof *na);
        if (na) arr = na;
    }
    g_sess.f = arr;
    g_sess.n = m;
    g_sess.skipped = skipped;
    g_sess.total = tot;
    g_sess.got_total = 0;
    g_sess.cur = -1;
    g_sess.start_us = now_us();
    g_sess.last_us = g_sess.start_us;
    g_abort = 0;
    g_sess.state = has_pending ? RECV_ST_READY : RECV_ST_RECEIVING;
    if (!has_pending) finalize_start_locked();   /* 全空文件也要走最终化 */
    return m;
}

/* prepare-upload：解析 → 挂起等 UI（≤60s）→ 回 HTTP 码并填 resp（200 时 JSON） */
static int recv_http_prepare(const char *body, const char *ip, const char *peer_fp,
                             const HttpPeer *peer, int via_web,
                             char *resp, int respsz)
{
    const char *infov, *filesv, *p;
    JsonIter it;
    char alias[64];
    long long sz;
    int n = 0, code = 200, r;
    unsigned my_gen = 0;                     /* 本次占位的序号（见 g_gen） */
    bool pend_accept;
    int probe_ticks = 0, peer_gone = 0;

    if (respsz > 0) resp[0] = 0;
    reap_zombies();                          /* 先回收旧僵尸槽位，避免复用其临时路径 */
    fin_reap();                              /* 回收上一次会话的最终化线程 */

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
        reject_note_http(HTTP_REJ_BUSY);
        return 409;
    }

    /* 检查+占位必须持锁原子完成：http 现在多连接并发，若先查后占分两次
     * 加锁，两个并发 prepare 可能同时通过检查互相覆盖 g_pend。因此进锁后
     * 只要不是 PH_NONE（有待决定/活动/终态未清场）就立即 409 礼貌拒绝。 */
    lock();
    if (g_phase != PH_NONE) {
        g_reject_evt = HTTP_REJ_BUSY;
        unlock();
        return 409;
    }
    pend_free_locked();
    g_phase = PH_PENDING;                    /* 先占位：锁内的并发 prepare 409 */
    g_pend.cfg_limit = files_limit_cfg();
    g_pend.eff_limit = g_pend.cfg_limit;
    g_pend.overflow = 0;
    g_pend_web = via_web ? 1 : 0;            /* 网页 PIN 来的请求：停共享时跟着作废 */
    my_gen = ++g_gen;                        /* 本次占位的身份（见 g_gen） */
    g_peer_gone_evt = 0;                     /* 上一份的"对方取消"标志不得串到本份 */
    g_pend.alias[0] = 0;
    g_pend.type[0] = 0;
    snprintf(g_pend.ip, sizeof g_pend.ip, "%s", ip ? ip : "");
    /* 对端证书指纹由 TLS 层在握手时算好传进来（官方口径：注册/准备里的
     * fingerprint 在 HTTPS 模式下被忽略，一律以证书为准）。 */
    snprintf(g_pend.fp, sizeof g_pend.fp, "%s", peer_fp ? peer_fp : "");
    json_get_str(infov, "alias", g_pend.alias, sizeof g_pend.alias);
    json_get_str(infov, "deviceType", g_pend.type, sizeof g_pend.type);
    if (!g_pend.alias[0]) snprintf(g_pend.alias, sizeof g_pend.alias, "%s", ip);
    snprintf(alias, sizeof alias, "%s", g_pend.alias);

    if (json_iter_first(filesv, &it)) {
        do {
            char raw[RP_REL_MAX + 2], rel[RP_REL_MAX + 1], sh[65];
            bool cut = false;
            size_t rl;

            raw[0] = rel[0] = sh[0] = 0;
            p = json_get_val(it.val, "fileName");
            if (!p || !json_val_str(p, raw, sizeof raw)) {
                g_pend.skipped++;
                continue;
            }
            rl = strlen(raw);
            /* 目录条目（尾部 '/'）不是文件；size 缺失/非法也不是可落盘文件。
             * 一律跳过并计数，避免落成同名空文件挡住后续子目录。 */
            if ((rl > 0 && (raw[rl - 1] == '/' || raw[rl - 1] == '\\'))) {
                g_pend.skipped++;
                continue;
            }
            p = json_get_val(it.val, "size");
            if (!p || !json_val_int(p, &sz) || sz < 0) {
                g_pend.skipped++;
                continue;
            }
            if (rp_parse_relpath(raw, rel, sizeof rel, NULL, &cut) != RP_OK) {
                g_pend.skipped++;
                continue;
            }
            p = json_get_val(it.val, "sha256");
            if (p && json_val_str(p, sh, sizeof sh) && strlen(sh) != 64)
                sh[0] = 0;

            if (n >= RECV_FILES_MAX) {
                /* 超过硬上限：整份拒绝（不再"丢弃多余、照收前 2048"——对端以为
                 * 全发了，实际少收一半且无任何提示）。413 让发送方明确看到失败。 */
                dlog("recv: prepare rejected, > %d files from %s",
                     RECV_FILES_MAX, ip ? ip : "?");
                g_reject_evt = HTTP_REJ_TOO_MANY;
                code = 413;
                break;
            }
            if (!pend_reserve_locked(n + 1)) {
                g_reject_evt = HTTP_REJ_NOMEM;
                code = 507;                  /* 内存不足：整份请求收掉 */
                break;
            }
            snprintf(g_pend.f[n].fileid, sizeof g_pend.f[n].fileid, "%s", it.key);
            snprintf(g_pend.f[n].name, sizeof g_pend.f[n].name, "%s", rel);
            g_pend.f[n].trunc = cut;
            g_pend.f[n].size = (SceOff)sz;
            snprintf(g_pend.f[n].sha256, sizeof g_pend.f[n].sha256, "%s", sh);
            g_pend.f[n].rname[0] = 0;
            g_pend.f[n].path_ok = false;
            pend_inc_set(n, true);
            n++;
        } while (json_iter_next(&it));
    }
    if (code != 200) {
        pend_free_locked();
        g_phase = PH_NONE;                   /* 撤销占位 */
        g_pend_hold = 0;
        unlock();
        return code;
    }
    if (n == 0) {                            /* 没有可收的文件 */
        pend_free_locked();
        g_phase = PH_NONE;                   /* 撤销占位 */
        g_pend_hold = 0;
        unlock();
        return 204;
    }
    g_pend.n = n;
    if (n > g_pend.cfg_limit)                /* 超过设置值：按最小档临时提升 */
        g_pend.eff_limit = files_limit_raise(n);
    pend_recheck_paths_locked();
    g_pend_result = 0;
    g_pend_hold = 0;                         /* 新请求从满窗开始（上一份的暂停状态不得串过来） */
    g_pend_dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
    dlog("recv: prepare %d files (%d skipped) from %s (%s), limit %d/%d", n,
         g_pend.skipped + g_pend.path_skipped, alias, g_pend.ip,
         g_pend.cfg_limit, g_pend.eff_limit);
    unlock();

    /* 等 UI 决定：轮询（本连接占一个 http worker；期间其他连接由各自的 worker
     * 并发处理，遇到 PH_PENDING 一律 409，不会踩这份状态）。窗口是"确认页上
     * 没有操作满 60s"——UI 在子页里时 hold=1，这里把截止时刻一直推到现在+满窗，
     * 等价于暂停；放开的瞬间就自动恢复成完整 60s（见 recv_decide_hold）。 */
    for (;;) {
        int expired;
        lock();
        r = g_pend_result;
        if (g_gen != my_gen) r = 3;          /* 占位已被 recv_clear 撤销/被新 prepare 顶替 */
        if (!r && g_pend_hold)
            g_pend_dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
        expired = !g_pend_hold && (SceLong64)now_us() >= g_pend_dl;
        unlock();
        if (r) break;
        if (expired) { r = 3; break; }
        /* 发送方放弃：官方在"等接受"阶段是 abort 掉 prepare 请求（关连接），
         * 不会调 /cancel——那时 sessionId 还没生成。每 200ms 探一次连接，
         * 断了就立刻收场，别让用户对着一个已经不存在的请求等满 60s。 */
        if (peer && ++probe_ticks >= 4) {
            probe_ticks = 0;
            if (http_peer_gone(peer) == 1) { peer_gone = 1; break; }
        }
        sceKernelDelayThread(50 * 1000);
    }

    if (peer_gone) {
        lock();
        if (g_gen == my_gen) {               /* 仍是本次占位：撤掉，把原因留给 UI */
            g_phase = PH_NONE;
            g_pend_hold = 0;
            g_peer_gone_evt = 1;
            pend_free_locked();              /* alias 已拷进局部量，不会再读 g_pend */
        }
        unlock();
        dlog("recv: prepare abandoned by sender (connection closed) from %s",
             ip ? ip : "?");
        return 403;                          /* 对端已经走了，这个码没人收 */
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
        int k, m = build_session_locked(NULL, g_pend_web);
        if (m == 0) {                        /* 全被勾掉：告诉对方不用传 */
            g_phase = PH_NONE;
            code = 204;
        } else {
            char eid[512];
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
            /* M1 仍用固定回执缓冲（M3 才动态化）：文件多到装不下时必须明确
             * 失败，绝不能把截断的 JSON 当 200 发出去（对端会解析失败/少收）。 */
            if (o >= respsz - 2) {
                dlog("recv: prepare response buffer too small (%d files)", m);
                code = 500;
                /* 全空文件会话可能已在最终化：绝不能 free 正在被线程使用的
                 * g_sess/暂存根，让它照常收尾（对端看到 500 会重试）。 */
                if (g_sess.state != RECV_ST_FINALIZING) {
                    rm_stage_roots_locked();
                    sess_free_locked();
                    g_phase = PH_NONE;
                }
            }
        }
        if (code == 200)
            dlog("recv: accept session=%s files=%d from %s",
                 g_sess.session, m, g_sess.peer_alias);
    } else {
        g_phase = PH_NONE;
        g_pend_hold = 0;
        code = 403;                          /* 拒绝 / 超时未决 */
        dlog("recv: prepare %s from %s", r == 2 ? "rejected" : "timeout", alias);
    }
    g_pend_result = 0;
    pend_free_locked();
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
    if (!sess_is_terminal(g_sess.state) && g_sess.state != RECV_ST_FINALIZING)
        sess_terminal(RECV_ST_FAIL, err);
}

/* 会话终态收敛（持有锁时调用）：单文件失败只置 st=3，等所有文件都收尾
 * （st != 0）才在这里汇总。失败当下不能直接判 PARTIAL——那会让还没开始
 * 上传的其余文件命中 upload 入口的终态检查、直接吃 403。
 *   g_abort 只收掉"还没结束"的会话（与旧行为一致：已收满的文件优先算完成）。 */
static void check_all_done_locked(void)
{
    int i, all = 1;
    if (sess_is_terminal(g_sess.state) || g_sess.state == RECV_ST_FINALIZING)
        return;
    for (i = 0; i < g_sess.n; i++) {
        if (g_sess.f[i].st == 0) { all = 0; break; }
    }
    if (all) {
        /* 全部到终态（含失败）：进入最终化，把暂存根整体搬到最终位置。
         * 成功/失败/部分成功由 finalize 线程收尾时统一判定（见 fin_thread）。 */
        finalize_start_locked();
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

/* 本会话里是否已有别的文件占用了同一相对路径（大小写不敏感）。 */
static bool session_has_rel_locked(const RFile *self, const char *rel)
{
    int i;
    for (i = 0; i < g_sess.n; i++) {
        if (&g_sess.f[i] == self) continue;
        if (rp_name_eq(g_sess.f[i].rel, rel)) return true;
    }
    return false;
}

/* 真正写盘前解析磁盘同名冲突（持锁调用）：磁盘上已有、临时路径正被遗弃槽位
 * 占用、或与会话内其他文件撞名 → 最后一个组件加 " (k)"。找不到可用名返回
 * false（该文件按失败处理）。
 * 目录内的文件写在本次新建的暂存根里，不会与磁盘既有文件撞名，合并时的重名
 * 由 finalize 统一加 " (k)"；因此这里只处理顶层平铺文件。 */
static bool resolve_disk_conflict_locked(RFile *f)
{
    char base_rel[RP_REL_MAX + 1];
    char cand[RP_REL_MAX + 1];
    char final[RP_PATH_MAX + 1];
    char part[RP_PATH_MAX + 1];
    SceIoStat st;
    int k = 0;

    if (strchr(f->rel, '/')) return true;
    snprintf(base_rel, sizeof base_rel, "%s", f->rel);
    for (;;) {
        if (k == 0)
            snprintf(cand, sizeof cand, "%s", base_rel);
        else if (!rel_with_unique(base_rel, k, cand, sizeof cand))
            return false;
        if (!path_budget_ok(cand)) return false;
        if (!compute_paths(cand, "", final, sizeof final, part, sizeof part))
            return false;
        if (sceIoGetstat(final, &st) < 0 &&
            !part_in_zombie(part) &&
            !session_has_rel_locked(f, cand))
            break;
        if (++k > 999) return false;
    }
    if (strcmp(cand, f->rel) != 0) {
        snprintf(f->rel, sizeof f->rel, "%s", cand);
        snprintf(f->name, sizeof f->name, "%s", path_base(cand));
        snprintf(f->final, sizeof f->final, "%s", final);
        snprintf(f->part, sizeof f->part, "%s", part);
    }
    return true;
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
    /* 接受时没做磁盘 stat（2048 文件不能锁内逐个探测）：真正写盘前在这里解析
     * 同名冲突，必要时把最后一级改成 " (k)"。 */
    if (!resolve_disk_conflict_locked(f)) {
        file_fail_locked(f, "无法分配文件名");
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

    /* 惰性建父目录（逐级 mkdir + 缓存）；嵌套目录在这里才真正出现。 */
    if (!mk_parents(f->final)) {
        dlog("recv: mkdir parents fail (%s)", f->final);
        lock();
        file_fail_locked(f, "无法创建目录");
        clear_pending_finish_locked();
        unlock();
        return 500;
    }
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
        g_sess.state != RECV_ST_FINALIZING &&   /* 最终化期间忽略：数据已收完 */
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

/* ---------------- 扫码链接：拉取会话（见 receive.h） ----------------
 * 与推送会话共用 g_phase/g_pend/g_sess：扫到分享二维码 → 后台线程 POST
 * prepare-download 拿清单 → 照常走确认页 → 接受后同一线程逐个 GET 文件。
 * 一次只允许一个"连接中"的链接；已出现待决定/活动会话时拒绝再开（与 prepare 的
 * 单活动会话口径一致）。 */
enum { LINK_ERR_NONE = 0, LINK_ERR_CONN = 1, LINK_ERR_REFUSED = 2,
       LINK_ERR_RESP = 3, LINK_ERR_CANCELLED = 4, LINK_ERR_BUSY = 5 };

static volatile int g_link_state;    /* 0=空闲 1=连接/取清单（还没出确认页） */
static volatile int g_link_err;      /* 一次性失败码（UI 取走即清零） */
static unsigned     g_link_gen;      /* 取消/重开的序号：迟到的线程不得回写 */
static SceUID       g_link_thr = -1;
/* 取消后旧线程可能还在 connect/POST（≤6s），新一次扫码会顶掉句柄：把旧句柄
 * 记在这里，退出后统一回收（同 scan.c 的 worker 收尸思路）。 */
#define LINK_ORPHANS 4
static SceUID       g_link_orphan[LINK_ORPHANS];
static char         g_link_host[64];
static int          g_link_port;
static char         g_link_pin[8];

static int link_thr(SceSize args, void *argp);
static void link_download(const char *host, int port, const char *sid);

/* 回收已退出的链接线程对象（未退出就留着，不碰运行中的线程） */
static void link_reap(void)
{
    int i;
    if (g_link_thr >= 0 && thread_exited(g_link_thr)) {
        sceKernelDeleteThread(g_link_thr);
        g_link_thr = -1;
    }
    for (i = 0; i < LINK_ORPHANS; i++) {
        if (g_link_orphan[i] <= 0) continue;
        if (!thread_exited(g_link_orphan[i])) continue;
        sceKernelDeleteThread(g_link_orphan[i]);
        g_link_orphan[i] = -1;
    }
}

static void link_track_orphan(SceUID t)
{
    int i;
    if (t <= 0) return;
    for (i = 0; i < LINK_ORPHANS; i++)
        if (g_link_orphan[i] <= 0) { g_link_orphan[i] = t; return; }
    dlog("recv: link orphan table full, thread %d dropped", (int)t);
}

int recv_link_open(const char *host, int port, const char *pin)
{
    SceUID t;

    if (!host || !host[0] || strlen(host) >= sizeof g_link_host ||
        port <= 0 || port > 65535 || !pin || strlen(pin) != 4)
        return -2;
    link_reap();
    lock();
    if (g_link_state != 0 || g_phase != PH_NONE) {
        unlock();
        return -1;
    }
    snprintf(g_link_host, sizeof g_link_host, "%s", host);
    g_link_port = port;
    snprintf(g_link_pin, sizeof g_link_pin, "%s", pin);
    g_link_err = LINK_ERR_NONE;
    g_link_state = 1;
    g_link_gen++;
    unlock();

    t = sceKernelCreateThread("psvsend_link", link_thr, 0x40, 0x40000, 0, 0, NULL);
    if (t < 0) {
        lock();
        g_link_state = 0;
        g_link_err = LINK_ERR_CONN;
        unlock();
        dlog("recv: link thread create fail 0x%08X", (unsigned)t);
        return -1;
    }
    if (sceKernelStartThread(t, 0, NULL) < 0) {
        sceKernelDeleteThread(t);
        lock();
        g_link_state = 0;
        g_link_err = LINK_ERR_CONN;
        unlock();
        return -1;
    }
    if (g_link_thr >= 0)
        link_track_orphan(g_link_thr);   /* 旧线程还在（取消后未退），交给收尸表 */
    g_link_thr = t;
    dlog("recv: link open %s:%d pin=%s", host, port, pin);
    return 0;
}

int recv_link_connecting(void)
{
    return g_link_state != 0;
}

int recv_link_error_take(void)
{
    int e;
    lock();
    e = g_link_err;
    g_link_err = LINK_ERR_NONE;
    unlock();
    return e;
}

void recv_link_cancel(void)
{
    lock();
    if (g_link_state == 1 && g_phase == PH_NONE) {
        g_link_gen++;
        g_link_state = 0;
        g_link_err = LINK_ERR_CANCELLED;
        dlog("recv: link open cancelled");
    }
    unlock();
}

/* 拉取会话的写盘回调：每块都推进进度并检查用户中止 */
typedef struct {
    SceUID fd;
    RFile *f;
    int    wr_fail;
} LinkSink;

static int link_sink(void *ctx, const unsigned char *buf, int len)
{
    LinkSink *s = (LinkSink *)ctx;
    int ab;

    if (sceIoWrite(s->fd, buf, (unsigned)len) != len) {
        s->wr_fail = 1;
        return -1;
    }
    lock();
    s->f->got += len;
    g_sess.got_total += len;
    ab = g_abort;
    unlock();
    return ab ? -1 : 0;
}

/* 逐个文件 GET + 写盘 + 收尾（会话终态/清理口径与推送侧一致） */
static void link_download(const char *host, int port, const char *sid)
{
    int i;

    for (i = 0; i < g_sess.n; i++) {
        RFile *f = &g_sess.f[i];
        LinkSink sink;
        SceUID fd;
        char path[320];
        int64_t total = -1;
        int status = 0, rc, wr_fail = 0;
        bool aborted = false, name_fail = false;

        lock();
        if (g_abort) aborted = true;
        else if (f->st == 0) {
            if (!resolve_disk_conflict_locked(f)) {
                name_fail = true;
            } else {
                f->busy = true;
                g_sess.cur = i;
                g_sess.state = RECV_ST_RECEIVING;
                g_sess.last_us = now_us();
                f->got = 0;
            }
        }
        unlock();
        if (aborted) break;
        if (name_fail) {
            lock();
            file_fail_locked(f, "无法分配文件名");
            unlock();
            continue;
        }
        if (f->st != 0) continue;

        if (!mk_parents(f->final)) {
            dlog("recv: link mkdir parents fail (%s)", f->final);
            lock();
            file_fail_locked(f, "无法创建目录");
            unlock();
            continue;
        }
        fd = sceIoOpen(f->part, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd < 0) {
            dlog("recv: link open part fail 0x%08X (%s)", (unsigned)fd, f->part);
            lock();
            file_fail_locked(f, "无法创建文件");
            unlock();
            continue;
        }
        snprintf(path, sizeof path,
                 "/api/localsend/v2/download?sessionId=%s&fileId=%s",
                 sid, f->fileid);
        sink.fd = fd;
        sink.f = f;
        sink.wr_fail = 0;
        rc = httpc_get_stream(host, port, path, link_sink, &sink, &total,
                              &status, 15000);
        wr_fail = sink.wr_fail;
        sceIoClose(fd);
        if (rc == 0 && total >= 0 && total != (int64_t)f->size)
            rc = -4;                         /* 清单与实际长度不符 */

        if (rc == 0) {
            if (sceIoRename(f->part, f->final) < 0) {
                lock();
                file_fail_locked(f, "文件落盘失败");
                unlock();
            } else {
                lock();
                f->st = 1;
                f->busy = false;
                update_cur_locked();
                check_all_done_locked();
                unlock();
            }
            continue;
        }

        lock();
        f->busy = false;
        update_cur_locked();
        if (g_abort) {
            unlock();
            break;                           /* 用户取消：循环外统一收尾 */
        }
        if (rc == -2 && wr_fail) {
            file_fail_locked(f, "写入失败（磁盘空间不足？）");
            unlock();
            continue;
        }
        if (rc == -4) {
            file_fail_locked(f, "文件大小与清单不符");
            unlock();
            continue;
        }
        /* 连接/断流/HTTP 非 200：对方已不可用，剩余文件不再尝试 */
        {
            char err[192];
            if (rc == -3)
                snprintf(err, sizeof err, "共享已停止（HTTP %d）", status);
            else
                snprintf(err, sizeof err, "下载中断");
            file_fail_locked(f, err);
            {
                int k;
                for (k = i + 1; k < g_sess.n; k++)
                    if (g_sess.f[k].st == 0) g_sess.f[k].st = 3;
            }
            check_all_done_locked();
        }
        unlock();
        break;
    }

    lock();
    g_sess_pull = 0;                     /* 下载线程收尾：不再持有 g_sess */
    if (g_abort) {
        if (g_sess.state != RECV_ST_FINALIZING) {
            cleanup_parts();
            if (!sess_is_terminal(g_sess.state))
                sess_terminal(RECV_ST_CANCEL, "用户取消");
        }
    } else if (g_sess.state != RECV_ST_FINALIZING &&
               !sess_is_terminal(g_sess.state)) {
        int k;
        for (k = 0; k < g_sess.n; k++)
            if (g_sess.f[k].st == 0) g_sess.f[k].st = 3;   /* 未开始的按失败收尾 */
        check_all_done_locked();          /* 全部到终态 → 进入最终化 */
    }
    g_sess.cur = -1;
    dlog("recv: link session %s end st=%d", g_sess.session, g_sess.state);
    clear_pending_finish_locked();
    unlock();
}

/* prepare-download 清单里的一条文件（解析结果） */
typedef struct {
    char   id[160];
    char   name[RP_REL_MAX + 1];
    SceOff size;
    bool   trunc;
} LinkItem;

/* 解析一条文件条目（对象）。名字与 prepare-upload 同一套口径：相对路径解析 +
 * 逐组件净化；目录条目/缺 size 的条目跳过。 */
static bool link_parse_item(const char *val, LinkItem *it)
{
    char raw[RP_REL_MAX + 2], rel[RP_REL_MAX + 1];
    const char *p;
    long long sz;
    size_t rl;
    bool cut = false;

    p = json_get_val(val, "id");
    if (!p || !json_val_str(p, it->id, sizeof it->id) || !it->id[0])
        return false;
    raw[0] = rel[0] = 0;
    p = json_get_val(val, "fileName");
    if (!p || !json_val_str(p, raw, sizeof raw)) return false;
    rl = strlen(raw);
    if (rl > 0 && (raw[rl - 1] == '/' || raw[rl - 1] == '\\')) return false;
    p = json_get_val(val, "size");
    if (!p || !json_val_int(p, &sz) || sz < 0) return false;
    if (rp_parse_relpath(raw, rel, sizeof rel, NULL, &cut) != RP_OK) return false;
    snprintf(it->name, sizeof it->name, "%s", rel);
    it->trunc = cut;
    it->size = (SceOff)sz;
    return true;
}

static int link_thr(SceSize args, void *argp)
{
    /* 清单最多 RECV_MAX_FILES 条：放静态区，别吃线程栈（256KB 还要装 HTTP 缓冲） */
    static LinkItem tmp[RECV_MAX_FILES];
    char host[64], pin[8], path[192], sid[64] = "";
    char resp[24576];                    /* 64 条长文件名清单也放得下 */
    char alias[64] = "";
    int port, status = 0, r, n = 0, i;
    unsigned my_gen, my_pend = 0;
    const char *filesv, *infov, *q;
    JsonIter it;

    (void)args; (void)argp;
    lock();
    my_gen = g_link_gen;
    snprintf(host, sizeof host, "%s", g_link_host);
    port = g_link_port;
    snprintf(pin, sizeof pin, "%s", g_link_pin);
    unlock();

    snprintf(path, sizeof path,
             "/api/localsend/v2/prepare-download?pin=%s", pin);
    r = httpc_post(host, port, path, NULL, resp, sizeof resp, &status, 6000);

    lock();
    if (g_link_gen != my_gen) { unlock(); return 0; }   /* 已取消/重开 */
    if (r != 0) {
        g_link_state = 0;
        g_link_err = LINK_ERR_CONN;
        unlock();
        dlog("recv: link %s:%d prepare connect fail", host, port);
        return 0;
    }
    if (status != 200) {
        g_link_state = 0;
        g_link_err = (status == 401 || status == 403) ? LINK_ERR_REFUSED
                                                      : LINK_ERR_RESP;
        unlock();
        dlog("recv: link %s:%d prepare -> %d", host, port, status);
        return 0;
    }
    unlock();

    /* 解析清单（与 prepare-upload 同一套规整：净化 + 超长保后缀截断） */
    infov = json_get_val(resp, "info");
    if (infov) json_get_str(infov, "alias", alias, sizeof alias);
    filesv = json_get_val(resp, "files");
    if (!json_get_str(resp, "sessionId", sid, sizeof sid) || !sid[0] || !filesv) {
        lock();
        if (g_link_gen == my_gen) {
            g_link_state = 0;
            g_link_err = LINK_ERR_RESP;
        }
        unlock();
        dlog("recv: link response unparseable: %.160s", resp);
        return 0;
    }
    /* files 有两种形态：我们自己的网页共享是数组，官方端是 "id → 文件" 的 map。
     * 两种都要能读，否则扫官方分享会"一条文件都解析不出来"。 */
    for (q = filesv; *q == ' ' || *q == '\t' || *q == '\n' || *q == '\r'; q++)
        ;
    if (*q == '[') {
        if (json_arr_first(filesv, &it)) {
            do {
                if (link_parse_item(it.val, &tmp[n])) n++;
            } while (n < RECV_MAX_FILES && json_arr_next(&it));
        }
    } else if (*q == '{') {
        if (json_iter_first(filesv, &it)) {
            do {
                if (link_parse_item(it.val, &tmp[n])) n++;
            } while (n < RECV_MAX_FILES && json_iter_next(&it));
        }
    }

    lock();
    if (g_link_gen != my_gen) { unlock(); return 0; }
    if (g_phase != PH_NONE) {
        g_link_state = 0;
        g_link_err = LINK_ERR_BUSY;
        unlock();
        dlog("recv: link refused, another session active");
        return 0;
    }
    if (n == 0) {
        g_link_state = 0;
        g_link_err = LINK_ERR_RESP;
        unlock();
        dlog("recv: link %s:%d has no files", host, port);
        return 0;
    }
    /* 占位：从这里开始与 prepare 完全同构（确认页/倒计时/ABA 保护都复用）。
     * 扫码拉取本期仍限 RECV_MAX_FILES 条（M3 再和 HTTP 回执一起动态化）。 */
    pend_free_locked();
    if (!pend_reserve_locked(n)) {
        g_link_state = 0;
        g_link_err = LINK_ERR_RESP;
        unlock();
        dlog("recv: link pending alloc failed");
        return 0;
    }
    g_phase = PH_PENDING;
    g_pend_web = 0;                      /* 拉取会话不受"停止本机网页共享"影响 */
    my_pend = ++g_gen;
    g_peer_gone_evt = 0;
    snprintf(g_pend.alias, sizeof g_pend.alias, "%s",
             alias[0] ? alias : host);
    snprintf(g_pend.type, sizeof g_pend.type, "%s", "web");
    snprintf(g_pend.ip, sizeof g_pend.ip, "%s", host);
    g_pend.fp[0] = 0;
    g_pend.n = n;
    g_pend.overflow = 0;
    g_pend.cfg_limit = files_limit_cfg();
    g_pend.eff_limit = n > g_pend.cfg_limit ? files_limit_raise(n) : g_pend.cfg_limit;
    for (i = 0; i < n; i++) {
        snprintf(g_pend.f[i].fileid, sizeof g_pend.f[i].fileid, "%s", tmp[i].id);
        snprintf(g_pend.f[i].name, sizeof g_pend.f[i].name, "%s", tmp[i].name);
        g_pend.f[i].size = tmp[i].size;
        g_pend.f[i].sha256[0] = 0;
        g_pend.f[i].rname[0] = 0;
        g_pend.f[i].trunc = tmp[i].trunc;
        pend_inc_set(i, true);
    }
    pend_recheck_paths_locked();
    g_pend_result = 0;
    g_pend_hold = 0;
    g_pend_dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
    g_link_state = 0;                    /* UI 改看 pending（确认页） */
    unlock();
    dlog("recv: link %s:%d session=%s files=%d", host, port, sid, n);

    /* 等 UI 决定（与 http prepare 的轮询/暂停/超时语义一致） */
    for (;;) {
        int result, expired;
        lock();
        result = g_pend_result;
        if (g_gen != my_pend) result = 3;     /* 已被 recv_clear 撤销 */
        if (!result && g_pend_hold)
            g_pend_dl = (SceLong64)now_us() + RECV_DECIDE_TIMEOUT_US;
        expired = !g_pend_hold && (SceLong64)now_us() >= g_pend_dl;
        unlock();
        if (result) { r = result; break; }
        if (expired) { r = 3; break; }
        sceKernelDelayThread(50 * 1000);
    }
    if (r != 1) {
        lock();
        if (g_gen == my_pend) {
            g_phase = PH_NONE;
            g_pend_hold = 0;
            pend_free_locked();
        }
        g_pend_result = 0;
        unlock();
        dlog("recv: link session %s %s", sid, r == 2 ? "rejected" : "timeout");
        return 0;
    }

    lock();
    if (g_gen != my_pend) {              /* 决定后立刻被撤销 */
        unlock();
        return 0;
    }
    n = build_session_locked(sid, 0);
    pend_free_locked();
    if (n == 0) {                        /* 全被勾掉：不下载 */
        g_phase = PH_NONE;
        g_pend_result = 0;
        unlock();
        return 0;
    }
    g_sess_pull = 1;                     /* 文件间空档也不许 recv_clear 清场 */
    if (g_sess.state == RECV_ST_READY)
        g_sess.state = RECV_ST_RECEIVING; /* 拉取马上开始，不显示"等待发送方" */
    g_pend_result = 0;
    unlock();
    dlog("recv: link accept session=%s files=%d", g_sess.session, n);

    link_download(host, port, sid);
    return 0;
}

/* 状态轮询热路径上的空闲超时判定（持锁调用；不做文件系统 IO）。 */
static void status_idle_check_locked(void)
{
    uint64_t now = now_us();
    if (g_sess.state == RECV_ST_FINALIZING) return;   /* 最终化不算空闲 */
    if (!any_busy_locked() && !sess_is_terminal(g_sess.state) &&
        now - g_sess.last_us > (uint64_t)RECV_IDLE_TIMEOUT_US) {
        sess_terminal(RECV_ST_TIMEOUT, "等待对方传输超时");
    }
}

int recv_status_summary(RecvStatusSummary *out)
{
    int i, r = 0;
    lock();
    if (g_phase == PH_ACTIVE) {
        if (out) {
            int done = 0, failed = 0;
            status_idle_check_locked();
            for (i = 0; i < g_sess.n; i++) {
                if (!g_sess.f) break;
                if (g_sess.f[i].st == 1) done++;
                else if (g_sess.f[i].st == 3) failed++;
            }
            out->state = g_sess.state;
            snprintf(out->peer_alias, sizeof out->peer_alias, "%s",
                     g_sess.peer_alias);
            out->count = g_sess.n;
            out->cur = g_sess.cur;
            out->done = done;
            out->failed = failed;
            out->skipped = g_sess.skipped;
            out->total = g_sess.total;
            out->got_total = g_sess.got_total;
            out->start_us = g_sess.start_us;
            snprintf(out->err, sizeof out->err, "%s", g_sess.err);
        }
        r = 1;
    }
    unlock();
    return r;
}

int recv_status_window(int base, int max, RecvStatusRow *out)
{
    int i, n = 0;
    if (!out || max <= 0) return 0;
    if (base < 0) base = 0;
    lock();
    if (g_phase == PH_ACTIVE && g_sess.f) {
        for (i = base; i < g_sess.n && n < max; i++, n++) {
            RFile *f = &g_sess.f[i];
            snprintf(out[n].name, sizeof out[n].name, "%s", f->rel);
            out[n].size = f->size;
            out[n].got = f->got < f->size ? f->got : f->size;
            out[n].st = (unsigned char)f->st;
        }
    }
    unlock();
    return n;
}

/* ---- 旧全量快照（兼容包装；M4 下线） ---- */
int recv_status_pull(RecvStatus *out)
{
    int i, r = 0;
    if (!out) return 0;
    lock();
    if (g_phase == PH_ACTIVE) {
        status_idle_check_locked();
        out->state = g_sess.state;
        snprintf(out->peer_alias, sizeof out->peer_alias, "%s", g_sess.peer_alias);
        out->count = g_sess.n;
        out->cur = g_sess.cur;
        out->total = g_sess.total;
        out->got_total = g_sess.got_total;
        out->start_us = g_sess.start_us;
        snprintf(out->err, sizeof out->err, "%s", g_sess.err);
        for (i = 0; i < g_sess.n && i < RECV_MAX_FILES; i++) {
            if (!g_sess.f) break;
            str_copy_utf8(out->name[i], (int)sizeof out->name[0],
                          g_sess.f[i].rel);
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
    fin_reap();
    lock();
    if (g_phase == PH_ACTIVE) {
        if (g_sess.state == RECV_ST_FINALIZING) {
            /* 最终化在跑：线程还在用 g_sess/暂存根，不能拆。推迟到它收尾时
             * 补做（clear_pending_finish_locked），避免会话永远卡在 PH_ACTIVE。 */
            g_clear_pending = 1;
            unlock();
            return;
        }
        if (any_busy_locked()) {             /* 收体进行中：别硬清，先请中止。
                                              * 清场与复位 phase 推迟到最后一个 worker
                                              * 收尾时（clear_pending_finish_locked） */
            g_abort = 1;
            g_clear_pending = 1;
            unlock();
            return;
        }
        cleanup_parts();
        /* 取消/超时/失败的会话：暂存根整体删掉。finalize 失败时保留（下次启动
         * 清扫）——那里面可能有已合并失败但未搬走的文件。 */
        if (!g_sess.fin_attempted) rm_stage_roots_locked();
        sess_free_locked();
    } else if (g_phase == PH_PENDING) {
        g_pend_result = 2;                   /* http 等待循环醒来按拒绝处理 */
        g_gen++;                             /* 同时撤销占位：等待中的 prepare 万一错过
                                              * 这次结果（50ms 轮询窗口）也不会回写状态 */
        pend_free_locked();
    }
    g_phase = PH_NONE;
    g_pend_hold = 0;
    g_abort = 0;
    g_clear_pending = 0;
    dir_cache_reset_locked();
    unlock();
}

/* ---------- 注册给 http 层的接收侧处理集（依赖倒置，类型见 net/http.h） ---------- */
static const HttpRecvOps s_http_ops = {
    recv_http_prepare,      /* prepare：清单 → 挂起等 UI 决定 */
    recv_http_upload,       /* upload ：经 http 层流回调边收边写盘 */
    recv_http_cancel,       /* cancel ：发送方放弃会话 */
    recv_abort_pending,     /* abort_pending：用户中止查询 */
    reject_note_http,       /* reject_note：http 层自拒的 prepare 原因 */
};

const HttpRecvOps *recv_http_ops(void)
{
    return &s_http_ops;
}
