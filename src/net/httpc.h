/* 极简 HTTP/1.1 客户端（明文；扫码链接拉取网页共享文件用，见 httpc.c）。
 * 只做本功能的两个动作：
 *   httpc_post      —— 发一个 POST（可带体），把响应体收进缓冲（prepare-download 的 JSON）
 *   httpc_get_stream —— GET 并把响应体分块交给回调（download 的文件字节流）
 * host 必须是 IPv4 字面量（Vita 端不做 DNS；二维码里是我们自己写入的局域网 IP）。 */
#ifndef PSVSEND_HTTPC_H
#define PSVSEND_HTTPC_H

#include <stdint.h>

/* GET 流式回调：buf[0..len) 是本次收到的一段响应体。
 * 返回 0=继续；<0=调用方主动中止（取消/写盘失败），httpc_get_stream 回 -2。 */
typedef int (*httpc_sink_fn)(void *ctx, const unsigned char *buf, int len);

/* POST http://host:port/path（path 含 query）。body 可为 NULL（按 0 长发）。
 * resp 收响应体（总能 NUL 结尾；放不下按失败处理）；*status 填 HTTP 状态码。
 * 返回：0=完整收到响应（不论状态码）；-1=连接/收发/超时/响应过大。 */
int httpc_post(const char *host, int port, const char *path,
               const char *body, char *resp, int cap, int *status,
               int timeout_ms);

/* GET http://host:port/path，边收边调 sink。
 * *total = 响应里的 Content-Length（没有则 -1）；*status = HTTP 状态码。
 * 返回：0=完整收完；-1=连接/收发/超时；-2=sink 主动中止；-3=HTTP 状态非 200；
 *       -4=响应是 chunked 或无长度（本功能不支持的形态）。 */
int httpc_get_stream(const char *host, int port, const char *path,
                     httpc_sink_fn sink, void *ctx, int64_t *total,
                     int *status, int timeout_ms);

#endif
