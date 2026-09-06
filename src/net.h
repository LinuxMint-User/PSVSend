/* 网络子系统：加载 SCE_SYSMODULE_NET 并初始化协议栈。
 * 幂等；失败后保存错误码，可查询。 */
#ifndef PSVSEND_NET_H
#define PSVSEND_NET_H

#include <stdbool.h>

/* 初始化；返回 0 成功，负值为 SceNet 错误码（幂等：重复调用返回缓存结果） */
int net_start(void);
/* 1=已就绪 0=未启动 <0=上次初始化失败码 */
int net_state(void);
/* 后台轮询（api_watch 线程每 500ms 调一次）：刷新下面的链路状态与本地 IP 缓存。
 * 这是全工程唯一的 netctl 状态/信息查询点——主线程/UI 只读缓存，不做系统调用，
 * 避免 Wi-Fi 恢复瞬间高频查询把 netctl 服务卡死（真机曾卡死主线程，UI 永久定格） */
void net_poll(void);
/* 本机 IP（静态缓冲）；由 net_poll 刷新，未就绪/未轮询到为 "0.0.0.0" */
const char *net_local_ip(void);
/* 是否已连上网络（Wi-Fi）；读 net_poll 刷新的缓存，不做系统调用（UI 每帧可调） */
bool net_connected(void);
/* 调试：最近一次 net_poll 的返回码（0=成功）与状态值 */
int net_ctl_err(void);
int net_ctl_state(void);

/* 重建 netctl 客户端（持续断网时由 watch 周期调用）：拆旧 term + 重新 init，
 * 解"netctl 在 Wi-Fi 关闭时初始化后收不到状态翻转"的卡死 */
void net_ctl_recycle(void);

/* 断网期间的"活性刺激"：向组播地址发一包 UDP。目的不是连通（必然失败），
 * 而是让系统感知"有应用在要网络"，促使待机省电断开的热点自动重连 */
int net_poke(void);

#endif
