/* 客户端更新检查（app/update.c）。
 * 后台一次性线程 HTTPS GET 本项目 GitHub Releases 的 atom 源，与本地
 * PSVSEND_APP_VERSION 比对。UI 只读状态；失败/超时落到 FAIL 不影响日常。
 * 自动检查频率与上次检查时间存 config（upd_auto / upd_last）。 */
#ifndef PSVSEND_APP_UPDATE_H
#define PSVSEND_APP_UPDATE_H

typedef enum {
    UPD_IDLE = 0,   /* 尚未执行过任何检查 */
    UPD_WORKING,    /* 检查进行中（后台线程） */
    UPD_NEW,        /* 发现新版：update_latest() 为远端 tag（如 "v2.0.1"） */
    UPD_NONE,       /* 已是最新 */
    UPD_FAIL        /* 检查失败（网络/解析/超时），可手动重试 */
} UpdState;

void update_init(void);        /* 状态复位（读 config 节流时间） */
void update_tick(void);        /* 主循环每帧可调：每秒节流；每会话一次自动评估 */
void update_check_now(void);   /* 手动立即检查（进行中忽略） */
UpdState update_state(void);
const char *update_latest(void);   /* UPD_NEW 时的远端版本串（含 v 前缀） */

#endif
