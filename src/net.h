/* 网络子系统：加载 SCE_SYSMODULE_NET 并初始化协议栈。
 * 幂等；失败后保存错误码，可查询。 */
#ifndef PSVSEND_NET_H
#define PSVSEND_NET_H

#include <stdbool.h>

/* 初始化；返回 0 成功，负值为 SceNet 错误码（幂等：重复调用返回缓存结果） */
int net_start(void);
/* 1=已就绪 0=未启动 <0=上次初始化失败码 */
int net_state(void);
/* 本机 IP（静态缓冲）；未就绪返回 "0.0.0.0" */
const char *net_local_ip(void);
/* 是否已连上网络（Wi-Fi） */
bool net_connected(void);
/* 调试：最近一次 sceNetCtlInetGetState 的返回码（0=成功）与状态值 */
int net_ctl_err(void);
int net_ctl_state(void);

#endif
