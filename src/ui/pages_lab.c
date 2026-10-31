/* 页面：实验室（PAGE_LAB）+ 摄像头基准（PAGE_CAM_BENCH）。
 *
 * 入口在设置页「实验」分组 → 「实验室」。这里放的是正式功能之外的实验性
 * 小工具，长期保留、手动触发；第一项就是后摄采集 + QR 解码基准（为将来
 * 「扫二维码下载别人分享的文件」探路）。 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "app/cam_bench.h"

/* 实验室页行 id（只有一个可选项，后续实验项按 +1 排） */
#define WID_LAB_CAM        0xA001
/* 摄像头基准页 */
#define WID_CB_RES         0xA101   /* 分辨率行：点一下切档（运行中不响应） */
#define WID_CB_START       0xA102   /* 开始 / 停止按钮 */

/* 基准页没有跨页状态，分辨率档位只在本页记住（不落盘：实验项，没必要污染配置） */
static int cb_res;

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
    Rect r = { 24, LIST_TOP, SCR_W - 48, ROW_H };
    RowGeom g;
    const char *sum = cam_bench_summary();
    char sub[160];
    uint32_t card, tc, dc;

    w_page_header(tr("Lab"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_LAB, HDRBTN_BACK, false);

    if (cam_bench_running())
        snprintf(sub, sizeof sub, "%s", tr("Running"));
    else if (sum)
        snprintf(sub, sizeof sub, "%s", sum);
    else
        snprintf(sub, sizeof sub, "%s", tr("Not run"));

    /* 单行列表：整行选中态（实验室目前只有一项） */
    card = theme->accent;
    tc = theme->accent_text;
    dc = theme->accent_text;
    w_rect(r, card);
    row_geom(r, ROW_2LINE, 0, &g);
    w_text_clip(g.x_text, g.y_main, g.main_sc, tc, tr("Camera benchmark"), g.w_text);
    w_text_clip(g.x_text, g.y_sub, g.sub_sc, dc, sub, g.w_text);
    add_row_hit(WID_LAB_CAM, LIST_TOP);

    {
        HintSeg segs[3] = { 0 };
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
        if (id == WID_LAB_CAM)      { lab_open_cam_bench(); return; }
        return;
    }
    if (in->confirm) lab_open_cam_bench();
    if (in->back)    lab_leave();
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
