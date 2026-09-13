/* 页面：设置 / 目录选择（自 pages.c 拆出；纯搬移，行为不变）。
 * 设置页行 = 分组标题(不可选) + 设置项，整页像素滚动；目录选择页供设置页与
 * 接收设置页共用（仅列文件夹，选定目标 = 当前进入到的目录）。 */
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/i18n.h"
#include "app/update.h"
#include "app/ime.h"

/* ================= 设置 =================
 * 设置页行 = 分组标题(不可选) + 设置项。整页像素滚动（模型同设备/文件列表）：
 * 可视区 LIST_TOP..LIST_BOTTOM，内容总高超出时拖动跟手、方向键自动滚到选中项。
 * g_app.set_sel 存设置项 id（SET_ITEM_*）；分组标题不参与选择。
 * 每行触摸分成两半：左半=上一档，右半=下一档（主机名行点任意半开键盘改名）。 */
enum {
    SET_ITEM_THEME = 0,    /* 显示：主题 */
    SET_ITEM_LANG,         /* 显示：界面语言 */
    SET_ITEM_KEY,          /* 操作：确认键布局 */
    SET_ITEM_HOSTNAME,     /* 设备：主机名（改名走系统键盘） */
    SET_ITEM_SAVEDIR,      /* 存储：默认保存目录（动作行：进目录选择器并落盘） */
    SET_ITEM_CHECK,        /* 更新：检查更新（动作行：按任意键/点任意半即查） */
    SET_ITEM_AUTO,         /* 更新：自动检查频率 */
    SET_ITEM_N
};

/* 渲染行槽（顺序即页面顺序）：HDR=分组标题 ITEM=设置项 HINT=提示行 */
enum {
    SET_SLOT_HDR_A = 0,    /* 分组：设备 */
    SET_SLOT_HOST,
    SET_SLOT_HDR_B,        /* 分组：显示 */
    SET_SLOT_THEME,
    SET_SLOT_LANG,
    SET_SLOT_HDR_C,        /* 分组：操作 */
    SET_SLOT_KEY,
    SET_SLOT_HINT,
    SET_SLOT_HDR_ST,       /* 分组：存储 */
    SET_SLOT_SAVEDIR,
    SET_SLOT_HDR_UPD,      /* 分组：更新 */
    SET_SLOT_CHECK,
    SET_SLOT_AUTO,
    SET_SLOT_HDR_D,        /* 分组：关于（页面最底部） */
    SET_SLOT_ABOUT_A,      /* logo：PSVSend 大字 + 版本号 */
    SET_SLOT_ABOUT_B,      /* 适配说明行 */
    SET_SLOT_ABOUT_C,      /* 项目仓库地址行（只读提示） */
    SET_SLOT_N
};
#define SET_HDR_H   44        /* 分组标题行高（含上方留白） */
#define SET_ROW_H   60        /* 设置项行步进（视觉行 56） */
#define SET_HINT_H  30        /* 结尾提示行高 */
#define SET_LOGO_H  60        /* 关于组 logo 行（大字，底部再留白） */
#define SET_ADAPT_H 40        /* 关于组适配说明行 */

static int set_scroll = 0;        /* 设置页内容偏移 */
static int set_press_scroll = 0;

/* 供 pages_warm_all 预热设置页：设置内容滚动偏移（0=顶，大值=由 render 夹到底） */
void settings_scroll_to(int v) { set_scroll = v; }

static int slot_item(int slot)
{
    switch (slot) {
    case SET_SLOT_HOST:  return SET_ITEM_HOSTNAME;
    case SET_SLOT_THEME: return SET_ITEM_THEME;
    case SET_SLOT_LANG:  return SET_ITEM_LANG;
    case SET_SLOT_KEY:   return SET_ITEM_KEY;
    case SET_SLOT_SAVEDIR: return SET_ITEM_SAVEDIR;
    case SET_SLOT_CHECK: return SET_ITEM_CHECK;
    case SET_SLOT_AUTO:  return SET_ITEM_AUTO;
    }
    return -1;
}

static int item_slot(int item)
{
    switch (item) {
    case SET_ITEM_HOSTNAME: return SET_SLOT_HOST;
    case SET_ITEM_SAVEDIR:  return SET_SLOT_SAVEDIR;
    case SET_ITEM_THEME:    return SET_SLOT_THEME;
    case SET_ITEM_LANG:     return SET_SLOT_LANG;
    case SET_ITEM_KEY:      return SET_SLOT_KEY;
    case SET_ITEM_CHECK:    return SET_SLOT_CHECK;
    case SET_ITEM_AUTO:     return SET_SLOT_AUTO;
    }
    return -1;
}

static int set_row_h(int slot)
{
    if (slot == SET_SLOT_HINT)   return SET_HINT_H;
    if (slot == SET_SLOT_ABOUT_A) return SET_LOGO_H;
    if (slot == SET_SLOT_ABOUT_B) return SET_ADAPT_H;
    if (slot == SET_SLOT_ABOUT_C) return SET_ADAPT_H;
    return slot_item(slot) >= 0 ? SET_ROW_H : SET_HDR_H;
}

static int set_row_top(int slot)
{
    int y = 0, s;
    for (s = 0; s < slot; s++) y += set_row_h(s);
    return y;
}

static int set_content_h(void)
{
    int y = 0, s;
    for (s = 0; s < SET_SLOT_N; s++) y += set_row_h(s);
    return y;
}

static void set_clamp_scroll(void)
{
    int m = set_content_h() - LIST_VIEW_H;
    if (m < 0) m = 0;
    if (set_scroll < 0) set_scroll = 0;
    if (set_scroll > m) set_scroll = m;
}

/* 从 slot 出发沿 dir 找下一个可选项行槽（跳过分组标题），越界原地不动 */
static int slot_step(int slot, int dir)
{
    int s = slot;
    for (;;) {
        s += dir;
        if (s < 0 || s >= SET_SLOT_N) return slot;
        if (slot_item(s) >= 0) return s;
    }
}

/* 方向键移动选中后保持整行可见（最小滚动量） */
static void set_keep_visible(void)
{
    int slot = item_slot(g_app.set_sel);
    int top;
    if (slot < 0) return;
    top = set_row_top(slot);
    if (top < set_scroll) set_scroll = top;
    if (top + SET_ROW_H > set_scroll + LIST_VIEW_H)
        set_scroll = top + SET_ROW_H - LIST_VIEW_H;
    set_clamp_scroll();
}

/* 改设置项并落盘：主题/语言循环切换、键位翻转；dir=±1 上一档/下一档 */
static void settings_change(int item, int dir)
{
    if (item == SET_ITEM_THEME) {
        g_app.theme_id += dir;
        if (g_app.theme_id < 0) g_app.theme_id = THEME_COUNT - 1;
        if (g_app.theme_id >= THEME_COUNT) g_app.theme_id = 0;
        g_cfg.theme_id = g_app.theme_id;
        theme_set(g_app.theme_id);
        config_save();
    } else if (item == SET_ITEM_LANG) {
        int p = i18n_lang_pref() + dir;
        if (p < I18N_LANG_AUTO) p = I18N_LANG_ZH;      /* 0↔1↔2 循环 */
        if (p >= I18N_LANG_COUNT) p = I18N_LANG_AUTO;
        i18n_set_lang(p);          /* 写 cfg + 存盘 + 重解析：立即生效 */
    } else if (item == SET_ITEM_KEY) {
        g_app.confirm_layout = g_app.confirm_layout ? 0 : 1;
        g_cfg.confirm_layout = g_app.confirm_layout;
        config_save();
    } else if (item == SET_ITEM_CHECK) {
        update_check_now();      /* 动作行：左右/确认/点任意半都触发检查（dir 无意义） */
    } else if (item == SET_ITEM_AUTO) {
        g_cfg.upd_auto += dir;
        if (g_cfg.upd_auto < 0) g_cfg.upd_auto = 3;
        if (g_cfg.upd_auto > 3) g_cfg.upd_auto = 0;
        config_save();
    }
    /* SET_ITEM_HOSTNAME：动作在 input 里走 ask_ime_host（系统键盘），不落档位循环 */
}

void page_settings_render(void)
{
    static const char *upd_mode_en[] = { "Off", "Daily", "Weekly", "Monthly" };
    char theme_v[64], layout_v[96], lang_v[32];
    char upd_v[64];
    int upd_st = update_state();
    int slot;

    w_page_header(tr("Settings"));
    set_clamp_scroll();
    snprintf(theme_v, sizeof theme_v, "%s", theme_names[g_cfg.theme_id]);
    snprintf(layout_v, sizeof layout_v, tr("%s confirm / %s back (%s)"),
             key_confirm(), key_back(),
             g_app.confirm_layout == 0 ? "US" : "JP");
    snprintf(lang_v, sizeof lang_v, "%s", i18n_lang_name(i18n_lang_pref()));
    upd_v[0] = 0;
    switch (upd_st) {
    case UPD_WORKING:
        snprintf(upd_v, sizeof upd_v, "%s", tr("Checking...")); break;
    case UPD_NEW:
        snprintf(upd_v, sizeof upd_v, tr("%s available"), update_latest());
        break;
    case UPD_NONE:
        snprintf(upd_v, sizeof upd_v, "%s", tr("Up to date")); break;
    case UPD_FAIL:
        snprintf(upd_v, sizeof upd_v, "%s", tr("Check failed")); break;
    default: break;                     /* UPD_IDLE：无右值 */
    }

    vita2d_enable_clipping();
    vita2d_set_clip_rectangle(0, LIST_TOP, SCR_W, LIST_BOTTOM);
    for (slot = 0; slot < SET_SLOT_N; slot++) {
        int item = slot_item(slot);
        int top = LIST_TOP - set_scroll + set_row_top(slot);
        int h = set_row_h(slot);
        if (top + h <= LIST_TOP) continue;      /* 整行滚到可视区上方外 */
        if (top >= LIST_BOTTOM) break;          /* 以下都滚到可视区下方外 */
        if (item < 0) {
            /* 分组标题 / 提示行 / 关于区（只读，不注册触摸） */
            const char *txt = NULL;
            switch (slot) {
            case SET_SLOT_HDR_A: txt = tr("Device"); break;
            case SET_SLOT_HDR_B: txt = tr("Display"); break;
            case SET_SLOT_HDR_C: txt = tr("Controls"); break;
            case SET_SLOT_HDR_ST: txt = tr("Storage"); break;
            case SET_SLOT_HDR_UPD: txt = tr("Update"); break;
            case SET_SLOT_HDR_D: txt = tr("About"); break;
            default: break; /* 下方各自 case，绝不落到空绘制 */
            }
            if (txt)
                w_text(28, top + 12, 1.05f, theme->text_dim, "%s", txt);
            if (slot == SET_SLOT_HINT)
                w_text(28, top + 6, 0.9f, theme->text_dim, "%s",
                       tr("Tap row halves to change"));
            if (slot == SET_SLOT_ABOUT_A) {
                /* 文字 logo：PSVSend 主色大字 + 右侧版本号 */
                int lw = 0, lh = 0;
                w_text_w(1.3f, "PSVSend", &lw, &lh);
                w_text(28, top + 16, 1.3f, theme->accent, "PSVSend");
                w_text(28 + lw + 16, top + 20, 1.0f, theme->text_dim,
                       "v" PSVSEND_APP_VERSION);
            }
            if (slot == SET_SLOT_ABOUT_B)
                w_text(28, top + 12, 0.9f, theme->text_dim, "%s",
                       tr("LocalSend client v1.15+ compatible (protocol v2.0)"));
            if (slot == SET_SLOT_ABOUT_C)
                w_text(28, top + 12, 0.9f, theme->text_dim, "%s",
                       "github.com/LinuxMint-User/PSVSend");
            continue;
        }
        Rect r = { 24, top, SCR_W - 48, SET_ROW_H - 4 };
        w_add(item * 2,     (Rect){ r.x, r.y, r.w / 2, r.h });
        w_add(item * 2 + 1, (Rect){ r.x + r.w / 2, r.y, r.w - r.w / 2, r.h });
        switch (item) {
        case SET_ITEM_HOSTNAME:
            w_row(r, tr("Hostname"),
                  g_cfg.alias[0] ? g_cfg.alias : DEFAULT_ALIAS,
                  item == g_app.set_sel);
            break;
        case SET_ITEM_SAVEDIR: {
            /* 值可能是长路径：label 一行、路径 clip 下一行（选中=整行反色） */
            uint32_t card = item == g_app.set_sel ? theme->accent : theme->card;
            uint32_t tc = item == g_app.set_sel ? theme->accent_text : theme->text;
            uint32_t dc = item == g_app.set_sel ? theme->accent_text : theme->text_dim;
            w_rect(r, card);
            w_text(r.x + 24, r.y + 3, 1.25f, tc, "%s", tr("Save folder"));
            w_text_clip(r.x + 24, r.y + 31, 1.0f, dc, g_cfg.save_dir,
                        r.w - 48);
            break;
        }
        case SET_ITEM_THEME:
            w_row(r, tr("Theme"), theme_v, item == g_app.set_sel);
            break;
        case SET_ITEM_LANG:
            w_row(r, tr("Language"), lang_v, item == g_app.set_sel);
            break;
        case SET_ITEM_KEY:
            w_row(r, tr("Confirm key"), layout_v, item == g_app.set_sel);
            break;
        case SET_ITEM_CHECK:
            /* 动作行：右侧显示检查状态；发现新版时用 accent 强调 */
            if (upd_st == UPD_NEW)
                w_row_c(r, tr("Check for updates"), upd_v,
                        item == g_app.set_sel ? theme->accent_text : theme->accent,
                        item == g_app.set_sel);
            else
                w_row(r, tr("Check for updates"), upd_v, item == g_app.set_sel);
            break;
        case SET_ITEM_AUTO:
            w_row(r, tr("Auto check"), tr(upd_mode_en[g_cfg.upd_auto]),
                  item == g_app.set_sel);
            break;
        }
    }
    vita2d_disable_clipping();

    /* 内容超长时的右侧细滚动条（后续设置项多了自动出现） */
    {
        int m = set_content_h() - LIST_VIEW_H;
        if (m > 0) {
            int bh = LIST_VIEW_H * LIST_VIEW_H / set_content_h();
            if (bh < 24) bh = 24;
            int by = LIST_TOP + (LIST_VIEW_H - bh) * set_scroll / m;
            w_rect((Rect){ 936, LIST_TOP, 4, LIST_VIEW_H }, theme->card);
            w_rect((Rect){ 936, by, 4, bh }, theme->text_dim);
        }
    }

    HintSeg segs[6] = { 0 };
    int ns = 0;
    segs[ns].icon = HICON_DPAD;      segs[ns++].text = tr("Choose");
    segs[ns].icon = icon_confirm();  segs[ns++].text = tr("Change");
    segs[ns].icon = icon_back();     segs[ns++].text = tr("Back");
    w_page_footer_segs(segs, ns);
}

void page_settings_input(const Input *in)
{
    if (in->drag_start || in->dragging) {
        int m = set_content_h() - LIST_VIEW_H;
        if (m < 0) m = 0;
        if (in->drag_start) set_press_scroll = set_scroll;
        int ns = set_press_scroll - in->drag_dy;
        if (ns < 0) ns = 0;
        if (ns > m) ns = m;
        set_scroll = ns;
        return;
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id >= 0) {
            int item = id / 2;
            if (item >= 0 && item < SET_ITEM_N) {
                g_app.set_sel = item;
                if (item == SET_ITEM_HOSTNAME) ask_ime_host();
                else if (item == SET_ITEM_SAVEDIR)
                    open_dir_pick(PAGE_SETTINGS, true, g_cfg.save_dir);
                else settings_change(item, (id & 1) ? 1 : -1);
            }
        }
        return;
    }
    if (in->up || in->down) {
        int slot = item_slot(g_app.set_sel);
        slot = slot_step(slot, in->down ? 1 : -1);
        g_app.set_sel = slot_item(slot);
        set_keep_visible();
    }
    if (in->left || in->right || in->confirm) {
        int item = g_app.set_sel;
        if (item == SET_ITEM_HOSTNAME) ask_ime_host();
        else if (item == SET_ITEM_SAVEDIR)
            open_dir_pick(PAGE_SETTINGS, true, g_cfg.save_dir);
        else settings_change(item, in->left ? -1 : 1);
    }
    if (in->back) g_app.page = PAGE_DEVICES;
}

/* ================= 目录选择页 =================
 * 设置页"默认保存目录"与接收 Setup"本次目录"共用：仅列文件夹，选定目标 =
 * "当前进入到的目录"（cur_dir），点文件夹进入、方块键或右下按钮确认。 */
static PageId dpick_origin = PAGE_SETTINGS;
static bool   dpick_persist = false;  /* true=写 config saveDir（设置页）；false=写本次 recv_dir */
static int    dpick_dirs[MAX_FILES];  /* files[] 中目录行下标（渲染/焦点按此索引） */
static int    dpick_dcount = 0;
static int    dpick_scroll = 0;
static int    dpick_press_scroll = 0;

/* 从 files[] 重建目录行索引（文件浏览列表可能混有文件，只给目录当候选） */
static void dpick_build(void)
{
    int n = 0, i;
    for (i = 0; i < g_app.file_count && n < MAX_FILES; i++)
        if (g_app.files[i].is_dir) dpick_dirs[n++] = i;
    dpick_dcount = n;
    if (g_app.file_sel >= n) g_app.file_sel = n > 0 ? n - 1 : 0;
}

void open_dir_pick(PageId origin, bool persist, const char *start_dir)
{
    dpick_origin = origin;
    dpick_persist = persist;
    strncpy(g_app.cur_dir, start_dir, sizeof g_app.cur_dir - 1);
    g_app.cur_dir[sizeof g_app.cur_dir - 1] = 0;
    g_app.file_sel = 0;
    dpick_scroll = 0;
    files_load();
    dpick_build();
    g_app.page = PAGE_DIR_PICK;
}

static void dpick_enter(int dn)
{
    if (dn < 0 || dn >= dpick_dcount) return;
    enter_dir(g_app.files[dpick_dirs[dn]].name);
    dpick_build();
    dpick_scroll = 0;
}

static void dpick_up(void)
{
    size_t len = strlen(g_app.cur_dir);
    if (len <= 5) {                 /* 已在 ux0:/ 根：回来源页，不改任何目录 */
        g_app.page = dpick_origin;
        return;
    }
    parent_dir();
    dpick_build();
    dpick_scroll = 0;
}

/* 确认当前目录：persist=写 config saveDir 并落盘；否则只写内存 recv_dir */
static void dpick_save(void)
{
    if (dpick_persist) {
        snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", g_app.cur_dir);
        config_save();
    } else {
        snprintf(g_app.recv_dir, sizeof g_app.recv_dir, "%s", g_app.cur_dir);
    }
    if (dpick_origin == PAGE_SETTINGS) g_app.set_sel = SET_ITEM_SAVEDIR;
    else g_app.inc_sel = 0;         /* 回接收设置页并高亮目录行 */
    g_app.page = dpick_origin;
}

void page_dir_pick_render(void)
{
    int i;
    w_page_header(tr("Choose folder"));
    w_text_clip(28, 56, 1.0f, theme->text_dim, g_app.cur_dir, SCR_W - 56);
    if (dpick_dcount > 0) {
        clamp_scroll(&dpick_scroll, dpick_dcount);
        vita2d_enable_clipping();
        vita2d_set_clip_rectangle(0, LIST_TOP, SCR_W, LIST_BOTTOM);
        for (i = dpick_scroll / ROW_STRIDE; i < dpick_dcount; i++) {
            int top = LIST_TOP - dpick_scroll + i * ROW_STRIDE;
            if (top >= LIST_BOTTOM) break;
            Rect r = { 24, top, SCR_W - 48, ROW_H };
            add_row_hit(i, top);
            char main[280];
            snprintf(main, sizeof main, "%s%s",
                     g_app.files[dpick_dirs[i]].name, DIR_SUFFIX);
            w_row(r, main, tr("folder"), i == g_app.file_sel);
        }
        vita2d_disable_clipping();
    } else {
        w_text(28, LIST_TOP + 8, 1.1f, theme->text_dim, "%s",
               tr("(empty folder)"));
    }
    /* 右下"存到此处"按钮，左侧同行走当前目录路径（即将写入的路径） */
    Rect bt = { SCR_W - 280, SCR_H - 42, 256, 36 };
    w_add(WID_DIRPICK_SAVE, bt);
    w_button(bt, tr("Save here"), true);
    w_text_clip(24, SCR_H - 38, 1.0f, theme->text_dim,
                g_app.cur_dir, SCR_W - 296);
    HintSeg segs[6] = { 0 };
    int ns = 0;
    /* 空目录里没有可选项：选择/打开灰掉 */
    bool has = dpick_dcount > 0;
    segs[ns].icon = HICON_DPAD;      segs[ns].dim = !has;
    segs[ns++].text = tr("Choose");
    segs[ns].icon = icon_confirm();  segs[ns].dim = !has;
    segs[ns++].text = tr("Open");
    segs[ns].icon = HICON_SQUARE;    segs[ns++].text = tr("Save here");
    /* 根目录无"上级"：返回键此时=退出选择（回来源页），提示随层级切换 */
    segs[ns].icon = icon_back();
    segs[ns++].text = strlen(g_app.cur_dir) <= 5 ? tr("Back") : tr("Up");
    w_page_footer_segs(segs, ns);
}

void page_dir_pick_input(const Input *in)
{
    if (dpick_dcount > 0 && (in->drag_start || in->dragging)) {
        list_drag(in, dpick_dcount, &dpick_scroll, &dpick_press_scroll,
                  &g_app.file_sel);
        return;                     /* 拖动期间不处理其它触摸动作 */
    }
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_DIRPICK_SAVE) { dpick_save(); return; }
        if (id >= 0 && id < dpick_dcount) {
            g_app.file_sel = id;
            dpick_enter(id);
        }
        return;
    }
    if (in->up && g_app.file_sel > 0) {
        g_app.file_sel--;
        keep_sel_visible(&dpick_scroll, dpick_dcount, g_app.file_sel);
    }
    if (in->down && g_app.file_sel < dpick_dcount - 1) {
        g_app.file_sel++;
        keep_sel_visible(&dpick_scroll, dpick_dcount, g_app.file_sel);
    }
    if (in->confirm && dpick_dcount > 0)
        dpick_enter(g_app.file_sel);
    if (in->square) { dpick_save(); return; }
    if (in->back) dpick_up();
}
