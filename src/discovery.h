/* 发现（PS Vita 特化版，见 discovery.c）：
 * Vita 系统保留 53317 端口（TCP/UDP bind 均 EACCES）且收不到组播，故标准
 * "组播收发"不可行。实际采用：
 *  - 周期组播/广播 announce（只发不收，对方 LocalSend 会反向 HTTP register 到我们）；
 *  - http.c 收到对方 register POST 时调用 discovery_peer_registered 记入设备表。
 * 双向互见完全不需要绑定端口。设备表由互斥锁保护；UI 经 api_device_snapshot 拿快照。 */
#ifndef PSVSEND_DISCOVERY_H
#define PSVSEND_DISCOVERY_H

#include <stdbool.h>
#include "api.h"

/* 启动发现（内部创建 socket + announce 线程）；返回 0 成功，负值为错误码 */
int discovery_start(void);

/* 快照拷贝（供 api 层）；返回当前设备数 */
int discovery_snapshot(Device *out, int max);
int discovery_device_count(void);
/* 1=运行 0=未启动 <0=启动失败码 */
int discovery_state(void);
/* 失败定位：最近一次启动失败发生在哪一步（"step 0x.."，无失败返回 ""） */
const char *discovery_fail_step(void);
/* http 服务器回调：收到对方 register POST（body=对方 JSON, src_ip=来源 IP） */
void discovery_peer_registered(const char *body, const char *src_ip);
/* 扫描线程回调：主动探测到的设备直接入表（内部带自排除） */
void discovery_upsert_peer(const Device *dev);

void disc_tick_announce(void);          /* 看门狗每 500ms 调：内部按 5s 节奏发 announce */
#endif
