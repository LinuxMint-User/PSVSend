/* 页面：设备列表 / 文件浏览 / 发送确认 / 接收确认 / 进度 / 设置
 * 目前数据为 mock：设备是演示数据，传输是模拟进度。
 * 渲染函数每帧先绘制并注册可触摸区域（w_add），输入函数处理动作与命中。
 *
 * 列表采用"像素滚动"模型：内容像素偏移 scroll（0=从 LIST_TOP 开始），
 * 触摸拖动让内容直接跟手（正常滑动页面），方向键移动选中时自动滚到可见。 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vita2d.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui.h"
#include "theme.h"

/* ---------- mock 数据 ---------- */
static const char *mock_alias[] = {
    "OPPO A1", "Xiaomi Pad 6", "Windows-PC", "Pixel 7", "MacBook Air",
};
static const char *mock_type[] = {
    "phone", "tablet", "desktop", "phone", "desktop",
};
#define MOCK_COUNT ((int)(sizeof mock_alias / sizeof mock_alias[0]))

/* ---------- 布局常量 ---------- */
#define LIST_TOP     76                 /* 列表可视区顶 */
#define LIST_BOTTOM  (SCR_H - 46 - 6)   /* 列表可视区底（页脚上方留白） */
#define LIST_VIEW_H  (LIST_BOTTOM - LIST_TOP)
#define ROW_H        56
#define ROW_STRIDE   62
#define WID_FILES_SEND 0x8001

/* 传输页（mock）：文件列表区域与总进度布局 */
#define XF_TOP        68
#define XF_BOTTOM     370      /* 文件列表可视区底（总进度条上方） */
#define XF_VIEW_H     (XF_BOTTOM - XF_TOP)
#define XF_ROW_H      58
#define PROG_TOTAL_MS 6000     /* mock 总时长：约 6 秒走完 */

static int dev_scroll = 0;        /* 列表内容像素偏移 */
static int file_scroll = 0;
static int xf_scroll = 0;         /* 传输页文件列表内容偏移 */
static int dev_press_scroll = 0;  /* 本次拖动按下时的滚动位置 */
static int file_press_scroll = 0;
static int xf_press_scroll = 0;
static int recv_focus = 2;        /* 接收确认焦点：0=Reject 1=Setup 2=Accept */
static int rs_scroll = 0;         /* 接收设置页文件列表内容偏移 */
static int rs_press_scroll = 0;
static bool rename_pop = false;   /* 改名占位弹窗（重命名功能 TODO） */

/* 本次传输的文件列表（mock；将来换真实传输会话状态） */
static int    xf_count = 0;
static char   xf_name[MAX_PICKED][128];
static SceOff xf_size[MAX_PICKED];

static const char *dir_yes = "/"; /* 目录名后缀 */

static void files_load(void);
static void start_send(void);
static void start_recv(void);
static void goto_devices(void);
static void open_files(void);
static void sim_recv_request(void);

/* ---------- 接收请求（mock 样本） ---------- */
static const struct {
    const char *name;
    SceOff      size;
} sim_pool[] = {
    { "monthly_report.pdf", 12LL * 1024 * 1024 + 344 * 1024 },
    { "holiday_photos.zip", 480LL * 1024 * 1024 },
    { "app_logs.txt",       512 * 1024 },
    { "concert_songs.mp3",  8LL * 1024 * 1024 },
    { "notes.md",           2 * 1024 },
    { "backup.tar",         256LL * 1024 * 1024 },
};
#define SIM_POOL_COUNT ((int)(sizeof sim_pool / sizeof sim_pool[0]))

/* 当前勾选接收的文件数 */
static int recv_included(void)
{
    int i, n = 0;
    for (i = 0; i < g_app.inc_count && i < MAX_INF; i++)
        if (g_app.inc_files[i].inc) n++;
    return n;
}

/* 三角键：模拟"对方发来文件请求"。用当前选中的设备当发送方，
 * 文件从样本池里按设备错开取，数量 2..5，方便逐个测试。 */
static void sim_recv_request(void)
{
    int cnt = 2 + g_app.dev_sel % 4;
    int k;
    if (cnt > MAX_INF) cnt = MAX_INF;
    g_app.inc_count = cnt;
    for (k = 0; k < cnt; k++) {
        int src = (g_app.dev_sel + k * 2) % SIM_POOL_COUNT;
        snprintf(g_app.inc_files[k].name, sizeof g_app.inc_files[k].name,
                 "%s", sim_pool[src].name);
        g_app.inc_files[k].rname[0] = 0;
        g_app.inc_files[k].inc = true;
        g_app.inc_files[k].size = sim_pool[src].size;
    }
    strncpy(g_app.recv_alias, g_app.dev_alias[g_app.dev_sel],
            sizeof g_app.recv_alias - 1);
    g_app.recv_alias[sizeof g_app.recv_alias - 1] = 0;
    strncpy(g_app.recv_type, g_app.dev_type[g_app.dev_sel],
            sizeof g_app.recv_type - 1);
    g_app.recv_type[sizeof g_app.recv_type - 1] = 0;
    strcpy(g_app.recv_dir, "ux0:data/psvsend/");
    g_app.recv_cancel = false;
    g_app.inc_sel = 0;
    rs_scroll = 0;
    recv_focus = 2;
    g_app.page = PAGE_RECV_CONFIRM;
}

/* ---------- 布局/滚动小工具 ---------- */

/* 确认/返回键在当前布局下的提示名（随 X/O 布局即时变化） */
static const char *key_confirm(void) { return g_app.confirm_layout == 0 ? "X" : "O"; }
static const char *key_back(void)    { return g_app.confirm_layout == 0 ? "O" : "X"; }

/* 确认/返回键在当前布局下的图形图标 */
static HintIcon icon_confirm(void) { return g_app.confirm_layout == 0 ? HICON_CROSS : HICON_CIRCLE; }
static HintIcon icon_back(void)    { return g_app.confirm_layout == 0 ? HICON_CIRCLE : HICON_CROSS; }

static int list_max_scroll(int count)
{
    int m = count * ROW_STRIDE - LIST_VIEW_H;
    return m > 0 ? m : 0;
}

static void clamp_scroll(int *st, int count)
{
    int m = list_max_scroll(count);
    if (*st < 0) *st = 0;
    if (*st > m) *st = m;
}

/* 让选中行保持完整可见（键盘导航用；最小滚动量） */
static void keep_sel_visible(int *st, int count, int sel)
{
    int top = sel * ROW_STRIDE;
    int bot = top + ROW_H;
    if (top < *st) *st = top;
    if (bot > *st + LIST_VIEW_H) *st = bot - LIST_VIEW_H;
    clamp_scroll(st, count);
}

/* 触摸拖动：内容跟随手指；选中 = 手指压住的行（贴边时顺行移动） */
static void list_drag(const Input *in, int count, int *scroll, int *press, int *sel)
{
    if (in->drag_start) *press = *scroll;
    int ns = *press - in->drag_dy;      /* 手指下移(dy>0) → 内容下移 → 滚动减小 */
    clamp_scroll(&ns, count);
    *scroll = ns;
    int vis_top = *scroll / ROW_STRIDE;
    int vis_bot = (*scroll + LIST_VIEW_H) / ROW_STRIDE;
    int row = (in->drag_y - LIST_TOP + *scroll) / ROW_STRIDE;
    if (row < vis_top) row = vis_top;
    if (row > vis_bot) row = vis_bot;
    if (row < 0) row = 0;
    if (row >= count) row = count - 1;
    *sel = row;
}

/* 把一行注册成触摸区（可视区外部分裁掉，避免误命中头部/页脚） */
static void add_row_hit(int id, int top)
{
    Rect h = { 24, top, SCR_W - 48, ROW_H };
    if (h.y < LIST_TOP) { h.h -= LIST_TOP - h.y; h.y = LIST_TOP; }
    if (h.y + h.h > LIST_BOTTOM) h.h = LIST_BOTTOM - h.y;
    if (h.h > 0) w_add(id, h);
}

void pages_init(void)
{
    int i;
    for (i = 0; i < MOCK_COUNT; i++) {
        strncpy(g_app.dev_alias[i], mock_alias[i], sizeof g_app.dev_alias[0] - 1);
        strncpy(g_app.dev_type[i], mock_type[i], sizeof g_app.dev_type[0] - 1);
    }
    g_app.dev_count = MOCK_COUNT;
    g_app.dev_sel = 0;
    g_app.dev_target = 0;
    g_app.page = PAGE_DEVICES;
    g_app.theme_id = 0;
    g_app.confirm_layout = 0;
    g_app.prog_running = false;
    g_app.done = false;
    dev_scroll = 0;
    file_scroll = 0;
    theme_set(g_app.theme_id);
}

/* ================= 设备列表 ================= */
void page_devices_render(void)
{
    int count = g_app.dev_count;
    w_page_header("PSVSend");
    if (count > 0) {
        int i;
        clamp_scroll(&dev_scroll, count);
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(0, LIST_TOP, SCR_W, LIST_BOTTOM);
        for (i = dev_scroll / ROW_STRIDE; i < count; i++) {
            int top = LIST_TOP - dev_scroll + i * ROW_STRIDE;
            if (top >= LIST_BOTTOM) break;
            Rect r = { 24, top, SCR_W - 48, ROW_H };
            add_row_hit(i, top);
            w_row(r, g_app.dev_alias[i], g_app.dev_type[i], i == g_app.dev_sel);
        }
        vita2d_disable_clipping();
    }
    if (count * ROW_STRIDE + 24 <= LIST_VIEW_H) {
        char foot[96];
        snprintf(foot, sizeof foot, "%d device(s) - demo data", count);
        w_text(28, LIST_TOP + count * ROW_STRIDE + 8, 1.0f, theme->text_dim, "%s", foot);
    }
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = "Choose";
    segs[ns].icon = icon_confirm();   segs[ns++].text = "Send";
    segs[ns].icon = HICON_TRIANGLE;   segs[ns++].text = "Sim receive";
    segs[ns].icon = HICON_NONE;       segs[ns++].text = "SELECT Settings";
    w_page_footer_segs(segs, ns);
}

void page_devices_input(const Input *in)
{
    int count = g_app.dev_count;
    if (count > 0 && (in->drag_start || in->dragging)) {
        list_drag(in, count, &dev_scroll, &dev_press_scroll, &g_app.dev_sel);
        return;   /* 拖动期间不处理其它触摸动作 */
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id >= 0 && id < count) {
            g_app.dev_sel = id;
            g_app.dev_target = id;
            open_files();
        }
        return;
    }
    if (in->up && g_app.dev_sel > 0) {
        g_app.dev_sel--;
        keep_sel_visible(&dev_scroll, count, g_app.dev_sel);
    }
    if (in->down && g_app.dev_sel < count - 1) {
        g_app.dev_sel++;
        keep_sel_visible(&dev_scroll, count, g_app.dev_sel);
    }
    if (in->confirm) {
        g_app.dev_target = g_app.dev_sel;
        open_files();
    }
    if (in->alt) sim_recv_request();   /* 三角键：模拟"对方发来文件请求" */
    if (in->menu) {
        g_app.set_sel = 0;
        g_app.page = PAGE_SETTINGS;
    }
}

/* ================= 文件浏览 ================= */
static void files_load(void)
{
    SceUID fd = sceIoDopen(g_app.cur_dir);
    SceIoDirent de;
    int n = 0;
    g_app.file_count = 0;
    if (fd < 0) return;
    memset(&de, 0, sizeof de);
    while (n < MAX_FILES && sceIoDread(fd, &de) > 0) {
        if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0)
            continue;
        strncpy(g_app.files[n].name, de.d_name, sizeof g_app.files[n].name - 1);
        g_app.files[n].name[sizeof g_app.files[n].name - 1] = 0;
        g_app.files[n].is_dir = (de.d_stat.st_mode & SCE_S_IFDIR) != 0;
        g_app.files[n].size = de.d_stat.st_size;
        n++;
    }
    sceIoDclose(fd);
    g_app.file_count = n;
    /* 目录在前，名称升序 */
    int i, j;
    for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
            int ai = g_app.files[i].is_dir ? 0 : 1;
            int aj = g_app.files[j].is_dir ? 0 : 1;
            if (aj < ai ||
                (ai == aj && strcmp(g_app.files[j].name, g_app.files[i].name) < 0)) {
                FsEntry t = g_app.files[i];
                g_app.files[i] = g_app.files[j];
                g_app.files[j] = t;
            }
        }
    }
    if (g_app.file_sel >= g_app.file_count) g_app.file_sel = 0;
}

static bool is_picked(int idx)
{
    int i;
    for (i = 0; i < g_app.picked_count; i++)
        if (strcmp(g_app.picked[i].name, g_app.files[idx].name) == 0)
            return true;
    return false;
}

static void toggle_pick(int idx)
{
    int i;
    for (i = 0; i < g_app.picked_count; i++) {
        if (strcmp(g_app.picked[i].name, g_app.files[idx].name) == 0) {
            g_app.picked_total -= g_app.picked[i].size;
            g_app.picked[i] = g_app.picked[g_app.picked_count - 1];
            g_app.picked_count--;
            return;
        }
    }
    if (g_app.picked_count >= MAX_PICKED) return;
    strncpy(g_app.picked[g_app.picked_count].name, g_app.files[idx].name,
            sizeof g_app.picked[0].name - 1);
    if (g_app.cur_dir[strlen(g_app.cur_dir) - 1] == '/')
        snprintf(g_app.picked[g_app.picked_count].path, sizeof g_app.picked[0].path,
                 "%s%s", g_app.cur_dir, g_app.files[idx].name);
    else
        snprintf(g_app.picked[g_app.picked_count].path, sizeof g_app.picked[0].path,
                 "%s/%s", g_app.cur_dir, g_app.files[idx].name);
    g_app.picked[g_app.picked_count].size = g_app.files[idx].size;
    g_app.picked_total += g_app.files[idx].size;
    g_app.picked_count++;
}

static void enter_dir(const char *name)
{
    size_t len = strlen(g_app.cur_dir);
    if (len > 0 && g_app.cur_dir[len - 1] == '/')
        snprintf(g_app.cur_dir, sizeof g_app.cur_dir, "%s%s", g_app.cur_dir, name);
    else
        snprintf(g_app.cur_dir, sizeof g_app.cur_dir, "%s/%s", g_app.cur_dir, name);
    g_app.file_sel = 0;
    file_scroll = 0;
    files_load();
}

static void parent_dir(void)
{
    size_t len = strlen(g_app.cur_dir);
    if (len <= 5) {                 /* 已是 ux0:/ 根 */
        g_app.page = PAGE_DEVICES;
        return;
    }
    char *last = strrchr(g_app.cur_dir, '/');
    if (last && last != g_app.cur_dir) {
        *last = 0;
        if (strlen(g_app.cur_dir) < 5) strcpy(g_app.cur_dir, "ux0:/");
    } else {
        strcpy(g_app.cur_dir, "ux0:/");
    }
    g_app.file_sel = 0;
    file_scroll = 0;
    files_load();
}

void page_files_render(void)
{
    int count = g_app.file_count;
    char title[192];
    snprintf(title, sizeof title, "Send to %s", g_app.dev_alias[g_app.dev_target]);
    w_page_header(title);
    w_text_clip(28, 56, 1.0f, theme->text_dim, g_app.cur_dir, SCR_W - 56);
    if (count > 0) {
        int i;
        clamp_scroll(&file_scroll, count);
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(0, LIST_TOP, SCR_W, LIST_BOTTOM);
        for (i = file_scroll / ROW_STRIDE; i < count; i++) {
            int top = LIST_TOP - file_scroll + i * ROW_STRIDE;
            if (top >= LIST_BOTTOM) break;
            Rect r = { 24, top, SCR_W - 48, ROW_H };
            add_row_hit(i, top);
            char main[280], sub[32];
            if (g_app.files[i].is_dir)
                snprintf(main, sizeof main, "%s%s", g_app.files[i].name, dir_yes);
            else if (is_picked(i))
                snprintf(main, sizeof main, "* %s", g_app.files[i].name);
            else
                snprintf(main, sizeof main, "%s", g_app.files[i].name);
            if (g_app.files[i].is_dir)
                strcpy(sub, "folder");
            else
                w_human_size(g_app.files[i].size, sub);
            w_row(r, main, sub, i == g_app.file_sel);
        }
        vita2d_disable_clipping();
    }
    if (count == 0)
        w_text(28, LIST_TOP + 8, 1.1f, theme->text_dim, "(empty folder)");
    char send_lb[24];
    snprintf(send_lb, sizeof send_lb, "Send(%d)", g_app.picked_count);
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = "Choose";
    segs[ns].icon = icon_confirm();   segs[ns++].text = "Open/Pick";
    segs[ns].icon = icon_back();      segs[ns++].text = "Up";
    segs[ns].icon = HICON_TRIANGLE;   segs[ns++].text = send_lb;
    w_page_footer_segs(segs, ns);
    Rect send = { SCR_W - 216, SCR_H - 42, 192, 36 };
    if (g_app.picked_count > 0) {
        char lb[32];
        snprintf(lb, sizeof lb, "Send (%d)", g_app.picked_count);
        w_add(WID_FILES_SEND, send);
        w_button(send, lb, true);
    }
}

void page_files_input(const Input *in)
{
    int count = g_app.file_count;
    if (count > 0 && (in->drag_start || in->dragging)) {
        list_drag(in, count, &file_scroll, &file_press_scroll, &g_app.file_sel);
        return;   /* 拖动期间不处理其它触摸动作 */
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_FILES_SEND) {
            if (g_app.picked_count > 0) start_send();
        } else if (id >= 0 && id < count) {
            if (g_app.files[id].is_dir) enter_dir(g_app.files[id].name);
            else {
                g_app.file_sel = id;
                toggle_pick(id);
            }
        }
        return;
    }
    if (in->up && g_app.file_sel > 0) {
        g_app.file_sel--;
        keep_sel_visible(&file_scroll, count, g_app.file_sel);
    }
    if (in->down && g_app.file_sel < count - 1) {
        g_app.file_sel++;
        keep_sel_visible(&file_scroll, count, g_app.file_sel);
    }
    if (in->confirm && count > 0) {
        int s = g_app.file_sel;
        if (s >= 0 && s < count) {
            if (g_app.files[s].is_dir) enter_dir(g_app.files[s].name);
            else toggle_pick(s);
        }
    }
    if (in->alt && g_app.picked_count > 0) start_send();
    if (in->back) parent_dir();
}

/* ================= 发送确认 ================= */
static void open_files(void)
{
    strcpy(g_app.cur_dir, "ux0:/");
    w_clear_picked();
    g_app.file_sel = 0;
    file_scroll = 0;
    files_load();
    g_app.page = PAGE_FILES;
}

static void start_send(void)
{
    int i;
    xf_count = g_app.picked_count;
    for (i = 0; i < xf_count && i < MAX_PICKED; i++) {
        snprintf(xf_name[i], sizeof xf_name[0], "%s", g_app.picked[i].name);
        xf_size[i] = g_app.picked[i].size;
    }
    if (xf_count == 0) {                 /* 防御：无文件时给个占位 */
        xf_count = 1;
        strcpy(xf_name[0], "<no files>");
        xf_size[0] = 0;
    }
    if (g_app.picked_count > 0)
        strcpy(g_app.prog_name, g_app.picked[0].name);
    g_app.prog_size = g_app.picked_total;
    g_app.prog_dir = 0;
    xf_scroll = 0;
    g_app.prog_pct = 0;
    g_app.prog_done = false;
    g_app.prog_cancel = false;
    g_app.prog_info = false;
    g_app.prog_ms = 0;
    g_app.prog_running = true;
    g_app.prog_start = sceKernelGetSystemTimeWide();
    g_app.page = PAGE_PROGRESS;
}

void page_send_confirm_render(void)
{
    int i, n = g_app.picked_count;
    w_page_header("Confirm Send");
    char line[256];
    Rect card = { 24, 80, SCR_W - 48, 330 };
    w_rect(card, theme->card);
    snprintf(line, sizeof line, "Target: %s", g_app.dev_alias[g_app.dev_target]);
    w_text(48, 100, 1.2f, theme->text, "%s", line);
    char sz[16];
    w_human_size(g_app.picked_total, sz);
    snprintf(line, sizeof line, "%d file(s)  total %s", n, sz);
    w_text(48, 140, 1.0f, theme->text_dim, "%s", line);
    int shown = n < 8 ? n : 8;
    for (i = 0; i < shown; i++) {
        char m[256];
        if (g_app.picked[i].name[0])
            snprintf(m, sizeof m, "  %s", g_app.picked[i].name);
        else
            snprintf(m, sizeof m, "  <file %d>", i + 1);
        w_text_clip(48, 176 + i * 26, 1.0f, theme->text, m, card.w - 60);
    }
    if (n > shown)
        w_text(48, 176 + shown * 26, 1.0f, theme->text_dim, "  ... %d more", n - shown);
    Rect cancel = { SCR_W / 2 - 220, 444, 200, 48 };
    Rect ok     = { SCR_W / 2 + 20, 444, 200, 48 };
    w_add(0, cancel);
    w_add(1, ok);
    w_button(cancel, "Cancel", false);
    w_button(ok, "Send", true);
    HintSeg segs[4];
    int ns = 0;
    segs[ns].icon = icon_confirm();  segs[ns++].text = "Send";
    segs[ns].icon = icon_back();     segs[ns++].text = "Cancel";
    w_page_footer_segs(segs, ns);
}

void page_send_confirm_input(const Input *in)
{
    if (in->drag_start || in->dragging) return;
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == 1) start_send();
        else if (id == 0) g_app.page = PAGE_FILES;
        return;
    }
    if (in->confirm) start_send();
    if (in->back) g_app.page = PAGE_FILES;
}

/* ================= 接收请求确认（页面式：来者信息 + 三按钮焦点 + 发送方取消态） ================= */
static void recv_close(void)
{
    rename_pop = false;
    goto_devices();
}

/* 若 Accept 因没勾选任何文件而不可用，别把焦点停在它上面 */
static void recv_focus_sane(void)
{
    if (recv_focus == 2 && recv_included() == 0) recv_focus = 1;
}

void page_recv_confirm_render(void)
{
    w_page_header("Incoming files");
    char line[256];
    Rect card = { 24, 80, SCR_W - 48, 320 };

    if (g_app.recv_cancel) {
        /* 发送方主动取消：提示 + 单个关闭按钮 */
        w_rect(card, theme->card);
        w_text(48, 120, 1.5f, theme->text, "%s", g_app.recv_alias);
        w_text(48, 172, 1.25f, theme->danger,
               "The sender cancelled this request.");
        w_text(48, 216, 1.0f, theme->text_dim,
               "No files will be received.");
        Rect close = { SCR_W / 2 - 100, 444, 200, 48 };
        w_add(2, close);
        w_button(close, "Close", true);
        HintSeg segs[2];
        int ns = 0;
        segs[ns].icon = icon_confirm(); segs[ns++].text = "Close";
        w_page_footer_segs(segs, ns);
        return;
    }

    w_rect(card, theme->card);
    int n = g_app.inc_count, seln = recv_included();
    int i;
    /* 来者名字（右上是平台类型） */
    w_text(48, 100, 1.5f, theme->text, "%s", g_app.recv_alias);
    int tw = 0, th = 0;
    w_text_w(1.1f, g_app.recv_type, &tw, &th);
    w_text(card.x + card.w - tw - 24, 106, 1.1f, theme->text_dim, "%s", g_app.recv_type);

    snprintf(line, sizeof line, "%s wants to send you %d file(s).",
             g_app.recv_alias, n);
    w_text(48, 152, 1.15f, theme->text, "%s", line);

    SceOff t = 0;
    for (i = 0; i < n; i++) t += g_app.inc_files[i].size;
    char sz[16];
    w_human_size(t, sz);
    if (seln == n)
        snprintf(line, sizeof line, "Total %s", sz);
    else
        snprintf(line, sizeof line, "Total %s  (%d of %d selected)", sz, seln, n);
    w_text(48, 190, 1.0f, theme->text_dim, "%s", line);

    /* 文件预览（前几条） */
    int show = n < 5 ? n : 5;
    for (i = 0; i < show; i++) {
        char m[256];
        w_human_size(g_app.inc_files[i].size, sz);
        snprintf(m, sizeof m, "  %s  (%s)", g_app.inc_files[i].name, sz);
        w_text_clip(60, 240 + i * 26, 1.0f, theme->text, m, card.w - 140);
    }
    if (n > show)
        w_text(60, 240 + show * 26, 1.0f, theme->text_dim, "  ... %d more", n - show);

    /* 底部三个按钮：Reject / Setup / Accept */
    Rect rej = { 183, 444, 190, 48 };
    Rect set = { 385, 444, 190, 48 };
    Rect acc = { 587, 444, 190, 48 };
    w_add(0, rej);
    w_add(1, set);
    w_add(2, acc);
    if (recv_focus == 0) {
        w_rect(rej, theme->danger);
        int w = 0, h = 0;
        w_text_w(1.3f, "Reject", &w, &h);
        w_text(rej.x + (rej.w - w) / 2, rej.y + (rej.h - h) / 2, 1.3f,
               theme->accent_text, "Reject");
    } else {
        w_rect(rej, theme->card);
        w_rect_outline(rej, theme->border);
        int w = 0, h = 0;
        w_text_w(1.3f, "Reject", &w, &h);
        w_text(rej.x + (rej.w - w) / 2, rej.y + (rej.h - h) / 2, 1.3f,
               theme->text, "Reject");
    }
    w_button(set, "Setup", recv_focus == 1);
    if (seln > 0) {
        w_button(acc, "Accept", recv_focus == 2);
    } else {
        w_rect(acc, theme->card);
        w_rect_outline(acc, theme->border);
        int w = 0, h = 0;
        w_text_w(1.3f, "Accept", &w, &h);
        w_text(acc.x + (acc.w - w) / 2, acc.y + (acc.h - h) / 2, 1.3f,
               theme->text_dim, "Accept");
    }

    HintSeg segs[8];
    int ns = 0;
    segs[ns].icon = HICON_SQUARE;     segs[ns++].text = "Sim cancel";
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = "Switch";
    segs[ns].icon = icon_confirm();   segs[ns++].text = "OK";
    segs[ns].icon = icon_back();      segs[ns++].text = "Reject";
    w_page_footer_segs(segs, ns);
}

void page_recv_confirm_input(const Input *in)
{
    if (in->drag_start || in->dragging) return;
    if (in->square) {   /* 方块键：模拟发送方取消请求 */
        if (!g_app.recv_cancel) {
            g_app.recv_cancel = true;
            recv_focus = 2;
        }
        return;
    }
    if (g_app.recv_cancel) {
        if (in->tap || in->confirm || in->back) recv_close();
        return;
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == 2) {
            recv_focus = 2;
            if (recv_included() > 0) start_recv();
        } else if (id == 1) {
            recv_focus = 1;
            g_app.inc_sel = 0;
            rs_scroll = 0;
            rename_pop = false;
            g_app.page = PAGE_RECV_SETUP;
        } else if (id == 0) {
            recv_focus = 0;
            recv_close();
        }
        return;
    }
    if (in->left || in->right) {
        int d = in->right ? 1 : -1;
        int guard;
        for (guard = 0; guard < 3; guard++) {
            recv_focus += d;
            if (recv_focus < 0) recv_focus = 2;
            if (recv_focus > 2) recv_focus = 0;
            if (recv_focus != 2 || recv_included() > 0) break;  /* 跳过不可点的 Accept */
        }
    }
    if (in->confirm) {
        if (recv_focus == 2 && recv_included() > 0) {
            start_recv();
        } else if (recv_focus == 1) {
            g_app.inc_sel = 0;
            rs_scroll = 0;
            rename_pop = false;
            g_app.page = PAGE_RECV_SETUP;
        } else if (recv_focus == 0) {
            recv_close();
        }
        return;
    }
    if (in->back) recv_close();
}

static void start_recv(void)
{
    int i, n = 0;
    SceOff tot = 0;
    for (i = 0; i < g_app.inc_count && i < MAX_INF; i++) {
        if (!g_app.inc_files[i].inc) continue;
        if (n >= MAX_PICKED) break;
        snprintf(xf_name[n], sizeof xf_name[0], "%s", g_app.inc_files[i].name);
        xf_size[n] = g_app.inc_files[i].size;
        tot += g_app.inc_files[i].size;
        n++;
    }
    if (n == 0) return;               /* 全部被跳过则不应开始 */
    xf_count = n;
    xf_scroll = 0;
    strcpy(g_app.prog_name, xf_name[0]);
    g_app.prog_size = tot;
    g_app.prog_dir = 1;
    g_app.prog_pct = 0;
    g_app.prog_done = false;
    g_app.prog_cancel = false;
    g_app.prog_info = false;
    g_app.prog_ms = 0;
    g_app.prog_running = true;
    g_app.prog_start = sceKernelGetSystemTimeWide();
    rename_pop = false;
    g_app.page = PAGE_PROGRESS;
}

/* ================= 接收设置页（本次保存目录 + 逐文件勾选/改名占位） ================= */
#define RSS_TOP     68
#define RSS_BOTTOM  (SCR_H - 46 - 14)  /* 页脚上方留空 */
#define RSS_VIEW    (RSS_BOTTOM - RSS_TOP)
#define RSS_ROW_H   56
#define RSS_STRIDE  60
#define RS_REN_ID   0x4000             /* 触摸 id：改名按钮 = 基址 + 行号 */
#define RS_CHK_ID   0x8000             /* 触摸 id：勾选框  = 基址 + 行号 */

static void rs_clamp(void)
{
    int rows = 1 + g_app.inc_count;
    int max_s = rows * RSS_STRIDE - RSS_VIEW;
    if (max_s < 0) max_s = 0;
    if (rs_scroll < 0) rs_scroll = 0;
    if (rs_scroll > max_s) rs_scroll = max_s;
}

static void rs_keep_visible(void)
{
    int rows = 1 + g_app.inc_count;
    int st = g_app.inc_sel * RSS_STRIDE;
    int bot = st + RSS_ROW_H;
    if (st < rs_scroll) rs_scroll = st;
    if (bot > rs_scroll + RSS_VIEW) rs_scroll = bot - RSS_VIEW;
    rs_clamp();
}

/* 加粗短线段（两遍略偏移，约 2px） */
static void line2(float x0, float y0, float x1, float y1, uint32_t c)
{
    vita2d_draw_line(x0, y0, x1, y1, c);
    vita2d_draw_line(x0 + 0.8f, y0, x1 + 0.8f, y1, c);
}

static void w_checkbox(Rect b, bool on)
{
    w_rect(b, theme->card);   /* 擦掉行底色再画，避免透出选中效果 */
    w_rect_outline(b, on ? theme->accent : theme->border);
    if (on) {
        w_rect((Rect){ b.x + 3, b.y + 3, b.w - 6, b.h - 6 }, theme->accent);
        line2(b.x + b.w * 0.24f, b.y + b.h * 0.55f,
              b.x + b.w * 0.43f, b.y + b.h * 0.74f, theme->accent_text);
        line2(b.x + b.w * 0.43f, b.y + b.h * 0.74f,
              b.x + b.w * 0.80f, b.y + b.h * 0.26f, theme->accent_text);
    }
}

void page_recv_setup_render(void)
{
    w_page_header("Receive setup");
    int rows = 1 + g_app.inc_count;
    char line[256], sz[16];
    rs_clamp();
    int i;

    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, RSS_TOP, SCR_W, RSS_BOTTOM);
    for (i = rs_scroll / RSS_STRIDE; i < rows; i++) {
        int top = RSS_TOP + i * RSS_STRIDE - rs_scroll;
        if (top >= RSS_BOTTOM) break;
        Rect r = { 24, top, SCR_W - 48, RSS_ROW_H };
        bool sel = (i == g_app.inc_sel);
        w_rect(r, theme->card);
        if (sel) w_rect((Rect){ 24, top, 4, RSS_ROW_H }, theme->accent);
        w_add(i, r);

        if (i == 0) {
            /* 保存目录行：只读默认值；真实目录选择 TODO */
            w_text(60, top + 8, 1.0f, theme->text_dim, "Save to");
            w_text_clip(60, top + 30, 1.1f, sel ? theme->text : theme->text_dim,
                        g_app.recv_dir, 700);
            int tw = 0, th = 0;
            w_text_w(1.0f, "default", &tw, &th);
            w_text(r.x + r.w - tw - 24, top + 16, 1.0f, theme->text_dim,
                   "%s", "default");
            continue;
        }

        int idx = i - 1;
        bool on = g_app.inc_files[idx].inc;
        uint32_t nc = sel ? theme->text : theme->text_dim;
        w_human_size(g_app.inc_files[idx].size, sz);
        int th = 0;
        w_text_w(1.0f, g_app.inc_files[idx].name, NULL, &th);
        w_text_clip(60, top + (RSS_ROW_H - th) / 2 - 2, 1.0f, nc,
                    g_app.inc_files[idx].name, 480);
        int sw = 0, sh = 0;
        w_text_w(1.0f, sz, &sw, &sh);
        w_text(696 - sw, top + (RSS_ROW_H - sh) / 2 - 2, 1.0f,
               theme->text_dim, "%s", sz);
        Rect ren = { 700, top + 9, 108, 38 };
        Rect chk = { 824, top + 9, 38, 38 };
        w_add(RS_REN_ID + i, ren);
        w_add(RS_CHK_ID + i, chk);
        w_rect_outline(ren, theme->border);
        int w2 = 0, h2 = 0;
        w_text_w(1.0f, "Rename", &w2, &h2);
        w_text(ren.x + (ren.w - w2) / 2, ren.y + (ren.h - h2) / 2, 1.0f,
               theme->text_dim, "Rename");
        w_checkbox(chk, on);
    }
    vita2d_disable_clipping();

    /* 内容超长时的细滚动条 */
    {
        int max_s = rows * RSS_STRIDE - RSS_VIEW;
        if (max_s > 0) {
            int bh = RSS_VIEW * RSS_VIEW / (rows * RSS_STRIDE);
            if (bh < 24) bh = 24;
            int by = RSS_TOP + (RSS_VIEW - bh) * rs_scroll / max_s;
            w_rect((Rect){ 936, RSS_TOP, 4, RSS_VIEW }, theme->card);
            w_rect((Rect){ 936, by, 4, bh }, theme->text_dim);
        }
    }

    HintSeg segs[8];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = "Choose";
    segs[ns].icon = icon_confirm();   segs[ns++].text = "Toggle";
    segs[ns].icon = HICON_TRIANGLE;   segs[ns++].text = "Rename";
    segs[ns].icon = icon_back();      segs[ns++].text = "Back";
    w_page_footer_segs(segs, ns);

    /* 改名占位弹窗 */
    if (rename_pop) {
        Rect card = w_modal_box(230);
        int idx = g_app.inc_sel - 1;
        snprintf(line, sizeof line, "%s",
                 (idx >= 0 && idx < g_app.inc_count)
                     ? g_app.inc_files[idx].name : "");
        w_text(card.x + 40, card.y + 28, 1.3f, theme->text, "Rename file");
        w_text_clip(card.x + 40, card.y + 76, 1.0f, theme->text_dim,
                    line, card.w - 80);
        w_text(card.x + 40, card.y + 124, 1.0f, theme->text_dim,
               "Editing names is not available yet.");
        w_text(card.x + 40, card.y + 158, 1.0f, theme->text_dim,
               "TODO: system keyboard / built-in input (see design doc).");
    }
}

void page_recv_setup_input(const Input *in)
{
    int rows = 1 + g_app.inc_count;
    if (rename_pop) {
        /* 改名占位弹窗：任意键/点击关闭 */
        if (in->tap || in->confirm || in->back || in->alt ||
            in->up || in->down || in->left || in->right ||
            in->drag_start || in->dragging)
            rename_pop = false;
        return;
    }
    if (in->drag_start || in->dragging) {
        int max_s = rows * RSS_STRIDE - RSS_VIEW;
        if (max_s < 0) max_s = 0;
        if (in->drag_start) rs_press_scroll = rs_scroll;
        int ns = rs_press_scroll - in->drag_dy;
        if (ns < 0) ns = 0;
        if (ns > max_s) ns = max_s;
        rs_scroll = ns;
        int vis_top = rs_scroll / RSS_STRIDE;
        int vis_bot = (rs_scroll + RSS_VIEW) / RSS_STRIDE;
        int row = (in->drag_y - RSS_TOP + rs_scroll) / RSS_STRIDE;
        if (row < vis_top) row = vis_top;
        if (row > vis_bot) row = vis_bot;
        if (row < 0) row = 0;
        if (row > rows - 1) row = rows - 1;
        g_app.inc_sel = row;
        return;
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id >= RS_REN_ID && id < RS_REN_ID + rows) {
            int row = id - RS_REN_ID;
            g_app.inc_sel = row;
            if (row > 0) rename_pop = true;
            return;
        }
        if (id >= RS_CHK_ID && id < RS_CHK_ID + rows) {
            int row = id - RS_CHK_ID;
            g_app.inc_sel = row;
            if (row > 0) {
                int fi = row - 1;
                g_app.inc_files[fi].inc = !g_app.inc_files[fi].inc;
            }
            return;
        }
        if (id >= 0 && id < rows) g_app.inc_sel = id;
        return;
    }
    if (in->up && g_app.inc_sel > 0) {
        g_app.inc_sel--;
        rs_keep_visible();
    }
    if (in->down && g_app.inc_sel < rows - 1) {
        g_app.inc_sel++;
        rs_keep_visible();
    }
    if (in->confirm) {
        if (g_app.inc_sel > 0) {
            int fi = g_app.inc_sel - 1;
            g_app.inc_files[fi].inc = !g_app.inc_files[fi].inc;
        }
    }
    if (in->alt) {
        if (g_app.inc_sel > 0) rename_pop = true;
    }
    if (in->back) {
        rename_pop = false;
        recv_focus_sane();
        g_app.page = PAGE_RECV_CONFIRM;   /* 临时设置已生效，返回接收页 */
    }
}

/* ================= 进度（模拟，LocalSend 式：文件列表 + 总进度 + 高级面板） ================= */
static void goto_devices(void)
{
    w_clear_picked();
    rename_pop = false;
    g_app.prog_running = false;
    g_app.dev_sel = g_app.dev_target;
    g_app.page = PAGE_DEVICES;
}

/* 取消传输：不退出页面，停在"已取消"态（按钮变 Done） */
static void cancel_transfer(void)
{
    g_app.prog_running = false;
    g_app.prog_cancel = true;
    g_app.prog_done = false;
    g_app.prog_ms = (int)((sceKernelGetSystemTimeWide() - g_app.prog_start) / 1000);
}

void page_progress_render(void)
{
    int i;
    /* 总进度（mock）：按时间推进 */
    if (g_app.prog_running) {
        uint64_t now = sceKernelGetSystemTimeWide();
        g_app.prog_ms = (int)((now - g_app.prog_start) / 1000);
        int pct = (int)((uint64_t)g_app.prog_ms * 100 / PROG_TOTAL_MS);
        if (pct >= 100) {
            pct = 100;
            g_app.prog_done = true;
            g_app.prog_running = false;
        }
        g_app.prog_pct = pct;
    }

    /* 由总进度推导每个文件的完成情况 */
    SceOff done[MAX_PICKED];
    SceOff total = 0;
    int fin = 0, active = 0;
    for (i = 0; i < xf_count; i++) total += xf_size[i];
    {
        SceOff rem = total * (SceOff)g_app.prog_pct / 100;
        for (i = 0; i < xf_count; i++) {
            if (xf_size[i] == 0) { done[i] = 0; continue; }   /* 空文件视为瞬间完成 */
            if (rem >= xf_size[i])      { done[i] = xf_size[i]; rem -= xf_size[i]; }
            else if (rem > 0)           { done[i] = rem; rem = 0; }
            else                         done[i] = 0;
        }
    }
    for (i = 0; i < xf_count; i++)
        if (xf_size[i] == 0 || done[i] >= xf_size[i]) fin++;
    active = xf_count - 1;
    for (i = 0; i < xf_count; i++)
        if (xf_size[i] > 0 && done[i] < xf_size[i]) { active = i; break; }

    /* 列表可视区滚动夹紧 */
    {
        int max_s = xf_count * XF_ROW_H - XF_VIEW_H;
        if (max_s < 0) max_s = 0;
        if (xf_scroll < 0) xf_scroll = 0;
        if (xf_scroll > max_s) xf_scroll = max_s;
    }

    w_page_header(g_app.prog_dir == 0 ? "Sending" : "Receiving");

    /* 每文件一行：名称 + 百分比 + 细进度条（内容随 xf_scroll 像素滚动） */
    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, XF_TOP, SCR_W, XF_BOTTOM);
    for (i = xf_scroll / XF_ROW_H; i < xf_count; i++) {
        int top = XF_TOP + i * XF_ROW_H - xf_scroll;
        if (top >= XF_BOTTOM) break;
        bool is_active = (i == active) && g_app.prog_running;
        int fp = xf_size[i] > 0 ? (int)(done[i] * 100 / xf_size[i]) : 100;
        if (fp > 100) fp = 100;
        char pctlb[16];
        snprintf(pctlb, sizeof pctlb, "%d%%", fp);
        uint32_t nc = is_active ? theme->text : theme->text_dim;
        int tw = 0, th = 0;
        w_text_w(1.1f, pctlb, &tw, &th);
        w_text(920 - tw, top + 2, 1.1f, nc, "%s", pctlb);
        w_text_clip(40, top + 2, 1.1f, nc, xf_name[i], 650);
        w_bar((Rect){ 40, top + 30, 660, 10 }, theme->card, theme->accent, fp);
    }
    vita2d_disable_clipping();

    /* 内容超长时右侧细滚动条 */
    {
        int max_s = xf_count * XF_ROW_H - XF_VIEW_H;
        if (max_s > 0) {
            int bh = XF_VIEW_H * XF_VIEW_H / (xf_count * XF_ROW_H);
            if (bh < 24) bh = 24;
            int by = XF_TOP + (XF_VIEW_H - bh) * xf_scroll / max_s;
            w_rect((Rect){ 936, XF_TOP, 4, XF_VIEW_H }, theme->card);
            w_rect((Rect){ 936, by, 4, bh }, theme->text_dim);
        }
    }

    /* 总进度条与状态 */
    char ov[48];
    snprintf(ov, sizeof ov, "Total   %d%%", g_app.prog_pct);
    w_text(40, 376, 1.15f, theme->text, "%s", ov);
    if (g_app.prog_done || g_app.prog_cancel) {
        const char *st = g_app.prog_done ? "Complete" : "Cancelled";
        uint32_t sc = g_app.prog_done ? theme->success : theme->danger;
        int tw = 0, th = 0;
        w_text_w(1.15f, st, &tw, &th);
        w_text(920 - tw, 376, 1.15f, sc, "%s", st);
    }
    w_bar((Rect){ 40, 400, 880, 16 }, theme->card, theme->accent, g_app.prog_pct);

    /* 高级面板：文件计数 / 耗时 / 速度（十进制 MB，1 MB = 1,000,000 B） */
    if (g_app.prog_info) {
        double sec = g_app.prog_ms / 1000.0;
        double mb = (double)(total * (SceOff)g_app.prog_pct / 100) / 1000000.0;
        double speed = sec > 0.05 ? mb / sec : 0.0;
        char st[160];
        snprintf(st, sizeof st, "Files %d/%d    Elapsed %d:%02d    Speed %.2f MB/s",
                 fin, xf_count, g_app.prog_ms / 60000,
                 (g_app.prog_ms / 1000) % 60, speed);
        w_text(40, 436, 1.0f, theme->text_dim, "%s", st);
    }

    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = icon_confirm();  segs[ns++].text = g_app.prog_running ? "Cancel" : "Done";
    segs[ns].icon = HICON_TRIANGLE;  segs[ns++].text = "Advanced";
    w_page_footer_segs(segs, ns);

    /* 底部右侧按钮：右边=取消/完成，其左=高级 */
    Rect main = { SCR_W - 24 - 160, SCR_H - 42, 160, 36 };
    Rect adv  = { main.x - 12 - 140, SCR_H - 42, 140, 36 };
    w_add(0, adv);
    w_add(1, main);
    w_button(adv, "Advanced", g_app.prog_info);
    if (g_app.prog_running) {
        w_rect(main, theme->danger);
        int tw = 0, th = 0;
        w_text_w(1.3f, "Cancel", &tw, &th);
        w_text(main.x + (main.w - tw) / 2, main.y + (main.h - th) / 2, 1.3f,
               theme->accent_text, "Cancel");
    } else {
        w_button(main, "Done", true);
    }
}

void page_progress_input(const Input *in)
{
    /* 文件列表滚动（拖动跟手 / 方向键逐行） */
    if (in->drag_start || in->dragging) {
        int max_s = xf_count * XF_ROW_H - XF_VIEW_H;
        if (max_s < 0) max_s = 0;
        if (in->drag_start) xf_press_scroll = xf_scroll;
        int ns = xf_press_scroll - in->drag_dy;
        if (ns < 0) ns = 0;
        if (ns > max_s) ns = max_s;
        xf_scroll = ns;
        return;
    }
    if (in->up || in->down) {
        int max_s = xf_count * XF_ROW_H - XF_VIEW_H;
        if (max_s < 0) max_s = 0;
        xf_scroll += in->up ? -XF_ROW_H : XF_ROW_H;
        if (xf_scroll < 0) xf_scroll = 0;
        if (xf_scroll > max_s) xf_scroll = max_s;
        return;
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == 1) {   /* 主按钮：传输中取消，否则退出 */
            if (g_app.prog_running) cancel_transfer();
            else goto_devices();
        } else if (id == 0) {
            g_app.prog_info = !g_app.prog_info;
        }
        return;
    }
    if (in->confirm || in->back) {
        if (g_app.prog_running) cancel_transfer();
        else goto_devices();
        return;
    }
    if (in->alt) g_app.prog_info = !g_app.prog_info;
}

/* ================= 设置 ================= */
void page_settings_render(void)
{
    w_page_header("Settings");
    char layout_v[64];
    const char *vals[2];
    vals[0] = theme_names[g_app.theme_id];
    snprintf(layout_v, sizeof layout_v, "%s confirm / %s back (%s)",
             key_confirm(), key_back(),
             g_app.confirm_layout == 0 ? "US" : "JP");
    vals[1] = layout_v;
    const char *labels[2] = { "Theme", "Confirm key" };
    int i;
    for (i = 0; i < 2; i++) {
        Rect r = { 24, 100 + i * 78, SCR_W - 48, 62 };
        Rect rl = { r.x, r.y, r.w / 2, r.h };
        Rect rr = { r.x + r.w / 2, r.y, r.w - r.w / 2, r.h };
        w_add(i * 2, rl);          /* 左半 = 上一个 */
        w_add(i * 2 + 1, rr);      /* 右半 = 下一个 */
        w_row(r, labels[i], vals[i], i == g_app.set_sel);
    }
    w_text(28, 100 + 2 * 78 + 10, 1.0f, theme->text_dim,
           "LEFT / tap left half: prev   RIGHT / tap right half: next");
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;      segs[ns++].text = "Choose";
    segs[ns].icon = icon_confirm();  segs[ns++].text = "Change";
    segs[ns].icon = icon_back();     segs[ns++].text = "Back";
    w_page_footer_segs(segs, ns);
}

/* 修改设置项：row=0 主题循环切换，row=1 键位布局翻转；dir=±1 */
static void settings_change(int row, int dir)
{
    if (row == 0) {
        g_app.theme_id += dir;
        if (g_app.theme_id < 0) g_app.theme_id = THEME_COUNT - 1;
        if (g_app.theme_id >= THEME_COUNT) g_app.theme_id = 0;
        theme_set(g_app.theme_id);
    } else if (row == 1) {
        g_app.confirm_layout = g_app.confirm_layout ? 0 : 1;
    }
}

void page_settings_input(const Input *in)
{
    if (in->drag_start || in->dragging) return;
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id >= 0) {
            g_app.set_sel = id / 2;
            settings_change(g_app.set_sel, (id & 1) ? 1 : -1);
        }
        return;
    }
    if (in->up && g_app.set_sel > 0) g_app.set_sel--;
    if (in->down && g_app.set_sel < 1) g_app.set_sel++;
    if (in->left) settings_change(g_app.set_sel, -1);
    if (in->right || in->confirm) settings_change(g_app.set_sel, 1);
    if (in->back) g_app.page = PAGE_DEVICES;
}
