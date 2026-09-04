/* 页面：设备列表 / 文件浏览 / 发送确认 / 接收确认 / 进度 / 设置
 * 数据：设备列表来自后端 UDP 发现（api.h 快照）；发送走 xfer 真实传输；
 * 接收请求/进度来自 receive 后端会话（receive.h 快照接口）。
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
#include "../config.h"
#include "../net.h"
#include "../api.h"
#include "../transfer.h"
#include "../receive.h"
#include "../scan.h"

/* ---------- 布局常量 ---------- */
#define LIST_TOP     76                 /* 列表可视区顶 */
#define LIST_BOTTOM  (SCR_H - 46 - 6)   /* 列表可视区底（页脚上方留白） */
#define LIST_VIEW_H  (LIST_BOTTOM - LIST_TOP)
#define ROW_H        56
#define ROW_STRIDE   62
#define WID_FILES_SEND 0x8001

/* 传输页：文件列表区域与总进度布局（发送/接收共用） */
#define XF_TOP        68
#define XF_BOTTOM     370      /* 文件列表可视区底（总进度条上方） */
#define XF_VIEW_H     (XF_BOTTOM - XF_TOP)
#define XF_ROW_H      58

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
static bool host_pop = false;     /* 设置页主机名改名的占位弹窗（文字输入 TODO） */

/* 进度页当前文件清单：发送时每帧由 xfer 快照覆盖；接收时会话快照，
 * 会话还没建好的头几帧用这里的本地清单兜底 */
static int    xf_count = 0;
static char   xf_name[MAX_PICKED][128];
static SceOff xf_size[MAX_PICKED];

static const char *dir_yes = "/"; /* 目录名后缀 */

static void files_load(void);
static void start_send(void);
static void start_recv(void);
static void goto_devices(void);
static void open_files(void);

/* 当前勾选接收的文件数 */
static int recv_included(void)
{
    int i, n = 0;
    for (i = 0; i < g_app.inc_count && i < MAX_INF; i++)
        if (g_app.inc_files[i].inc) n++;
    return n;
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
    config_init();                       /* 读配置：主题/键位布局/设备名等 */
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
    dev_scroll = 0;
    file_scroll = 0;
    theme_set(g_app.theme_id);
    api_start();                         /* 网络底座：net + UDP 发现线程 */
}

/* ================= 设备列表 ================= */

/* 每帧渲染前从后端发现表拷一份快照到 g_app（锁内拷贝，无竞态）。
 * 设备增删/超时消失都由发现线程更新，这里只做显示用的镜像。 */
static void dev_sync(void)
{
    Device tmp[API_MAX_DEVICES];
    int n = api_device_snapshot(tmp, API_MAX_DEVICES);
    int i;
    if (n > MAX_DEVICES) n = MAX_DEVICES;
    for (i = 0; i < n; i++) {
        snprintf(g_app.dev_alias[i], sizeof g_app.dev_alias[0], "%s",
                 tmp[i].alias[0] ? tmp[i].alias : tmp[i].ip);
        if (tmp[i].model[0] && strlen(tmp[i].model) < 28)
            snprintf(g_app.dev_sub[i], sizeof g_app.dev_sub[0], "%s", tmp[i].model);
        else if (tmp[i].dtype[0])
            snprintf(g_app.dev_sub[i], sizeof g_app.dev_sub[0], "%s", tmp[i].dtype);
        else
            g_app.dev_sub[i][0] = 0;
        snprintf(g_app.dev_kind[i], sizeof g_app.dev_kind[0], "%s", tmp[i].dtype);
        snprintf(g_app.dev_ip[i], sizeof g_app.dev_ip[0], "%s", tmp[i].ip);
        g_app.dev_port[i] = tmp[i].port;
        snprintf(g_app.dev_proto[i], sizeof g_app.dev_proto[0], "%s",
                 tmp[i].protocol[0] ? tmp[i].protocol : "http");
        snprintf(g_app.dev_fp[i], sizeof g_app.dev_fp[0], "%s", tmp[i].fingerprint);
    }
    g_app.dev_count = n;
    if (n == 0) g_app.dev_sel = 0;
    else if (g_app.dev_sel >= n) g_app.dev_sel = n - 1;
}

void page_devices_render(void)
{
    int count;
    dev_sync();
    count = g_app.dev_count;
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
            w_row(r, g_app.dev_alias[i], g_app.dev_sub[i], i == g_app.dev_sel);
        }
        vita2d_disable_clipping();
    } else {
        if (!api_network_ready()) {
            const char *why = api_discovery_fail();
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "Network not ready.");
            if (why[0]) {
                w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim, why);
            } else {
                w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim,
                       "Waiting for Wi-Fi to come back up...");
            }
        } else if (!net_connected()) {
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "Wi-Fi link is down.");
        } else {
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "No devices found.");
            if (scan_active()) {
                char st[96];
                snprintf(st, sizeof st, "Scanning... %d/%d hosts",
                         scan_done(), scan_total() > 0 ? scan_total() : 254);
                w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim, st);
            } else {
                w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim,
                       "Press Triangle to scan this network.");
            }
        }
    }
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = "Choose";
    segs[ns].icon = icon_confirm();   segs[ns++].text = "Send";
    segs[ns].icon = HICON_TRIANGLE;   segs[ns++].text = "Scan";
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
    if (in->alt) {
        scan_trigger();          /* 三角键：手动扫一遍网段 */
    }
    if (in->confirm) {
        g_app.dev_target = g_app.dev_sel;
        open_files();
    }
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

/* 发起真实发送：目标取自已同步的设备镜像（dev_target），文件取勾选列表。
 * 发送在 xfer 后台线程执行；进度页每帧从 xfer 拷快照渲染。 */
static void start_send(void)
{
    XferFile ff[XFER_MAX_FILES];
    int i, n = 0;
    if (g_app.picked_count <= 0) return;
    for (i = 0; i < g_app.picked_count && n < XFER_MAX_FILES; i++) {
        snprintf(ff[n].name, sizeof ff[n].name, "%s", g_app.picked[i].name);
        snprintf(ff[n].path, sizeof ff[n].path, "%s", g_app.picked[i].path);
        ff[n].size = g_app.picked[i].size;
        n++;
    }
    g_app.prog_dir = 0;
    xf_scroll = 0;
    g_app.prog_pct = 0;
    g_app.prog_done = false;
    g_app.prog_cancel = false;
    g_app.prog_info = false;
    g_app.prog_ms = 0;
    g_app.prog_running = true;
    g_app.prog_start = sceKernelGetSystemTimeWide();
    xf_count = n;
    if (g_app.dev_target >= 0 && g_app.dev_target < g_app.dev_count &&
        g_app.dev_ip[g_app.dev_target][0]) {
        int t = g_app.dev_target;
        xfer_start(g_app.dev_ip[t], g_app.dev_port[t],
                   g_app.dev_proto[t], g_app.dev_fp[t], ff, n);
    } else {
        /* 目标失效（如设备刚离线）：xfer_start 会把失败原因写进快照，进度页显示 */
        XferFile dummy;
        memset(&dummy, 0, sizeof dummy);
        xfer_start("", 0, "http", "", &dummy, 1);
    }
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

/* ================= 接收请求确认（真实：后端 PENDING 会话） ================= */
static bool g_recv_expired = false;  /* 请求 60s 没人响应、已被后端作废 */
static uint64_t g_recv_supp_ms = 0;  /* 决定后抑制重弹到：毫秒时间戳 */

/* 拒绝当前请求并离开接收流程（后端回 403，发送方会看到拒绝） */
static void recv_reject_close(void)
{
    recv_clear();                    /* PENDING → 唤醒 http 线程按拒绝处理 */
    g_recv_supp_ms = (uint64_t)sceKernelGetSystemTimeWide() / 1000 + 2000;
    rename_pop = false;
    goto_devices();
}

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

    /* 请求还在等 UI 决定吗？不在了（超时被后端作废）→ 只给关闭，不再给决定按钮 */
    g_recv_expired = recv_pending_pull(NULL) == 0;
    if (g_recv_expired) {
        w_rect(card, theme->card);
        w_text(48, 120, 1.5f, theme->text, "%s", g_app.recv_alias);
        w_text(48, 172, 1.25f, theme->text_dim,
               "This request has expired.");
        w_text(48, 216, 1.0f, theme->text_dim,
               "No response was sent to the sender.");
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

    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = "Switch";
    segs[ns].icon = icon_confirm();   segs[ns++].text = "Accept";
    segs[ns].icon = icon_back();      segs[ns++].text = "Reject";
    w_page_footer_segs(segs, ns);
}

void page_recv_confirm_input(const Input *in)
{
    if (in->drag_start || in->dragging) return;
    if (g_recv_expired) {           /* 请求已过期：任意键/点击关闭 */
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
            recv_reject_close();
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
            recv_reject_close();
        }
        return;
    }
    if (in->back) recv_reject_close();
}

/* 接受：把勾选集写回后端并请其建立会话（回执 200 后对方即可开传），
 * 跳接收进度页。此时会话可能还没建好，进度页首帧用本地清单兜底。 */
static void start_recv(void)
{
    bool inc[RECV_MAX_FILES];
    int i, n = 0;
    SceOff tot = 0;
    if (recv_pending_pull(NULL) != 1) {   /* 已过期（竞态防护，正常不会到） */
        recv_close();
        return;
    }
    for (i = 0; i < g_app.inc_count && i < RECV_MAX_FILES; i++)
        inc[i] = g_app.inc_files[i].inc;
    recv_set_include(inc);
    for (i = 0; i < g_app.inc_count && i < RECV_MAX_FILES; i++) {
        if (!g_app.inc_files[i].inc) continue;
        if (n >= MAX_PICKED) break;
        snprintf(xf_name[n], sizeof xf_name[0], "%s", g_app.inc_files[i].name);
        xf_size[n] = g_app.inc_files[i].size;
        tot += g_app.inc_files[i].size;
        n++;
    }
    if (n == 0) { recv_reject_close(); return; }   /* 全被勾掉：当拒绝 */
    xf_count = n;
    xf_scroll = 0;
    g_app.prog_name[0] = 0;
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
    recv_decide(true);               /* 唤醒后端：组会话、回 200 给发送方 */
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

/* ================= 进度（LocalSend 式：文件列表 + 总进度 + 高级面板） ================= */
static void goto_devices(void)
{
    w_clear_picked();
    rename_pop = false;
    g_app.prog_running = false;
    g_app.dev_sel = g_app.dev_target;
    g_app.page = PAGE_DEVICES;
}

void page_progress_render(void)
{
    int i;
    SceOff done[MAX_PICKED];
    SceOff total = 0;
    int fin = 0, active = -1;
    bool fail_state = false;
    XferInfo xv;
    const char *msg = NULL;
    uint32_t msg_col = theme->text_dim;

    if (g_app.prog_dir == 0) {
        /* 发送路径（真实）：从 xfer 模块拷快照映射到行显示数组 */
        xfer_info(&xv);
        xf_count = xv.count > MAX_PICKED ? MAX_PICKED : xv.count;
        for (i = 0; i < xf_count; i++) {
            snprintf(xf_name[i], sizeof xf_name[0], "%s",
                     xv.f[i].name[0] ? xv.f[i].name : "?");
            xf_size[i] = xv.f[i].size;
            done[i] = xv.f[i].sent < xv.f[i].size ? xv.f[i].sent : xv.f[i].size;
            if (xf_size[i] == 0 || done[i] >= xf_size[i]) fin++;
            else if (active < 0) active = i;
        }
        if (xv.active)
            g_app.prog_ms = (int)((sceKernelGetSystemTimeWide() - xv.start_us) / 1000);
        g_app.prog_pct = xv.total > 0
            ? (int)(xv.total_sent * 100 / xv.total)
            : (xv.finished && xv.ok ? 100 : 0);
        if (g_app.prog_pct > 100) g_app.prog_pct = 100;
        g_app.prog_running = xv.active;
        g_app.prog_done = xv.finished && xv.ok;
        g_app.prog_cancel = xv.cancelled;
        fail_state = xv.finished && !xv.ok && !xv.cancelled;
        if (fail_state) {
            msg = xv.err[0] ? xv.err : "Transfer failed";
            msg_col = theme->danger;
        } else if (xv.active && xv.total_sent == 0 && xv.cur < 0) {
            msg = xv.msg[0] ? xv.msg : "Waiting for receiver to accept...";
        } else if (xv.active && xv.cur >= 0) {
            msg = xv.msg[0] ? xv.msg : "Sending...";
        }
        for (i = 0; i < xf_count; i++) total += xf_size[i];
    } else {
        /* 接收路径（真实）：每帧从 receive 模块拷会话快照；会话还没建好
         * （Accept 刚点、http 线程未醒的几帧）用 start_recv 捕获的清单兜底。 */
        RecvStatus rs;
        memset(&rs, 0, sizeof rs);
        bool has = recv_status_pull(&rs) == 1;
        if (has) {
            xf_count = rs.count > MAX_PICKED ? MAX_PICKED : rs.count;
            for (i = 0; i < xf_count; i++) {
                snprintf(xf_name[i], sizeof xf_name[0], "%s",
                         rs.name[i][0] ? rs.name[i] : "?");
                xf_size[i] = rs.size[i];
                done[i] = rs.got[i] < rs.size[i] ? rs.got[i] : rs.size[i];
                if (xf_size[i] == 0 || done[i] >= xf_size[i]) fin++;
            }
            active = rs.cur;                       /* 正在收的文件（-1=无） */
            total = rs.total;
            if (rs.state == RECV_ST_READY || rs.state == RECV_ST_RECEIVING)
                g_app.prog_ms =
                    (int)((sceKernelGetSystemTimeWide() - rs.start_us) / 1000);
            g_app.prog_pct = rs.total > 0
                ? (int)(rs.got_total * 100 / rs.total)
                : (rs.state == RECV_ST_DONE ? 100 : 0);
            if (g_app.prog_pct > 100) g_app.prog_pct = 100;
            g_app.prog_running = rs.state == RECV_ST_READY ||
                                 rs.state == RECV_ST_RECEIVING;
            g_app.prog_done = rs.state == RECV_ST_DONE;
            g_app.prog_cancel = rs.state == RECV_ST_CANCEL;
            fail_state = rs.state == RECV_ST_FAIL ||
                         rs.state == RECV_ST_TIMEOUT;
            if (fail_state) {
                msg = rs.err[0] ? rs.err : "Receive failed";
                msg_col = theme->danger;
            } else if (rs.state == RECV_ST_CANCEL) {
                msg = rs.err[0] ? rs.err : "Cancelled";
            } else if (rs.state == RECV_ST_READY) {
                msg = "Waiting for sender to start...";
            } else if (rs.state == RECV_ST_RECEIVING) {
                msg = "Receiving...";
            }
        } else {
            /* 首帧兜底：按已接受的清单画 0%，马上会被真实会话快照取代。
             * Accept 后 http 线程应在几十 ms 内建好会话；若过了 2s 还没会话、
             * 且请求也不再 PENDING（说明后端已把决定处理掉但没建成会话，
             * 如请求恰好在决定前过期）→ 不能卡在进度页，给个失败态可退出。 */
            for (i = 0; i < xf_count; i++) {
                done[i] = 0;
                total += xf_size[i];
                if (xf_size[i] == 0) fin++;
            }
            msg = "Waiting for sender to start...";
            if (recv_pending_pull(NULL) == 0 &&
                (uint64_t)sceKernelGetSystemTimeWide() - g_app.prog_start >
                    2000000ULL) {
                g_app.prog_running = false;
                g_app.prog_cancel = true;
                msg = "Session was not created (request expired).";
                msg_col = theme->danger;
            }
        }
    }
    if (active < 0 && xf_count > 0) active = xf_count - 1;

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
    if (g_app.prog_done || g_app.prog_cancel || fail_state) {
        const char *st = g_app.prog_done ? "Complete"
                       : (g_app.prog_cancel ? "Cancelled" : "Failed");
        uint32_t sc = g_app.prog_done ? theme->success : theme->danger;
        int tw = 0, th = 0;
        w_text_w(1.15f, st, &tw, &th);
        w_text(920 - tw, 376, 1.15f, sc, "%s", st);
    }
    w_bar((Rect){ 40, 400, 880, 16 }, theme->card, theme->accent, g_app.prog_pct);

    /* 状态行：失败原因 / 等待对方接受 / 正在发送 */
    if (msg) {
        w_text_clip(40, 418, 1.0f, msg_col, msg, 880);
    }

    /* 高级面板：文件计数 / 耗时 / 速度（十进制 MB，1 MB = 1,000,000 B） */
    if (g_app.prog_info) {
        double sec = g_app.prog_ms / 1000.0;
        double mb = (double)(total * (SceOff)g_app.prog_pct / 100) / 1000000.0;
        double speed = sec > 0.05 ? mb / sec : 0.0;
        char st[160];
        snprintf(st, sizeof st, "Files %d/%d    Elapsed %d:%02d    Speed %.2f MB/s",
                 fin, xf_count, g_app.prog_ms / 60000,
                 (g_app.prog_ms / 1000) % 60, speed);
        w_text(40, 456, 1.0f, theme->text_dim, "%s", st);
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

/* 离开进度页：接收方向先清场（终态/取消后让后端回空闲，不然一直占着会话） */
static void progress_leave(void)
{
    if (g_app.prog_dir == 1) recv_clear();
    goto_devices();
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
            if (g_app.prog_running) {
                if (g_app.prog_dir == 0) xfer_cancel();
                else recv_abort();
            } else {
                progress_leave();
            }
        } else if (id == 0) {
            g_app.prog_info = !g_app.prog_info;
        }
        return;
    }
    if (in->confirm || in->back) {
        if (g_app.prog_running) {
            if (g_app.prog_dir == 0) xfer_cancel();
            else recv_abort();
        } else {
            progress_leave();
        }
        return;
    }
    if (in->alt) g_app.prog_info = !g_app.prog_info;
}

/* ================= 设置 ================= */
#define SET_ROWS 3            /* 0=主题 1=确认键 2=主机名(壳子) */

void page_settings_render(void)
{
    char theme_v[64], layout_v[96];
    const char *vals[SET_ROWS];
    const char *labels[SET_ROWS];
    int i;

    w_page_header("Settings");
    snprintf(theme_v, sizeof theme_v, "%s", theme_names[g_cfg.theme_id]);
    snprintf(layout_v, sizeof layout_v, "%s confirm / %s back (%s)",
             key_confirm(), key_back(),
             g_app.confirm_layout == 0 ? "US" : "JP");
    labels[0] = "Theme";
    labels[1] = "Confirm key";
    labels[2] = "Hostname";
    vals[0] = theme_v;
    vals[1] = layout_v;
    vals[2] = g_cfg.alias[0] ? g_cfg.alias : DEFAULT_ALIAS;
    for (i = 0; i < SET_ROWS; i++) {
        Rect r = { 24, 100 + i * 78, SCR_W - 48, 62 };
        Rect rl = { r.x, r.y, r.w / 2, r.h };
        Rect rr = { r.x + r.w / 2, r.y, r.w - r.w / 2, r.h };
        w_add(i * 2, rl);          /* 左半 = 上一个/编辑 */
        w_add(i * 2 + 1, rr);      /* 右半 = 下一个/编辑 */
        w_row(r, labels[i], vals[i], i == g_app.set_sel);
    }
    w_text(28, 100 + SET_ROWS * 78 + 10, 1.0f, theme->text_dim,
           "LEFT / tap left half: prev   RIGHT / tap right half: next");
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;      segs[ns++].text = "Choose";
    segs[ns].icon = icon_confirm();  segs[ns++].text = "Change";
    segs[ns].icon = icon_back();     segs[ns++].text = "Back";
    w_page_footer_segs(segs, ns);

    /* 主机名编辑占位弹窗（文字输入方案未定，先壳子） */
    if (host_pop) {
        Rect card = w_modal_box(230);
        w_text(card.x + 40, card.y + 28, 1.3f, theme->text, "Hostname");
        w_text_clip(card.x + 40, card.y + 76, 1.0f, theme->text_dim,
                    vals[2], card.w - 80);
        w_text(card.x + 40, card.y + 124, 1.0f, theme->text_dim,
               "This name is shown to other LocalSend devices.");
        w_text(card.x + 40, card.y + 158, 1.0f, theme->text_dim,
               "Editing needs text input - not available yet (TODO).");
    }
}

/* 修改设置项并落盘：row=0 主题循环切换，row=1 键位布局翻转；dir=±1 */
static void settings_change(int row, int dir)
{
    if (row == 0) {
        g_app.theme_id += dir;
        if (g_app.theme_id < 0) g_app.theme_id = THEME_COUNT - 1;
        if (g_app.theme_id >= THEME_COUNT) g_app.theme_id = 0;
        g_cfg.theme_id = g_app.theme_id;
        theme_set(g_app.theme_id);
        config_save();
    } else if (row == 1) {
        g_app.confirm_layout = g_app.confirm_layout ? 0 : 1;
        g_cfg.confirm_layout = g_app.confirm_layout;
        config_save();
    }
    /* row=2 主机名：无文字输入，改不了（见占位弹窗） */
}

void page_settings_input(const Input *in)
{
    if (host_pop) {
        /* 占位弹窗：任意键/点击关闭 */
        if (in->tap || in->confirm || in->back || in->alt ||
            in->up || in->down || in->left || in->right ||
            in->drag_start || in->dragging)
            host_pop = false;
        return;
    }
    if (in->drag_start || in->dragging) return;
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id >= 0) {
            g_app.set_sel = id / 2;
            if (g_app.set_sel == 2) host_pop = true;   /* 主机名行：占位弹窗 */
            else settings_change(g_app.set_sel, (id & 1) ? 1 : -1);
        }
        return;
    }
    if (in->up && g_app.set_sel > 0) g_app.set_sel--;
    if (in->down && g_app.set_sel < SET_ROWS - 1) g_app.set_sel++;
    if (in->left || in->right || in->confirm) {
        if (g_app.set_sel == 2) host_pop = true;
        else settings_change(g_app.set_sel, in->left ? -1 : 1);
    }
    if (in->back) g_app.page = PAGE_DEVICES;
}

/* ================= 新接收请求自动弹窗 ================= */
/* 把"待决定请求"快照复制到 UI 工作副本并进入接收确认页 */
static void open_recv_request(const RecvPending *rp)
{
    int i, n = rp->count;
    if (n > RECV_MAX_FILES) n = RECV_MAX_FILES;
    if (n > MAX_INF) n = MAX_INF;
    snprintf(g_app.recv_alias, sizeof g_app.recv_alias, "%s", rp->peer_alias);
    snprintf(g_app.recv_type, sizeof g_app.recv_type, "%s",
             rp->peer_type[0] ? rp->peer_type : "unknown");
    g_app.inc_count = n;
    for (i = 0; i < n; i++) {
        snprintf(g_app.inc_files[i].name, sizeof g_app.inc_files[i].name,
                 "%s", rp->files[i].name);
        g_app.inc_files[i].rname[0] = 0;
        g_app.inc_files[i].inc = true;      /* 默认全收，Setup 里可取消勾选 */
        g_app.inc_files[i].size = rp->files[i].size;
    }
    snprintf(g_app.recv_dir, sizeof g_app.recv_dir, "%s", PSVSEND_DL_DIR);
    g_app.inc_sel = 0;
    rs_scroll = 0;
    rename_pop = false;
    recv_focus = 2;
    g_app.page = PAGE_RECV_CONFIRM;
}

/* 每帧由 ui_main 调用：出现"待决定接收请求"且当前页面可打断时自动弹确认页。
 * 用户拒绝/接受后 g_recv_supp_ms 置 2s 抑制窗，防对端立刻重试又弹回；
 * 接收流程页/传输中本身不打断（switch 的 default）。 */
void pages_tick(void)
{
    RecvPending rp;
    uint64_t now_ms = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
    if (now_ms < g_recv_supp_ms) return;     /* 决定后的抑制窗内不弹 */
    if (recv_pending_pull(&rp) != 1) return; /* 无"待决定"请求 */
    switch (g_app.page) {
    case PAGE_DEVICES:
    case PAGE_FILES:
    case PAGE_SEND_CONFIRM:
    case PAGE_SETTINGS:
        break;
    default:
        return;                              /* 接收流程/传输中不打断 */
    }
    open_recv_request(&rp);
}
