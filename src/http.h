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

/* 启动服务器：依次尝试候选端口，返回实际端口；全部失败返回 <0 */
int http_start(void);
/* 当前绑定的端口（未启动返回 0） */
int http_port(void);
/* register POST 到达时的回调（body=对方 JSON，src_ip=对方来源 IP） */
void http_set_register_cb(void (*cb)(const char *body, const char *src_ip));

#endif
