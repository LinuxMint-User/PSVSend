/* scan.h —— 主动 HTTP 设备扫描（协议 §3.2 legacy 模式）。
 * Vita 收不了 UDP 组播、绑不了 53317，设备表无法靠"监听"填充；除等对方
 * 主动 register 外，本模块让 PSV 自己向本机 /24 各 IP 的 53317 发
 * POST /api/localsend/v2/register，从 200 响应里拿对端 member info 入表。
 * 扫描在后台线程跑（TCP 无 UDP 的"主线程亲缘"限制）。 */
#ifndef PSVSEND_SCAN_H
#define PSVSEND_SCAN_H

#include <stdbool.h>

/* 触发一轮全扫（后台执行；已在扫时顺延一轮）。首次调用会创建扫描线程。 */
void scan_trigger(void);

/* 1=扫描线程已建立 */
bool scan_up(void);

/* 1=正在扫 0=空闲 */
int scan_active(void);

/* 本轮进度（active 时有效） */
int scan_done(void);
int scan_total(void);

#endif
