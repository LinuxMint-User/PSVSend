/* 发送模块（LocalSend v2 HTTP 客户端，见 transfer.c）。
 * 流程：POST /api/localsend/v2/prepare-upload（此请求会一直挂到接收方
 * 接受或拒绝为止，返回 {sessionId, files{文件ID:token}}）→ 逐文件
 * POST /api/localsend/v2/upload?sessionId&fileId&token 传原始字节。
 * 全程在独立线程跑（TCP 在后台线程已被 http 服务器验证可靠），UI 通过
 * xfer_info() 每帧拷贝快照。 */
#ifndef PSVSEND_TRANSFER_H
#define PSVSEND_TRANSFER_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/types.h>

#define XFER_MAX_FILES 64   /* 与 UI 的 MAX_PICKED 一致 */

/* 待发文件（xfer_start 入参，一次拷贝，线程内部使用） */
typedef struct {
    char   name[128];
    char   path[512];
    SceOff size;
} XferFile;

/* 传输过程快照（UI 只读展示；读取用 xfer_info 锁内拷贝） */
typedef struct {
    struct {
        char   name[128];
        SceOff size;
        SceOff sent;
        int    state;       /* 0=等待 1=上传中 2=完成 3=失败 */
    } f[XFER_MAX_FILES];
    int     count;
    int     cur;            /* 当前处理下标（-1=无） */
    bool    active;         /* 传输线程运行中 */
    bool    finished;       /* 线程已退出 */
    bool    ok;             /* 全部成功 */
    bool    cancelled;      /* 用户取消 */
    int     last_code;      /* 最近 HTTP 状态码 */
    SceOff  total;          /* 总字节 */
    SceOff  total_sent;     /* 已发字节 */
    uint64_t start_us;      /* 启动时刻（sceKernelGetSystemTimeWide） */
    char    msg[96];        /* 状态行：等待接受/上传中/已完成/失败原因 */
    char    err[160];       /* 失败详情（无失败为空串） */
} XferInfo;

/* 启动发送（目标 ip/port/协议/指纹 + 文件列表）；已在传输中或参数非法返回 <0，
 * 并把失败原因写进内部快照（UI 照常展示）。成功返回 0 并创建线程。
 * proto="https" 时走 mbedTLS（TLS1.2 客户端），fp 为对方证书 SHA-256 hex
 * （LocalSend 协议：HTTPS 模式下 announce 的 fingerprint 即证书哈希），
 * 握手时把收到的叶子证书哈希与 fp 比对做 pin。proto 非 https 一律纯 HTTP。 */
int  xfer_start(const char *ip, int port, const char *proto, const char *fp,
                const XferFile *files, int n);

/* 请求取消：置标志，线程在轮询/发送中检测并尽快收尾（不进 abort 流程） */
void xfer_cancel(void);

/* 锁内拷贝当前快照（每帧调用无妨；线程结束时快照保留最后一次状态） */
void xfer_info(XferInfo *out);

#endif
