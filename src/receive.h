/* receive.h —— 接收方向（LocalSend v2 上传 API 的服务端会话，实现见 receive.c）。
 * 对方(发送方)按我们 announce 里的 http 端口 POST 过来：
 *   prepare-upload  文件清单 JSON → 挂起等 UI 接受/拒绝（回 200 {sessionId,files{id:token}}
 *                   或 403；已有活动会话时 409）
 *   upload?sessionId&fileId&token  裸文件字节流 → 边收边写盘 downloads/（临时 .part，
 *                   收完改名；sha256 校验失败 422）
 *   cancel?sessionId  发送方放弃会话
 * UI 轮询拉取"待确认请求 / 传输状态"，把勾选集合与接受决定写回；http 线程轮询唤醒。
 * 单活动会话：同一时刻只有一个接收会话，第二个 prepare-upload 回 409。 */
#ifndef PSVSEND_RECEIVE_H
#define PSVSEND_RECEIVE_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/types.h>

#define RECV_MAX_FILES 32       /* 单次接收会话文件数上限 */

/* UI：待确认请求快照（recv_pending_pull 锁内拷贝） */
typedef struct {
    char peer_alias[64];
    char peer_type[24];
    char peer_ip[16];
    int  count;
    struct { char name[192]; SceOff size; } files[RECV_MAX_FILES];
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

/* 后端初始化（建锁 + 清扫上次中断残留的 *.part）；api_start 调用 */
void recv_init(void);

/* UI 轮询：有"等待决定"的接收请求？返回 1 并拷贝 out（out==NULL 时仅探测
 * 不拷贝，用 1/0 回答"有没有"）；0=无（含已消失/已超时） */
int recv_pending_pull(RecvPending *out);

/* UI 在决定前同步"本次勾选接收的文件"（下标与 pending 的 files 一致；
 * 未勾选的会在接受后被后端标记跳过、不出现在 prepare 响应里）。 */
void recv_set_include(const bool inc[RECV_MAX_FILES]);

/* UI 决定（仅 PENDING 有效）：accept=1 接受（生成 sessionId/token，回 200 全收）
 * / 0 拒绝（回 403）。 */
void recv_decide(bool accept);

/* 用户中止进行中的接收：正在流式收体时置标志让 http 线程尽快收尾
 * （删 .part、回 CANCEL 态），空闲（READY/文件间）时立即生效。 */
void recv_abort(void);

/* UI 轮询会话状态；返回 1=有会话（含终态）0=空闲 */
int recv_status_pull(RecvStatus *out);

/* 接收会话结束展示完毕，清场回空闲 */
void recv_clear(void);

/* ---- http.c 调用（http 连接线程上下文） ---- */

/* http 层收体轮询时查"用户是否请求中止"（无锁读 volatile；返回 1 后应尽快收尾） */
bool recv_abort_pending(void);

/* prepare-upload：解析清单 → PENDING 阻塞等 UI 决定（最多 60s）→ 填 resp 返回 HTTP
 * 码（200=resp 为 JSON；403 拒绝/超时；400 坏体；409 已有活动会话） */
int recv_http_prepare(const char *body, const char *ip, char *resp, int respsz);

/* 流式读回调（http.c 提供）：阻塞轮询读，最多 max 字节，返回实际字节数；
 * 0=对端关闭或空闲超时；<0 socket 错误 */
typedef int (*recv_stream_fn)(void *ctx, unsigned char *buf, int max);

/* upload：校验 query(sessionId/fileId/token)+来源 IP → 经 fn 读 total 字节流式写盘。
 * 返回 HTTP 码（200 / 400 缺参 / 403 会话或 token 不符 / 409 会话已被取消 /
 *  422 sha256 不符 / 500 写盘等内部错）。 */
int recv_http_upload(const char *query, const char *ip, SceOff total,
                     recv_stream_fn fn, void *ctx);

/* cancel：发送方放弃 → 200（清理进行中的临时文件；已完成文件保留） */
int recv_http_cancel(const char *query, const char *ip);

#endif
