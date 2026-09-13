/* 页面共享状态与开机流程。
 * 具体页面按职责分文件：
 *   pages_send.c      设备列表 / 文件浏览 / 发送确认
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
char   xf_name[MAX_PICKED][128];
SceOff xf_size[MAX_PICKED];
int    xf_scroll = 0;

void pages_init(void)
{
    int i;
    config_init();                       /* 读配置：主题/键位布局/设备名/语言等 */
    i18n_init();                         /* 解析界面语言（跟随系统或偏好） */
    if (g_cfg.theme_id < 0 || g_cfg.theme_id >= THEME_COUNT) g_cfg.theme_id = 0;
    g_app.theme_id = g_cfg.theme_id;
    g_app.confirm_layout = g_cfg.confirm_layout ? 1 : 0;
    g_app.dev_count = 0;
    g_app.dev_sel = 0;
    g_app.dev_target = 0;
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
    theme_set(g_app.theme_id);
    api_start();                         /* 网络底座：net + UDP 发现线程 */
}

/* ================= 开机预热（常用页） =================
 * 只烤"主页面(设备列表) + 文件选择页 + 设置页(滚顶+滚底含 About)"三个
 * 高频页：当前语言静态文案按真实字号/字体路由烤进 atlas 后，这三页首开
 * 与设置页滚到底都零卡。发送/接收/进度等低频页维持首开现烤（已接受）。
 * 页面画在隐帧上，由 ui_main 的 ui_warm_pass 呈现开屏画面、本函数不呈现。
 * 数据前提：开机 pages_init 之后、主循环之前调用，多数页空/idle，渲染
 * 函数对空状态安全。每页记一次耗时供测量。 */
void pages_warm_all(void)
{
    uint64_t t0 = (uint64_t)sceKernelGetSystemTimeWide();
    uint64_t s = t0, p;

#define WARM_ONE(name, code) do { \
        code; \
        p = (uint64_t)sceKernelGetSystemTimeWide(); \
        dlog("warm %s=%lldms cum=%lldms", name, \
             (long long)((p - s) / 1000), (long long)((p - t0) / 1000)); \
        s = p; \
    } while (0)

    /* 顺序即累积去重后的真实首开成本：设备页 → 文件页 → 设置页顶/底 */
    WARM_ONE("dev",   g_app.page = PAGE_DEVICES; page_devices_render());
    WARM_ONE("files", g_app.page = PAGE_FILES;   page_files_render());
    WARM_ONE("set-top",  g_app.page = PAGE_SETTINGS; settings_scroll_to(0);
             page_settings_render());
    WARM_ONE("set-about", settings_scroll_to(0x7FFFFFFF);  /* render 首行夹到最大滚动 */
             page_settings_render());
    settings_scroll_to(0);
#undef WARM_ONE

    dlog("warm total=%lldms", (long long)((s - t0) / 1000));
    g_app.page = PAGE_DEVICES;       /* 复位到开机页 */
}
