/* 页面共享状态与开机流程。
 * 具体页面按职责分文件：
 *   pages_send.c      发送主页（设备栏 + 已选文件栏）/ 文件浏览 / 发送等待
 *   pages_progress.c  进度页（发送/接收共用）
 *   pages_recv.c      接收确认 / 接收设置（含系统键盘改名）
 *   pages_settings.c  设置 / 目录选择
 * 布局常量与各页共用的小工具见 ui/pages_internal.h。
 *
 * 本文件只保留跨页共享的进度行清单（xf_*）与开机初始化/预热。 */
#include <psp2/kernel/threadmgr/thread.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/i18n.h"
#include "app/api.h"
#include "core/dlog.h"

/* 进度页当前文件清单：发送由 start_send（pages_send.c）先写入、每帧再由
 * xfer 快照覆盖；接收由 start_recv（pages_recv.c）先以本地清单兜底首帧、
 * 再由 receive 会话快照覆盖（渲染在 pages_progress.c）。 */
int    xf_count = 0;
char   xf_name[MAX_PICKED][192];
SceOff xf_size[MAX_PICKED];
int    xf_scroll = 0;

void pages_init(void)
{
    int i;
    config_init();                       /* 读配置：主题/键位布局/设备名/语言等 */
    i18n_init();                         /* 解析界面语言（跟随系统或偏好） */
    if (g_cfg.theme_id < 0 || g_cfg.theme_id >= THEME_COUNT) g_cfg.theme_id = 0;
    g_app.theme_id = g_cfg.theme_id;
    g_app.light_mode = g_cfg.light_mode ? THEME_LIGHT : THEME_DARK;
    g_app.confirm_layout = g_cfg.confirm_layout ? 1 : 0;
    g_app.dev_count = 0;
    g_app.dev_sel = 0;
    g_app.dev_target = 0;
    g_app.pane_focus = 1;   /* 开机无文件：焦点落文件栏（先选文件再挑设备） */
    g_app.pane_swap = g_cfg.pane_swap ? 1 : 0;  /* 两栏布局：0=设备在左 1=文件在左 */
    g_app.picked_count = 0;
    g_app.picked_sel = 0;
    g_app.picked_total = 0;
    for (i = 0; i < MAX_DEVICES; i++) {
        g_app.dev_alias[i][0] = 0;
        g_app.dev_sub[i][0] = 0;
        g_app.dev_kind[i][0] = 0;
        g_app.dev_ip[i][0] = 0;
        g_app.dev_port[i] = 0;
        g_app.dev_proto[i][0] = 0;
        g_app.dev_fp[i][0] = 0;
    }
    g_app.page = PAGE_DEVICES;
    g_app.prog_running = false;
    g_app.done = false;
    theme_set_custom(g_cfg.custom_h, g_cfg.custom_s, g_cfg.custom_v);
    theme_set(g_app.theme_id, g_app.light_mode);
    api_start();                         /* 网络底座：net + UDP 发现线程 */
}
