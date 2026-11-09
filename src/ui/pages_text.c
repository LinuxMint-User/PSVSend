/* 页面：剪贴板文本查看（PAGE_TEXT_VIEW）。
 *
 * 官方 LocalSend 把"发剪贴板文字"实现成一个普通文件（fileName = 随机
 * UUID.txt，fileType = text/*），接收端拿到的就是一段文字内容。这里给一个
 * 只读预览页：进入时把内容读进内存、按屏幕宽度自动换行、可滚动。
 * 接收方向传完一条文本消息后由进度页自动打开本页（见 pages_progress.c）。
 *
 * 与官方一致，文本默认**不落盘**：接收侧只写到临时文件，用户在这里点"保存"
 * 才改名成正式文件；直接返回就丢弃。PSV 系统默认没有粘贴入口（要 PasteUnlock
 * 之类插件），所以不提供"复制到剪贴板"。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "app/api.h"
#include "core/i18n.h"
#include "core/dlog.h"

#define WID_HDR_BACK_TXT 0x8210
#define WID_TXT_SAVE     0x8211

#define TEXT_MAX_BYTES   (32 * 1024)   /* 读入上限：超出只显示前 32 KB */
#define TEXT_SCALE       1.0f
#define TEXT_LINE_H      26
#define TEXT_AREA_X      36
#define TEXT_AREA_W      (SCR_W - 72)
#define TEXT_AREA_Y      96
#define TEXT_AREA_H      (SCR_H - FOOTER_H - TEXT_AREA_Y - 12)  /* 延到页脚上沿 */
#define TEXT_BTN_W       150
#define TEXT_BTN_H       34

static char  *t_buf = NULL;      /* 文本（已把 CRLF 规整成 LF） */
static int    t_len = 0;
static int   *t_line = NULL;     /* 每行起始字节偏移，t_lines 个 */
static int    t_lines = 0;
static int    t_truncated = 0;   /* 文件超过 TEXT_MAX_BYTES */
static int    t_empty = 0;
static int    t_saved = 0;       /* 已存盘：返回时不再丢弃 */
static int    t_scroll = 0;      /* 首行行号 */
static ScrollGlide t_glide;
static int    t_press_scroll = 0;

/* UTF-8 单字符字节数（非法首字节按 1 处理） */
static int cp_len(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/* 第 off 个字节起、长 n 字节的字符在当前字号下的像素宽 */
static int cp_width(int off, int n)
{
    char tmp[8];
    int w = 0;

    if (n > (int)sizeof tmp - 1) n = (int)sizeof tmp - 1;
    memcpy(tmp, t_buf + off, (size_t)n);
    tmp[n] = 0;
    w_text_w(TEXT_SCALE, tmp, &w, NULL);
    return w;
}

/* 按屏幕宽度贪心换行：优先在行内最后一个空格断，找不到就硬断；'\n' 永远断行。
 * 结果存成行首偏移表，渲染时只画可见行。 */
static void text_layout(void)
{
    int i = 0, line_start = 0, w = 0, last_space = -1;

    t_lines = 0;
    while (i < t_len) {
        unsigned char c = (unsigned char)t_buf[i];
        int n, cw, k;

        if (c == '\n') {
            t_line[t_lines++] = i + 1;
            i++;
            line_start = i;
            w = 0;
            last_space = -1;
            continue;
        }
        n = cp_len(c);
        if (i + n > t_len) n = t_len - i;
        cw = cp_width(i, n);
        if (w + cw > TEXT_AREA_W && i > line_start) {
            /* 优先在行内最后一个空格处断（空格留在上一行末尾），否则硬断 */
            int at_space = last_space > line_start;
            line_start = at_space ? last_space + 1 : i;
            t_line[t_lines++] = line_start;
            /* 被挪到新行的尾巴（≤ 一个字符）重新计宽 */
            w = 0;
            for (k = line_start; k < i; k++)
                w += cp_width(k, cp_len((unsigned char)t_buf[k]));
            last_space = -1;
            continue;               /* 当前字符在新行里重新判断 */
        }
        if (c == ' ') last_space = i;
        w += cw;
        i += n;
    }
    if (t_lines == 0) t_line[t_lines++] = 0;
}

static void text_free(void)
{
    free(t_buf);
    free(t_line);
    t_buf = NULL;
    t_line = NULL;
    t_len = t_lines = 0;
}

void page_text_open(const char *path)
{
    SceUID fd;
    int cap, len = 0;

    text_free();
    t_truncated = t_empty = 0;
    t_saved = 0;
    t_scroll = 0;
    glide_reset(&t_glide);

    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        dlog("text: open %s failed 0x%08X", path, (unsigned)fd);
    } else {
        cap = TEXT_MAX_BYTES;
        t_buf = malloc((size_t)cap + 1);
        if (t_buf) {
            len = (int)sceIoRead(fd, t_buf, (SceSize)cap);
            if (len < 0) len = 0;
            if (len == cap) {           /* 还有数据 → 被截断 */
                char extra;
                if (sceIoRead(fd, &extra, 1) == 1) t_truncated = 1;
            }
        }
        sceIoClose(fd);
    }

    if (!t_buf) {
        t_buf = malloc(1);
        len = 0;
    }
    if (!t_buf) { g_app.page = PAGE_PROGRESS; return; }   /* 极端：内存不足 */

    /* CRLF → LF（Windows 端复制来的文本很常见），顺带去掉末尾的 NUL */
    {
        int i = 0, o = 0;
        while (i < len) {
            char c = t_buf[i++];
            if (c == '\r') {
                if (i < len && t_buf[i] == '\n') continue;
                c = '\n';
            }
            t_buf[o++] = c;
        }
        len = o;
    }
    t_len = len;
    t_buf[t_len] = 0;
    t_empty = (t_len == 0);

    t_line = malloc(sizeof(int) * (size_t)(t_len + 2));
    if (!t_line) { text_free(); g_app.page = PAGE_PROGRESS; return; }
    text_layout();

    dlog("text: open %s len=%d lines=%d trunc=%d", path, t_len, t_lines, t_truncated);
    g_app.page = PAGE_TEXT_VIEW;
}

static int visible_lines(void)
{
    /* 截断提示占底部一行：出现提示时少显示一行，避免压在正文最后一行上 */
    int h = TEXT_AREA_H - (t_truncated ? TEXT_LINE_H : 0);
    int n = h / TEXT_LINE_H;
    return n < 1 ? 1 : n;
}

static int max_scroll(void)
{
    int m = t_lines - visible_lines();
    return m < 0 ? 0 : m;
}

void page_text_render(void)
{
    int i, vis, y;
    Rect card = { TEXT_AREA_X - 12, TEXT_AREA_Y - 10,
                  TEXT_AREA_W + 24, TEXT_AREA_H + 20 };

    t_scroll = glide_step(&t_glide, t_scroll, max_scroll());

    w_page_header(tr("Clipboard text"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_TXT, HDRBTN_BACK, false);
    w_rect(card, theme->card);

    if (t_empty) {
        w_text(TEXT_AREA_X, TEXT_AREA_Y + 8, TEXT_SCALE, theme->text_dim,
               "%s", tr("(empty text)"));
    } else {
        vis = visible_lines();
        for (i = 0; i < vis; i++) {
            int li = t_scroll + i;
            int s, e;
            char save;

            if (li >= t_lines) break;
            s = t_line[li];
            e = (li + 1 < t_lines) ? t_line[li + 1] : t_len;
            if (e > s && t_buf[e - 1] == '\n') e--;
            if (e > t_len) e = t_len;
            if (e <= s) continue;              /* 空行：跳过绘制（行高仍占位） */
            y = TEXT_AREA_Y + i * TEXT_LINE_H;
            save = t_buf[e];
            t_buf[e] = 0;
            w_text(TEXT_AREA_X, y, TEXT_SCALE, theme->text, "%s", t_buf + s);
            t_buf[e] = save;
        }
    }

    if (t_truncated)
        w_text_clip(TEXT_AREA_X, TEXT_AREA_Y + TEXT_AREA_H - 18, 0.9f,
                    theme->warn,
                    tr("Text is too long; only the beginning is shown"),
                    TEXT_AREA_W);

    {
        HintSeg segs[3] = { 0 };
        int ns = 0;
        segs[ns].key = HKEY_DPAD;
        segs[ns++].text = tr("Scroll");
        segs[ns].key = HKEY_SQUARE;
        segs[ns].text = tr("Save");
        segs[ns++].dim = t_saved;
        segs[ns].key = HKEY_BACK;
        segs[ns++].text = tr("Back");
        w_page_footer_segs(segs, ns);
    }

    /* 保存：文本默认只在临时文件里，点这里才落正式名；不点就随会话丢弃。
     * 触摸按钮固定在屏幕右下角（页脚右侧空位），键盘走 □ 键。 */
    {
        Rect save = { SCR_W - 28 - TEXT_BTN_W,
                      SCR_H - FOOTER_H + (FOOTER_H - TEXT_BTN_H) / 2,
                      TEXT_BTN_W, TEXT_BTN_H };
        if (t_saved) {
            w_text_right(save.x + save.w, save.y + 7, 1.0f, theme->text_dim,
                         "%s", tr("Saved"));
        } else {
            w_add(WID_TXT_SAVE, save);
            w_button(save, tr("Save"), false);
        }
    }
}

/* 离开本页：没保存就丢弃（与官方"文本只存在内存/会话里"一致）。 */
static void text_leave(void)
{
    if (!t_saved) api_recv_text_discard();
    text_free();
    g_app.page = PAGE_PROGRESS;
}

/* 落正式文件名；失败只在日志里留痕（临时文件仍在，可再按一次重试） */
static void text_save(void)
{
    char path[512];

    if (api_recv_text_save(path, sizeof path))
        t_saved = 1;
    else
        dlog("text: save failed");
}

void page_text_input(const Input *in)
{
    if (in->drag_start || in->dragging) {
        glide_stop(&t_glide);
        if (in->drag_start) t_press_scroll = t_scroll * TEXT_LINE_H;
        t_scroll = (t_press_scroll - in->drag_dy) / TEXT_LINE_H;
        if (t_scroll < 0) t_scroll = 0;
        if (t_scroll > max_scroll()) t_scroll = max_scroll();
        return;
    }
    if (in->drag_end) {
        glide_release(&t_glide, in);
        return;
    }
    glide_stop(&t_glide);
    if (in->tap) {
        int id = w_hit(in->tap_x, in->tap_y);
        if (id == WID_HDR_BACK_TXT) {
            text_leave();
        } else if (id == WID_TXT_SAVE && !t_saved) {
            text_save();
        }
        return;
    }
    if (in->back) {
        text_leave();
        return;
    }
    if (in->square && !t_saved) {
        text_save();
        return;
    }
    if (in->up)   t_scroll--;
    if (in->down) t_scroll++;
    if (in->left)  t_scroll -= visible_lines();
    if (in->right) t_scroll += visible_lines();
    if (t_scroll < 0) t_scroll = 0;
    if (t_scroll > max_scroll()) t_scroll = max_scroll();
}
