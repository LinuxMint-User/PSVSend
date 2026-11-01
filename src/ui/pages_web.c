/* 页面：网页共享（PAGE_WEB）—— 官方 web share 口径的浏览器入口。
 *
 * 三个入口：
 *   主页 START     已选 >0 → 进页并直接开「下载分享」；=0 → 只进页面停在选择态
 *   主页页头地球   只打开页面（不自动启动）
 *   设置 › 操作    「网页共享」行（纯按键用户）
 *
 * 两种模式互斥：下载分享（PSV → 浏览器，分享本次已选文件）与网页接收
 * （浏览器 → PSV，走既有 prepare-upload / 上传确认页）。页面只负责 PIN、
 * 启停与展示；HTTP 路由与鉴权在 net/web.c，浏览器侧只拿 fileName / size。 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "app/api.h"

#define WID_WEB_MODE0   0xB001      /* 选择态第一行：分享已选文件 */
#define WID_WEB_MODE1   0xB002      /* 选择态第二行：网页接收 */
#define WID_WEB_START   0xB003      /* 选择态右下"启动"按钮 */

static int  web_sel;                /* 选择态焦点：0=分享 1=接收 */
static int  web_back_page;          /* 返回去向：PAGE_DEVICES / PAGE_SETTINGS */
static bool web_stop_hold;          /* 手指正按住停止按钮（画蓄力弧用） */
static WebEntry s_entries[MAX_PICKED];

static void start_download_share(void)
{
    int i;
    if (g_app.picked_count <= 0) return;
    for (i = 0; i < g_app.picked_count; i++) {
        snprintf(s_entries[i].name, sizeof s_entries[i].name, "%s",
                 g_app.picked[i].name);
        snprintf(s_entries[i].path, sizeof s_entries[i].path, "%s",
                 g_app.picked[i].path);
        s_entries[i].size = g_app.picked[i].size;
    }
    api_web_start_download(s_entries, g_app.picked_count);
}

static void web_enter(bool start_download)
{
    /* PIN 每次进页面生成（运行中重进不重生成：那会让已连上的浏览器失效）。
     * 不落 config，web_stop 时清空。 */
    if (api_web_mode() == WEB_MODE_NONE) {
        api_web_new_pin();
        web_sel = 0;
    }
    web_stop_hold = false;
    g_app.page = PAGE_WEB;
    if (start_download) start_download_share();
}

void open_web(bool start_download)
{
    web_back_page = PAGE_DEVICES;
    web_enter(start_download);
}

/* 设置页入口：不自动启动，返回设置页 */
void open_web_settings(void)
{
    web_back_page = PAGE_SETTINGS;
    web_enter(false);
}

static void web_start_selected(void)
{
    if (web_sel == 0) {
        if (g_app.picked_count <= 0) return;     /* 无已选：启动按钮已灰 */
        start_download_share();
    } else {
        api_web_start_upload();
    }
}

static void web_leave(void)
{
    api_web_stop();
    web_stop_hold = false;
    g_app.page = (PageId)web_back_page;
}

/* ================= 选择态 ================= */

static void render_choose(void)
{
    Rect r0 = { 24, LIST_TOP, SCR_W - 48, ROW_H };
    Rect r1 = { 24, LIST_TOP + ROW_STRIDE, SCR_W - 48, ROW_H };
    char sub[96];
    SceOff total = g_app.picked_total;
    int n = g_app.picked_count;

    if (n > 0) {
        char sz[24];
        w_human_size(total, sz);
        snprintf(sub, sizeof sub, tr("%d files · %s"), n, sz);
    } else {
        snprintf(sub, sizeof sub, "%s", tr("No files selected"));
    }
    w_row(r0, tr("Share selected files"), sub, web_sel == 0);
    w_add(WID_WEB_MODE0, r0);
    w_row(r1, tr("Receive in browser"),
          tr("Send files from a browser to PS Vita"), web_sel == 1);
    w_add(WID_WEB_MODE1, r1);

    /* 触摸用户的启动入口（纯按键走确认键） */
    {
        Rect bt = { SCR_W - 216, SCR_H - 42, 192, 36 };
        bool ok = web_sel == 1 || n > 0;
        w_add(WID_WEB_START, bt);
        w_button(bt, tr("Start"), ok);
    }

    {
        HintSeg segs[4] = { 0 };
        int ns = 0;
        segs[ns].key = HKEY_DPAD;
        segs[ns].dir_off = HDIR_HORZ;
        segs[ns++].text = tr("Switch");
        segs[ns].key = HKEY_CONFIRM;
        segs[ns].dim = (web_sel == 0 && n <= 0);
        segs[ns++].text = tr("Start");
        segs[ns].key = HKEY_BACK; segs[ns++].text = tr("Back");
        w_page_footer_segs(segs, ns);
    }
}

/* ================= 运行态 ================= */

/* 标签 + 值一行（左对齐固定列） */
static void web_stat(int y, const char *label, const char *value)
{
    w_text(48, y, 1.0f, theme->text_dim, "%s", label);
    w_text(220, y, 1.0f, theme->text, "%s", value);
}

/* 停止共享按钮：会话数下方、页脚上方（左列）。外环 + 间隙蓄力弧 + 实心圆标签；
 * 触摸与返回键都按住 0.7s 才触发（替代原来的确认弹窗，防误触）。 */
#define WEB_STOP_CX    90
#define WEB_STOP_CY    392
#define WEB_STOP_R     34       /* 外环半径（也是触摸命中半径） */
#define WEB_STOP_ARC_R 28       /* 蓄力弧半径：环与实心圆之间的空隙 */
#define WEB_STOP_DISC  22       /* 实心圆半径 */

static bool web_stop_hit(int x, int y)
{
    int dx = x - WEB_STOP_CX, dy = y - WEB_STOP_CY;
    return dx * dx + dy * dy <= WEB_STOP_R * WEB_STOP_R;
}

/* 24 段短线段拼弧（vita2d 没有画弧 API，也不能直接 draw_array，见 hintbar.c）。
 * pct=1 / off=0 当外环用；off=2 画出约 5px 粗的轨道与进度。 */
static void web_stop_arc(float r, float pct, uint32_t c, float off)
{
    const float step = 6.2831853f / 24.0f;
    int n = (int)(pct * 24.0f + 0.5f), i;
    if (n > 24) n = 24;
    for (i = 0; i < n; i++) {
        float a0 = -1.5707963f + step * i;      /* -90° = 12 点方向，顺时针 */
        float a1 = a0 + step;
        float x0 = WEB_STOP_CX + r * cosf(a0), y0 = WEB_STOP_CY + r * sinf(a0);
        float x1 = WEB_STOP_CX + r * cosf(a1), y1 = WEB_STOP_CY + r * sinf(a1);
        vita2d_draw_line(x0, y0, x1, y1, c);
        if (off > 0.0f) {
            float nx = -(y1 - y0), ny = (x1 - x0);
            float l = sqrtf(nx * nx + ny * ny);
            if (l > 0.01f) {
                nx = nx / l * off; ny = ny / l * off;
                vita2d_draw_line(x0 + nx, y0 + ny, x1 + nx, y1 + ny, c);
                vita2d_draw_line(x0 - nx, y0 - ny, x1 - nx, y1 - ny, c);
            }
        }
    }
}

/* 圆形长按按钮：外环 / 轨道 / 蓄力弧（只在按住它时显示）/ 实心圆 + 标签 */
static void draw_stop_button(void)
{
    float pct = web_stop_hold ? ui_input_touch_charge() : 0.0f;
    int tw = 0;

    web_stop_arc(WEB_STOP_R, 1.0f, theme->border, 0.0f);
    web_stop_arc(WEB_STOP_ARC_R, 1.0f, theme->border, 2.0f);
    if (pct > 0.0f)
        web_stop_arc(WEB_STOP_ARC_R, pct, theme->accent, 2.0f);
    vita2d_draw_fill_circle(WEB_STOP_CX, WEB_STOP_CY, WEB_STOP_DISC, theme->bg);

    w_text_w(0.8f, tr("Stop"), &tw, NULL);
    w_text(WEB_STOP_CX - tw / 2.0f, WEB_STOP_CY - 8, 0.8f, theme->text,
           "%s", tr("Stop"));
    w_text_w(0.65f, tr("Hold to stop"), &tw, NULL);
    w_text(WEB_STOP_CX - tw / 2.0f, 436, 0.65f, theme->text_dim,
           "%s", tr("Hold to stop"));
}

static void render_running(const WebSnap *s)
{
    Rect card = { 24, LIST_TOP, SCR_W - 48, LIST_BOTTOM - LIST_TOP };
    char buf[192];

    w_rect(card, theme->card);

    /* 网关地址：浏览器要打开的就是这一行 */
    w_text(48, 96, 0.9f, theme->text_dim, "%s", tr("Open in a browser"));
    if (s->port > 0)
        snprintf(buf, sizeof buf, "http://%s:%d", s->ip, s->port);
    else
        snprintf(buf, sizeof buf, "%s", tr("Network not ready."));
    w_text(48, 120, 1.6f, theme->text, "%s", buf);

    /* PIN：共享页最该被看清的东西 */
    w_text(48, 170, 0.9f, theme->text_dim, "%s", tr("PIN"));
    w_text(48, 192, 2.2f, theme->accent, "%s", s->pin[0] ? s->pin : "----");

    /* 模式 */
    web_stat(250, tr("Mode"),
             s->mode == WEB_MODE_DOWNLOAD ? tr("Share files") : tr("Receive files"));

    /* 来源：IP + 浏览器短标签（如 "192.168.1.5  Chrome 131"）。UA 原文太长，
     * 由 net/web.c 解析成"名字 + 主版本号"，与官方客户端的显示口径一致。 */
    if (s->client_ip[0]) {
        if (s->client_browser[0])
            snprintf(buf, sizeof buf, "%s  %s", s->client_ip, s->client_browser);
        else
            snprintf(buf, sizeof buf, "%s", s->client_ip);
        web_stat(280, tr("Connected from"), buf);
    } else {
        web_stat(280, tr("Connected from"), tr("Waiting for a browser"));
    }

    snprintf(buf, sizeof buf, "%d", s->sessions);
    web_stat(322, tr("Sessions"), buf);

    draw_stop_button();

    /* 断网：只警告，不停会话（恢复后浏览器刷新即可继续） */
    if (!api_link_up())
        w_text(48, 462, 0.9f, theme->warn, "%s", tr("Wi-Fi link is down."));

    {
        HintSeg segs[1] = { 0 };
        segs[0].key = HKEY_BACK;
        segs[0].text = tr("Hold to stop sharing");
        w_page_footer_charge(HKEY_BACK, ui_input_back_charge());
        w_page_footer_segs(segs, 1);
    }
}

void page_web_render(void)
{
    WebSnap s;
    api_web_snapshot(&s);

    w_page_header(tr("Web share"));
    /* 运行态不再有"返回"入口：唯一的退出方式是长按停止（防误触） */
    if (s.mode == WEB_MODE_NONE)
        w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_WEB, HDRBTN_BACK, false);

    if (s.mode == WEB_MODE_NONE)
        render_choose();
    else
        render_running(&s);
}

void page_web_input(const Input *in)
{
    WebSnap s;
    api_web_snapshot(&s);
    bool running = s.mode != WEB_MODE_NONE;

    if (running) {
        /* 停止共享：返回键或圆形按钮按住 0.7s（短按一律不响应） */
        web_stop_hold = in->touch_hold && web_stop_hit(in->touch_x, in->touch_y);
        if (in->back_long) { web_leave(); return; }
        if (in->touch_long && web_stop_hit(in->touch_x, in->touch_y)) {
            web_leave();
            return;
        }
        return;
    }

    web_stop_hold = false;

    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_HDR_BACK_WEB) { web_leave(); return; }
        if (id == WID_WEB_MODE0) { web_sel = 0; return; }
        if (id == WID_WEB_MODE1) { web_sel = 1; return; }
        if (id == WID_WEB_START) { web_start_selected(); return; }
        return;
    }

    if (in->up || in->down || in->left || in->right) {
        web_sel = web_sel ? 0 : 1;
        return;
    }
    if (in->confirm) { web_start_selected(); return; }
    if (in->back)    { web_leave(); return; }
}
