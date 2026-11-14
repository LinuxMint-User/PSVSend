/* 页面：发送流程——发送主页（设备栏 + 已选文件栏）/ 文件选择页 / 发送等待页。
 * 流程对齐官方 LocalSend：先在文件栏把文件选好（方块键进文件选择页），
 * 最后在设备栏点/按确认键挑目标即发，先进等待页等对方接受（pages_recv.c 的
 * pages_tick 检测到对端接受后自动切进度页），避免一发起就进"无进展"的进度页。
 * "选定目标 → 真正连接"的窗口由此压到近 0，设备列表也最新鲜（d77 的 connect
 * 超时是兜底，不是根治）。
 * 设备栏来自后端 UDP 发现（api.h 快照）；文件浏览走 sceIo 目录枚举；
 * 发送由 start_send 组装清单交给 xfer 后台线程，进度页在 pages_progress.c。
 *
 * 列表采用"像素滚动"模型：内容像素偏移 scroll（0=从列表可视区顶开始），
 * 触摸拖动让内容直接跟手（正常滑动页面），方向键移动选中时自动滚到可见。 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <vita2d.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "core/config.h"
#include "core/json_util.h"
#include "core/dlog.h"
#include "app/api.h"

static int dev_scroll = 0;         /* 主页设备栏内容像素偏移 */
static int dev_press_scroll = 0;   /* 本次拖动按下时的滚动位置 */
static int pick_scroll = 0;        /* 主页已选文件栏内容像素偏移 */
static int pick_press_scroll = 0;
static int file_scroll = 0;        /* 文件选择页（目录列表）内容像素偏移 */
static int file_press_scroll = 0;
static ScrollGlide dev_glide;      /* 主页设备栏触摸惯性 */
static ScrollGlide pick_glide;     /* 主页已选文件栏触摸惯性 */
static ScrollGlide file_glide;     /* 文件选择页触摸惯性 */
static char pick_dir[512] = "ux0:/";  /* 文件选择页上次所在目录（本次运行内记忆） */
/* 页头下细条的两个来源（提示优先于扫描反馈）：
 *  - 手动扫描的进度/结果：round 结束时边沿检测记下结束时刻，其后约 4s 显示；
 *  - "请先选择文件"提示：没选文件就点设备时短暂显示。 */
static uint64_t dev_scan_end_us = 0;
static int      dev_scan_was = 0;
static int      dev_scan_last_found = 0;
static uint64_t dev_hint_us = 0;
static uint64_t dev_warn_us = 0;    /* 接收请求被拒的红色提示截止时刻 */
static int      dev_warn_reason = 0;/* HTTP_REJ_*（最近一次） */
static int sw_focus = 0;           /* 发送等待页焦点：0=取消/完成，1=验证（仅 https 对端） */
/* 文件夹相关短提示（主页细条 / 目录页右上）；设置函数在文件夹枚举一节 */
static char     folder_note[96];
static uint64_t folder_note_us;
static char     dir_msg[96];
static uint64_t dir_msg_us;

static void start_send(int dev_idx);
static void open_files(void);
static void open_folders(void);
static void folder_note_set(const char *fmt, ...);

/* ---------- 主页两栏几何 ---------- */
typedef struct { int x, w; } PaneRect;

/* 设备栏/文件栏的横向位置（左右顺序由 App.pane_swap 决定） */
static void pane_rects(PaneRect *dev, PaneRect *fil)
{
    if (!g_app.pane_swap) {
        dev->x = PANE_X;
        dev->w = PANE_W_DEV;
        fil->x = PANE_X + PANE_W_DEV + PANE_GAP;
        fil->w = PANE_W_FILE;
    } else {
        fil->x = PANE_X;
        fil->w = PANE_W_FILE;
        dev->x = PANE_X + PANE_W_FILE + PANE_GAP;
        dev->w = PANE_W_DEV;
    }
}

/* 栏标题 + 焦点下划线：焦点栏亮色标题 + accent 下划线，另一栏整条变暗 */
static void pane_title(const PaneRect *p, const char *text, bool focused)
{
    w_text(p->x + 4, PANE_TITLE_Y, 1.0f,
           focused ? theme->text : theme->text_dim, "%s", text);
    if (focused)
        w_rect((Rect){ p->x, PANE_TITLE_Y + 22, p->w, 2 }, theme->accent);
}

/* 主页焦点栏默认落点：已有文件 → 设备栏（下一步就是挑目标），否则文件栏 */
void pane_focus_default(void)
{
    g_app.pane_focus = g_app.picked_count > 0 ? 0 : 1;
}

/* 行配色：焦点栏选中行 = accent 实底，非焦点栏选中行 = border 弱高亮 */
static void row_colors(bool selected, bool focused,
                       uint32_t *bg, uint32_t *main_c, uint32_t *sub_c)
{
    if (selected) {
        *bg     = focused ? theme->accent : theme->border;
        *main_c = focused ? theme->accent_text : theme->text;
        *sub_c  = focused ? theme->accent_text : theme->text_dim;
    } else {
        *bg     = theme->card;
        *main_c = theme->text;
        *sub_c  = theme->text_dim;
    }
}

static void dev_hint_show(void)
{
    dev_hint_us = (uint64_t)sceKernelGetSystemTimeWide() + 3000000ull;
}

/* ================= 发送主页（设备栏 + 已选文件栏） ================= */

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

/* 页头下细条：显示"焦点栏当前光标项"的完整标识（行内被截断的信息在这里补全）。
 *   文件栏 —— 选中文件的完整路径（行内文件名过长会中间省略，这里能核对到底
 *             是哪个文件，尤其移除前确认用）；
 *   设备栏 —— 目标地址 proto://ip:port（行内只有别名与型号，地址列表里没有）。
 * 扫描进度/结果不在这里：它归设备栏标题行右侧（就在它描述的那一栏里）。
 * "请先选择文件"是短时警告，独占整行、优先于上述两条。 */
#define STRIP_Y 56

/* 细条空闲轮播：只有"没东西可显示"（无身份生成、无临时警告、无选中项）时才轮到，
 * 8 秒一条；离开空闲态重置到第一条，回来不会接着半截。扫描中不轮播（进度已在
 * 设备栏标题行，细条再动会分心）。 */
static const char *tip_key[] = {
    "Hold R for button labels",
    "Long-press △ to clear selected files",
    "Web share: open the address in a phone browser",
    "Rename files and pick a folder before receiving",
    "Settings: theme, language, save folder",
    "Press L to scan a QR code",
};
/* 扫码入口关闭时不播那条提示（其余提示条数不变） */
static int tip_count(void)
{
    return g_cfg.qr_scan ? (int)(sizeof tip_key / sizeof tip_key[0])
                         : (int)(sizeof tip_key / sizeof tip_key[0]) - 1;
}
#define TIP_US  (8 * 1000000ull)
static int      tip_idx;
static uint64_t tip_next_us;

static void tips_reset(void)
{
    tip_idx = 0;
    tip_next_us = 0;
}

static void tips_render(uint64_t now)
{
    if (api_scan_active()) { tips_reset(); return; }
    if (!tip_next_us) {
        tip_next_us = now + TIP_US;
    } else if (now >= tip_next_us) {
        tip_idx = (tip_idx + 1) % tip_count();
        tip_next_us = now + TIP_US;
    }
    if (tip_idx >= tip_count()) tip_idx = 0;   /* 开关在设置页被关掉后回落 */
    w_text_clip(28, STRIP_Y, 0.9f, theme->text_dim, tr(tip_key[tip_idx]),
                SCR_W - 56);
}

static void dev_strip(void)
{
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    int i;

    /* 首启：后台正在生成 per-device 证书（2~9s），http/发现暂缓启动。
     * 这条优先于其它提示，就绪后自动消失。 */
    if (api_identity_pending()) {
        tips_reset();
        w_text(28, STRIP_Y, 0.9f, theme->text_dim, "%s",
               tr("Generating device identity (first run)..."));
        return;
    }

    /* 接收请求被拒（文件数超限 / 清单过大 / 忙 / 内存不足）：红色细条短时提示。
     * 后端一次性给出原因，这里取走后保留 8s；不在主页时事件留在后端，回来再显示。 */
    {
        int rej = api_recv_reject_take();
        if (rej) {
            dev_warn_reason = rej;
            dev_warn_us = now + 8000000ull;
        }
        if (dev_warn_us && now < dev_warn_us) {
            const char *m = NULL;
            switch (dev_warn_reason) {
            case HTTP_REJ_TOO_MANY:  m = tr("Too many files (max 2048)"); break;
            case HTTP_REJ_TOO_LARGE: m = tr("File list too large (max 4 MB)"); break;
            case HTTP_REJ_BUSY:      m = tr("Busy: another receive request is pending"); break;
            case HTTP_REJ_NOMEM:     m = tr("Not enough memory to receive"); break;
            default: break;
            }
            if (m) {
                tips_reset();
                w_text(28, STRIP_Y, 0.9f, theme->danger, "%s", m);
                return;
            }
        }
        dev_warn_us = 0;
    }

    /* 文件夹枚举结果提示（如"N 个空文件夹已跳过"）：加完文件夹回主页时短时显示 */
    if (folder_note_us && now < folder_note_us) {
        tips_reset();
        w_text_clip(28, STRIP_Y, 0.9f, theme->warn, folder_note, SCR_W - 56);
        return;
    }
    folder_note_us = 0;

    if (dev_hint_us && now < dev_hint_us) {
        tips_reset();
        w_text(28, STRIP_Y, 0.9f, theme->warn, "%s", tr("Select files first"));
        return;
    }
    dev_hint_us = 0;

    if (g_app.pane_focus == 1) {
        if (g_app.picked_count > 0) {
            i = g_app.picked_sel;
            if (i < 0 || i >= g_app.picked_count) i = 0;
            w_text_lead(28, STRIP_Y, 0.9f, theme->text_dim,
                        g_app.picked[i].path, SCR_W - 56);
            tips_reset();
            return;
        }
    } else if (g_app.dev_count > 0) {
        char addr[48];
        i = g_app.dev_sel;
        if (i < 0 || i >= g_app.dev_count) i = 0;
        snprintf(addr, sizeof addr, "%s://%s:%d",
                 g_app.dev_proto[i], g_app.dev_ip[i], g_app.dev_port[i]);
        w_text_clip(28, STRIP_Y, 0.9f, theme->text_dim, addr, SCR_W - 56);
        tips_reset();
        return;
    }
    tips_render(now);
}

/* 设备栏：有设备画列表（alias + 型号/类型），否则画网络/扫描状态 */
static void dev_pane_render(const PaneRect *p, bool focused)
{
    int i, count = g_app.dev_count;
    char title[48];
    snprintf(title, sizeof title, tr("Devices (%d)"), count);
    pane_title(p, title, focused);

    /* 扫描进度/结果画在标题行右侧 —— 就在它描述的那一栏里，不再占页头下的
     * 细条（细条空出来专给"当前光标项的完整标识"）。状态推进每帧一次、与是否
     * 绘制无关；空态（无设备）由下面的栏内文案负责，这里不画。 */
    {
        char st[64];
        bool has = false;
        uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
        if (api_scan_active()) {
            int d = api_scan_done(), t = api_scan_total();
            if (t <= 0) t = 254;
            if (d < 0 || d > t) d = 0;
            dev_scan_was = 1;
            dev_scan_end_us = 0;      /* 新一轮开始，旧的结果提示作废 */
            snprintf(st, sizeof st, tr("Scanning... %d/%d"), d, t);
            has = true;
        } else if (dev_scan_was) {    /* round 结束边沿：记时刻与结果 */
            dev_scan_was = 0;
            dev_scan_end_us = now;
            /* 以"表里现有设备数"为准（而非仅主动探测的 api_scan_found()）：
             * 扫描期间入表的既可能有探测命中，也可能有设备自己发的 register，
             * round 结束时列表里实际有几台就报几台，横幅才不与列表矛盾 */
            dev_scan_last_found = count;
        }
        if (!has) {
            if (dev_scan_end_us && now - dev_scan_end_us < 4000000ull) {
                if (dev_scan_last_found > 0)
                    snprintf(st, sizeof st, tr("Scan complete, %d device%s found"),
                             dev_scan_last_found, dev_scan_last_found == 1 ? "" : "s");
                else
                    snprintf(st, sizeof st, tr("Scan complete, no devices"));
                has = true;
            } else {
                dev_scan_end_us = 0;  /* 提示过期，清标记 */
            }
        }
        if (count > 0 && has) {
            int tw = 0, sw = 0, avail;
            w_text_w(1.0f, title, &tw, NULL);
            w_text_w(0.9f, st, &sw, NULL);
            avail = p->w - 8 - tw - 16;   /* 标题右侧剩余（含 16px 间距） */
            if (sw <= avail)
                w_text_right(p->x + p->w - 4, PANE_TITLE_Y + 3, 0.9f,
                             theme->text_dim, "%s", st);
            else if (avail >= 120)        /* 宽度不够就裁尾（保标题），别整条消失 */
                w_text_clip(p->x + p->w - 4 - avail, PANE_TITLE_Y + 3, 0.9f,
                            theme->text_dim, st, avail);
        }
    }

    if (count > 0) {
        area_clamp(&dev_scroll, count, PANE_VIEW_H);
        vita2d_enable_clipping();
        /* 注意：本 API 是 (x_min, y_min, x_max, y_max)，不是 (x, y, w, h) */
        vita2d_set_clip_rectangle(p->x, PANE_TOP, p->x + p->w, PANE_BOTTOM);
        for (i = dev_scroll / ROW_STRIDE; i < count; i++) {
            int top = PANE_TOP - dev_scroll + i * ROW_STRIDE;
            uint32_t bg, mc, sc;
            Rect r;
            RowGeom g;
            const char *sub = g_app.dev_sub[i];
            if (top >= PANE_BOTTOM) break;
            r = (Rect){ p->x, top, p->w, ROW_H };
            area_row_hit(WID_DEVS_BASE + i, p->x, p->w, top);
            row_colors(i == g_app.dev_sel, focused, &bg, &mc, &sc);
            w_rect(r, bg);
            /* 名称 / 型号两行排：挤在一行时名称（1.25 缩放）会把栏宽吃满，
             * 右侧型号必被截断——分两行各自吃满栏宽，谁都不会被对方挤掉。 */
            row_geom(r, ROW_2LINE, 0, &g);
            if (sub[0]) {
                w_text_clip(g.x_text, g.y_main, g.main_sc, mc,
                            g_app.dev_alias[i], g.w_text);
                w_text_clip(g.x_text, g.y_sub, g.sub_sc, sc, sub, g.w_text);
            } else {
                /* 无型号：名称单行居中 */
                w_text_clip(g.x_text, r.y + (r.h - w_font_px(g.main_sc)) / 2,
                            g.main_sc, mc, g_app.dev_alias[i], g.w_text);
            }
        }
        vita2d_disable_clipping();
    } else if (!api_network_ready()) {
        const char *why = api_discovery_fail();
        /* 空态 / 状态句都是本地化文案（长度随语言变）→ 按栏宽裁尾，不裸画 */
        int wmax = p->w - 8;
        w_text_clip(p->x + 4, PANE_TOP + 10, 1.15f, theme->text,
                    tr("Network not ready."), wmax);
        if (why[0])
            w_text_clip(p->x + 4, PANE_TOP + 48, 1.0f, theme->text_dim, why, wmax);
        else
            w_text_clip(p->x + 4, PANE_TOP + 48, 1.0f, theme->text_dim,
                        tr("Waiting for Wi-Fi to come back up..."), wmax);
    } else if (!api_link_up()) {
        w_text_clip(p->x + 4, PANE_TOP + 10, 1.15f, theme->text,
                    tr("Wi-Fi link is down."), p->w - 8);
    } else if (api_scan_active()) {
        w_text_clip(p->x + 4, PANE_TOP + 10, 1.15f, theme->text,
                    tr("Scanning network..."), p->w - 8);
    } else {
        w_text_clip(p->x + 4, PANE_TOP + 10, 1.15f, theme->text,
                    tr("No devices found."), p->w - 8);
        w_text_clip(p->x + 4, PANE_TOP + 48, 1.0f, theme->text_dim,
                    tr("Press Triangle to scan this network."), p->w - 8);
    }
}

/* 已选文件栏：有文件画清单（行尾 ✕ = 触摸直接删除该项），空则给两个可聚焦动作
 * "选择文件" / "发送文件夹"（焦点在 pick_empty_sel，确认键激活当前项）。
 * 焦点落在"行"上（强调行底），确认键=移除该项；选中行的完整路径由页头下的细条
 * 显示，行内被截断的文件名可在那里核对。 */
static void pick_pane_render(const PaneRect *p, bool focused)
{
    int i, count = g_app.picked_count;
    char title[64];
    snprintf(title, sizeof title, tr("Selected files (%d)"), count);
    pane_title(p, title, focused);
    if (count <= 0) {
        int bw = 240, bx = p->x + (p->w - bw) / 2;
        Rect b1 = { bx, PANE_TOP + 96, bw, 48 };
        Rect b2 = { bx, PANE_TOP + 96 + 60, bw, 48 };
        if (g_app.pick_empty_sel < 0 || g_app.pick_empty_sel > 1)
            g_app.pick_empty_sel = 0;
        w_add(WID_FILES_PICK, b1);
        w_button(b1, tr("Select files"), focused && g_app.pick_empty_sel == 0);
        w_add(WID_FILES_FOLDER, b2);
        w_button(b2, tr("Send folder"), focused && g_app.pick_empty_sel == 1);
        return;
    }
    if (g_app.picked_sel >= count) g_app.picked_sel = count - 1;
    if (g_app.picked_sel < 0) g_app.picked_sel = 0;
    area_clamp(&pick_scroll, count, PANE_VIEW_H);
    vita2d_enable_clipping();
    /* 注意：本 API 是 (x_min, y_min, x_max, y_max)，不是 (x, y, w, h) */
    vita2d_set_clip_rectangle(p->x, PANE_TOP, p->x + p->w, PANE_BOTTOM);
    for (i = pick_scroll / ROW_STRIDE; i < count; i++) {
        int top = PANE_TOP - pick_scroll + i * ROW_STRIDE;
        char sz[16];
        Rect r, xh;
        RowGeom g;
        int sw = 0;
        int xbtn = p->x + p->w - 40;      /* 行尾 ✕ 区（40px 宽），贴栏右边缘 */
        int cx = xbtn + 20, cy = top + ROW_H / 2;
        uint32_t bg, mc, sc;
        bool sel = (i == g_app.picked_sel);
        if (top >= PANE_BOTTOM) break;
        r = (Rect){ p->x, top, p->w, ROW_H };
        row_colors(sel, focused, &bg, &mc, &sc);
        area_row_hit(WID_PICKS_BASE + i, p->x, p->w, top);
        xh = (Rect){ xbtn, top + 10, 36, ROW_H - 20 };   /* ✕ 命中区同样裁到可视区内 */
        if (xh.y < PANE_TOP) { xh.h -= PANE_TOP - xh.y; xh.y = PANE_TOP; }
        if (xh.y + xh.h > PANE_BOTTOM) xh.h = PANE_BOTTOM - xh.y;
        if (xh.h > 0) w_add(WID_PICKX_BASE + i, xh);
        w_rect(r, bg);
        row_geom(r, ROW_VALUE, 0, &g);
        w_human_size(g_app.picked[i].size, sz);
        w_text_w(g.sub_sc, sz, &sw, NULL);
        /* 名称可用宽 = 内容左锚点 → 尺寸左界（尺寸宽按实测扣，不再写死预留） */
        g.w_text = (xbtn - 12 - sw - ROW_GAP) - g.x_text;
        if (g.w_text < 40) g.w_text = 40;
        w_text_right(xbtn - 12, g.y_sub, g.sub_sc, sc, "%s", sz);
        /* 文件名 → 中间省略（保住扩展名）；上方 w_text_right 的尺寸值仍按实测扣宽 */
        w_text_mid(g.x_text, g.y_main, g.main_sc, mc,
                   g_app.picked[i].name, g.w_text);
        /* ✕ 只是常驻的触摸删除钮，不承载焦点 */
        w_icon_cross((float)cx, (float)cy, 8.0f,
                     sel ? (focused ? theme->accent_text : theme->text)
                         : theme->text_dim);
    }
    vita2d_disable_clipping();
    /* 内容超长时右侧细滚动条 */
    {
        int m = area_max_scroll(count, PANE_VIEW_H);
        if (m > 0) {
            int bh = PANE_VIEW_H * PANE_VIEW_H / (count * ROW_STRIDE);
            int by;
            if (bh < 24) bh = 24;
            by = PANE_TOP + (PANE_VIEW_H - bh) * pick_scroll / m;
            w_rect((Rect){ p->x + p->w - 4, PANE_TOP, 4, PANE_VIEW_H }, theme->card);
            w_rect((Rect){ p->x + p->w - 4, by, 4, bh }, theme->text_dim);
        }
    }
}

/* 从已选清单移除第 idx 项（行尾 ✕ 与键盘"移除"共用）。
 * 用移位删除而非 swap-remove：swap 会把最后一项搬到被删位置，用户看到的是
 * "删中间一个、末尾那个文件突然瞬移过来"，顺序也会被打乱。 */
static void unpick(int idx)
{
    int i;
    if (idx < 0 || idx >= g_app.picked_count) return;
    g_app.picked_total -= g_app.picked[idx].size;
    for (i = idx; i < g_app.picked_count - 1; i++)
        g_app.picked[i] = g_app.picked[i + 1];
    g_app.picked_count--;
    if (g_app.picked_sel >= g_app.picked_count)
        g_app.picked_sel = g_app.picked_count > 0 ? g_app.picked_count - 1 : 0;
    area_clamp(&pick_scroll, g_app.picked_count, PANE_VIEW_H);
}

/* 清空已选清单（三角键·文件栏聚焦 + 按住蓄满时用） */
static void unpick_all(void)
{
    g_app.picked_count = 0;
    g_app.picked_total = 0;
    g_app.picked_sel = 0;
    pick_scroll = 0;
    pick_press_scroll = 0;
    glide_reset(&pick_glide);
}

/* 设备栏确认：设备有效 + 有文件才发。
 * 无设备 / 设备刚离线：什么都不做（页脚该项已变灰，不能再给"请先选择文件"这种错提示）。
 * 有设备但没文件：弹提示并把焦点让给文件栏（下一步就是去选文件）。 */
static void try_send(int dev_idx)
{
    if (dev_idx < 0 || dev_idx >= g_app.dev_count || !g_app.dev_ip[dev_idx][0])
        return;
    if (g_app.picked_count <= 0) {
        dev_hint_show();
        g_app.pane_focus = 1;
        return;
    }
    start_send(dev_idx);
}

/* ---------- 页头右侧触摸按钮（触摸补齐） ----------
 * 此前进设置只有 SELECT、扫描只有 △，触摸用户无路可走（无设备时设备栏更是没有
 * 任何可点之物）。方框 + 图标、槽位与热区口径统一在 widgets.c 的 w_hdr_button。
 * 只触发动作、**不改焦点**：页头是全局入口，与左右栏无关；页脚提示条照旧只讲
 * 按键，两个入口各说各的。扫描按钮只在空闲时可点（扫描中画灰）——按不动比
 * "再按一下顺延下一轮"那套次级语义更好懂，那是给按键的。 */
/* 按住 R 展开成文字：平时 40px 等宽图标，展开宽度按当前语言实测，右端锚定、
 * 从右往左 20ms 错峰（设置 → 扫描 → 网页共享 → 扫码），收起反向。只在主页渲染；
 * 从别的页面回来时帧间隔过大 → 直接落到目标态，不补一段动画。 */
#define HDR_N 4
/* 从左到右：扫码 / 网页共享 / 网络扫描 / 设置。扫码入口可在设置里关掉，
 * 关掉时只画后三个（第一个下标 1），布局与动画照旧右端锚定。 */
static const char *hdr_lbl_key[HDR_N] = { "Scan QR", "Web share", "Scan", "Settings" };
static const HdrBtn hdr_kind[HDR_N] = {
    HDRBTN_QR, HDRBTN_WEB, HDRBTN_SCAN, HDRBTN_SETTINGS
};
static const int hdr_wid[HDR_N] = {
    WID_HDR_QR, WID_HDR_WEB, WID_HDR_SCAN, WID_HDR_SETTINGS
};
static float    hdr_exp;             /* 0=图标 1=文字 */
static uint64_t hdr_exp_us;          /* 上次动画步进时刻 */
static int      hdr_lbl_lang = -1;   /* 已测量的语言（切换语言后重量） */
static int      hdr_lbl_w[HDR_N];    /* 展开目标宽 */

static void hdr_measure(void)
{
    int i, lang = i18n_lang();
    if (lang == hdr_lbl_lang) return;
    for (i = 0; i < HDR_N; i++) {
        int tw = 0, w;
        w_text_w(0.8f, tr(hdr_lbl_key[i]), &tw, NULL);
        w = tw + 20;
        if (w > 140) w = 140;            /* 未来语言兜底：超宽在绘制时裁尾 */
        if (w < HDR_BTN_SZ) w = HDR_BTN_SZ;
        hdr_lbl_w[i] = w;
    }
    hdr_lbl_lang = lang;
}

static void hdr_step(void)
{
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    uint64_t dt = hdr_exp_us ? now - hdr_exp_us : 16667;
    hdr_exp_us = now;
    if (dt > 200000) {                   /* 切页回来/长卡顿：不补动画 */
        hdr_exp = ui_input_r_down() ? 1.0f : 0.0f;
        return;
    }
    if (dt < 1000) dt = 1000;
    if (ui_input_r_down()) {
        hdr_exp += (float)dt / 220000.0f;
        if (hdr_exp > 1.0f) hdr_exp = 1.0f;
    } else {
        hdr_exp -= (float)dt / 150000.0f;   /* 收起略快 */
        if (hdr_exp < 0.0f) hdr_exp = 0.0f;
    }
}

/* 单个按钮的展开进度：from_right = 距最右按钮的格数（0=最右），
 * 先右后左错峰 + ease-out；n = 本次实际显示的按钮数。 */
static float hdr_p(int from_right, int n)
{
    float span = 1.0f - (float)(n - 1) * 0.09f;
    float p;
    if (span < 0.01f) span = 0.01f;
    p = (hdr_exp - (float)from_right * 0.09f) / span;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return 1.0f - (1.0f - p) * (1.0f - p);
}

static void hdr_buttons(void)
{
    bool dim[HDR_N] = { false, false,
                        api_scan_active() || api_identity_pending(), false };
    int first = g_cfg.qr_scan ? 0 : 1;     /* 关闭扫码入口时首按钮是网页共享 */
    int n = HDR_N - first;
    int i, x = HDR_SLOT_R + HDR_BTN_SZ / 2;

    hdr_measure();
    hdr_step();
    for (i = HDR_N - 1; i >= first; i--) {
        float p = hdr_p(HDR_N - 1 - i, n);
        int w = HDR_BTN_SZ +
                (int)((float)(hdr_lbl_w[i] - HDR_BTN_SZ) * p + 0.5f);
        x -= w;
        w_hdr_button_var(x, w, hdr_wid[i], hdr_kind[i], tr(hdr_lbl_key[i]), p,
                         dim[i]);
        x -= HDR_BTN_EDGE;
    }
}

void page_devices_render(void)
{
    PaneRect dev, fil;
    dev_sync();
    w_page_header("PSVSend");
    hdr_buttons();
    dev_strip();
    pane_rects(&dev, &fil);
    dev_scroll = glide_step(&dev_glide, dev_scroll,
                            area_max_scroll(g_app.dev_count, PANE_VIEW_H));
    pick_scroll = glide_step(&pick_glide, pick_scroll,
                             area_max_scroll(g_app.picked_count, PANE_VIEW_H));
    dev_pane_render(&dev, g_app.pane_focus == 0);
    pick_pane_render(&fil, g_app.pane_focus == 1);

    /* 页脚只声明"本页有哪些键 + 文案 + 状态"，顺序交给 hintbar 排
     * （默认键序 + 固定靠左、可变靠右）。确认/三角两段随焦点栏变 → 可变段靠右。 */
    HintSeg segs[10] = { 0 };       /* 9 段（含扫码 L 段），页脚模块上限 10 */
    int ns = 0;
    segs[ns].key = HKEY_SELECT;       segs[ns++].text = tr("SELECT Settings");
    segs[ns].key = HKEY_START;        segs[ns++].text = tr("START Web share");
    /* 扫码入口：设置里关掉后页脚不再出现 L 段（功能随之不可用） */
    if (g_cfg.qr_scan) {
        segs[ns].key = HKEY_L;        segs[ns++].text = tr("Scan QR");
    }
    /* 主页按住 R 会展开页头按钮文字：这条 R 段由页面自己声明，不依赖页脚溢出
     * （简/繁中文案短，页脚放得下，自动补的 R 永远不会出现）。 */
    segs[ns].key = HKEY_R;
    segs[ns++].text = tr(ui_input_r_down() ? "Less hints" : "More hints");
    /* 十字键两条：左右切栏（两栏恒可切），上下在焦点栏内移动选中
     * （选到该栏首/末项时对应臂画灰，读起来就是"到头了"） */
    segs[ns].key = HKEY_DPAD;         segs[ns].dir_off = HDIR_VERT;
    segs[ns++].text = tr("Switch");
    {
        int cnt = g_app.pane_focus == 0 ? g_app.dev_count : g_app.picked_count;
        int sel = g_app.pane_focus == 0 ? g_app.dev_sel : g_app.picked_sel;
        uint8_t off = HDIR_HORZ;
        if (g_app.pane_focus == 1 && cnt == 0) {
            /* 空文件栏是两个动作按钮：上下切换，不要画成"到头了" */
            cnt = 2;
            sel = g_app.pick_empty_sel;
        }
        if (sel <= 0)       off |= HDIR_UP;
        if (sel >= cnt - 1) off |= HDIR_DOWN;
        segs[ns].key = HKEY_DPAD;     segs[ns].dir_off = off;
        segs[ns++].text = tr("Choose");
    }
    segs[ns].key = HKEY_SQUARE;       segs[ns++].text = tr("Select files");
    segs[ns].key = HKEY_CONFIRM;      segs[ns].varies = true;
    /* 确认键语义随焦点栏变化：设备栏=发送（一台设备都没有则灰掉）；
     * 文件栏有文件=移除该项，空栏=去选文件 */
    if (g_app.pane_focus == 0) {
        segs[ns].dim = g_app.dev_count <= 0;
        segs[ns++].text = tr("Send");
    } else {
        segs[ns++].text = g_app.picked_count > 0 ? tr("Remove")
                          : (g_app.pick_empty_sel ? tr("Send folder")
                                                  : tr("Select files"));
    }
    segs[ns].key = HKEY_TRIANGLE;     segs[ns].varies = true;
    /* 三角键语义随焦点栏变化：设备栏=短按扫描网段；文件栏=按住蓄满清空已选
     * （蓄力环画在该段图标周围；无已选则灰掉、不画环） */
    if (g_app.pane_focus == 0) {
        segs[ns].dim = api_identity_pending();   /* 首启证书生成中：扫描暂不可用 */
        segs[ns++].text = tr("Scan");
    } else {
        segs[ns].dim = g_app.picked_count <= 0;
        segs[ns++].text = tr("Hold to clear all");
    }
    /* 蓄力环只给文件栏（设备栏那段是"Scan"，按住不该冒出进度环） */
    w_page_footer_charge(HKEY_TRIANGLE,
                         g_app.pane_focus == 1 ? ui_input_alt_charge() : 0.0f);
    w_page_footer_segs(segs, ns);
}

void page_devices_input(const Input *in)
{
    int count = g_app.dev_count;
    PaneRect dev, fil;
    pane_rects(&dev, &fil);

    /* 触摸拖动：按按下点落在哪一栏决定滚哪一栏（触摸不切焦点） */
    if (in->drag_start || in->dragging) {
        bool over_dev = in->drag_x0 >= dev.x && in->drag_x0 < dev.x + dev.w;
        glide_stop(&dev_glide);
        glide_stop(&pick_glide);
        if (over_dev && count > 0)
            area_drag(in, count, &dev_scroll, &dev_press_scroll,
                      &g_app.dev_sel, PANE_TOP, PANE_VIEW_H);
        else if (!over_dev && g_app.picked_count > 0)
            area_drag(in, g_app.picked_count, &pick_scroll, &pick_press_scroll,
                      &g_app.picked_sel, PANE_TOP, PANE_VIEW_H);
        return;   /* 拖动期间不处理其它触摸动作 */
    }
    if (in->drag_end) {
        /* 抬手帧仍带着起点：按这次拖动落在哪一栏，只让那一栏继续滑 */
        bool over_dev = in->drag_x0 >= dev.x && in->drag_x0 < dev.x + dev.w;
        if (over_dev && count > 0)          glide_release(&dev_glide, in);
        else if (!over_dev && g_app.picked_count > 0)
                                            glide_release(&pick_glide, in);
        return;
    }
    glide_stop(&dev_glide);
    glide_stop(&pick_glide);
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id >= WID_DEVS_BASE && id < WID_DEVS_BASE + count) {
            g_app.dev_sel = id - WID_DEVS_BASE;
            try_send(g_app.dev_sel);      /* 点设备即发（无文件则提示） */
        } else if (id >= WID_PICKX_BASE && id < WID_PICKX_BASE + g_app.picked_count) {
            unpick(id - WID_PICKX_BASE);  /* 行尾 ✕：从已选清单移除 */
        } else if (id >= WID_PICKS_BASE && id < WID_PICKS_BASE + g_app.picked_count) {
            g_app.picked_sel = id - WID_PICKS_BASE;
        } else if (id == WID_FILES_PICK) {
            open_files();
        } else if (id == WID_FILES_FOLDER) {
            open_folders();
        } else if (id == WID_HDR_SETTINGS) {
            settings_open();
            g_app.page = PAGE_SETTINGS;
        } else if (id == WID_HDR_QR) {
            open_scan();          /* 不切焦点：页头是全局入口，与左右栏无关 */
        } else if (id == WID_HDR_SCAN) {
            api_scan_trigger();   /* 不切焦点：页头是全局入口，与左右栏无关 */
        } else if (id == WID_HDR_WEB) {
            open_web(g_app.picked_count > 0);   /* 页头只打开页面；有已选才自动开分享 */
        }
        return;
    }
    /* L 肩键：进扫码取景（设置里关掉扫码入口后不响应） */
    if (in->l) {
        if (g_cfg.qr_scan) open_scan();
        return;
    }
    /* 十字键：←/→ 切栏，↑/↓ 在焦点栏内移动 */
    if (in->left || in->right) {
        g_app.pane_focus = g_app.pane_focus ? 0 : 1;
        ui_input_alt_reset();   /* 切栏即作废蓄力：三角语义跟着栏走，别跨栏继承 */
    }
    if (in->up || in->down) {
        if (g_app.pane_focus == 0 && count > 0) {
            if (in->up && g_app.dev_sel > 0) g_app.dev_sel--;
            if (in->down && g_app.dev_sel < count - 1) g_app.dev_sel++;
            area_keep_visible(&dev_scroll, count, g_app.dev_sel, PANE_VIEW_H);
        } else if (g_app.pane_focus == 1 && g_app.picked_count > 0) {
            if (in->up && g_app.picked_sel > 0) g_app.picked_sel--;
            if (in->down && g_app.picked_sel < g_app.picked_count - 1)
                g_app.picked_sel++;
            area_keep_visible(&pick_scroll, g_app.picked_count, g_app.picked_sel,
                              PANE_VIEW_H);
        } else if (g_app.pane_focus == 1) {
            /* 空文件栏：上下在两个动作按钮间切换 */
            g_app.pick_empty_sel = in->up ? 0 : 1;
        }
    }
    if (in->confirm) {
        if (g_app.pane_focus == 0)
            try_send(g_app.dev_sel);
        else if (g_app.picked_count > 0)
            unpick(g_app.picked_sel);
        else if (g_app.pick_empty_sel)
            open_folders();             /* 空文件栏：确认 = 当前焦点动作 */
        else
            open_files();
    }
    if (in->square) open_files();     /* 方块键：随时进文件选择页 */
    if (in->alt) {                    /* 三角键短按：设备栏=扫描网段 */
        if (g_app.pane_focus == 0)
            api_scan_trigger();
        /* 文件栏不响应短按：清空已选改成"按住蓄满"，见下面的 alt_long */
    }
    if (in->alt_long) {               /* 三角键按住蓄满：文件栏清空已选 */
        if (g_app.pane_focus == 1 && g_app.picked_count > 0)
            unpick_all();
    }
    if (in->menu) {
        settings_open();
        g_app.page = PAGE_SETTINGS;
    }
    /* START：网页共享。已选文件 >0 时直接开「下载分享」，否则只进页面选模式。 */
    if (in->start) open_web(g_app.picked_count > 0);
}

/* ================= 文件浏览 ================= */
void files_load(void)
{
    SceUID fd = sceIoDopen(g_app.cur_dir);
    SceIoDirent de;
    bool has_up = strlen(g_app.cur_dir) > 5;   /* 非根目录才有上一级 */
    int n = 0;
    g_app.file_count = 0;
    if (fd < 0) return;
    memset(&de, 0, sizeof de);
    /* 第 0 行固定放 ".."（上级目录）：触摸用户靠它回上一层（✗ 键他们按不到）。
     * 文件系统读出来的 "."/".." 照旧跳过，这项是我们自己造的；不参与下面的
     * 排序，天然钉在首行。根目录不插——无处可回。 */
    if (has_up) {
        strcpy(g_app.files[0].name, "..");
        g_app.files[0].is_dir = true;
        g_app.files[0].size  = 0;
        n = 1;
    }
    while (n < MAX_FILES && sceIoDread(fd, &de) > 0) {
        bool is_dir = (de.d_stat.st_mode & SCE_S_IFDIR) != 0;
        if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0)
            continue;
        /* 目录模式（发送文件夹）：只列目录，不列文件 */
        if (g_app.dir_mode && !is_dir)
            continue;
        strncpy(g_app.files[n].name, de.d_name, sizeof g_app.files[n].name - 1);
        g_app.files[n].name[sizeof g_app.files[n].name - 1] = 0;
        g_app.files[n].is_dir = is_dir;
        g_app.files[n].size = de.d_stat.st_size;
        n++;
    }
    sceIoDclose(fd);
    g_app.file_count = n;
    /* 目录在前，名称升序（".." 是第 0 项，从它后面开始排） */
    int i, j;
    for (i = has_up ? 1 : 0; i < n; i++) {
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

/* files_load 之后光标的默认落点：跳过第 0 行的 ".."，省得每次进目录都先往下按一下
 * （空目录只剩 ".."、根目录没有它时落 0）。 */
int files_first_sel(void)
{
    return (g_app.file_count > 1 && strcmp(g_app.files[0].name, "..") == 0) ? 1 : 0;
}

/* 当前浏览目录 + 文件名拼成完整路径（去重/发送都按它比，不用裸文件名） */
static void join_path(const char *name, char *out, int n)
{
    size_t len = strlen(g_app.cur_dir);
    if (len > 0 && g_app.cur_dir[len - 1] == '/')
        snprintf(out, n, "%s%s", g_app.cur_dir, name);
    else
        snprintf(out, n, "%s/%s", g_app.cur_dir, name);
}

/* 在已选清单里找同一路径；-1 = 未选 */
static int find_picked(const char *path)
{
    int i;
    for (i = 0; i < g_app.picked_count; i++)
        if (strcmp(g_app.picked[i].path, path) == 0) return i;
    return -1;
}

static bool is_picked(int idx)
{
    char path[512];
    join_path(g_app.files[idx].name, path, sizeof path);
    return find_picked(path) >= 0;
}

/* 勾选/取消勾选当前目录第 idx 个文件（按完整路径去重，不同目录同名文件互不影响） */
static void toggle_pick(int idx)
{
    char path[512];
    int k;
    join_path(g_app.files[idx].name, path, sizeof path);
    k = find_picked(path);
    if (k >= 0) {                   /* 已选 → 取消 */
        unpick(k);
        return;
    }
    if (g_app.picked_count >= MAX_PICKED) {
        folder_note_set(tr("Too many files (max %d)"), MAX_PICKED);
        return;
    }
    /* 按 UTF-8 边界截断：文件系统名最长 255 字节，已选栏按 512 字节装，
     * 若切在半个汉字上，这个非法序列会一路带到 prepare 的 JSON 里。 */
    str_copy_utf8(g_app.picked[g_app.picked_count].name,
                  (int)sizeof g_app.picked[0].name, g_app.files[idx].name);
    snprintf(g_app.picked[g_app.picked_count].path, sizeof g_app.picked[0].path,
             "%s", path);
    g_app.picked[g_app.picked_count].size = g_app.files[idx].size;
    g_app.picked_total += g_app.files[idx].size;
    g_app.picked_count++;
}

/* 本目录下的普通文件是否已全被勾选（三角键全选/取消全选用；空目录算未全选） */
static bool all_files_picked(void)
{
    int i, n = 0;
    for (i = 0; i < g_app.file_count; i++) {
        if (g_app.files[i].is_dir) continue;
        n++;
        if (!is_picked(i)) return false;
    }
    return n > 0;
}

/* 三角键：本目录文件全没选满 → 全选；已选满 → 全取消 */
static void toggle_select_all(void)
{
    int i;
    bool all = all_files_picked();
    for (i = 0; i < g_app.file_count; i++) {
        if (g_app.files[i].is_dir) continue;
        if (all) toggle_pick(i);                 /* 全是已选 → 逐个取消 */
        else if (!is_picked(i)) toggle_pick(i);  /* 补上没选的 */
    }
    if (g_app.picked_sel >= g_app.picked_count)
        g_app.picked_sel = g_app.picked_count > 0 ? g_app.picked_count - 1 : 0;
    area_clamp(&pick_scroll, g_app.picked_count, PANE_VIEW_H);
}

/* 记住当前目录：下次进文件选择页直接从这儿开始 */
static void remember_dir(void)
{
    strncpy(pick_dir, g_app.cur_dir, sizeof pick_dir - 1);
    pick_dir[sizeof pick_dir - 1] = 0;
}

/* 文件选择页出口：保留已选、记住目录，回主页（焦点按有无文件定） */
static void done_files(void)
{
    remember_dir();
    g_app.dir_mode = false;
    pane_focus_default();
    g_app.page = PAGE_DEVICES;
}

void enter_dir(const char *name)
{
    size_t len;
    /* 列表首行的 ".." 不是真目录名：直接回上级，别拼出 "ux0:/dir/.." 这种路径 */
    if (strcmp(name, "..") == 0) { parent_dir(); return; }
    len = strlen(g_app.cur_dir);
    if (len > 0 && g_app.cur_dir[len - 1] == '/')
        snprintf(g_app.cur_dir, sizeof g_app.cur_dir, "%s%s", g_app.cur_dir, name);
    else
        snprintf(g_app.cur_dir, sizeof g_app.cur_dir, "%s/%s", g_app.cur_dir, name);
    file_scroll = 0;
    glide_reset(&file_glide);
    remember_dir();
    files_load();
    g_app.file_sel = files_first_sel();   /* 落在第一个真实项上，不停在 ".." */
}

void parent_dir(void)
{
    size_t len = strlen(g_app.cur_dir);
    if (len <= 5) {                 /* 已是 ux0:/ 根 */
        done_files();
        return;
    }
    char *last = strrchr(g_app.cur_dir, '/');
    if (last && last != g_app.cur_dir) {
        *last = 0;
        if (strlen(g_app.cur_dir) < 5) strcpy(g_app.cur_dir, "ux0:/");
    } else {
        strcpy(g_app.cur_dir, "ux0:/");
    }
    file_scroll = 0;
    glide_reset(&file_glide);
    remember_dir();
    files_load();
    g_app.file_sel = files_first_sel();
}

/* ================= 文件夹枚举（后台线程 + 扫描态） =================
 * 目标：把当前目录整棵递归成逐文件条目。FAT 深目录的 opendir/readdir 会阻塞
 * 几十~几百 ms，不能放 UI 线程；枚举在独立线程跑，结果写进静态区，UI 在
 * pages_tick 里看到终态后归并（folder_scan_tick）。
 *
 * 路径口径与接收侧 recv_path.h 一致：相对路径 ≤511 字节、单级 ≤191 字节、
 * 深度 ≤16（含顶层目录）。超出的条目跳过（对端也存不下这种路径）。 */
#define FSCAN_COMP_MAX  191
#define FSCAN_DEPTH_MAX 16
enum {
    FSCAN_IDLE = 0,     /* 空闲（无结果） */
    FSCAN_RUNNING,      /* 线程枚举中 */
    FSCAN_DONE,         /* 有结果，待归并 */
    FSCAN_EMPTY,        /* 整个文件夹为空 */
    FSCAN_TOO_MANY      /* 文件数超过 MAX_PICKED，整批拒绝 */
};

static struct {
    volatile int state;
    volatile int cancel;
    int  n;                     /* 已收集文件数 */
    int  empty_dirs;            /* 空目录数（跳过） */
    int  skipped;               /* 因名字/路径/深度超限被跳过的文件数 */
    char root[512];             /* 被扫描的绝对目录 */
    char top[192];              /* 顶层目录名（相对路径第一级） */
    PickedFile ent[MAX_PICKED];
} s_fs;

static void folder_note_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(folder_note, sizeof folder_note, fmt, ap);
    va_end(ap);
    folder_note_us = (uint64_t)sceKernelGetSystemTimeWide() + 6000000ull;
}

static void dir_msg_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dir_msg, sizeof dir_msg, fmt, ap);
    va_end(ap);
    dir_msg_us = (uint64_t)sceKernelGetSystemTimeWide() + 5000000ull;
}

static void folder_scan_cancel(void)
{
    if (s_fs.state == FSCAN_RUNNING) s_fs.cancel = 1;
}

static bool folder_scan_running(void)
{
    return s_fs.state == FSCAN_RUNNING;
}

/* 丢弃未消费的旧结果（进入目录页时调用；线程运行中不动） */
static void folder_scan_reset(void)
{
    if (s_fs.state != FSCAN_RUNNING) s_fs.state = FSCAN_IDLE;
}

static int fs_add(const char *abs, const char *rel, SceOff size)
{
    if (s_fs.n >= MAX_PICKED) return -1;      /* 超限：交给调用方整批拒绝 */
    snprintf(s_fs.ent[s_fs.n].name, sizeof s_fs.ent[s_fs.n].name, "%s", rel);
    snprintf(s_fs.ent[s_fs.n].path, sizeof s_fs.ent[s_fs.n].path, "%s", abs);
    s_fs.ent[s_fs.n].size = size;
    s_fs.n++;
    return 0;
}

/* 深度优先枚举。depth = rel 的当前层数（顶层目录 = 1）。
 * 返回 0=正常，1=超过文件上限（中止），2=已取消。 */
static int fs_walk(const char *abs, const char *rel, int depth)
{
    SceUID fd = sceIoDopen(abs);
    SceIoDirent de;
    int children = 0, r = 0;

    if (fd < 0) return 0;                 /* 打不开的目录跳过，不算失败 */
    memset(&de, 0, sizeof de);
    while (sceIoDread(fd, &de) > 0) {
        char cabs[512], crel[512];
        bool is_dir;
        int n;

        if (s_fs.cancel) { r = 2; break; }
        if (strcmp(de.d_name, ".") == 0 || strcmp(de.d_name, "..") == 0)
            continue;
        children++;
        is_dir = (de.d_stat.st_mode & SCE_S_IFDIR) != 0;
        /* 单级超长：跳过（文件计数提示，目录只跳过本身，不猜里面有多少文件） */
        if (strlen(de.d_name) > FSCAN_COMP_MAX) {
            if (!is_dir) s_fs.skipped++;
            continue;
        }

        n = snprintf(cabs, sizeof cabs, "%s%s%s", abs,
                     (abs[0] && abs[strlen(abs) - 1] == '/') ? "" : "/",
                     de.d_name);
        if (n < 0 || n >= (int)sizeof cabs) {
            if (!is_dir) s_fs.skipped++;
            continue;
        }

        if (is_dir) {
            if (depth + 1 > FSCAN_DEPTH_MAX) continue;
            n = snprintf(crel, sizeof crel, "%s/%s", rel, de.d_name);
            if (n < 0 || n >= (int)sizeof crel) continue;
            r = fs_walk(cabs, crel, depth + 1);
        } else {
            if (depth + 1 > FSCAN_DEPTH_MAX) { s_fs.skipped++; continue; }
            n = snprintf(crel, sizeof crel, "%s/%s", rel, de.d_name);
            if (n < 0 || n >= (int)sizeof crel) { s_fs.skipped++; continue; }
            if (fs_add(cabs, crel, de.d_stat.st_size) != 0) { r = 1; break; }
        }
        if (r) break;
    }
    sceIoDclose(fd);
    if (r) return r;
    if (children == 0 && depth > 1) s_fs.empty_dirs++;   /* 空目录：跳过并计数 */
    return 0;
}

static int fs_thr(SceSize args, void *argp)
{
    int r;
    (void)args; (void)argp;
    __sync_synchronize();
    r = fs_walk(s_fs.root, s_fs.top, 1);
    __sync_synchronize();
    if (s_fs.cancel)            s_fs.state = FSCAN_IDLE;   /* 用户取消：结果作废 */
    else if (r == 1)            s_fs.state = FSCAN_TOO_MANY;
    else if (s_fs.n == 0)       s_fs.state = FSCAN_EMPTY;
    else                        s_fs.state = FSCAN_DONE;
    sceKernelExitDeleteThread(0);
    return 0;
}

/* 开始扫描 dir（绝对路径）。0=已启动，<0=拒绝（正在扫 / 参数非法 / 线程起不来）。 */
static int folder_scan_start(const char *dir)
{
    size_t len, base;
    SceUID th;

    if (!dir || !dir[0] || s_fs.state == FSCAN_RUNNING) return -1;
    s_fs.n = 0;
    s_fs.empty_dirs = 0;
    s_fs.skipped = 0;
    s_fs.cancel = 0;
    snprintf(s_fs.root, sizeof s_fs.root, "%s", dir);
    /* 顶层目录名 = 路径最后一级 */
    len = strlen(dir);
    while (len > 1 && dir[len - 1] == '/') len--;
    base = len;
    while (base > 0 && dir[base - 1] != '/') base--;
    if (base >= len) return -1;            /* 根目录没有名字：不扫 */
    if (len - base > FSCAN_COMP_MAX) return -1;   /* 顶层名超长：对端存不下 */
    snprintf(s_fs.top, sizeof s_fs.top, "%.*s", (int)(len - base), dir + base);

    th = sceKernelCreateThread("psvsend_fscan", fs_thr, 0x40, 0x20000, 0,
                               0, NULL);
    if (th < 0) {
        dlog("fs: thread create fail 0x%08X", (unsigned)th);
        return -1;
    }
    s_fs.state = FSCAN_RUNNING;
    if (sceKernelStartThread(th, 0, NULL) < 0) {
        sceKernelDeleteThread(th);
        s_fs.state = FSCAN_IDLE;
        return -1;
    }
    return 0;
}

/* 扫描终态 → 归并到已选清单并回主页；空/超限留在目录页提示。
 * 由 pages_tick 每帧调用（页面切换不放在 render 里，避免同帧输入打到新页）。 */
void folder_scan_tick(void)
{
    int st = s_fs.state;
    int i, dup = 0;

    if (st != FSCAN_DONE && st != FSCAN_EMPTY && st != FSCAN_TOO_MANY) return;
    if (g_app.page != PAGE_FILES || !g_app.dir_mode) return;  /* 结果先留着 */

    if (st == FSCAN_EMPTY) {
        s_fs.state = FSCAN_IDLE;
        dir_msg_set("%s", tr("Folder is empty"));
        return;
    }
    if (st == FSCAN_TOO_MANY) {
        s_fs.state = FSCAN_IDLE;
        dir_msg_set(tr("Too many files (max %d)"), MAX_PICKED);
        return;
    }
    /* FSCAN_DONE：按绝对路径去重后整批归并；净新增超限则整批拒绝，不截断 */
    for (i = 0; i < s_fs.n; i++)
        if (find_picked(s_fs.ent[i].path) >= 0) dup++;
    if (g_app.picked_count + (s_fs.n - dup) > MAX_PICKED) {
        s_fs.state = FSCAN_IDLE;
        dir_msg_set(tr("Too many files (max %d)"), MAX_PICKED);
        return;
    }
    for (i = 0; i < s_fs.n; i++) {
        if (find_picked(s_fs.ent[i].path) >= 0) continue;
        g_app.picked[g_app.picked_count] = s_fs.ent[i];
        g_app.picked_total += s_fs.ent[i].size;
        g_app.picked_count++;
    }
    /* 有文件因名字/路径/深度超限被跳过时优先提示（用户最可能没察觉少文件） */
    if (s_fs.skipped > 0)
        folder_note_set(tr("%d file(s) skipped (name too long)"), s_fs.skipped);
    else if (s_fs.empty_dirs > 0)
        folder_note_set(tr("%d empty folder(s) skipped"), s_fs.empty_dirs);
    s_fs.state = FSCAN_IDLE;
    g_app.dir_mode = false;
    done_files();                    /* 回主页，焦点按有无文件落到设备栏 */
}

/* 目录模式下确认发送当前目录（根目录不可发：没有可用的顶层名） */
static void send_current_folder(void)
{
    if (strlen(g_app.cur_dir) <= 5) return;
    if (folder_scan_start(g_app.cur_dir) != 0)
        dir_msg_set("%s", tr("Cannot read folder"));
}

/* 光标停在哪一级就发哪一级：停在子目录行 → 发那个子目录；停在 ".."（或列表
 * 为空）→ 发当前目录。这样在 ux0:/ 根下不用先进入子目录，选中按 □ 就能发。 */
static bool cursor_is_subdir(void)
{
    int s = g_app.file_sel;
    return s >= 0 && s < g_app.file_count && g_app.files[s].is_dir &&
           strcmp(g_app.files[s].name, "..") != 0;
}

static void send_folder_at_cursor(void)
{
    char path[512];
    if (cursor_is_subdir()) {
        join_path(g_app.files[g_app.file_sel].name, path, sizeof path);
        if (folder_scan_start(path) != 0)
            dir_msg_set("%s", tr("Cannot read folder"));
    } else {
        send_current_folder();
    }
}

void page_files_render(void)
{
    int count = g_app.file_count;
    bool dm = g_app.dir_mode;
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();

    /* 目录模式 + 后台枚举中：整页只留扫描态与取消（FAT 深目录不能卡 UI） */
    if (dm && folder_scan_running()) {
        HintSeg segs[2] = { 0 };
        Rect b = { SCR_W - 216, SCR_H - 42, 192, 36 };
        w_page_header(tr("Select folder"));
        w_text_lead(28, 56, 1.0f, theme->text_dim, s_fs.root, SCR_W - 56);
        w_text(40, 150, 1.2f, theme->text, "%s", tr("Scanning folder..."));
        segs[0].key = HKEY_BACK;
        segs[0].text = tr("Cancel");
        w_page_footer_segs(segs, 1);
        w_add(WID_FILES_CANCEL_DIR, b);
        w_button(b, tr("Cancel"), true);
        return;
    }

    w_page_header(tr(dm ? "Select folder" : "Select files"));
    w_text_lead(28, 56, 1.0f, theme->text_dim, g_app.cur_dir,
                SCR_W - (dm ? 360 : 260));
    if (!dm) {   /* 右端显示已选总数：跨目录挑文件时也能看到累计 */
        char cnt[48];
        int cw = 0, ch = 0;
        snprintf(cnt, sizeof cnt, tr("%d selected"), g_app.picked_count);
        w_text_w(1.0f, cnt, &cw, &ch);
        w_text(SCR_W - 28 - cw, 56, 1.0f,
               g_app.picked_count > 0 ? theme->text : theme->text_dim, "%s", cnt);
    } else if (dir_msg_us && now < dir_msg_us) {
        w_text_right(SCR_W - 28, 56, 1.0f, theme->warn, "%s", dir_msg);
    } else {
        dir_msg_us = 0;
    }
    file_scroll = glide_step(&file_glide, file_scroll, list_max_scroll(count));
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
            bool is_up = strcmp(g_app.files[i].name, "..") == 0;
            if (g_app.files[i].is_dir)
                snprintf(main, sizeof main, "%s%s", g_app.files[i].name, DIR_SUFFIX);
            else if (is_picked(i))
                snprintf(main, sizeof main, "* %s", g_app.files[i].name);
            else
                snprintf(main, sizeof main, "%s", g_app.files[i].name);
            if (is_up)
                sub[0] = 0;              /* ".."是我们造的上级入口，不显示"文件夹"副文本 */
            else if (g_app.files[i].is_dir)
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
    {
        HintSeg segs[6] = { 0 };
        int ns = 0;
        /* 只声明本页用到的键，顺序由 hintbar 排。空目录里没有可选项：
         * 选择/打开/全选都灰掉。首行的 ".." 是我们插的上级入口、不是内容，
         * 不能拿它把空目录撑成"非空"（否则只剩 ".." 时全选提示不灰）。 */
        int entries = count;
        bool has;
        uint8_t off;
        if (entries > 0 && strcmp(g_app.files[0].name, "..") == 0) entries--;
        has = entries > 0;
        /* 上下选文件：选到首/末行时把方向键对应臂画灰 */
        off = HDIR_HORZ;
        if (g_app.file_sel <= 0)         off |= HDIR_UP;
        if (g_app.file_sel >= count - 1) off |= HDIR_DOWN;

        if (dm) {
            /* 目录模式：面板只列目录；□ = 发送光标所在目录（子目录行=该子目录，
             * ".."=当前目录）。根目录自身没有顶层名，不可发。 */
            bool root = strlen(g_app.cur_dir) <= 5;
            bool can_send = cursor_is_subdir() || !root;
            Rect send = { SCR_W - 216, SCR_H - 42, 192, 36 };
            Rect cancel = { send.x - 12 - 120, SCR_H - 42, 120, 36 };
            segs[ns].key = HKEY_DPAD;     segs[ns].dim = count <= 0;
            segs[ns].dir_off = off;       segs[ns++].text = tr("Choose");
            segs[ns].key = HKEY_CONFIRM;  segs[ns].dim = !has;
            segs[ns++].text = tr("Open/Pick");
            segs[ns].key = HKEY_BACK;     segs[ns++].text = root ? tr("Back") : tr("Up");
            segs[ns].key = HKEY_SQUARE;   segs[ns].dim = !can_send;
            segs[ns++].text = tr("Send folder");
            w_page_footer_segs(segs, ns);
            w_add(WID_FILES_SEND_DIR, send);
            w_add(WID_FILES_CANCEL_DIR, cancel);
            w_button(send, tr("Send this folder"), !root);
            w_button(cancel, tr("Cancel"), true);
            return;
        }

        segs[ns].key = HKEY_DPAD;         segs[ns].dim = !has;
        segs[ns].dir_off = off;           segs[ns++].text = tr("Choose");
        segs[ns].key = HKEY_CONFIRM;      segs[ns].dim = !has;
        segs[ns++].text = tr("Open/Pick");
        /* ✗ = 回上层目录；到根目录时退回设备页，只是文案换词 */
        segs[ns].key = HKEY_BACK;
        segs[ns++].text = strlen(g_app.cur_dir) <= 5 ? tr("Back") : tr("Up");
        segs[ns].key = HKEY_SQUARE;       segs[ns++].text = tr("Done");
        segs[ns].key = HKEY_TRIANGLE;     segs[ns].dim = !has;
        segs[ns].varies = true;           /* 全选↔取消全选：随状态变 → 靠右 */
        segs[ns++].text = all_files_picked() ? tr("Deselect all") : tr("Select all");
        w_page_footer_segs(segs, ns);
        /* 右下角"完成"触摸按钮（与方块键同义）：保留已选回主页 */
        {
            Rect done = { SCR_W - 216, SCR_H - 42, 192, 36 };
            w_add(WID_FILES_DONE, done);
            w_button(done, tr("Done"), true);
        }
    }
}

void page_files_input(const Input *in)
{
    int count = g_app.file_count;
    bool dm = g_app.dir_mode;

    /* 目录模式 + 枚举中：只响应取消（✗ / 右下按钮）；线程下个目录项就会看到标志 */
    if (dm && folder_scan_running()) {
        if (in->tap) {
            if (w_hit(in->tap_x, in->tap_y) == WID_FILES_CANCEL_DIR)
                folder_scan_cancel();
        } else if (in->back) {
            folder_scan_cancel();
        }
        return;
    }
    if (count > 0 && (in->drag_start || in->dragging)) {
        glide_stop(&file_glide);
        list_drag(in, count, &file_scroll, &file_press_scroll, &g_app.file_sel);
        return;   /* 拖动期间不处理其它触摸动作 */
    }
    if (in->drag_end) {
        if (count > 0) glide_release(&file_glide, in);
        return;
    }
    glide_stop(&file_glide);
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_FILES_CANCEL_DIR) {
            done_files();                 /* 目录模式：取消（不发送） */
        } else if (id == WID_FILES_SEND_DIR) {
            send_current_folder();
        } else if (id == WID_FILES_DONE) {
            done_files();
        } else if (id >= 0 && id < count) {
            if (g_app.files[id].is_dir) enter_dir(g_app.files[id].name);
            else if (!dm) {
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
            else if (!dm) toggle_pick(s);
        }
    }
    if (dm) {
        if (in->square) send_folder_at_cursor();
        if (in->back) parent_dir();
        return;
    }
    if (in->square) done_files();            /* 方块键：完成，回主页 */
    if (in->alt) toggle_select_all();        /* 三角键：本目录全选/取消全选 */
    if (in->back) parent_dir();
}

/* ================= 发送 ================= */
/* 进文件选择页：从上次目录开始（首次 ux0:/），已选清单保留（可跨目录追加） */
static void open_files(void)
{
    g_app.dir_mode = false;
    strncpy(g_app.cur_dir, pick_dir, sizeof g_app.cur_dir - 1);
    g_app.cur_dir[sizeof g_app.cur_dir - 1] = 0;
    file_scroll = 0;
    glide_reset(&file_glide);
    files_load();
    g_app.file_sel = files_first_sel();
    g_app.page = PAGE_FILES;
}

/* 进"发送文件夹"页：同一张浏览页的目录模式（只列目录），从上次目录开始 */
static void open_folders(void)
{
    if (folder_scan_running()) return;
    folder_scan_reset();              /* 丢弃上一轮未消费的结果 */
    dir_msg_us = 0;
    g_app.dir_mode = true;
    strncpy(g_app.cur_dir, pick_dir, sizeof g_app.cur_dir - 1);
    g_app.cur_dir[sizeof g_app.cur_dir - 1] = 0;
    file_scroll = 0;
    glide_reset(&file_glide);
    files_load();
    g_app.file_sel = files_first_sel();
    g_app.page = PAGE_FILES;
}

/* 发起真实发送：dev_idx 是本帧设备快照的下标，这里立刻把目标解析成
 * ip/port/proto/指纹 再交给 xfer 后台线程——不给"设备表变动导致下标漂移"
 * 留窗口（这正是把发送挪到主页设备栏的意义）。 */
static void start_send(int dev_idx)
{
    XferFile *ff;
    int i, n = 0, rc;
    if (g_app.picked_count <= 0) return;
    if (dev_idx < 0 || dev_idx >= g_app.dev_count || !g_app.dev_ip[dev_idx][0])
        return;                   /* 设备刚好离线：不发，留在主页 */
    /* 512 条 × ~1KB 不能放 UI 线程栈（默认栈只有 256KB），堆上一次分配 */
    ff = (XferFile *)malloc(sizeof(XferFile) * (size_t)g_app.picked_count);
    if (!ff) return;
    for (i = 0; i < g_app.picked_count && n < XFER_MAX_FILES; i++) {
        snprintf(ff[n].name, sizeof ff[n].name, "%s", g_app.picked[i].name);
        snprintf(ff[n].path, sizeof ff[n].path, "%s", g_app.picked[i].path);
        ff[n].size = g_app.picked[i].size;
        n++;
    }
    rc = api_send_start(g_app.dev_ip[dev_idx], g_app.dev_port[dev_idx],
                        g_app.dev_proto[dev_idx], g_app.dev_fp[dev_idx], ff, n);
    free(ff);
    if (rc != 0) {
        /* 启动被拒（上一次传输还在收尾）：留在本页，不要切等待页——等待页读的是
         * xfer 的共享快照，那还是上一批的状态，看着会像"这次发失败了"。 */
        return;
    }
    g_app.dev_target = dev_idx;   /* 仅用于进度页返回时恢复选中行 */
    g_app.prog_dir = 0;
    spd_reset();                  /* 上一笔的速度采样作废（间隔 <1s 时不会自动清） */
    xf_scroll = 0;
    g_app.prog_pct = 0;
    g_app.prog_done = false;
    g_app.prog_cancel = false;
    g_app.prog_info = false;
    g_app.prog_ms = 0;
    g_app.prog_running = true;
    g_app.prog_start = sceKernelGetSystemTimeWide();
    sw_focus = 0;                  /* 焦点默认落在"取消/完成"上，验证是次要动作 */
    g_app.page = PAGE_SEND_WAIT;   /* 先进等待页；对端接受后由 pages_tick 切进度页 */
}

/* ================= 发送等待页 =================
 * 只是"等对方在接收端点接受"的过渡页：进度页在对端接受前没有任何进展，
 * 摆在那会让用户以为卡死。对端接受（开始上传）后才由 pages_tick 切进度页。
 * 若在开始上传前就出结果（被拒/连不上），结果就地报在本页，不再跳到进度页
 * 去报——过渡页自己就是"这次请求"的完整反馈面。
 *
 * 交互：主操作是"取消/完成"（焦点态 accent 实底）；https 对端时右侧多一个
 * "验证"按钮，左右切换焦点、确认键激活当前按钮，触摸同义；返回键等价于退出。
 * 等待中=取消（通知后台收尾）；已出结果=退出。都回设备页，已选文件保留。 */
/* 身份验证入口是否可用：与官方一致，只对 https 对端开放（明文对端的
 * fingerprint 是自报随机串，比对没有意义）。对端指纹来自发现公告，发送时
 * 已用证书 pin 校验过同一串，故可信。 */
static bool wait_can_verify(void)
{
    return g_app.dev_target >= 0 && g_app.dev_target < g_app.dev_count &&
           strcmp(g_app.dev_proto[g_app.dev_target], "https") == 0 &&
           g_app.dev_fp[g_app.dev_target][0] != 0;
}

void page_send_wait_render(void)
{
    char line[256], sz[16];
    XferInfo xv;
    int n = g_app.picked_count;
    bool ended, failed;

    api_send_info(&xv);
    ended  = xv.finished;                  /* 开始上传前线程就结束了：被拒/出错 */
    failed = ended && !xv.ok;

    w_page_header(tr("Sending"));
    Rect card = { 24, 76, SCR_W - 48, 210 };
    w_rect(card, theme->card);
    snprintf(line, sizeof line, tr("Target: %s"), g_app.dev_alias[g_app.dev_target]);
    w_text_clip(48, 96, 1.2f, theme->text, line, card.w - 48);
    w_human_size(g_app.picked_total, sz);
    snprintf(line, sizeof line, tr("%d file(s)  total %s"), n, sz);
    w_text(48, 132, 1.0f, theme->text_dim, "%s", line);
    if (ended) {
        const char *m = xv.err[0] ? xv.err
                      : (xv.msg[0] ? xv.msg : tr("Transfer failed"));
        w_text_clip(48, 190, 1.2f, failed ? theme->danger : theme->text,
                    m, card.w - 48);
    } else if (xv.pin_need) {
        /* 对方开了 receive PIN：后台已挂起等输入，系统键盘由 pages_tick 弹 */
        w_text_clip(48, 190, 1.3f, theme->accent,
                    xv.pin_need == 2 ? tr("PIN incorrect, try again.")
                                     : tr("PIN required"), card.w - 48);
        w_text_clip(48, 230, 1.0f, theme->text_dim,
                    tr("Enter the PIN to continue."), card.w - 48);
    } else if (xv.hashing) {
        /* 发送前 sha256 预扫：这段可能几秒到两分钟（8.4MiB/s），必须让用户看到
         * 在动——否则和"卡死"没区别。
         * 进度条按【文件个数】均分：每个文件占 1/N，文件内部再按字节填自己那一段。
         * 这样大小文件混发时观感均匀（大文件在自己那格里慢慢走，小文件一闪而过），
         * 不会出现"整条按字节跑、计数却一直停在 1/N"的别扭。 */
        char cnt[32];
        int cw = 0, ch = 0, pct = 0, k;
        int left = 48, right = card.x + card.w - 24;   /* 与上方文字同宽（912） */
        int n = xv.count > 0 ? xv.count : 1;
        int cur = xv.hash_cur >= 0 ? xv.hash_cur : 0;
        double frac = 1.0;
        Rect bar;
        if (cur < xv.count) {
            SceOff cs = xv.hash_cur_size;
            frac = cs > 0 ? (double)xv.hash_cur_done / (double)cs : 1.0;
            if (frac > 1.0) frac = 1.0;
        }
        pct = (int)(((double)cur + frac) * 100.0 / (double)n);
        w_text_clip(48, 190, 1.3f, theme->accent, tr("Calculating checksum..."),
                    card.w - 48);
        snprintf(cnt, sizeof cnt, "%d/%d", cur + 1, n);
        w_text_w(1.0f, cnt, &cw, &ch);
        bar = (Rect){ left, 234, right - left - cw - 16, 12 };
        w_bar(bar, theme->border, theme->accent, pct);
        /* 段分隔线：每段够宽才画，避免几十个小文件时糊成一片 */
        if (n > 1 && bar.w / n >= 8) {
            for (k = 1; k < n; k++)
                w_rect((Rect){ bar.x + bar.w * k / n, bar.y, 1, bar.h }, theme->card);
        }
        w_text_right(right, 230, 1.0f, theme->text_dim, "%s", cnt);
    } else {
        w_text_clip(48, 190, 1.3f, theme->accent,
                    tr("Waiting for receiver to accept..."), card.w - 48);
        w_text_clip(48, 230, 1.0f, theme->text_dim,
                    tr("Please accept on the other device."), card.w - 48);
    }
    /* 底部按钮：取消/完成（左）+ 验证（右，仅 https 对端）。验证是次要动作，
     * 默认焦点不落在它上面；左右切换，触摸可直接点。 */
    bool can_verify = wait_can_verify();
    const char *act = ended ? tr("Done") : tr("Cancel");
    Rect cancel = can_verify ? (Rect){ 270, 444, 200, 48 }
                             : (Rect){ SCR_W / 2 - 100, 444, 200, 48 };
    Rect vfy = { 490, 444, 200, 48 };
    if (!can_verify) sw_focus = 0;
    w_add(0, cancel);
    w_button(cancel, act, sw_focus == 0);
    if (can_verify) {
        w_add(1, vfy);
        w_button(vfy, tr("Verify"), sw_focus == 1);
    }
    HintSeg segs[4] = { 0 };
    int ns = 0;
    if (can_verify) {
        segs[ns].key = HKEY_DPAD; segs[ns].dir_off = HDIR_VERT;
        segs[ns++].text = tr("Switch");
    }
    if (sw_focus == 1) {
        segs[ns].key = HKEY_CONFIRM; segs[ns++].text = tr("Verify");
        segs[ns].key = HKEY_BACK;    segs[ns++].text = act;
    } else {
        segs[ns].key  = HKEY_CONFIRM;
        segs[ns].key2 = HKEY_BACK;   /* 确认/返回都退出本页 → 合并成「确认/返回 …」 */
        segs[ns++].text = act;
    }
    w_page_footer_segs(segs, ns);
}

/* 等待页退出：还在等就通知后台收尾，然后回设备页（已选文件保留） */
static void wait_exit(void)
{
    XferInfo xv;
    api_send_info(&xv);
    if (!xv.finished) api_send_cancel();
    goto_devices();
}

void page_send_wait_input(const Input *in)
{
    bool can_verify = wait_can_verify();
    if (in->drag_start || in->dragging) return;
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == 0) {
            wait_exit();
        } else if (id == 1 && can_verify) {
            open_verify(g_app.dev_fp[g_app.dev_target],
                        g_app.dev_alias[g_app.dev_target]);
        }
        return;
    }
    if (in->left || in->right) {
        if (can_verify) sw_focus = sw_focus ? 0 : 1;
        return;
    }
    if (in->confirm) {
        if (sw_focus == 1 && can_verify) {
            open_verify(g_app.dev_fp[g_app.dev_target],
                        g_app.dev_alias[g_app.dev_target]);
        } else {
            wait_exit();
        }
        return;
    }
    if (in->back) wait_exit();
}
