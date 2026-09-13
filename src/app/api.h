/* 前后端契约（design.md §4.4）。
 * 本轮后端只做"网络底座"：config 持久化 + net 初始化 + UDP 发现 + 设备表。
 * UI 通过 api_device_snapshot() 每帧拷贝快照渲染，避免共享表竞态。 */
#ifndef PSVSEND_API_H
#define PSVSEND_API_H

#include <stdbool.h>
#include "net/device.h"        /* Device / DEVICE_MAX（类型住 net 层，见 device.h） */

/* 启动后端：config_init + net_start + discovery_start（各模块内部幂等）。
 * 后台线程自理：api_watch（500ms 巡检重试）+ http（收对方 register/info）。
 * 注意：UDP announce 不能进后台线程（Vita sendto 只在主线程可靠），由 UI 每帧
 * 调 api_tick() 节流驱动。 */
void api_start(void);

/* UI 主循环每帧调用：500ms 节流喂 announce 节奏。必须跑在主线程。 */
void api_tick(void);
/* UI 主循环每帧在 swap 之后调用：断网时每 4s 发一包 UDP"活性刺激"唤醒
 * 待机省电断开的 Wi-Fi（sendto 只可靠在主线程；Wi-Fi 重连时可能阻塞数秒，
 * 故放在一帧渲染完之后）。必须跑在主线程。 */
void api_poke(void);

/* 拷贝设备快照到 out（最多 max 台），返回当前设备数（快照锁内完成） */
int api_device_snapshot(Device *out, int max);
/* 后端网络是否已就绪并跑着发现线程 */
bool api_network_ready(void);
/* 发现是否因初始化失败不可用（1=可用 0=未起 <0=失败码） */
int  api_discovery_state(void);
/* 失败定位（"步骤 错误码"，如 "join 224.0.0.167 0x8041010D"；未失败返回 ""） */
const char *api_discovery_fail(void);

#endif
