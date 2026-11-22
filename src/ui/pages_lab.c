/* 页面：实验室（PAGE_LAB）+ 摄像头基准（PAGE_CAM_BENCH）+ 扫码取景（PAGE_SCAN）。
 *
 * 实验室入口在设置页「实验」分组，只放正式功能之外的实验性小工具：后摄采集 +
 * QR 解码基准（测吞吐）。扫码取景已转正，入口在主页（肩键 L / 页头扫码按钮），
 * 代码仍与相机基准放在同一文件（共用页面布局与相机生命周期工具）。 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "app/cam_bench.h"
#include "app/cam_scan.h"
#include "app/api.h"

/* 实验室页行 id（后续实验项按 +1 排） */
#define WID_LAB_CAM        0xA001
#define LAB_ROWS           1
/* 摄像头基准页 */
#define WID_CB_RES         0xA101   /* 分辨率行：点一下切档（运行中不响应） */
#define WID_CB_START       0xA102   /* 开始 / 停止按钮 */
/* 扫码取景页：冻结后每个候选框一个触摸区（+ 下标） */
#define WID_SCAN_BOX       0xA201

/* 基准页没有跨页状态，分辨率档位只在本页记住（不落盘：实验项，没必要污染配置） */
static int cb_res;

/* 实验室列表选中行（0=基准） */
static int lab_sel;

/* ================= 实验室列表 ================= */

void open_lab(void)
{
    /* 设置页进来时字体预读线程还在后台读 16MB×2：基准要测 CPU/IO，先停掉，
     * 回设置页再恢复（ui_font_preload_begin 是幂等的）。 */
    ui_font_preload_end();
    g_app.page = PAGE_LAB;
}

static void lab_leave(void)
{
    ui_font_preload_begin();
    g_app.page = PAGE_SETTINGS;
}

static void lab_open_cam_bench(void)
{
    g_app.page = PAGE_CAM_BENCH;
}

void page_lab_render(void)
{
    const char *sum = cam_bench_summary();
    int i;

    w_page_header(tr("Lab"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_LAB, HDRBTN_BACK, false);

    for (i = 0; i < LAB_ROWS; i++) {
        Rect r = { 24, LIST_TOP + i * ROW_STRIDE, SCR_W - 48, ROW_H };
        RowGeom g;
        bool on = (lab_sel == i);
        uint32_t bg = on ? theme->accent : theme->card;
        uint32_t tc = on ? theme->accent_text : theme->text;
        uint32_t dc = on ? theme->accent_text : theme->text_dim;
        char sub[160];

        if (cam_bench_running())
            snprintf(sub, sizeof sub, "%s", tr("Running"));
        else if (sum)
            snprintf(sub, sizeof sub, "%s", sum);
        else
            snprintf(sub, sizeof sub, "%s", tr("Not run"));

        w_rect(r, bg);
        row_geom(r, ROW_2LINE, 0, &g);
        w_text_clip(g.x_text, g.y_main, g.main_sc, tc,
                    tr("Camera benchmark"), g.w_text);
        w_text_clip(g.x_text, g.y_sub, g.sub_sc, dc, sub, g.w_text);
        add_row_hit(WID_LAB_CAM + i, r.y);
    }

    {
        HintSeg segs[4] = { 0 };
        int ns = 0;
        segs[ns].key = HKEY_CONFIRM; segs[ns++].text = tr("Open");
        segs[ns].key = HKEY_BACK;    segs[ns++].text = tr("Back");
        w_page_footer_segs(segs, ns);
    }
}

void page_lab_input(const Input *in)
{
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_HDR_BACK_LAB) { lab_leave(); return; }
        if (id == WID_LAB_CAM)  { lab_sel = 0; lab_open_cam_bench(); return; }
        return;
    }
    /* 只有一行，方向键不改变选中（保持 0） */
    if (in->confirm) {
        lab_open_cam_bench();
        return;
    }
    if (in->back) lab_leave();
}

/* ================= 摄像头基准 ================= */

static void cb_leave(void)
{
    /* 返回键=停止并退出；已跑完但线程句柄还没回收时也顺手收掉（stop 会等它结束） */
    cam_bench_stop();
    g_app.page = PAGE_LAB;
}

static void cb_toggle(void)
{
    if (cam_bench_running())
        cam_bench_stop();
    else
        cam_bench_start(cb_res);
}

/* 标签 + 数值一行（数值左对齐固定列，读起来是对齐的表） */
static void cb_stat(int y, const char *label, const char *fmt, ...)
{
    char val[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof val, fmt, ap);
    va_end(ap);
    w_text(48, y, 1.0f, theme->text_dim, "%s", label);
    w_text(236, y, 1.0f, theme->text, "%s", val);
}

void page_cam_bench_render(void)
{
    CamBenchSnap s;
    Rect card = { 24, LIST_TOP + ROW_STRIDE + 8, SCR_W - 48, LIST_BOTTOM - (LIST_TOP + ROW_STRIDE + 8) };
    Rect rr = { 24, LIST_TOP, SCR_W - 48, ROW_H };
    char status[96];
    struct vita2d_texture *tex = cam_bench_preview_tex();
    bool running;

    cam_bench_snapshot(&s);
    running = (s.state == CAM_BENCH_RUNNING);

    w_page_header(tr("Camera benchmark"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_CB, HDRBTN_BACK, false);

    /* 分辨率行：左/右键或点行切换（运行中锁定） */
    w_row(rr, tr("Resolution"),
          cb_res == 0 ? "640 x 480" : "320 x 240", false);
    if (!running) w_add(WID_CB_RES, rr);

    w_rect(card, theme->card);

    /* 状态行 */
    switch (s.state) {
    case CAM_BENCH_RUNNING:
        snprintf(status, sizeof status, "%s  ", tr("Running"));
        {
            char st[32];
            /* 线程刚起、预热帧阶段 stage 还是 -1：按第 1 档显示 */
            snprintf(st, sizeof st, tr("Stage %d/3"), s.stage < 0 ? 1 : s.stage + 1);
            strncat(status, st, sizeof status - strlen(status) - 1);
        }
        break;
    case CAM_BENCH_DONE:
        snprintf(status, sizeof status, "%s", tr("Done"));
        break;
    case CAM_BENCH_ERROR:
        snprintf(status, sizeof status, "%s 0x%08X", tr("Failed"), (unsigned)s.err);
        break;
    default:
        snprintf(status, sizeof status, "%s", tr("Not run"));
        break;
    }
    w_text(48, 166, 1.1f, s.state == CAM_BENCH_ERROR ? theme->accent : theme->text,
           "%s", status);
    if (running) {
        char f[48];
        snprintf(f, sizeof f, "%d/%d", s.frame, CAM_BENCH_FRAMES);
        w_text(400, 166, 1.0f, theme->text_dim, "%s", f);
    }

    /* 实时统计 */
    cb_stat(202, tr("Capture"), "%.1f fps", (double)s.cap_fps);
    cb_stat(232, tr("Read"),    "%.2f / %.2f ms",
            (double)s.read_avg_ms, (double)s.read_max_ms);
    cb_stat(262, tr("Decode"),  "%.2f / %.2f ms",
            (double)s.dec_avg_ms, (double)s.dec_max_ms);
    cb_stat(292, tr("QR ok / fail"), "%d / %d", s.dec_ok, s.dec_fail);
    cb_stat(322, tr("Preview"), "%.2f / %.2f ms",
            (double)s.prev_avg_ms, (double)s.prev_max_ms);

    /* 三档结果（跑完或中途停止后都有已跑部分） */
    {
        int y = 362, i;
        char lab[64], val[96];
        for (i = 0; i < CAM_BENCH_STAGES; i++) {
            if (s.stage_done_frames[i] <= 0) continue;
            snprintf(lab, sizeof lab, "%d  %s", i + 1,
                     i == 0 ? tr("Capture only") : (i == 1 ? tr("+ decode") : tr("+ preview")));
            if (i == 0)
                snprintf(val, sizeof val, "%.1f fps", (double)s.stage_fps[i]);
            else
                snprintf(val, sizeof val, "%.1f fps  %.1f / %.1f ms",
                         (double)s.stage_fps[i],
                         (double)s.stage_dec_avg[i], (double)s.stage_dec_max[i]);
            w_text(48, y, 0.9f, theme->text_dim, "%s", lab);
            w_text(236, y, 0.9f, theme->text, "%s", val);
            y += 26;
        }
    }

    /* 预览（右半）：320x240 灰度，1:1 贴，外加描边 */
    w_text(600, 166, 0.9f, theme->text_dim, "%s", tr("Preview"));
    if (tex) {
        vita2d_draw_texture(tex, 600, 190);
        w_rect_outline((Rect){ 600, 190, 320, 240 }, theme->border);
    } else {
        Rect ph = { 600, 190, 320, 240 };
        w_rect(ph, theme->bg);
        w_rect_outline(ph, theme->border);
        w_text(600 + 80, 190 + 108, 0.9f, theme->text_dim, "%s",
               tr("No preview yet"));
    }

    /* 页脚：运行中只留"停止/返回"；未运行时给分辨率切换提示 */
    {
        HintSeg segs[4] = { 0 };
        int ns = 0;
        if (!running) {
            segs[ns].key = HKEY_DPAD;
            segs[ns].dir_off = HDIR_VERT;
            segs[ns++].text = tr("Resolution");
        }
        segs[ns].key = HKEY_CONFIRM;
        segs[ns++].text = running ? tr("Stop") : tr("Start");
        segs[ns].key = HKEY_BACK; segs[ns++].text = tr("Back");
        w_page_footer_segs(segs, ns);
    }

    /* 右下角开始/停止触摸按钮（画在页脚之后才不会被页脚背景盖掉） */
    {
        Rect bt = { SCR_W - 216, SCR_H - 42, 192, 36 };
        w_add(WID_CB_START, bt);
        w_button(bt, running ? tr("Stop") : tr("Start"), true);
    }
}

void page_cam_bench_input(const Input *in)
{
    bool running = cam_bench_running();

    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_HDR_BACK_CB) { cb_leave(); return; }
        if (id == WID_CB_START)    { cb_toggle(); return; }
        if (id == WID_CB_RES && !running) {
            cb_res = (cb_res + 1) % CAM_BENCH_RES_N;
        }
        return;
    }
    if (in->left || in->right) {
        if (!running) cb_res = (cb_res + 1) % CAM_BENCH_RES_N;
        return;
    }
    if (in->confirm) { cb_toggle(); return; }
    if (in->back)    { cb_leave(); return; }
}

/* ================= 扫码取景（实验室测试项） =================
 * 流程：取景（30fps 预览）→ 按住 R 解码 → 命中即定格（画面/解码都停）→
 * 多个候选用方向键选、○ 确认 → 详情（是分享链接 / 不是）→ 是链接就 Open：
 * 后台去对方分享页取清单，随后与普通接收请求一样弹接收确认页（勾选/改名/选目录），
 * 接受后走接收进度页。整条链路复用 receive 模块的 pending/status 语义。
 *
 * 链接校验纯字符串：只认 http://host[:port]/?pin=NNNN（我们自己 web 共享页的
 * 二维码格式），其余一律当"不是文件传输链接"。 */

#define SCAN_CARD_X   24
#define SCAN_CARD_Y   LIST_TOP
#define SCAN_CARD_W   (SCR_W - 48)
#define SCAN_CARD_H   (LIST_BOTTOM - LIST_TOP)
/* 预览按 4:3 撑满卡片高度、水平居中（两侧留卡片底色） */
#define SCAN_PREV_H   SCAN_CARD_H
#define SCAN_PREV_W   (CAM_SCAN_PREV_W * SCAN_PREV_H / CAM_SCAN_PREV_H)
#define SCAN_PREV_X   (SCAN_CARD_X + (SCAN_CARD_W - SCAN_PREV_W) / 2)
#define SCAN_PREV_Y   SCAN_CARD_Y
/* 取景页左右"按住扫描"触摸区：宽 150，高度 = 预览高 - 192，垂直居中于预览区，
 * 落在预览两侧的卡片底色里（不盖住画面）。绘制与命中判断共用这几个常量。 */
#define SCAN_HOLD_W   150
#define SCAN_HOLD_H   (SCAN_PREV_H - 192)
#define SCAN_HOLD_Y   (SCAN_PREV_Y + (SCAN_PREV_H - SCAN_HOLD_H) / 2)
#define SCAN_HOLD_LX  (SCAN_CARD_X + 12)
#define SCAN_HOLD_RX  (SCAN_CARD_X + SCAN_CARD_W - 12 - SCAN_HOLD_W)

enum { SCAN_VIEW = 0, SCAN_PICK, SCAN_CONNECT };

typedef struct {
    bool ok;
    char host[64];
    int  port;
    char pin[5];
} ScanLink;

static int         scan_phase;
static int         scan_sel;
static CamScanSnap scan_snap;               /* 本帧快照（input 读上一帧的结果） */
static ScanLink    scan_link;
static char        scan_text[CAM_SCAN_TEXT];/* 选中候选的原文 */
static int         scan_err;                /* 打开分享失败的提示（api_recv_link_error_take） */
static bool        scan_touch_hold;         /* 取景页手指正按在左右按住区里 */
static bool        scan_static_snap;        /* 候选来自暂存快照（相机已被确认页停掉） */

/* http://host[:port]/?pin=NNNN —— 与我们网页共享页二维码的内容格式一致 */
static bool link_parse(const char *s, ScanLink *out)
{
    const char *p = s, *h, *he;
    size_t hl;
    int port = 80, n, i;

    memset(out, 0, sizeof *out);
    if (strncasecmp(p, "http://", 7) != 0) return false;
    p += 7;

    h = p;
    while (*p && *p != ':' && *p != '/' && *p != '?') p++;
    he = p;
    hl = (size_t)(he - h);
    if (hl == 0 || hl >= sizeof out->host) return false;
    for (i = 0; h + i < he; i++)
        if (!(isalnum((unsigned char)h[i]) || h[i] == '.' || h[i] == '-'))
            return false;

    if (*p == ':') {
        int v = 0;
        p++;
        n = 0;
        while (*p >= '0' && *p <= '9' && n < 6) {
            v = v * 10 + (*p - '0');
            p++;
            n++;
        }
        if (n == 0 || n > 5 || v <= 0 || v > 65535) return false;
        port = v;
    }
    if (*p == '/') p++;
    if (*p != '?') return false;
    p++;
    if (strncmp(p, "pin=", 4) != 0) return false;
    p += 4;
    for (i = 0; i < 4; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        out->pin[i] = p[i];
    }
    out->pin[4] = 0;
    p += 4;
    if (*p != 0 && *p != '&') return false;

    memcpy(out->host, h, hl);
    out->host[hl] = 0;
    out->port = port;
    out->ok = true;
    return true;
}

/* 候选摘要：是分享链接就报 host/port/PIN，否则直接给判定 */
static void link_summary(const char *text, char *buf, size_t n)
{
    ScanLink l;
    if (link_parse(text, &l))
        snprintf(buf, n, "%s  %s:%d  PIN %s", tr("Share link"),
                 l.host, l.port, l.pin);
    else
        snprintf(buf, n, "%s", tr("Not a file transfer link"));
}

/* 相机坐标（640×480）→ 屏幕预览坐标，并夹在预览区内 */
static void map_box(const int *b, int *x0, int *y0, int *x1, int *y1)
{
    int px1 = SCAN_PREV_X + SCAN_PREV_W, py1 = SCAN_PREV_Y + SCAN_PREV_H;

    *x0 = SCAN_PREV_X + b[0] * SCAN_PREV_W / CAM_SCAN_FRAME_W;
    *y0 = SCAN_PREV_Y + b[1] * SCAN_PREV_H / CAM_SCAN_FRAME_H;
    *x1 = SCAN_PREV_X + b[2] * SCAN_PREV_W / CAM_SCAN_FRAME_W;
    *y1 = SCAN_PREV_Y + b[3] * SCAN_PREV_H / CAM_SCAN_FRAME_H;
    if (*x0 < SCAN_PREV_X) *x0 = SCAN_PREV_X;
    if (*y0 < SCAN_PREV_Y) *y0 = SCAN_PREV_Y;
    if (*x1 > px1) *x1 = px1;
    if (*y1 > py1) *y1 = py1;
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
}

/* 空间选码：从当前候选出发，在 (ox,oy) 方向上找最合适的一个。
 * 以包围盒中心为点，先过两道门槛：
 *   1) 必须落在该方向的半平面（上/下看 dy 的符号，左/右看 dx）；
 *   2) 方向锥：上/下要求 |dy| ≥ |dx|，左/右要求 |dx| ≥ |dy|（±45°）。
 * 第 2 道是"并排但高低略偏"的关键：两码中心差几十像素的高差、横向却隔着一大段
 * 距离时，它们对彼此只算左右邻居，不会因为"中心更低"就被当成上下（真机上
 * 会把页脚的下臂点亮、按下真的跳过去，明显不符合直觉）。
 * 过门槛后代价 = 主轴距离 + 2×侧偏，正对方向的那个优先。
 * 返回候选下标，没有可走的候选返回 -1。 */
static int scan_best(int ox, int oy)
{
    int n = scan_snap.n, i, best = -1;
    int cx, cy;
    long best_cost = 0;

    if (n <= 1 || scan_sel < 0 || scan_sel >= n) return -1;
    cx = (scan_snap.items[scan_sel].box[0] + scan_snap.items[scan_sel].box[2]) / 2;
    cy = (scan_snap.items[scan_sel].box[1] + scan_snap.items[scan_sel].box[3]) / 2;
    for (i = 0; i < n; i++) {
        int ix, iy;
        long ddx, ddy, adx, ady, primary, cross, cost;

        if (i == scan_sel) continue;
        ix = (scan_snap.items[i].box[0] + scan_snap.items[i].box[2]) / 2;
        iy = (scan_snap.items[i].box[1] + scan_snap.items[i].box[3]) / 2;
        ddx = (long)ix - cx;
        ddy = (long)iy - cy;
        if (ox > 0 && ddx <= 0) continue;
        if (ox < 0 && ddx >= 0) continue;
        if (oy > 0 && ddy <= 0) continue;
        if (oy < 0 && ddy >= 0) continue;
        adx = ddx < 0 ? -ddx : ddx;
        ady = ddy < 0 ? -ddy : ddy;
        if (ox && adx < ady) continue;   /* 左右移动：候选必须横向占优 */
        if (oy && ady < adx) continue;   /* 上下移动：候选必须纵向占优 */
        primary = ox ? adx : ady;
        cross   = ox ? ady : adx;
        cost = primary + cross * 2;
        if (best < 0 || cost < best_cost) { best = i; best_cost = cost; }
    }
    return best;
}

static void scan_move(int ox, int oy)
{
    int best;

    if (scan_phase != SCAN_PICK || scan_snap.n <= 1) return;
    best = scan_best(ox, oy);
    if (best >= 0) {                     /* 该方向没有候选就不动，不绕回 */
        scan_sel = best;
        scan_err = 0;                    /* 换选后旧候选的错误不再适用 */
    }
}

/* 当前候选在四个方向上分别走不走得动（页脚灰臂用，口径同 hintbar 的 dir_off） */
static unsigned scan_dir_avail(void)
{
    unsigned m = 0;

    if (scan_best(0, -1) >= 0) m |= HDIR_UP;
    if (scan_best(0, 1) >= 0)  m |= HDIR_DOWN;
    if (scan_best(-1, 0) >= 0) m |= HDIR_LEFT;
    if (scan_best(1, 0) >= 0)  m |= HDIR_RIGHT;
    return m;
}

/* 2px 矩形描边（vita2d 没有现成的粗线） */
static void scan_box(int x0, int y0, int x1, int y1, uint32_t c)
{
    w_rect((Rect){ x0, y0, x1 - x0, 2 }, c);
    w_rect((Rect){ x0, y1 - 2, x1 - x0, 2 }, c);
    w_rect((Rect){ x0, y0, 2, y1 - y0 }, c);
    w_rect((Rect){ x1 - 2, y0, 2, y1 - y0 }, c);
}

/* 左右按住区：半透明底 + 2px 描边；按住时描边/文字用 accent，空闲用弱色。
 * 标签居中、限宽裁剪，短标签不会溢出。 */
static void scan_hold_zone(Rect r, bool on)
{
    const char *lbl = on ? tr("Scanning...") : tr("Scan");
    uint32_t bd = on ? theme->accent : theme->border;
    int tw = 0;

    w_rect(r, 0x50000000u);
    scan_box(r.x, r.y, r.x + r.w, r.y + r.h, bd);
    w_text_w(0.9f, lbl, &tw, NULL);
    if (tw > r.w - 12) tw = r.w - 12;
    w_text_clip(r.x + (r.w - tw) / 2, r.y + (r.h - 18) / 2, 0.9f,
                on ? theme->accent : theme->text_dim, lbl, r.w - 12);
}

/* 取景框四角（只是对准提示；解码用的是整幅画面） */
static void scan_frame_mark(int cx, int cy)
{
    const int hw = 120, hh = 120, arm = 26, t = 3;
    uint32_t c = theme->accent;
    w_rect((Rect){ cx - hw, cy - hh, arm, t }, c);
    w_rect((Rect){ cx - hw, cy - hh, t, arm }, c);
    w_rect((Rect){ cx + hw - arm, cy - hh, arm, t }, c);
    w_rect((Rect){ cx + hw - t, cy - hh, t, arm }, c);
    w_rect((Rect){ cx - hw, cy + hh - t, arm, t }, c);
    w_rect((Rect){ cx - hw, cy + hh - arm, t, arm }, c);
    w_rect((Rect){ cx + hw - arm, cy + hh - t, arm, t }, c);
    w_rect((Rect){ cx + hw - t, cy + hh - arm, t, arm }, c);
}

void open_scan(void)
{
    scan_phase = SCAN_VIEW;
    scan_sel = 0;
    scan_err = 0;
    scan_static_snap = false;
    scan_text[0] = 0;
    memset(&scan_link, 0, sizeof scan_link);
    cam_scan_clear_hit();
    cam_scan_start();
    g_app.page = PAGE_SCAN;
}

/* 离开扫码页：相机停掉；还在连分享的话把连接请求一并取消（否则它拿到清单后
 * 会顶一张确认页出来，而用户已经不在扫码流程里了）。 */
static void scan_leave(void)
{
    if (api_recv_link_connecting()) {
        api_recv_link_cancel();
        api_recv_link_error_take();      /* 丢掉自己造成的 CANCELLED，别串到下次 */
    }
    cam_scan_stop();
    scan_phase = SCAN_VIEW;
    scan_err = 0;
    scan_touch_hold = false;
    scan_static_snap = false;
    g_app.page = PAGE_DEVICES;           /* 扫码从主页进入，退出回主页 */
}

/* 被接收确认页顶掉（pages_recv.c 调）：相机停掉省资源，但把定格候选留在
 * scan_snap 里（纹理也保留最后一帧）——确认页拒绝/关闭后能原样回到候选页。
 * 若当时还在取景（没有定格结果），标记为"无暂存"，返回时重开相机。 */
void scan_leave_for_recv(void)
{
    cam_scan_stop();
    scan_touch_hold = false;
    scan_err = 0;
    if (scan_phase == SCAN_PICK || scan_phase == SCAN_CONNECT) {
        scan_static_snap = true;
        scan_phase = SCAN_PICK;
    } else {
        scan_static_snap = false;
    }
}

/* 从接收确认页返回到扫码流程：有定格候选就回候选页，否则回取景并重开相机 */
void scan_resume_pick(void)
{
    scan_err = 0;
    if (scan_static_snap) {
        scan_phase = SCAN_PICK;
    } else {
        scan_phase = SCAN_VIEW;
        scan_static_snap = false;
        cam_scan_clear_hit();
        cam_scan_start();
    }
    g_app.page = PAGE_SCAN;
}

/* 聚焦第 i 个候选：换选时清掉上一次的打开错误，并把判定结果算好 */
static void scan_focus(int i)
{
    if (i < 0 || i >= scan_snap.n) return;
    if (i != scan_sel) scan_err = 0;
    scan_sel = i;
    snprintf(scan_text, sizeof scan_text, "%s", scan_snap.items[i].text);
    link_parse(scan_text, &scan_link);
}

/* 清掉冻结结果、回到取景（相机若已被确认页停掉则重新启动） */
static void scan_rescan(void)
{
    cam_scan_clear_hit();
    cam_scan_start();
    scan_phase = SCAN_VIEW;
    scan_sel = 0;
    scan_err = 0;
    scan_touch_hold = false;
    scan_static_snap = false;
    memset(&scan_link, 0, sizeof scan_link);
    scan_text[0] = 0;
}

/* 打开当前候选：非链接码只重算不改状态（信息带已经说明原因） */
static void scan_open_selected(void)
{
    scan_focus(scan_sel);
    if (!scan_link.ok) return;
    scan_err = 0;
    if (api_recv_link_open(scan_link.host, scan_link.port, scan_link.pin) != 0) {
        scan_err = 5;                    /* 本机已有接收会话 / 上一次连接还没收 */
        return;                          /* 留在候选页，信息带报错 */
    }
    scan_phase = SCAN_CONNECT;
}

/* 统一返回：永远回上一个界面/状态（取景 ← 候选 ← 连接中） */
static void scan_back(void)
{
    if (scan_phase == SCAN_PICK) {
        scan_rescan();
        return;
    }
    if (scan_phase == SCAN_CONNECT) {
        if (api_recv_link_connecting()) {
            api_recv_link_cancel();
            api_recv_link_error_take();  /* 丢掉自己造成的 CANCELLED */
        }
        scan_phase = SCAN_PICK;          /* 候选留着，回到选码 */
        scan_err = 0;
        return;
    }
    scan_leave();
}

static const char *scan_err_text(int e)
{
    switch (e) {
    case 1:  return tr("Cannot connect to the share");
    case 2:  return tr("Share refused (wrong PIN or stopped)");
    case 3:  return tr("Unexpected response from the share");
    case 5:  return tr("Another transfer is in progress");
    case 4:  return tr("Cannot open this link");
    default: return tr("Cannot open this link");
    }
}

void page_scan_render(void)
{
    Rect card = { SCAN_CARD_X, SCAN_CARD_Y, SCAN_CARD_W, SCAN_CARD_H };
    Rect prev = { SCAN_PREV_X, SCAN_PREV_Y, SCAN_PREV_W, SCAN_PREV_H };
    struct vita2d_texture *tex;
    bool hold = false;
    int i;

    w_page_header(tr("Scan a QR code"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_SCAN, HDRBTN_BACK, false);
    w_rect(card, theme->card);

    /* 暂存快照（确认页返回的候选）没有相机可读：保留旧候选，不刷新快照 */
    if (!scan_static_snap) {
        hold = (scan_phase == SCAN_VIEW) && (ui_input_r_down() || scan_touch_hold);
        cam_scan_set_hold(hold);
        cam_scan_snapshot(&scan_snap);
    }

    /* 命中 → 按一次"快门"：帧与解码都已在引擎里停住，这里只切界面。
     * 在 render 里转，是因为冻结不一定伴随按键事件（可能正好在松开 R 之后）。 */
    if (!scan_static_snap && scan_phase == SCAN_VIEW &&
        scan_snap.frozen && scan_snap.n > 0) {
        scan_phase = SCAN_PICK;
        scan_sel = 0;
        scan_focus(0);
        scan_touch_hold = false;
        scan_err = 0;
    }

    /* 连接分享失败 → 回候选页就地报错；成功的话 pages_tick 已经把接收确认页
     * 顶上来（本页在可打断列表里），这里不用再做什么。 */
    if (scan_phase == SCAN_CONNECT) {
        int e = api_recv_link_error_take();
        if (e) {
            scan_phase = SCAN_PICK;
            scan_err = e;
        }
    }

    tex = cam_scan_preview_tex();
    if (tex)
        vita2d_draw_texture_scale(tex, (float)SCAN_PREV_X, (float)SCAN_PREV_Y,
                                  (float)SCAN_PREV_W / CAM_SCAN_PREV_W,
                                  (float)SCAN_PREV_H / CAM_SCAN_PREV_H);
    else
        w_text(SCAN_CARD_X + 24, SCAN_CARD_Y + 24, 1.0f, theme->text_dim, "%s",
               tr("Starting camera..."));

    if (scan_phase == SCAN_VIEW) {
        const char *msg;
        if (scan_snap.state == CAM_SCAN_ERROR)
            msg = tr("Camera unavailable");
        else if (!tex)
            msg = tr("Starting camera...");
        else if (hold)
            msg = tr("Scanning...");
        else
            msg = tr("Hold R to scan");

        w_rect((Rect){ SCAN_PREV_X, SCAN_PREV_Y, SCAN_PREV_W, 64 }, 0xB0000000u);
        w_text(SCAN_PREV_X + 14, SCAN_PREV_Y + 10, 1.0f, theme->text, "%s", msg);
        if (scan_snap.state == CAM_SCAN_ERROR)
            w_text(SCAN_PREV_X + 14, SCAN_PREV_Y + 34, 0.8f, theme->warn,
                   "0x%08X", (unsigned)scan_snap.err);
        else
            w_text(SCAN_PREV_X + 14, SCAN_PREV_Y + 34, 0.8f, theme->text_dim, "%s",
                   tr("Point at a QR code"));
        if (scan_snap.state != CAM_SCAN_ERROR)
            scan_frame_mark(SCAN_PREV_X + SCAN_PREV_W / 2,
                            SCAN_PREV_Y + SCAN_PREV_H / 2);

        /* 左右按住区：整块可按住取景；只在取景态注册热区 */
        {
            Rect hl = { SCAN_HOLD_LX, SCAN_HOLD_Y, SCAN_HOLD_W, SCAN_HOLD_H };
            Rect hr = { SCAN_HOLD_RX, SCAN_HOLD_Y, SCAN_HOLD_W, SCAN_HOLD_H };
            scan_hold_zone(hl, scan_touch_hold);
            scan_hold_zone(hr, scan_touch_hold);
            w_add(WID_SCAN_HOLD_L, hl);
            w_add(WID_SCAN_HOLD_R, hr);
        }
    } else {
        int n = scan_snap.n;
        char sum[160];

        /* PICK / CONNECT 共用：定格画面 + 候选框 + 底部信息带。
         * CONNECT 下候选只画不注册（连接中点击无意义）。 */
        w_rect(prev, 0x70000000u);
        for (i = 0; i < n && i < CAM_SCAN_MAX; i++) {
            int x0, y0, x1, y1;
            char idx[8];
            map_box(scan_snap.items[i].box, &x0, &y0, &x1, &y1);
            scan_box(x0 - 4, y0 - 4, x1 + 4, y1 + 4,
                     i == scan_sel ? theme->accent : theme->text_dim);
            snprintf(idx, sizeof idx, "%d", i + 1);
            w_text(x0 - 2, y0 - 24, 0.8f,
                   i == scan_sel ? theme->accent : theme->text_dim, "%s", idx);
            if (scan_phase == SCAN_PICK)
                w_add(WID_SCAN_BOX + i,
                      (Rect){ x0 - 8, y0 - 8, x1 - x0 + 16, y1 - y0 + 16 });
        }

        /* 底部信息带：第一行 n/N + 判定摘要；第二行按优先级显示连接态/错误/原文 */
        w_rect((Rect){ SCAN_CARD_X, LIST_BOTTOM - 70, SCAN_CARD_W, 70 }, 0xE0000000u);
        if (scan_sel >= 0 && scan_sel < n) {
            char num[16];
            snprintf(num, sizeof num, "%d/%d  ", scan_sel + 1, n);
            link_summary(scan_snap.items[scan_sel].text, sum, sizeof sum);
            w_text_clip(SCAN_CARD_X + 16, LIST_BOTTOM - 62, 1.0f, theme->accent,
                        num, 60);
            w_text_clip(SCAN_CARD_X + 80, LIST_BOTTOM - 62, 1.0f, theme->text,
                        sum, SCAN_CARD_W - 96);
        }
        if (scan_phase == SCAN_CONNECT)
            w_text_clip(SCAN_CARD_X + 16, LIST_BOTTOM - 34, 0.8f, theme->accent,
                        tr("Opening share..."), SCAN_CARD_W - 32);
        else if (scan_err)
            w_text_clip(SCAN_CARD_X + 16, LIST_BOTTOM - 34, 0.8f, theme->warn,
                        scan_err_text(scan_err), SCAN_CARD_W - 32);
        else if (scan_sel >= 0 && scan_sel < n)
            w_text_clip(SCAN_CARD_X + 16, LIST_BOTTOM - 34, 0.8f, theme->text_dim,
                        scan_snap.items[scan_sel].text, SCAN_CARD_W - 32);
    }

    {
        HintSeg segs[4] = { 0 };
        int ns = 0;
        if (scan_phase == SCAN_VIEW) {
            segs[ns].key = HKEY_R; segs[ns++].text = tr("Hold to scan");
            segs[ns].key = HKEY_BACK; segs[ns++].text = tr("Back");
        } else if (scan_phase == SCAN_PICK) {
            if (scan_snap.n > 1) {
                /* 四向都能选（按包围盒位置走），只把"当前这个码那边没有候选"的
                 * 臂灰掉——与列表页"到头灰臂"的语义一致 */
                segs[ns].key = HKEY_DPAD;
                segs[ns].dir_off = (uint8_t)(
                    (HDIR_UP | HDIR_DOWN | HDIR_LEFT | HDIR_RIGHT) &
                    ~scan_dir_avail());
                segs[ns++].text = tr("Choose");
            }
            segs[ns].key = HKEY_CONFIRM; segs[ns++].text = tr("Confirm");
            segs[ns].key = HKEY_BACK; segs[ns++].text = tr("Scan again");
        } else if (scan_phase == SCAN_CONNECT) {
            segs[ns].key = HKEY_BACK; segs[ns++].text = tr("Cancel");
        }
        w_page_footer_segs(segs, ns);
    }
}

void page_scan_input(const Input *in)
{
    /* 取景态左右按住区：按住即扫描；松手/滑走/切状态立刻停 */
    if (scan_phase == SCAN_VIEW && in->touch_hold) {
        scan_touch_hold =
            (in->touch_x >= SCAN_HOLD_LX && in->touch_x < SCAN_HOLD_LX + SCAN_HOLD_W &&
             in->touch_y >= SCAN_HOLD_Y && in->touch_y < SCAN_HOLD_Y + SCAN_HOLD_H) ||
            (in->touch_x >= SCAN_HOLD_RX && in->touch_x < SCAN_HOLD_RX + SCAN_HOLD_W &&
             in->touch_y >= SCAN_HOLD_Y && in->touch_y < SCAN_HOLD_Y + SCAN_HOLD_H);
    } else {
        scan_touch_hold = false;
    }

    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_HDR_BACK_SCAN) { scan_back(); return; }
        /* 按住区松手产生的 tap 不做事（动作已在按住期间完成） */
        if (id == WID_SCAN_HOLD_L || id == WID_SCAN_HOLD_R) return;
        if (scan_phase == SCAN_PICK && id >= WID_SCAN_BOX &&
            id < WID_SCAN_BOX + CAM_SCAN_MAX) {
            int i = id - WID_SCAN_BOX;
            if (i < scan_snap.n) {
                if (i != scan_sel) scan_focus(i);   /* 第一次点：只聚焦 */
                else scan_open_selected();          /* 再点一次：打开 */
            }
        }
        return;
    }
    /* 候选之间按框的位置四向走（2×2 四码时上下左右都符合直觉）；没有候选
     * 的方向不动。页脚的四臂亮灭与这里同一套判定（scan_dir_avail）。 */
    if (in->up)    { scan_move(0, -1); return; }
    if (in->down)  { scan_move(0, 1);  return; }
    if (in->left)  { scan_move(-1, 0); return; }
    if (in->right) { scan_move(1, 0);  return; }
    if (in->confirm) {
        if (scan_phase == SCAN_PICK) scan_open_selected();
        return;                              /* 连接中忽略确认键 */
    }
    if (in->back) { scan_back(); return; }
}
