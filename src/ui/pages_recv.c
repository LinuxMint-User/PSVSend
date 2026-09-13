/* 页面：接收确认 / 接收设置（自 pages.c 拆出；纯搬移，行为不变）。
 * 含系统键盘"改名/改主机名"挂起事务（page_ime_pump）与"新接收请求自动弹窗"
 * （pages_tick/open_recv_request）。进度页在 pages_progress.c（收发共用）。 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <vita2d.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/i18n.h"
#include "app/ime.h"
#include "app/api.h"

/* 接收确认焦点：0=Reject 1=Setup 2=Accept */
static int recv_focus = 2;
static int rs_scroll = 0;         /* 接收设置页文件列表内容偏移 */
static int rs_press_scroll = 0;
/* 接收设置改名已接系统键盘（ime_ask）：rename_pop 保留为恒 false 的清理位，
 * 避免牵连各页退出时的复位赋值；真正的输入弹窗由 ask_rename() 直接驱动 */
static bool rename_pop = false;

static void start_recv(void);

/* 当前勾选接收的文件数 */
static int recv_included(void)
{
    int i, n = 0;
    for (i = 0; i < g_app.inc_count && i < MAX_INF; i++)
        if (g_app.inc_files[i].inc) n++;
    return n;
}

/* ================= 接收请求确认（真实：后端 PENDING 会话） ================= */
static bool g_recv_expired = false;  /* 请求 60s 没人响应、已被后端作废 */
static uint64_t g_recv_supp_ms = 0;  /* 决定后抑制重弹到：毫秒时间戳 */

/* 拒绝当前请求并离开接收流程（后端回 403，发送方会看到拒绝） */
static void recv_reject_close(void)
{
    api_recv_clear();                /* PENDING → 唤醒 http 线程按拒绝处理 */
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
    w_page_header(tr("Incoming files"));
    char line[256];
    Rect card = { 24, 80, SCR_W - 48, 320 };

    /* 请求还在等 UI 决定吗？不在了（超时被后端作废）→ 只给关闭，不再给决定按钮 */
    g_recv_expired = api_recv_pending_pull(NULL) == 0;
    if (g_recv_expired) {
        w_rect(card, theme->card);
        w_text(48, 120, 1.5f, theme->text, "%s", g_app.recv_alias);
        w_text(48, 172, 1.25f, theme->text_dim, "%s",
               tr("This request has expired."));
        w_text(48, 216, 1.0f, theme->text_dim, "%s",
               tr("No response was sent to the sender."));
        Rect close = { SCR_W / 2 - 100, 444, 200, 48 };
        w_add(2, close);
        w_button(close, tr("Close"), true);
        HintSeg segs[2] = { 0 };
        int ns = 0;
        segs[ns].key  = HKEY_CONFIRM;
        segs[ns].key2 = HKEY_BACK;   /* 确认/返回都能关闭 → 合并成「确认/返回 关闭」 */
        segs[ns++].text = tr("Close");
        w_page_footer_segs(segs, ns);
        return;
    }

    w_rect(card, theme->card);
    int n = g_app.inc_count, seln = recv_included();
    int i;
    /* 来者名字（右上是平台类型） */
    w_text(48, 100, 1.5f, theme->text, "%s", g_app.recv_alias);
    int tw = 0, th = 0;
    w_text_w(1.1f, tr(g_app.recv_type), &tw, &th);
    w_text(card.x + card.w - tw - 24, 106, 1.1f, theme->text_dim, "%s",
           tr(g_app.recv_type));

    snprintf(line, sizeof line, tr("%s wants to send you %d file(s)."),
             g_app.recv_alias, n);
    w_text(48, 152, 1.15f, theme->text, "%s", line);

    SceOff t = 0;
    for (i = 0; i < n; i++) t += g_app.inc_files[i].size;
    char sz[16];
    w_human_size(t, sz);
    if (seln == n)
        snprintf(line, sizeof line, tr("Total %s"), sz);
    else
        snprintf(line, sizeof line, tr("Total %s  (%d of %d selected)"),
                 sz, seln, n);
    w_text(48, 190, 1.0f, theme->text_dim, "%s", line);

    if (g_app.recv_overflow > 0) {
        snprintf(line, sizeof line,
                 tr("%d more file(s) exceed the receive limit and will be skipped."),
                 g_app.recv_overflow);
        w_text(48, 212, 0.9f, theme->warn, "%s", line);
    }

    /* 文件预览（前几条） */
    int show = n < 5 ? n : 5;
    int py = g_app.recv_overflow > 0 ? 244 : 240;
    for (i = 0; i < show; i++) {
        char m[256];
        w_human_size(g_app.inc_files[i].size, sz);
        snprintf(m, sizeof m, "  %s  (%s)", g_app.inc_files[i].name, sz);
        w_text_clip(60, py + i * 26, 1.0f, theme->text, m, card.w - 140);
    }
    if (n > show)
        w_text(60, py + show * 26, 1.0f, theme->text_dim,
               tr("  ... %d more"), n - show);

    /* 底部三个按钮：Reject / Setup / Accept */
    const char *s_reject = tr("Reject");
    const char *s_setup  = tr("Setup");
    const char *s_accept = tr("Accept");
    Rect rej = { 183, 444, 190, 48 };
    Rect set = { 385, 444, 190, 48 };
    Rect acc = { 587, 444, 190, 48 };
    w_add(0, rej);
    w_add(1, set);
    w_add(2, acc);
    if (recv_focus == 0) {
        w_rect(rej, theme->danger);
        int w = 0, h = 0;
        w_text_w(1.3f, s_reject, &w, &h);
        w_text(rej.x + (rej.w - w) / 2, rej.y + (rej.h - h) / 2, 1.3f,
               theme->accent_text, "%s", s_reject);
    } else {
        w_rect(rej, theme->card);
        w_rect_outline(rej, theme->border);
        int w = 0, h = 0;
        w_text_w(1.3f, s_reject, &w, &h);
        w_text(rej.x + (rej.w - w) / 2, rej.y + (rej.h - h) / 2, 1.3f,
               theme->text, "%s", s_reject);
    }
    w_button(set, s_setup, recv_focus == 1);
    if (seln > 0) {
        w_button(acc, s_accept, recv_focus == 2);
    } else {
        w_rect(acc, theme->card);
        w_rect_outline(acc, theme->border);
        int w = 0, h = 0;
        w_text_w(1.3f, s_accept, &w, &h);
        w_text(acc.x + (acc.w - w) / 2, acc.y + (acc.h - h) / 2, 1.3f,
               theme->text_dim, "%s", s_accept);
    }

    /* 页脚：只声明本页用到的键，位置交给 hintbar 按默认键序排。确认段文案不写死
     * Accept（那会在焦点压到别的按钮上时撒谎）；焦点落在 Reject 上时它与返回键
     * 同动作 → 合并成「确认/返回 Reject」，两个键都显示，文案不重复。 */
    HintSeg segs[6] = { 0 };
    int ns = 0;
    segs[ns].key = HKEY_DPAD;  segs[ns].dir_off = HDIR_VERT;
    segs[ns++].text = tr("Switch");
    if (recv_focus == 0) {
        segs[ns].key  = HKEY_CONFIRM;
        segs[ns].key2 = HKEY_BACK;
        segs[ns++].text = s_reject;
    } else {
        segs[ns].key = HKEY_CONFIRM;
        segs[ns++].text = recv_focus == 1 ? s_setup : s_accept;
        segs[ns].key = HKEY_BACK;  segs[ns++].text = s_reject;
    }
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
    if (api_recv_pending_pull(NULL) != 1) {   /* 已过期（竞态防护，正常不会到） */
        recv_close();
        return;
    }
    for (i = 0; i < g_app.inc_count && i < RECV_MAX_FILES; i++)
        inc[i] = g_app.inc_files[i].inc;
    api_recv_set_include(inc);
    api_recv_set_dir(g_app.recv_dir);   /* 本次保存目录（Setup 里可临时改；默认=config saveDir） */
    for (i = 0; i < g_app.inc_count && i < RECV_MAX_FILES; i++) {
        if (!g_app.inc_files[i].inc) continue;
        if (n >= MAX_PICKED) break;
        /* 进度页清单显示"保存名"（改名后为新名，否则对方原名） */
        snprintf(xf_name[n], sizeof xf_name[0], "%s",
                 g_app.inc_files[i].rname[0]
                     ? g_app.inc_files[i].rname : g_app.inc_files[i].name);
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
    api_recv_decide(true);           /* 唤醒后端：组会话、回 200 给发送方 */
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

/* 系统键盘挂起事务：设置页主机名与接收设置逐文件改名共用同一状态机
 * （分属不同页面不会同时出现）。busy 供主循环据此跳过页面按键/触摸。 */
enum {
    IME_TX_NONE = 0,
    IME_TX_RENAME,     /* 接收设置：改 inc_files[ime_tx_fi] 的保存名 */
    IME_TX_HOST,       /* 设置页：改本机设备名 alias（config） */
};
/* 接收改名的键盘时限：须赶在 prepare 等 UI 决定窗口（receive.c 的
 * RECV_DECIDE_TIMEOUT_US = 60s）内完成，窗口一过 pending 清场、改名白做；
 * 取 50s 略短于窗口，给关键盘后点接受/拒绝留时间。主机名是纯设置项、
 * 无业务时限，打开时传 0 不限（见 ime.h limit_us）。 */
#define RN_IME_LIMIT_US (50ll * 1000 * 1000)
static int  ime_tx = IME_TX_NONE;  /* 当前挂起/打开的事务（NONE=空闲） */
static int  ime_tx_fi = -1;        /* RENAME 的目标行（HOST 不使用） */
static char ime_out[128];          /* 键盘结果落点（须存活到 poll 结束，ime 异步写） */

/* 把键盘输入净化后写入 rname 并同步后端（净化规则与后端 sanitize 一致，
 * 保证行上显示的即落盘名）。无效输入 → 沿用原名。 */
static void rename_apply(int fi, const char *in)
{
    char cur[128];
    int i, o;
    bool any = false;
    if (fi < 0 || fi >= g_app.inc_count) return;
    for (i = 0, o = 0; in[i] && o < (int)sizeof cur - 1 && i < 159; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
        cur[o++] = (char)c;
        if (c != '_') any = true;
    }
    while (o > 0 && (cur[o - 1] == ' ' || cur[o - 1] == '.')) o--;
    cur[o] = 0;
    if (!any || !cur[0] || strcmp(cur, ".") == 0 || strcmp(cur, "..") == 0) {
        g_app.inc_files[fi].rname[0] = 0;   /* 净化后无效：沿用原名 */
        api_recv_set_name(fi, "");
        return;
    }
    if (strcmp(cur, g_app.inc_files[fi].name) == 0)
        cur[0] = 0;                         /* 等于原名：无需改名 */
    snprintf(g_app.inc_files[fi].rname, sizeof g_app.inc_files[fi].rname,
             "%s", cur);
    api_recv_set_name(fi, cur);
}

/* 请求打开系统键盘改行 fi 的保存名。仅登记，真正的 ime_ask_begin 由主循环
 * 帧间 page_ime_pump() 执行——不能在本函数调用：它处于 vita2d 绘制批次中，
 * 对话框在绘制中打开会抢占显示通道导致弹不出（d55 卡死根因之一）。 */
static void ask_rename(int fi)
{
    if (ime_tx != IME_TX_NONE || fi < 0 || fi >= g_app.inc_count) return;
    ime_tx = IME_TX_RENAME;
    ime_tx_fi = fi;   /* 初值在 pump 里现取（主线程串行，内容一致） */
}

/* 主机名净化：剥控制字符、去首尾空白；UTF-8 按完整码点截断到 alias 容量
 * （放不下整字符即停，绝不切半多字节字符）。结果为空 → 默认名。未变不写盘。 */
static void host_apply(const char *in)
{
    char cur[sizeof g_cfg.alias];
    int i = 0, o = 0, s;

    while (in[i] && o < (int)sizeof cur - 1) {
        unsigned char c = (unsigned char)in[i];
        int nb;
        if (c < 0x80) nb = 1;
        else if (c < 0xE0) nb = 2;
        else if (c < 0xF0) nb = 3;
        else if (c < 0xF8) nb = 4;
        else nb = 1;                       /* 非法首字节防御：按单字节保留 */
        if (c < 0x20 || c == 0x7F) { i++; continue; }   /* 剥离控制字符 */
        if (o + nb > (int)sizeof cur - 1) break;        /* 放不下整字符 */
        while (nb--) cur[o++] = in[i++];
    }
    cur[o] = 0;
    for (s = 0; cur[s] == ' '; s++) ;      /* 去首部空白 */
    while (o > s && cur[o - 1] == ' ') o--;
    if (o > s)
        memmove(cur, cur + s, o - s);
    cur[o - s] = 0;
    if (!cur[0])
        snprintf(cur, sizeof cur, "%s", DEFAULT_ALIAS);  /* 空输入 → 默认名 */
    if (strcmp(cur, g_cfg.alias) == 0) return;
    snprintf(g_cfg.alias, sizeof g_cfg.alias, "%s", cur);
    config_save();
}

/* 请求打开系统键盘改本机设备名（alias）。登记时机同 ask_rename。 */
void ask_ime_host(void)
{
    if (ime_tx != IME_TX_NONE) return;
    ime_tx = IME_TX_HOST;
}

/* 系统键盘改名事务进行中（已登记或已打开）？主循环据此跳过页面按键/触摸。 */
bool page_ime_busy(void)
{
    return ime_tx != IME_TX_NONE;
}

/* 主循环每帧在"帧间"（非绘制中，参考 mGBA-psp2：init 在渲染循环外、
 * 对话框打开后循环内每帧 draw→poll→draw）驱动系统键盘：
 *   - 有登记的请求 → 打开键盘（成功则进入轮询，失败清除请求）；
 *   - 键盘已打开 → poll 收尾，结束（确认/取消）时按事务写回并复位。 */
void page_ime_pump(void)
{
    int r;
    if (ime_active()) {
        r = ime_ask_poll();
        if (r == 0) return;                 /* 仍在进行：继续出帧渲染 */
        if (r == 1) {
            if (ime_tx == IME_TX_RENAME) rename_apply(ime_tx_fi, ime_out);
            else if (ime_tx == IME_TX_HOST) host_apply(ime_out);
        }
        ime_tx = IME_TX_NONE;
        return;
    }
    if (ime_tx == IME_TX_RENAME) {
        if (ime_ask_begin(tr("Rename file"),
                          g_app.inc_files[ime_tx_fi].rname[0]
                              ? g_app.inc_files[ime_tx_fi].rname
                              : g_app.inc_files[ime_tx_fi].name,
                          ime_out, sizeof ime_out, RN_IME_LIMIT_US) != 1)
            ime_tx = IME_TX_NONE;           /* 打开失败：放弃（dlog 已记） */
    } else if (ime_tx == IME_TX_HOST) {
        if (ime_ask_begin(tr("Hostname"),
                          g_cfg.alias[0] ? g_cfg.alias : DEFAULT_ALIAS,
                          ime_out, sizeof ime_out, 0) != 1)   /* 0=不限时 */
            ime_tx = IME_TX_NONE;
    }
}

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
    w_page_header(tr("Receive setup"));
    int rows = 1 + g_app.inc_count;
    char sz[16];
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
            /* 保存目录行：本次目录（默认=config saveDir）。确认键/点击进入
             * 目录选择器临时改本次目录（内存态，接受时才传给后端生效）。
             * 本行整行可点（下方 w_add 注册），故行尾不再画"更改"二字：
             * 它此前用 accent_text，而选中行底色其实是 card（只有左侧一条
             * accent 竖条），浅色主题下白字白底完全不可见。 */
            w_text(60, top + 8, 1.0f, theme->text_dim, "%s", tr("Save to"));
            w_text_clip(60, top + 30, 1.1f, sel ? theme->text : theme->text_dim,
                        g_app.recv_dir, 780);
            continue;
        }

        int idx = i - 1;
        bool on = g_app.inc_files[idx].inc;
        uint32_t nc = sel ? theme->text : theme->text_dim;
        w_human_size(g_app.inc_files[idx].size, sz);
        if (g_app.inc_files[idx].rname[0]) {
            /* 已改名：上行小字原名，下行保存名 */
            w_text_clip(60, top + 4, 0.8f, theme->text_dim,
                        g_app.inc_files[idx].name, 460);
            w_text_clip(60, top + 22, 1.0f, nc,
                        g_app.inc_files[idx].rname, 460);
        } else {
            int th = 0;
            w_text_w(1.0f, g_app.inc_files[idx].name, NULL, &th);
            w_text_clip(60, top + (RSS_ROW_H - th) / 2 - 2, 1.0f, nc,
                        g_app.inc_files[idx].name, 460);
        }
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
        w_text_w(1.0f, tr("Rename"), &w2, &h2);
        w_text(ren.x + (ren.w - w2) / 2, ren.y + (ren.h - h2) / 2, 1.0f,
               theme->text_dim, "%s", tr("Rename"));
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

    HintSeg segs[8] = { 0 };
    int ns = 0;
    /* 上下选行：选到首/末行时把方向键对应臂画灰 */
    uint8_t off = HDIR_HORZ;
    if (g_app.inc_sel <= 0)          off |= HDIR_UP;
    if (g_app.inc_sel >= rows - 1)   off |= HDIR_DOWN;
    segs[ns].key = HKEY_DPAD;         segs[ns].dir_off = off;
    segs[ns++].text = tr("Choose");
    /* 提示随焦点行变：目录行确认键=进目录选择器、三角键无响应；文件行才是
     * 确认键=切勾选、三角键=改名（此前写死 Toggle/Rename，焦点在目录行时与
     * 实际按键对不上） */
    segs[ns].key = HKEY_CONFIRM;
    segs[ns++].text = g_app.inc_sel > 0 ? tr("Toggle") : tr("Change folder");
    segs[ns].key = HKEY_BACK;         segs[ns++].text = tr("Back");
    if (g_app.inc_sel > 0) {
        segs[ns].key = HKEY_TRIANGLE; segs[ns++].text = tr("Rename");
    }
    w_page_footer_segs(segs, ns);
}

void page_recv_setup_input(const Input *in)
{
    int rows = 1 + g_app.inc_count;
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
            if (row > 0) ask_rename(row - 1);
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
        if (id >= 0 && id < rows) {
            g_app.inc_sel = id;
            if (id == 0) open_dir_pick(PAGE_RECV_SETUP, false, g_app.recv_dir);
        }
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
        } else {
            open_dir_pick(PAGE_RECV_SETUP, false, g_app.recv_dir);   /* 目录行 */
        }
    }
    if (in->alt) {
        if (g_app.inc_sel > 0) ask_rename(g_app.inc_sel - 1);
    }
    if (in->back) {
        rename_pop = false;
        recv_focus_sane();
        g_app.page = PAGE_RECV_CONFIRM;   /* 临时设置已生效，返回接收页 */
    }
}

/* ================= 新接收请求自动弹窗 ================= */
/* 把"待决定请求"快照复制到 UI 工作副本并进入接收确认页 */
static void open_recv_request(const RecvPending *rp)
{
    int i, n = rp->count;
    if (n > RECV_MAX_FILES) n = RECV_MAX_FILES;
    if (n > MAX_INF) n = MAX_INF;
    g_app.recv_overflow = rp->overflow;
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
    snprintf(g_app.recv_dir, sizeof g_app.recv_dir, "%s", g_cfg.save_dir);
    g_app.inc_sel = 0;
    rs_scroll = 0;
    rename_pop = false;
    recv_focus = 2;
    g_app.page = PAGE_RECV_CONFIRM;
}

/* 每帧由 ui_main 调用：① 发送等待页在接收方接受后自动切进度页；② 出现
 * "待决定接收请求"且当前页面可打断时自动弹确认页。
 * 用户拒绝/接受后 g_recv_supp_ms 置 2s 抑制窗，防对端立刻重试又弹回；
 * 接收流程页/传输中本身不打断（switch 的 default）。 */
void pages_tick(void)
{
    RecvPending rp;
    uint64_t now_ms = (uint64_t)sceKernelGetSystemTimeWide() / 1000;

    /* 发送等待页：只在对端接受、真正开始上传（cur>=0）后才切进度页。
     * 开始上传前就出结果（被拒 403 / 连不上）留在等待页原地报出——否则用户
     * 看到的是一次"进进度页才报错"的跳转，而不是就地反馈。 */
    if (g_app.page == PAGE_SEND_WAIT) {
        XferInfo xv;
        api_send_info(&xv);
        if (xv.cur >= 0 || xv.total_sent > 0)
            g_app.page = PAGE_PROGRESS;
    }

    if (now_ms < g_recv_supp_ms) return;     /* 决定后的抑制窗内不弹 */
    if (api_recv_pending_pull(&rp) != 1) return; /* 无"待决定"请求 */
    switch (g_app.page) {
    case PAGE_DEVICES:
    case PAGE_FILES:
    case PAGE_SETTINGS:
        break;
    default:
        return;                              /* 发送等待/接收流程/传输中不打断 */
    }
    open_recv_request(&rp);
}
