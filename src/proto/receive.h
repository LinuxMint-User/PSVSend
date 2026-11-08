/* receive.h —— 接收方向（LocalSend v2 上传 API 的服务端会话，实现见 receive.c）。
 * 对方(发送方)按我们 announce 里的 http 端口 POST 过来：
 *   prepare-upload  文件清单 JSON → 挂起等 UI 接受/拒绝（回 200 {sessionId,files{id:token}}
 *                   或 403；已有活动会话时 409）
 *   upload?sessionId&fileId&token  裸文件字节流 → 边收边写盘：顶层目录的文件先写
 *                   暂存根 <目录名>.psvsend-part/（内部直接还原相对路径），收完
 *                   由 finalize 线程整体改名/合并到保存目录；平铺文件写
 *                   <名>.psvsend.tmp 后改名。sha256 校验失败 422。保存目录：
 *                   config saveDir 持久默认，UI 可在接受前 recv_set_dir 覆盖为
 *                   本次目录（内存态）。
 *                   **单文件失败只影响该文件**（与官方一致）：写盘/校验/落盘等错误
 *                   不终止会话，其余文件照收；全部收尾后若有失败，会话终态为
 *                   RECV_ST_PARTIAL。断流/取消/超时仍是会话级（对端已不可达）。
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
#include "proto/recv_path.h" /* RP_REL_MAX：相对路径窗口行缓冲 */

/* 后端硬上限（单次接收会话；超出的整份拒绝）。
 * RECV_MAX_FILES 仅用于扫码拉取（prepare-download）清单的临时上限。 */
#define RECV_FILES_MAX 2048
#define RECV_MAX_FILES 64

/* ---- M1 新契约：summary + 窗口 ----
 * 后端按实际文件数动态分配，UI 不再整份拷贝；确认页/进度页只读 summary，
 * 列表按 [base, base+max) 拉窗口。行内 name 是相对路径（可能含 '/'）。 */
typedef struct {
    char peer_alias[64];
    char peer_type[24];
    char peer_ip[16];
    char peer_fp[65];
    int  count;                  /* 解析出的可用条目数（含 path_ok=false 的） */
    int  selected;               /* 勾选且路径预算可用的文件数 */
    int  overflow;               /* 旧字段：M3 起超限整份拒绝，恒为 0 */
    int  skipped;                /* 非法路径/目录条目/路径超预算等跳过数 */
    int  trunc;                  /* 名字被缩短过的条目数 */
    SceOff total;                /* 全部条目的期望字节数 */
    SceOff selected_size;        /* 勾选且可收文件的期望字节数 */
    int  cfg_limit;              /* 设置里的接收文件上限（M3 固定 64，M5 接 config） */
    int  eff_limit;              /* 本次实际生效档位（文件数超过 cfg 时临时提升） */
} RecvPendingSummary;

typedef struct {
    char name[RP_REL_MAX + 1];   /* 规整后的相对路径（落盘口径） */
    char rname[RP_COMP_MAX + 1]; /* UI 改名后的最后一级（空=沿用原名） */
    SceOff size;
    bool trunc;                  /* 名字过长被缩短过 */
    bool selected;               /* 当前勾选状态 */
    bool ok;                     /* 路径预算可用；false = 接受时会被跳过 */
} RecvPendingRow;

typedef struct {
    int  state;                  /* RecvStateId */
    char peer_alias[64];
    int  count;                  /* 实际参与的文件数 */
    int  cur;                    /* 正在收的文件下标（-1=无） */
    int  done;                   /* st==1 的文件数 */
    int  failed;                 /* st==3 的文件数 */
    int  skipped;                /* 接受时跳过（非法/超预算/冲突）的文件数 */
    SceOff total;
    SceOff got_total;
    uint64_t start_us;
    char err[192];
} RecvStatusSummary;

typedef struct {
    char name[RP_REL_MAX + 1];   /* 相对路径 */
    SceOff size;
    SceOff got;
    unsigned char st;            /* 0=未完成 1=完成 3=失败 */
} RecvStatusRow;

/* 接收会话状态（进度页/结束态展示） */
typedef enum {
    RECV_ST_NONE = 0,    /* 无活动会话（UI 可回主页） */
    RECV_ST_READY,       /* 已接受，等对方开始传 */
    RECV_ST_RECEIVING,   /* 上传中 */
    RECV_ST_FINALIZING,  /* 文件已收完，正在把暂存根整理到最终位置（不可取消） */
    RECV_ST_DONE,        /* 全部文件收完 */
    RECV_ST_FAIL,        /* 传输失败（中断/校验/写盘） */
    RECV_ST_CANCEL,      /* 发送方放弃 */
    RECV_ST_TIMEOUT,     /* 接受后对方一直没传 */
    RECV_ST_PARTIAL      /* 全部文件已收尾，但其中有失败（部分成功） */
} RecvStateId;

/* 后端初始化（建锁 + 清扫上次中断残留的 *.psvsend.tmp）；api_start 调用 */
void recv_init(void);

/* ---- 接收状态契约（UI 只读 summary + 窗口） ---- */
/* 待决定请求的汇总；返回 1=有（out 可 NULL 仅探测） */
int recv_pending_summary(RecvPendingSummary *out);
/* 拷贝 pending 的 [base, base+max) 行；返回实际拷贝行数 */
int recv_pending_window(int base, int max, RecvPendingRow *out);
/* 确认页勾选：idx 为全局下标（0..count-1） */
void recv_pending_toggle(int idx, bool on);
/* 确认页逐文件改名：只改最后一级文件名（空串=恢复原名） */
void recv_pending_rename(int idx, const char *name);

/* 会话汇总；返回 1=有会话（含终态） */
int recv_status_summary(RecvStatusSummary *out);
/* 拷贝会话的 [base, base+max) 行；返回实际拷贝行数 */
int recv_status_window(int base, int max, RecvStatusRow *out);

/* 上一份待确认请求是否因"发送方放弃"而消失（一次性：取走即清零）。
 * 官方发送方在等接受的阶段取消时是 abort 掉 prepare 请求（关连接），
 * 不会、也无法调 /cancel——那时 sessionId 还没生成。 */
int recv_pending_peer_gone_take(void);
/* 最近一次 prepare 被拒的原因（HTTP_REJ_*，一次性：取走即清零）。
 * 覆盖文件数超限 / 清单过大 / 忙 / 内存不足，UI 在主页细条上短时提示。 */
int recv_reject_take(void);
/* 名字 → 可落盘的单级文件名：按 ux0: 合法性净化 + 超长时"保后缀、截主名"，
 * 见 receive.c。落盘与 UI 改名共用，保证"界面上显示的即落盘名"；
 * UI 侧只经 api_recv_sanitize_name 调用。
 * 返回 true = 名字超出 191 字节预算、被截短了（接收确认页"文件名过长"提示的依据；
 * 路径前缀、首尾空白/点之类的缩短不算——那不是"过长"）。 */
bool recv_sanitize_name(const char *in, char *out, int n);

/* UI 在接受前设定"本次保存目录"（内存态，仅本会话；NULL/空 → 回退
 * config saveDir 默认）。不持久化，下次会话由 UI 重新给出默认值。 */
void recv_set_dir(const char *dir);

/* UI 决定（仅 PENDING 有效）：accept=1 接受（生成 sessionId/token，回 200 全收）
 * / 0 拒绝（回 403）。 */
void recv_decide(bool accept);

/* 决定窗口的暂停/重开（仅 PENDING 有效，其余时候空操作）。窗口语义是"确认页
 * 上没有操作满 60s"：UI 进接收设置页/验证页（含其中打开的目录选择页）时
 * hold=1 暂停，回到确认页 hold=0 并把窗口重开成满 60s（不是接着走）。
 * 对方撤销由探活独立判定，暂停不影响它。 */
void recv_decide_hold(int on);
/* 确认页上有任何操作 → 重开满窗（暂停期间调用无效）。 */
void recv_decide_touch(void);
/* 确认页倒计时：剩余整秒；-1 = 无待决定请求或正在暂停（不该显示数字）。 */
int  recv_decide_left_s(void);

/* 用户中止进行中的接收：正在流式收体时置标志让 http 线程尽快收尾
 * （删临时文件、回 CANCEL 态），空闲（READY/文件间）时立即生效。 */
void recv_abort(void);

/* 网页共享已停止：把"来自浏览器网页"（prepare 带有效 PIN）的待决定请求按拒绝
 * 收尾、进行中的上传中止；普通 LocalSend 发送方不受影响。 */
void recv_cancel_web(void);

/* ---- 扫码链接（拉取会话）：扫到别人网页共享的二维码后主动去下载 ----
 * 与上面的推送会话共用同一份"待决定/活动会话"状态与确认页/进度页：
 *   recv_link_open 起后台线程 POST prepare-download?pin=，拿到清单后进入
 *   PH_PENDING（确认页照常勾选/改名/选目录）；接受后同一线程逐个 GET
 *   download?sessionId=&fileId= 写盘，进度/错误/中止语义与推送会话一致。
 * host 必须是 IPv4 字面量（二维码里是本机写的局域网 IP）。 */
/* 返回 0=已开始连接；-1=已有待决定/活动会话或上一次连接还没结束；-2=参数非法 */
int recv_link_open(const char *host, int port, const char *pin);
/* 1=正在连接/取清单（还没出现待决定请求），0=不在连接阶段。UI 用它显示"正在打开" */
int recv_link_connecting(void);
/* 取走一次性失败码：0=无 1=连不上 2=对方拒绝/不是共享页 3=响应异常
 * 4=已被取消（离开扫码页） */
int recv_link_error_take(void);
/* 取消"连接中"的扫码链接（已出现的待决定/活动会话不归它管，走 recv_decide/abort） */
void recv_link_cancel(void);

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
