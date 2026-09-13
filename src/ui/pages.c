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
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/i18n.h"
#include "net/net.h"
#include "app/api.h"
#include "app/update.h"
#include "app/ime.h"
#include "proto/transfer.h"
#include "proto/receive.h"
#include "net/scan.h"
#include "core/dlog.h"

/* 布局常量与各页共用的滚动小工具见 ui/pages_internal.h（拆分后集中共享） */

/* 传输页：文件列表区域与总进度布局（发送/接收共用） */
#define XF_TOP        68
#define XF_BOTTOM     370      /* 文件列表可视区底（总进度条上方） */
#define XF_VIEW_H     (XF_BOTTOM - XF_TOP)
#define XF_ROW_H      58

static int dev_scroll = 0;        /* 列表内容像素偏移 */
static int file_scroll = 0;
int xf_scroll = 0;                /* 传输页文件列表内容偏移（pages_recv.c 也用） */
static int dev_press_scroll = 0;  /* 本次拖动按下时的滚动位置 */
static int file_press_scroll = 0;
static int xf_press_scroll = 0;
/* 手动扫描的反馈横幅状态（设备页头与列表之间的一条细字）：
 * 扫描中显示进度，round 结束时边沿检测记下结束时刻，其后约 4s 显示结果 */
static uint64_t dev_scan_end_us = 0;
static int      dev_scan_was = 0;
static int      dev_scan_last_found = 0;

/* 进度页当前文件清单：发送时每帧由 xfer 快照覆盖；接收时会话快照，
 * 会话还没建好的头几帧用这里的本地清单兜底 */
int    xf_count = 0;
char   xf_name[MAX_PICKED][128];
SceOff xf_size[MAX_PICKED];

static void start_send(void);
static void open_files(void);

/* 布局/滚动小工具（key_confirm/clamp_scroll/list_drag/add_row_hit 等）已移至
 * ui/pages_internal.h，供 pages.c / pages_recv.c / pages_settings.c 共享 */

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
    Device tmp[DEVICE_MAX];
    int n = api_device_snapshot(tmp, DEVICE_MAX);
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

/* 页头下细条：手动扫描的实时进度/结果（与列表是否为空无关；
 * 三角键按下后下一帧起可见，round 结束后约 4s 报告"扫到 N 台"） */
static void dev_scan_strip(void)
{
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    if (scan_active()) {
        int d = scan_done(), t = scan_total(), f = g_app.dev_count;
        char st[96];
        if (t <= 0) t = 254;
        if (d < 0 || d > t) d = 0;
        dev_scan_was = 1;
        dev_scan_end_us = 0;          /* 新一轮开始，旧的结果提示作废 */
        snprintf(st, sizeof st, tr("Scanning... %d/%d hosts, %d found"),
                 d, t, f);
        w_text(28, 56, 0.9f, theme->text_dim, "%s", st);
    } else if (dev_scan_was) {        /* round 结束边沿：记时刻与结果 */
        dev_scan_was = 0;
        dev_scan_end_us = now;
        /* 以"表里现有设备数"为准（而非仅主动探测的 scan_found()）：
         * 扫描期间入表的既可能有探测命中，也可能有设备自己发的 register，
         * round 结束时列表里实际有几台就报几台，横幅才不与列表矛盾 */
        dev_scan_last_found = g_app.dev_count;
    }
    if (dev_scan_end_us && now - dev_scan_end_us < 4000000ull) {
        char st[96];
        if (dev_scan_last_found > 0)
            snprintf(st, sizeof st, tr("Scan complete, %d device%s found"),
                     dev_scan_last_found,
                     dev_scan_last_found == 1 ? "" : "s");
        else
            snprintf(st, sizeof st, tr("Scan complete, no devices"));
        w_text(28, 56, 0.9f, theme->text_dim, "%s", st);
        return;
    }
    dev_scan_end_us = 0;              /* 提示过期，清标记 */
}

void page_devices_render(void)
{
    int count;
    dev_sync();
    count = g_app.dev_count;
    w_page_header("PSVSend");
    dev_scan_strip();
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
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "%s",
                   tr("Network not ready."));
            if (why[0]) {
                w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim, "%s", why);
            } else {
                w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim, "%s",
                       tr("Waiting for Wi-Fi to come back up..."));
            }
        } else if (!net_connected()) {
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "%s",
                   tr("Wi-Fi link is down."));
        } else if (scan_active()) {
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "%s",
                   tr("Scanning network..."));
        } else {
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "%s",
                   tr("No devices found."));
            w_text(28, LIST_TOP + 48, 1.0f, theme->text_dim, "%s",
                   tr("Press Triangle to scan this network."));
        }
    }
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = tr("Choose");
    segs[ns].icon = icon_confirm();   segs[ns++].text = tr("Send");
    segs[ns].icon = HICON_TRIANGLE;   segs[ns++].text = tr("Scan");
    segs[ns].icon = HICON_NONE;       segs[ns++].text = tr("SELECT Settings");
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
void files_load(void)
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

void enter_dir(const char *name)
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

void parent_dir(void)
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
    snprintf(title, sizeof title, tr("Send to %s"), g_app.dev_alias[g_app.dev_target]);
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
                snprintf(main, sizeof main, "%s%s", g_app.files[i].name, DIR_SUFFIX);
            else if (is_picked(i))
                snprintf(main, sizeof main, "* %s", g_app.files[i].name);
            else
                snprintf(main, sizeof main, "%s", g_app.files[i].name);
            if (g_app.files[i].is_dir)
                strcpy(sub, tr("folder"));
            else
                w_human_size(g_app.files[i].size, sub);
            w_row(r, main, sub, i == g_app.file_sel);
        }
        vita2d_disable_clipping();
    }
    if (count == 0)
        w_text(28, LIST_TOP + 8, 1.1f, theme->text_dim, "%s",
               tr("(empty folder)"));
    char send_lb[24];
    snprintf(send_lb, sizeof send_lb, tr("Send(%d)"), g_app.picked_count);
    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = HICON_DPAD;       segs[ns++].text = tr("Choose");
    segs[ns].icon = icon_confirm();   segs[ns++].text = tr("Open/Pick");
    /* 根目录无"上级"：返回键此时=退回设备页（与选择保存路径页一致地换标签） */
    segs[ns].icon = icon_back();
    segs[ns++].text = strlen(g_app.cur_dir) <= 5 ? tr("Back") : tr("Up");
    segs[ns].icon = HICON_TRIANGLE;   segs[ns++].text = send_lb;
    w_page_footer_segs(segs, ns);
    Rect send = { SCR_W - 216, SCR_H - 42, 192, 36 };
    if (g_app.picked_count > 0) {
        char lb[32];
        snprintf(lb, sizeof lb, tr("Send (%d)"), g_app.picked_count);
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
    w_page_header(tr("Confirm Send"));
    char line[256];
    Rect card = { 24, 80, SCR_W - 48, 330 };
    w_rect(card, theme->card);
    snprintf(line, sizeof line, tr("Target: %s"), g_app.dev_alias[g_app.dev_target]);
    w_text(48, 100, 1.2f, theme->text, "%s", line);
    char sz[16];
    w_human_size(g_app.picked_total, sz);
    snprintf(line, sizeof line, tr("%d file(s)  total %s"), n, sz);
    w_text(48, 140, 1.0f, theme->text_dim, "%s", line);
    int shown = n < 8 ? n : 8;
    for (i = 0; i < shown; i++) {
        char m[256];
        if (g_app.picked[i].name[0])
            snprintf(m, sizeof m, "  %s", g_app.picked[i].name);
        else
            snprintf(m, sizeof m, tr("  <file %d>"), i + 1);
        w_text_clip(48, 176 + i * 26, 1.0f, theme->text, m, card.w - 60);
    }
    if (n > shown)
        w_text(48, 176 + shown * 26, 1.0f, theme->text_dim, "%s",
               tr("  ... %d more"), n - shown);
    Rect cancel = { SCR_W / 2 - 220, 444, 200, 48 };
    Rect ok     = { SCR_W / 2 + 20, 444, 200, 48 };
    w_add(0, cancel);
    w_add(1, ok);
    w_button(cancel, tr("Cancel"), false);
    w_button(ok, tr("Send"), true);
    HintSeg segs[4];
    int ns = 0;
    segs[ns].icon = icon_confirm();  segs[ns++].text = tr("Send");
    segs[ns].icon = icon_back();     segs[ns++].text = tr("Cancel");
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

/* ================= 进度（LocalSend 式：文件列表 + 总进度 + 高级面板） ================= */
void goto_devices(void)
{
    w_clear_picked();
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
            msg = xv.err[0] ? xv.err : tr("Transfer failed");
            msg_col = theme->danger;
        } else if (xv.active && xv.total_sent == 0 && xv.cur < 0) {
            msg = xv.msg[0] ? xv.msg : tr("Waiting for receiver to accept...");
        } else if (xv.active && xv.cur >= 0) {
            msg = xv.msg[0] ? xv.msg : tr("Sending...");
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
                msg = rs.err[0] ? rs.err : tr("Receive failed");
                msg_col = theme->danger;
            } else if (rs.state == RECV_ST_CANCEL) {
                msg = rs.err[0] ? rs.err : tr("Cancelled");
            } else if (rs.state == RECV_ST_READY) {
                msg = tr("Waiting for sender to start...");
            } else if (rs.state == RECV_ST_RECEIVING) {
                msg = tr("Receiving...");
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
            msg = tr("Waiting for sender to start...");
            if (recv_pending_pull(NULL) == 0 &&
                (uint64_t)sceKernelGetSystemTimeWide() - g_app.prog_start >
                    2000000ULL) {
                g_app.prog_running = false;
                g_app.prog_cancel = true;
                msg = tr("Session was not created (request expired).");
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

    w_page_header(tr(g_app.prog_dir == 0 ? "Sending" : "Receiving"));

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
    snprintf(ov, sizeof ov, tr("Total   %d%%"), g_app.prog_pct);
    w_text(40, 376, 1.15f, theme->text, "%s", ov);
    if (g_app.prog_done || g_app.prog_cancel || fail_state) {
        const char *st = g_app.prog_done ? tr("Complete")
                       : (g_app.prog_cancel ? tr("Cancelled") : tr("Failed"));
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
        snprintf(st, sizeof st,
                 tr("Files %d/%d    Elapsed %d:%02d    Speed %.2f MB/s"),
                 fin, xf_count, g_app.prog_ms / 60000,
                 (g_app.prog_ms / 1000) % 60, speed);
        w_text(40, 456, 1.0f, theme->text_dim, "%s", st);
    }

    HintSeg segs[6];
    int ns = 0;
    segs[ns].icon = icon_confirm();
    segs[ns++].text = g_app.prog_running ? tr("Cancel") : tr("Done");
    segs[ns].icon = HICON_TRIANGLE;  segs[ns++].text = tr("Advanced");
    w_page_footer_segs(segs, ns);

    /* 底部右侧按钮：右边=取消/完成，其左=高级 */
    Rect main = { SCR_W - 24 - 160, SCR_H - 42, 160, 36 };
    Rect adv  = { main.x - 12 - 140, SCR_H - 42, 140, 36 };
    w_add(0, adv);
    w_add(1, main);
    w_button(adv, tr("Advanced"), g_app.prog_info);
    if (g_app.prog_running) {
        w_rect(main, theme->danger);
        int tw = 0, th = 0;
        w_text_w(1.3f, tr("Cancel"), &tw, &th);
        w_text(main.x + (main.w - tw) / 2, main.y + (main.h - th) / 2, 1.3f,
               theme->accent_text, "%s", tr("Cancel"));
    } else {
        w_button(main, tr("Done"), true);
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
