/* 迷你 HTTP 服务器（discovery/收发共用，见 http.c）。
 * 真机事实：Vita 系统保留 53317 端口（TCP/UDP 都 EACCES），组播也收不了，
 * 所以本模块在【可绑定】的候选端口上提供 LocalSend v2 HTTP 端点：
 *   POST /api/localsend/v2/register     —— 别人听到我们的 announce 后会 POST
 *                                           自己信息过来；我们把 body 交给注册
 *                                           回调（discovery 记入设备表），并回
 *                                           我们的信息让对方把我们加进列表。
 *   GET  /api/localsend/v2/info          —— 返回我们的信息。
 *   POST /api/localsend/v2/prepare-upload —— 接收方向：文件清单（receive 模块
 *                                           挂起等 UI 决定，最长 60s）。
 *   POST /api/localsend/v2/upload?...   —— 接收方向：流式收文件体，边收边写盘
 *                                           （绝不全量进内存）。
 *   POST /api/localsend/v2/cancel?...   —— 接收方向：发送方放弃会话。
 *  其他一律 404。
 * announce 里声明的 port 就是这里实际绑到的端口。 */
#ifndef PSVSEND_HTTP_H
#define PSVSEND_HTTP_H

#include <stdbool.h>
#include <stdint.h>

/* 启动服务器：依次尝试候选端口，返回实际端口；全部失败返回 <0 */
int http_start(void);
/* 当前绑定的端口（未启动返回 0） */
int http_port(void);
/* 探活：监听 socket 与 accept 线程是否还健康（0=已死，需 http_restart 重建） */
int http_alive(void);
/* 停服务器（关监听、等 accept 线程退出）；唤醒/链路变化后用 */
void http_stop(void);
/* 停掉再按候选端口重绑（http_stop + http_start） */
int http_restart(void);
/* register POST 到达时的回调（body=对方 JSON，src_ip=对方来源 IP） */
void http_set_register_cb(void (*cb)(const char *body, const char *src_ip));

/* ---------- 接收方向路由：注册式（依赖倒置） ----------
 * http 层只做 HTTP 语义与连接读写，不认 LocalSend 协议；接收侧的处理实现住在
 * proto/receive.c，由组合层（app/api.c）启动时注册进来。这样 net 层不必
 * include proto 层（此前 http.c→receive.h 与 transfer.c→http.h 构成依赖环）。 */
typedef int (*http_stream_fn)(void *ctx, unsigned char *buf, int max);

typedef struct {
    /* POST /prepare-upload：body=清单 JSON，ip=来源；回执写 resp，返回 HTTP 状态码 */
    int  (*prepare)(const char *body, const char *ip, char *resp, int respsz);
    /* POST /upload?...：total<0 表示 chunked（无 Content-Length）；rd/ctx 由 http 层
     * 提供，协议层经它流式读 body（阻塞轮询语义：返回实读字节 / 0 关闭或超时 / <0 错） */
    int  (*upload)(const char *query, const char *ip, int64_t total,
                   http_stream_fn rd, void *ctx);
    /* POST /cancel?...：发送方放弃会话 */
    int  (*cancel)(const char *query, const char *ip);
    /* 收 body 轮询时查"用户是否请求中止"（返回 1 后 http 层尽快收手） */
    bool (*abort_pending)(void);
} HttpRecvOps;

/* 注册接收侧处理集（api.c 启动时调一次；未注册时相关路由回 500） */
void http_set_recv_ops(const HttpRecvOps *ops);

#endif
