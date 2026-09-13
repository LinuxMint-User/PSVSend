/* 页面：进度页（发送/接收共用）。
 * 发送路径每帧从 xfer 快照取真实进度；接收路径从 receive 会话快照取。
 * 顶部为文件列表（像素滚动），下方为总进度条与高级面板（计数/耗时/速度）。
 * 行清单 xf_* 定义在 pages.c：发送由 pages_send.c 的 start_send 写入，
 * 接收由 pages_recv.c 的 start_recv 写入（首帧兜底），本页每帧覆盖。 */
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/i18n.h"
#include "proto/transfer.h"
#include "proto/receive.h"

/* 传输页：文件列表区域与总进度布局（发送/接收共用） */
#define XF_TOP        68
#define XF_BOTTOM     370      /* 文件列表可视区底（总进度条上方） */
#define XF_VIEW_H     (XF_BOTTOM - XF_TOP)
#define XF_ROW_H      58

static int xf_press_scroll = 0;

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
