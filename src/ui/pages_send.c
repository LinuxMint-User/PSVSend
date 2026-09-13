/* 页面：发送流程——设备列表 / 文件浏览 / 发送确认。
 * 设备列表来自后端 UDP 发现（api.h 快照）；文件浏览走 sceIo 目录枚举；
 * 发送由 start_send 组装清单交给 xfer 后台线程，进度页在 pages_progress.c。
 *
 * 列表采用"像素滚动"模型：内容像素偏移 scroll（0=从 LIST_TOP 开始），
 * 触摸拖动让内容直接跟手（正常滑动页面），方向键移动选中时自动滚到可见。 */
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "app/api.h"

static int dev_scroll = 0;        /* 设备列表内容像素偏移 */
static int file_scroll = 0;       /* 文件列表内容像素偏移 */
static int dev_press_scroll = 0;  /* 本次拖动按下时的滚动位置 */
static int file_press_scroll = 0;
/* 手动扫描的反馈横幅状态（设备页头与列表之间的一条细字）：
 * 扫描中显示进度，round 结束时边沿检测记下结束时刻，其后约 4s 显示结果 */
static uint64_t dev_scan_end_us = 0;
static int      dev_scan_was = 0;
static int      dev_scan_last_found = 0;

static void start_send(void);
static void open_files(void);

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
    if (api_scan_active()) {
        int d = api_scan_done(), t = api_scan_total(), f = g_app.dev_count;
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
        /* 以"表里现有设备数"为准（而非仅主动探测的 api_scan_found()）：
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
        } else if (!api_link_up()) {
            w_text(28, LIST_TOP + 10, 1.15f, theme->text, "%s",
                   tr("Wi-Fi link is down."));
        } else if (api_scan_active()) {
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
        api_scan_trigger();      /* 三角键：手动扫一遍网段 */
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
        api_send_start(g_app.dev_ip[t], g_app.dev_port[t],
                       g_app.dev_proto[t], g_app.dev_fp[t], ff, n);
    } else {
        /* 目标失效（如设备刚离线）：api_send_start 会把失败原因写进快照，进度页显示 */
        XferFile dummy;
        memset(&dummy, 0, sizeof dummy);
        api_send_start("", 0, "http", "", &dummy, 1);
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
