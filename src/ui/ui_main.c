/* UI 主循环：渲染 + 输入 → 页面调度。
 * widget 命中表由 w_clear/w_add 在本帧渲染时填充，输入处理时查询。 */
#include <string.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include "ui.h"
#include "theme.h"
#include "../api.h"
#include "../dlog.h"

App g_app;
extern void pages_init(void);

/* ---------- 字体（按像素字号分槽，懒加载） ----------
 * libvita2d 的字体 atlas 按字形缓存、不分字号：同一字体对象若某字符
 * 先以小字号光栅化，之后大字号绘制会直接放大那份小位图（draw_scale
 * = size/缓存字号 > 1）。LINEAR 过滤会把 atlas 相邻槽的渗色随放大一起
 * 放大 → 大字顶/底出现横线；且同词中部分字是原生光栅化、部分字是
 * 放大/插值出来的，笔划粗细不一，视觉参差。对策：每个像素字号一个
 * 独立字体对象（独立 atlas），draw_scale 恒为 1，全部原生光栅化。
 * 首次用到某字号才 load；同字号 latin/cjk 各一份。 */
#define FONT_PATHS_BASE "app0:/fonts/"
#define FONT_LAT_FILE   FONT_PATHS_BASE "DroidSans.ttf"
#define FONT_CJK_FILE   FONT_PATHS_BASE "DroidSansFallbackFull.ttf"
#define MAX_FONT_SIZES 24
static struct { int size; vita2d_font *lat; vita2d_font *cjk; } g_fs[MAX_FONT_SIZES];
static int g_fs_n;

/* 返回 size 像素字号的字体：cjk=0 拉丁、1 CJK；加载失败返回 NULL。
 * 表满（UI 档位应远少于 MAX_FONT_SIZES）时回退 0 号槽。 */
vita2d_font *font_get(int size, int cjk)
{
    int i;
    for (i = 0; i < g_fs_n; i++)
        if (g_fs[i].size == size)
            return cjk ? g_fs[i].cjk : g_fs[i].lat;
    if (g_fs_n < MAX_FONT_SIZES) {
        vita2d_font *lat = vita2d_load_font_file(FONT_LAT_FILE);
        vita2d_font *cj  = vita2d_load_font_file(FONT_CJK_FILE);
        if (!lat || !cj)
            dlog("font load failed size=%d lat=%p cjk=%p", size,
                 (void *)lat, (void *)cj);
        g_fs[g_fs_n].size = size;
        g_fs[g_fs_n].lat  = lat;
        g_fs[g_fs_n].cjk  = cj;
        g_fs_n++;
        return cjk ? cj : lat;
    }
    return cjk ? g_fs[0].cjk : g_fs[0].lat;
}

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
    ui_input_init();           /* 开启触摸采样 */
    pages_init();

    while (!g_app.done) {
        api_tick();          /* announce 节奏（500ms 节流；sendto 只在主线程可靠） */
        pages_tick();        /* 检测新到待决定的接收请求 → 弹接收确认页 */

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
