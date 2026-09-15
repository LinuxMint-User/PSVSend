/* receive.h —— 接收方向（LocalSend v2 上传 API 的服务端会话，实现见 receive.c）。
 * 对方(发送方)按我们 announce 里的 http 端口 POST 过来：
 *   prepare-upload  文件清单 JSON → 挂起等 UI 接受/拒绝（回 200 {sessionId,files{id:token}}
 *                   或 403；已有活动会话时 409）
 *   upload?sessionId&fileId&token  裸文件字节流 → 边收边写盘保存目录（临时文件
 *                   <名>.psvsend.tmp，收完改名；sha256 校验失败 422）。保存目录：config saveDir
 *                   持久默认，UI 可在接受前 recv_set_dir 覆盖为本次目录（内存态）。
 *   cancel?sessionId  发送方放弃会话
 * UI 轮询拉取"待确认请求 / 传输状态"，把勾选集合与接受决定写回；http 线程轮询唤醒。
 * 单活动会话：同一时刻只服务一个接收会话（另一台设备的第二个 prepare-upload 回
 * 409）；但同一会话内的多个文件由发送方并发 upload，接收侧按文件独立流式收体。 */
#ifndef PSVSEND_RECEIVE_H
#define PSVSEND_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/types.h>
#include "net/http.h"      /* HttpRecvOps / http_stream_fn（注册给 http 层的处理集） */

#define RECV_MAX_FILES 64       /* 单次接收会话文件数上限（超出的在确认页明示丢弃） */

/* UI：待确认请求快照（recv_pending_pull 锁内拷贝） */
typedef struct {
    char peer_alias[64];
    char peer_type[24];
    char peer_ip[16];
    int  count;                  /* 列入清单的文件数（≤ RECV_MAX_FILES） */
    int  overflow;               /* 发送方实际文件数超过上限、被丢弃的个数 */
    /* name 已经是"规整后的落盘名"（净化 + 超长保后缀截断），不是对方原样给的
     * 原始字符串——确认页显示与实际落盘一致。trunc=名字过长被缩短过。 */
    struct { char name[192]; SceOff size; bool trunc; } files[RECV_MAX_FILES];
    SceOff total;
} RecvPending;

/* UI：接收会话状态（recv_status_pull 锁内拷贝，进度页/结束态展示） */
typedef enum {
    RECV_ST_NONE = 0,    /* 无活动会话（UI 可回主页） */
    RECV_ST_READY,       /* 已接受，等对方开始传 */
    RECV_ST_RECEIVING,   /* 上传中 */
    RECV_ST_DONE,        /* 全部文件收完 */
    RECV_ST_FAIL,        /* 传输失败（中断/校验/写盘） */
    RECV_ST_CANCEL,      /* 发送方放弃 */
    RECV_ST_TIMEOUT      /* 接受后对方一直没传 */
} RecvStateId;

typedef struct {
    int  state;                    /* RecvStateId */
    char peer_alias[64];
    int  count;                    /* 实际参与的文件数（已剔除跳过） */
    int  cur;                      /* 当前正在收的文件下标（-1=无） */
    char name[RECV_MAX_FILES][192];
    SceOff size[RECV_MAX_FILES];
    SceOff got[RECV_MAX_FILES];    /* 每文件已收字节 */
    SceOff total;                  /* 本次要收的总字节 */
    SceOff got_total;
    uint64_t start_us;             /* 会话开始（accept 决定时刻） */
    char err[192];                 /* FAIL/CANCEL/TIMEOUT 原因（其它状态空） */
} RecvStatus;

/* 后端初始化（建锁 + 清扫上次中断残留的 *.psvsend.tmp）；api_start 调用 */
void recv_init(void);

/* UI 轮询：有"等待决定"的接收请求？返回 1 并拷贝 out（out==NULL 时仅探测
 * 不拷贝，用 1/0 回答"有没有"）；0=无（含已消失/已超时） */
int recv_pending_pull(RecvPending *out);

/* UI 在决定前同步"本次勾选接收的文件"（下标与 pending 的 files 一致；
 * 未勾选的会在接受后被后端标记跳过、不出现在 prepare 响应里）。 */
void recv_set_include(const bool inc[RECV_MAX_FILES]);

/* UI 在接受前逐文件指定"保存名"（idx 与 pending.files 下标一致）：非空 =
 * 以此名落盘（后端会再 sanitize + 冲突排重），空串 = 保持对方原名。
 * 仅 PENDING 生效；不改名的文件不必调用。 */
void recv_set_name(int idx, const char *name);

/* 名字 → 可落盘的单级文件名：按 ux0: 合法性净化 + 超长时"保后缀、截主名"，
 * 见 receive.c。落盘与 UI 改名共用，保证"界面上显示的即落盘名"；
 * UI 侧只经 api_recv_sanitize_name 调用。 */
void recv_sanitize_name(const char *in, char *out, int n);

/* UI 在接受前设定"本次保存目录"（内存态，仅本会话；NULL/空 → 回退
 * config saveDir 默认）。不持久化，下次会话由 UI 重新给出默认值。 */
void recv_set_dir(const char *dir);

/* UI 决定（仅 PENDING 有效）：accept=1 接受（生成 sessionId/token，回 200 全收）
 * / 0 拒绝（回 403）。 */
void recv_decide(bool accept);

/* 用户中止进行中的接收：正在流式收体时置标志让 http 线程尽快收尾
 * （删临时文件、回 CANCEL 态），空闲（READY/文件间）时立即生效。 */
void recv_abort(void);

/* UI 轮询会话状态；返回 1=有会话（含终态）0=空闲 */
int recv_status_pull(RecvStatus *out);

/* 接收会话结束展示完毕，清场回空闲 */
void recv_clear(void);

/* ---- HTTP 路由实现集（注册给 net/http.c，类型见 http.h 的 HttpRecvOps） ---- */

/* 返回接收侧处理集：组合层（app/api.c）在 api_start 时注册给 http 层。
 * 各成员语义见 http.h；实现要点：
 *  - prepare：解析清单 → 挂起等 UI 决定（最多 60s）→ 填 resp 返 HTTP 码
 *             （200=resp 为 JSON；403 拒绝/超时；400 坏体；409 已有活动会话）
 *  - upload：校验 query(sessionId/fileId/token)+来源 IP → 经流回调边收边写盘
 *             （200 / 400 缺参 / 403 会话或 token 不符 / 409 已取消 /
 *              422 sha256 不符 / 500 写盘等内部错）
 *  - cancel：发送方放弃 → 200（清理进行中的临时文件；已完成文件保留）
 *  - abort_pending：http 层收体轮询时查"用户是否请求中止"（无锁读 volatile） */
const HttpRecvOps *recv_http_ops(void);

#endif
