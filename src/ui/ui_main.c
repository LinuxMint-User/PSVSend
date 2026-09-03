/* UI 主循环：渲染 + 输入 → 页面调度。
 * widget 命中表由 w_clear/w_add 在本帧渲染时填充，输入处理时查询。 */
#include <string.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include "ui.h"
#include "theme.h"
#include "../api.h"

App g_app;
vita2d_pvf *g_font;
extern void pages_init(void);

/* ---------- widget 命中表 ---------- */
#define MAX_W 96
static Rect g_widgets[MAX_W];
static int  g_ids[MAX_W];
static int  g_wcount;

void w_clear(void) { g_wcount = 0; }

void w_add(int id, Rect r)
{
    if (g_wcount < MAX_W) {
        g_ids[g_wcount] = id;
        g_widgets[g_wcount] = r;
        g_wcount++;
    }
}

int w_hit(int x, int y)
{
    int i;
    for (i = g_wcount - 1; i >= 0; i--) {
        Rect *r = &g_widgets[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return g_ids[i];
    }
    return -1;
}

/* ---------- 页面调度 ---------- */
static void render_page(void)
{
    switch (g_app.page) {
    case PAGE_DEVICES:       page_devices_render(); break;
    case PAGE_FILES:         page_files_render(); break;
    case PAGE_SEND_CONFIRM:  page_send_confirm_render(); break;
    case PAGE_RECV_CONFIRM:  page_recv_confirm_render(); break;
    case PAGE_RECV_SETUP:    page_recv_setup_render(); break;
    case PAGE_PROGRESS:      page_progress_render(); break;
    case PAGE_SETTINGS:      page_settings_render(); break;
    default: break;
    }
}

static void input_page(const Input *in)
{
    switch (g_app.page) {
    case PAGE_DEVICES:       page_devices_input(in); break;
    case PAGE_FILES:         page_files_input(in); break;
    case PAGE_SEND_CONFIRM:  page_send_confirm_input(in); break;
    case PAGE_RECV_CONFIRM:  page_recv_confirm_input(in); break;
    case PAGE_RECV_SETUP:    page_recv_setup_input(in); break;
    case PAGE_PROGRESS:      page_progress_input(in); break;
    case PAGE_SETTINGS:      page_settings_input(in); break;
    default: break;
    }
}

/* ---------- 主循环 ---------- */
void ui_run(void)
{
    g_font = vita2d_load_default_pvf();
    ui_input_init();           /* 开启触摸采样 */
    pages_init();

    while (!g_app.done) {
        api_tick();          /* 网络巡检 + announce（500ms 节流，跑在主循环） */

        vita2d_start_drawing();
        vita2d_set_clear_color(theme->bg);
        vita2d_clear_screen();

        w_clear();
        render_page();

        Input in;
        ui_input_poll(&in);
        if (in.tap || in.up || in.down || in.left || in.right ||
            in.confirm || in.back || in.menu || in.alt || in.square ||
            in.drag_start || in.dragging)
            input_page(&in);

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }
}
