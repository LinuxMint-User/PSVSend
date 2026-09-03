/* 前后端契约（design.md §4.4）。
 * 本轮后端只做"网络底座"：config 持久化 + net 初始化 + UDP 发现 + 设备表。
 * UI 通过 api_device_snapshot() 每帧拷贝快照渲染，避免共享表竞态。 */
#ifndef PSVSEND_API_H
#define PSVSEND_API_H

#include <stdbool.h>

#define API_MAX_DEVICES 32     /* 与 UI 的 MAX_DEVICES 保持一致 */

/* 一台被发现的 LocalSend 设备（发现消息里的关键字段） */
typedef struct {
    char ip[16];             /* 发送方 IP（announce 来源） */
    int  port;               /* 该设备 HTTP 服务端口 */
    char alias[64];          /* 设备名 */
    char model[64];          /* 设备型号（可空） */
    char dtype[16];          /* 平台类型 mobile/desktop/web/... */
    char fingerprint[64];    /* 身份串（防自发现用） */
    char protocol[8];        /* http / https */
    bool download;           /* 是否开了下载 API（能否主动收） */
} Device;

/* 启动后端：config_init + net_start + discovery_start（各模块内部幂等）。
 * 后台线程自理：api_watch（500ms 巡检重试）+ http（收对方 register/info）。
 * 注意：UDP announce 不能进后台线程（Vita sendto 只在主线程可靠），由 UI 每帧
 * 调 api_tick() 节流驱动。 */
void api_start(void);

/* UI 主循环每帧调用：500ms 节流喂 announce 节奏。必须跑在主线程。 */
void api_tick(void);

/* 拷贝设备快照到 out（最多 max 台），返回当前设备数（快照锁内完成） */
int api_device_snapshot(Device *out, int max);
/* 后端网络是否已就绪并跑着发现线程 */
bool api_network_ready(void);
/* 发现是否因初始化失败不可用（1=可用 0=未起 <0=失败码） */
int  api_discovery_state(void);
/* 失败定位（"步骤 错误码"，如 "join 224.0.0.167 0x8041010D"；未失败返回 ""） */
const char *api_discovery_fail(void);

#endif
