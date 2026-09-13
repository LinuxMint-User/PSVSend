/* 前后端契约（design.md §4.4）。
 * 本轮后端只做"网络底座"：config 持久化 + net 初始化 + UDP 发现 + 设备表。
 * UI 通过 api_device_snapshot() 每帧拷贝快照渲染，避免共享表竞态。 */
#ifndef PSVSEND_API_H
#define PSVSEND_API_H

#include <stdbool.h>
#include "net/device.h"        /* Device / DEVICE_MAX（类型住 net 层，见 device.h） */
#include "proto/transfer.h"    /* XferFile / XferInfo / XFER_MAX_FILES：发送契约数据类型 */

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

/* ---- UI 门面：链路状态 + 主动扫描 ----
 * r5：UI 只经本契约调后端，不再直接 include net/ 内部头。以下均为薄转发。 */

/* 链路/网络状态（读 net_poll 刷新的缓存，不做系统调用，UI 每帧可调） */
bool api_link_up(void);              /* 是否已连上 Wi-Fi */
int  api_ctl_state(void);            /* 最近一次 net_poll 的状态值（诊断） */
const char *api_local_ip(void);      /* 本机 IP（静态缓冲；未就绪为 "0.0.0.0"） */

/* 主动扫描设备（net/scan.c）：触发一轮 / 查询进度与结果 */
void api_scan_trigger(void);         /* 三角键手动扫网段；已在扫时顺延一轮 */
int  api_scan_active(void);          /* 1=正在扫 0=空闲 */
int  api_scan_done(void);            /* 本轮已探主机数（active 时有效） */
int  api_scan_total(void);           /* 本轮待探主机数（active 时有效） */
int  api_scan_found(void);           /* 本轮发现的设备数 */

/* ---- UI 门面：发送（proto/transfer.c） ----
 * 发送数据类型（XferFile/XferInfo/XFER_MAX_FILES）随本契约暴露，UI 不再
 * 直接 include proto/transfer.h。以下均为薄转发。 */

/* 启动发送：目标 ip/port/协议/指纹 + 文件列表；<0=参数非法/已在传输中
 * （失败原因写进快照，UI 照常展示） */
int  api_send_start(const char *ip, int port, const char *proto, const char *fp,
                    const XferFile *files, int n);
/* 请求取消发送：置标志，后台线程轮询到后尽快收尾 */
void api_send_cancel(void);
/* 锁内拷贝当前发送快照（每帧可调；线程结束后保留最后一次状态） */
void api_send_info(XferInfo *out);

#endif
