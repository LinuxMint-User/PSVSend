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
#include <string.h>
#include <vita2d.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "core/json_util.h"
#include "app/api.h"

static int dev_scroll = 0;         /* 主页设备栏内容像素偏移 */
static int dev_press_scroll = 0;   /* 本次拖动按下时的滚动位置 */
static int pick_scroll = 0;        /* 主页已选文件栏内容像素偏移 */
static int pick_press_scroll = 0;
static int file_scroll = 0;        /* 文件选择页（目录列表）内容像素偏移 */
static int file_press_scroll = 0;
static char pick_dir[512] = "ux0:/";  /* 文件选择页上次所在目录（本次运行内记忆） */
/* 页头下细条的两个来源（提示优先于扫描反馈）：
 *  - 手动扫描的进度/结果：round 结束时边沿检测记下结束时刻，其后约 4s 显示；
 *  - "请先选择文件"提示：没选文件就点设备时短暂显示。 */
static uint64_t dev_scan_end_us = 0;
static int      dev_scan_was = 0;
static int      dev_scan_last_found = 0;
static uint64_t dev_hint_us = 0;

static void start_send(int dev_idx);
static void open_files(void);

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

/* 页头下细条：优先"请先选择文件"提示，其次手动扫描的实时进度/结果
 * （与列表是否为空无关；三角键按下后下一帧起可见，round 结束后约 4s
 * 报告"扫到 N 台"）。两种提示共用这一行，避免挤占两栏的竖直空间。 */
static void dev_strip(void)
{
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    if (dev_hint_us && now < dev_hint_us) {
        w_text(28, 56, 0.9f, theme->warn, "%s", tr("Select files first"));
        return;
    }
    dev_hint_us = 0;
    if (api_scan_active()) {
        int d = api_scan_done(), t = api_scan_total(), f = g_app.dev_count;
        char st[96];
        if (t <= 0) t = 254;
        if (d < 0 || d > t) d = 0;
        dev_scan_was = 1;
        dev_scan_end_us = 0;          /* 新一轮开始，旧的结果提示作废 */
        /* 分母是 /24 上限、不是"必须探完才能用"的门槛：设备入表即可选、可发，
         * 扫描只是后台补全列表。已有设备时明说可继续，免得用户对着 12/254 干等
         * （设备通常 ~0.7s 就出现）；一台未有时不出现该句，那时它只是空话。 */
        if (f > 0)
            snprintf(st, sizeof st,
                     tr("Scanning... %d of %d possible hosts, %d found, you could proceed"),
                     d, t, f);
        else
            snprintf(st, sizeof st, tr("Scanning... %d of %d possible hosts"), d, t);
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

    /* 文件栏聚焦且已选非空：细条改显示选中行的完整路径。行内文件名过长会被
     * 截断，这里能核对到底是哪个文件（尤其删除前确认用）。 */
    if (g_app.pane_focus == 1 && g_app.picked_count > 0) {
        int i = g_app.picked_sel;
        if (i < 0 || i >= g_app.picked_count) i = 0;
        w_text_lead(28, 56, 0.9f, theme->text_dim, g_app.picked[i].path, SCR_W - 56);
    }
}

/* 设备栏：有设备画列表（alias + 型号/类型），否则画网络/扫描状态 */
static void dev_pane_render(const PaneRect *p, bool focused)
{
    int i, count = g_app.dev_count;
    char title[48];
    snprintf(title, sizeof title, tr("Devices (%d)"), count);
    pane_title(p, title, focused);
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

/* 已选文件栏：有文件画清单（行尾 ✕ = 触摸直接删除该项），空则给"选择文件"按钮。
 * 焦点落在"行"上（强调行底），确认键=移除该项；选中行的完整路径由页头下的细条
 * 显示，行内被截断的文件名可在那里核对。 */
static void pick_pane_render(const PaneRect *p, bool focused)
{
    int i, count = g_app.picked_count;
    char title[64];
    snprintf(title, sizeof title, tr("Selected files (%d)"), count);
    pane_title(p, title, focused);
    if (count <= 0) {
        Rect b = { p->x + p->w / 2 - 110, PANE_TOP + 130, 220, 48 };
        w_add(WID_FILES_PICK, b);
        w_button(b, tr("Select files"), true);
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

void page_devices_render(void)
{
    PaneRect dev, fil;
    dev_sync();
    w_page_header("PSVSend");
    dev_strip();
    pane_rects(&dev, &fil);
    dev_pane_render(&dev, g_app.pane_focus == 0);
    pick_pane_render(&fil, g_app.pane_focus == 1);

    /* 页脚只声明"本页有哪些键 + 文案 + 状态"，顺序交给 hintbar 排
     * （默认键序 + 固定靠左、可变靠右）。确认/三角两段随焦点栏变 → 可变段靠右。 */
    HintSeg segs[6] = { 0 };
    int ns = 0;
    segs[ns].key = HKEY_SELECT;       segs[ns++].text = tr("SELECT Settings");
    /* 十字键两条：左右切栏（两栏恒可切），上下在焦点栏内移动选中
     * （选到该栏首/末项时对应臂画灰，读起来就是"到头了"） */
    segs[ns].key = HKEY_DPAD;         segs[ns].dir_off = HDIR_VERT;
    segs[ns++].text = tr("Switch");
    {
        int cnt = g_app.pane_focus == 0 ? g_app.dev_count : g_app.picked_count;
        int sel = g_app.pane_focus == 0 ? g_app.dev_sel : g_app.picked_sel;
        uint8_t off = HDIR_HORZ;
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
        segs[ns++].text = g_app.picked_count > 0 ? tr("Remove") : tr("Select files");
    }
    segs[ns].key = HKEY_TRIANGLE;     segs[ns].varies = true;
    /* 三角键语义随焦点栏变化：设备栏=短按扫描网段；文件栏=按住蓄满清空已选
     * （蓄力环画在该段图标周围；无已选则灰掉、不画环） */
    if (g_app.pane_focus == 0) {
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
        if (over_dev && count > 0)
            area_drag(in, count, &dev_scroll, &dev_press_scroll,
                      &g_app.dev_sel, PANE_TOP, PANE_VIEW_H);
        else if (!over_dev && g_app.picked_count > 0)
            area_drag(in, g_app.picked_count, &pick_scroll, &pick_press_scroll,
                      &g_app.picked_sel, PANE_TOP, PANE_VIEW_H);
        return;   /* 拖动期间不处理其它触摸动作 */
    }
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
        }
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
        }
    }
    if (in->confirm) {
        if (g_app.pane_focus == 0)
            try_send(g_app.dev_sel);
        else if (g_app.picked_count > 0)
            unpick(g_app.picked_sel);
        else
            open_files();   /* 文件栏空态：确认 = 进文件选择页 */
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
    if (g_app.picked_count >= MAX_PICKED) return;
    /* 按 UTF-8 边界截断：256 字节的名字塞进 128 字节的已选栏，若切在半个汉字上，
     * 这个非法序列会一路带到 prepare 的 JSON 里。 */
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
    pane_focus_default();
    g_app.page = PAGE_DEVICES;
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
    remember_dir();
    files_load();
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
    g_app.file_sel = 0;
    file_scroll = 0;
    remember_dir();
    files_load();
}

void page_files_render(void)
{
    int count = g_app.file_count;
    w_page_header(tr("Select files"));
    w_text_lead(28, 56, 1.0f, theme->text_dim, g_app.cur_dir, SCR_W - 260);
    {   /* 右端显示已选总数：跨目录挑文件时也能看到累计 */
        char cnt[48];
        int cw = 0, ch = 0;
        snprintf(cnt, sizeof cnt, tr("%d selected"), g_app.picked_count);
        w_text_w(1.0f, cnt, &cw, &ch);
        w_text(SCR_W - 28 - cw, 56, 1.0f,
               g_app.picked_count > 0 ? theme->text : theme->text_dim, "%s", cnt);
    }
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
    HintSeg segs[6] = { 0 };
    int ns = 0;
    /* 只声明本页用到的键，顺序由 hintbar 排。空目录里没有可选项：
     * 选择/打开/全选都灰掉 */
    bool has = count > 0;
    /* 上下选文件：选到首/末行时把方向键对应臂画灰 */
    uint8_t off = HDIR_HORZ;
    if (g_app.file_sel <= 0)         off |= HDIR_UP;
    if (g_app.file_sel >= count - 1) off |= HDIR_DOWN;
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
    Rect done = { SCR_W - 216, SCR_H - 42, 192, 36 };
    w_add(WID_FILES_DONE, done);
    w_button(done, tr("Done"), true);
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
        if (id == WID_FILES_DONE) {
            done_files();
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
    if (in->square) done_files();            /* 方块键：完成，回主页 */
    if (in->alt) toggle_select_all();        /* 三角键：本目录全选/取消全选 */
    if (in->back) parent_dir();
}

/* ================= 发送 ================= */
/* 进文件选择页：从上次目录开始（首次 ux0:/），已选清单保留（可跨目录追加） */
static void open_files(void)
{
    strncpy(g_app.cur_dir, pick_dir, sizeof g_app.cur_dir - 1);
    g_app.cur_dir[sizeof g_app.cur_dir - 1] = 0;
    g_app.file_sel = 0;
    file_scroll = 0;
    files_load();
    g_app.page = PAGE_FILES;
}

/* 发起真实发送：dev_idx 是本帧设备快照的下标，这里立刻把目标解析成
 * ip/port/proto/指纹 再交给 xfer 后台线程——不给"设备表变动导致下标漂移"
 * 留窗口（这正是把发送挪到主页设备栏的意义）。 */
static void start_send(int dev_idx)
{
    XferFile ff[XFER_MAX_FILES];
    int i, n = 0;
    if (g_app.picked_count <= 0) return;
    if (dev_idx < 0 || dev_idx >= g_app.dev_count || !g_app.dev_ip[dev_idx][0])
        return;                   /* 设备刚好离线：不发，留在主页 */
    for (i = 0; i < g_app.picked_count && n < XFER_MAX_FILES; i++) {
        snprintf(ff[n].name, sizeof ff[n].name, "%s", g_app.picked[i].name);
        snprintf(ff[n].path, sizeof ff[n].path, "%s", g_app.picked[i].path);
        ff[n].size = g_app.picked[i].size;
        n++;
    }
    if (api_send_start(g_app.dev_ip[dev_idx], g_app.dev_port[dev_idx],
                       g_app.dev_proto[dev_idx], g_app.dev_fp[dev_idx], ff, n) != 0) {
        /* 启动被拒（上一次传输还在收尾）：留在本页，不要切等待页——等待页读的是
         * xfer 的共享快照，那还是上一批的状态，看着会像"这次发失败了"。 */
        return;
    }
    g_app.dev_target = dev_idx;   /* 仅用于进度页返回时恢复选中行 */
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
    g_app.page = PAGE_SEND_WAIT;   /* 先进等待页；对端接受后由 pages_tick 切进度页 */
}

/* ================= 发送等待页 =================
 * 只是"等对方在接收端点接受"的过渡页：进度页在对端接受前没有任何进展，
 * 摆在那会让用户以为卡死。对端接受（开始上传）后才由 pages_tick 切进度页。
 * 若在开始上传前就出结果（被拒/连不上），结果就地报在本页，不再跳到进度页
 * 去报——过渡页自己就是"这次请求"的完整反馈面。
 *
 * 交互：本页只有一个主操作，画成焦点态（accent 实底），页脚按语义确认键的
 * 图标提示（确认键=激活该按钮），触摸按钮同义；返回键是等价的退出（与进度
 * 页一致），不再单独提示，免得一个动作在页脚出现两条一样的说明。
 * 等待中=取消（通知后台收尾）；已出结果=退出。都回设备页，已选文件保留。 */
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
    } else {
        w_text_clip(48, 190, 1.3f, theme->accent,
                    tr("Waiting for receiver to accept..."), card.w - 48);
        w_text_clip(48, 230, 1.0f, theme->text_dim,
                    tr("Please accept on the other device."), card.w - 48);
    }
    Rect btn = { SCR_W / 2 - 100, 444, 200, 48 };
    w_add(0, btn);
    w_button(btn, ended ? tr("Done") : tr("Cancel"), true);   /* 唯一动作，焦点态 */
    HintSeg segs[2] = { 0 };
    int ns = 0;
    segs[ns].key  = HKEY_CONFIRM;
    segs[ns].key2 = HKEY_BACK;   /* 确认/返回都退出本页 → 合并成「确认/返回 …」 */
    segs[ns++].text = ended ? tr("Done") : tr("Cancel");
    w_page_footer_segs(segs, ns);
}

void page_send_wait_input(const Input *in)
{
    XferInfo xv;
    if (in->drag_start || in->dragging) return;
    if (in->tap) {
        if (w_hit(in->tap_x, in->tap_y) == 0) {
            api_send_info(&xv);
            if (!xv.finished) api_send_cancel();   /* 还在等 → 通知后台收尾 */
            goto_devices();
        }
        return;
    }
    if (in->confirm || in->back) {
        api_send_info(&xv);
        if (!xv.finished) api_send_cancel();
        goto_devices();
    }
}
