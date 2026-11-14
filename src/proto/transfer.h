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

#define XFER_MAX_FILES 512   /* 与 UI 的 MAX_PICKED 一致；超过整批拒绝，不截断 */

/* 待发文件（xfer_start 入参，一次拷贝，线程内部使用）。
 * name 是发送给对端的 fileName：散文件=文件名，文件夹内文件=相对路径
 * （顶层目录名/子路径/文件名，正斜杠），与官方客户端"发送文件夹"同口径。 */
typedef struct {
    char   name[512];
    char   path[512];
    SceOff size;
} XferFile;

/* 单文件进度行（窗口快照；512 文件时不再每帧整份拷贝） */
typedef struct {
    char   name[512];       /* 相对路径（界面显示口径） */
    SceOff size;
    SceOff sent;
    int    state;           /* 0=等待 1=上传中 2=完成 3=失败 */
} XferRow;

/* 传输过程汇总（UI 只读展示；读取用 xfer_info 锁内拷贝）。
 * 文件行不进这里——按 [base, base+n) 走 xfer_window 窗口拉取。 */
typedef struct {
    int     count;
    int     cur;            /* 当前处理下标（-1=无） */
    int     done;           /* state==2 的文件数 */
    int     failed;         /* state==3 的文件数 */
    bool    active;         /* 传输线程运行中 */
    bool    finished;       /* 线程已退出 */
    bool    ok;             /* 全部成功 */
    bool    cancelled;      /* 用户取消 */
    int     last_code;      /* 最近 HTTP 状态码 */
    int     pin_need;       /* 0=无 1=对方要求 PIN（UI 弹键盘）2=上次 PIN 不对 */
    SceOff  total;          /* 总字节 */
    SceOff  total_sent;     /* 已发字节 */
    bool    hashing;        /* 发送前的 sha256 预扫阶段（进度条按 hash_done 走） */
    int     hash_cur;       /* 正在算校验的文件下标（-1=无） */
    SceOff  hash_done;      /* 预扫已读字节（整批） */
    SceOff  hash_cur_done;  /* 当前文件已扫字节（分段进度条用） */
    SceOff  hash_cur_size;  /* 当前文件总字节（等待页分段进度用） */
    SceOff  hash_total;     /* 预扫总字节（= total） */
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

/* 对方回 401（要求 PIN）时，UI 用系统键盘输入后回填（IME 确认时调用）：
 * xfer_set_pin 交出输入并唤醒等待中的 prepare 重试；xfer_pin_cancel 表示
 * 用户放弃输入（线程按失败收尾，不再干等）。 */
void xfer_set_pin(const char *pin);
void xfer_pin_cancel(void);

/* 锁内拷贝当前快照（每帧调用无妨；线程结束时快照保留最后一次状态） */
void xfer_info(XferInfo *out);

/* 锁内拷贝文件行 [base, base+max)，返回实际行数（窗口之外返回 0）。
 * 进度页每帧按滚动位置拉取，512 文件时也不再整份拷贝。 */
int  xfer_window(int base, int max, XferRow *out);

/* 轻量查询：是否正在发送（锁内读 active；receive 侧"发送中拒绝接收"用） */
bool xfer_active(void);

#endif
