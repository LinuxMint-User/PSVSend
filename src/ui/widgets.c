/* 控件绘制：文字 / 矩形 / 列表行 / 按钮 / 进度条 / 弹窗 / 头尾栏 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vita2d.h>
#include <psp2/pvf.h>
#include "ui.h"
#include "theme.h"

extern vita2d_pvf *g_font;

/* ---------- 文字 ---------- */
/* vita2d_pvf_draw_text 的 y 是"基线"：字形从基线向上伸、少量向下垂。
 * 我们统一 w_text 的 y 语义为"文本行可视顶部（升部线）"，
 * 绘制时内部把基线放在 y + ascent*scale。
 * ascent 按 1/64 像素单位从默认字体字形信息里读一次（与 vita2d 内部换算同源），
 * 失败时兜底 8.0（10pt 字号典型升部）。 */
static float font_ascent  = 8.0f;
static bool  font_metric_ok = false;

static void ensure_font_metrics(void)
{
    if (font_metric_ok || !g_font) return;
    font_metric_ok = true;
    /* vita2d_pvf 内部布局：+4 为字体链表头节点，节点 +0 是 ScePvfFontId */
    ScePvfFontId *node = *(ScePvfFontId **)((char *)g_font + 4);
    if (!node) return;
    ScePvfCharInfo ci;
    if (scePvfGetCharInfo(node[0], 'A', &ci) >= 0) {
        float asc = (float)(ci.glyphMetrics.ascender64 >> 6);
        if (asc >= 2.0f && asc <= 60.0f) font_ascent = asc;
    }
}

void w_text(float x, float y, float scale, uint32_t color, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_font) {
        ensure_font_metrics();
        vita2d_pvf_draw_text(g_font, (int)x,
                             (int)(y + font_ascent * scale + 0.5f),
                             color, scale, buf);
    }
}

void w_text_w(float scale, const char *text, int *w, int *h)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (g_font && text)
        vita2d_pvf_text_dimensions(g_font, scale, text, w, h);
}

static const char *utf8_skip(const char *p)
{
    unsigned char c = (unsigned char)*p;
    if (c < 0x80) return p + 1;
    if ((c & 0xE0) == 0xC0) return p + 2;
    if ((c & 0xF0) == 0xE0) return p + 3;
    if ((c & 0xF8) == 0xF0) return p + 4;
    return p + 1;
}

void w_text_clip(float x, float y, float scale, uint32_t color,
                 const char *text, int max_w)
{
    char buf[512];
    int cur_w = 0;
    const char *p = text;
    int n = 0;
    while (*p) {
        const char *next = utf8_skip(p);
        char tmp[512];
        memcpy(tmp, buf, n);
        memcpy(tmp + n, p, next - p);
        tmp[n + (next - p)] = 0;
        int w = 0, h = 0;
        w_text_w(scale, tmp, &w, &h);
        if (w > max_w) break;
        memcpy(buf, tmp, n + (next - p));
        n += (next - p);
        buf[n] = 0;
        cur_w = w;
        p = next;
    }
    if (cur_w > 0 || n == 0)
        w_text(x, y, scale, color, "%s", buf);
}

/* ---------- 图形 ---------- */
void w_rect(Rect r, uint32_t color)
{
    vita2d_draw_rectangle(r.x, r.y, r.w, r.h, color);
}

void w_rect_outline(Rect r, uint32_t color)
{
    vita2d_draw_rectangle(r.x, r.y, r.w, 2, color);
    vita2d_draw_rectangle(r.x, r.y + r.h - 2, r.w, 2, color);
    vita2d_draw_rectangle(r.x, r.y, 2, r.h, color);
    vita2d_draw_rectangle(r.x + r.w - 2, r.y, 2, r.h, color);
}

void w_bar(Rect r, uint32_t bg, uint32_t fg, int pct)
{
    w_rect(r, bg);
    if (pct > 0) {
        Rect f = { r.x + 2, r.y + 2, (r.w - 4) * (pct > 100 ? 100 : pct) / 100, r.h - 4 };
        w_rect(f, fg);
    }
}

/* ---------- 布局组件 ---------- */
#define HEADER_H 52
#define FOOTER_H 46

void w_page_header(const char *title)
{
    w_text(28, 10, 1.7f, theme->text, "%s", title);
    w_rect((Rect){ 0, HEADER_H - 2, SCR_W, 2 }, theme->border);
}

void w_page_footer(const char *hint)
{
    Rect f = { 0, SCR_H - FOOTER_H, SCR_W, FOOTER_H };
    w_rect(f, theme->bg);
    w_rect((Rect){ 0, f.y, SCR_W, 2 }, theme->border);
    w_text(28, f.y + 12, 1.0f, theme->text_dim, "%s", hint);
}

/* ---------- 按键图标（底部提示用） ---------- */
/* 图标以 (cx,cy) 为中心，画在约 22x22 内。
 * 注意：只用 vita2d_draw_line / rectangle / fill_circle 等高层绘制，
 * 不要直接调 vita2d_draw_array（无着色器绑定，会 GPU fault 导致整机重启）。
 * 空心图形用主题背景色挖空内部形成描边，因此只用于底部/卡片等纯色底上。 */

/* 加粗线段：垂直方向偏移 ±0.8 画 3 条近似 2~3px */
static void thick_seg(float x0, float y0, float x1, float y1, uint32_t c)
{
    float nx = -(y1 - y0), ny = (x1 - x0);
    float l = sqrtf(nx * nx + ny * ny);
    float ox = 0.0f, oy = 0.0f;
    if (l > 0.01f) { ox = nx / l * 0.8f; oy = ny / l * 0.8f; }
    vita2d_draw_line(x0 + ox, y0 + oy, x1 + ox, y1 + oy, c);
    vita2d_draw_line(x0, y0, x1, y1, c);
    vita2d_draw_line(x0 - ox, y0 - oy, x1 - ox, y1 - oy, c);
}

static void w_icon(HintIcon ic, float cx, float cy)
{
    uint32_t c = theme->text;
    switch (ic) {
    case HICON_CROSS:
        thick_seg(cx - 8, cy - 8, cx + 8, cy + 8, c);
        thick_seg(cx - 8, cy + 8, cx + 8, cy - 8, c);
        break;
    case HICON_CIRCLE:
        vita2d_draw_fill_circle(cx, cy, 9.0f, c);
        vita2d_draw_fill_circle(cx, cy, 5.5f, theme->bg);
        break;
    case HICON_TRIANGLE:
        thick_seg(cx, cy - 10, cx - 9, cy + 7, c);
        thick_seg(cx, cy - 10, cx + 9, cy + 7, c);
        thick_seg(cx - 9, cy + 7, cx + 9, cy + 7, c);
        break;
    case HICON_SQUARE:
        w_rect((Rect){ (int)cx - 9, (int)cy - 9, 18, 18 }, c);
        w_rect((Rect){ (int)cx - 5, (int)cy - 5, 10, 10 }, theme->bg);
        break;
    case HICON_START:
        /* 横向胶囊：填充 + 两条竖向挖空 */
        w_rect((Rect){ (int)cx - 12, (int)cy - 7, 24, 14 }, c);
        w_rect((Rect){ (int)cx - 9, (int)cy - 7, 3, 14 }, theme->bg);
        w_rect((Rect){ (int)cx + 6, (int)cy - 7, 3, 14 }, theme->bg);
        break;
    case HICON_DPAD:
        w_rect((Rect){ (int)cx - 3, (int)cy - 10, 6, 20 }, c);
        w_rect((Rect){ (int)cx - 10, (int)cy - 3, 20, 6 }, c);
        break;
    case HICON_UP:
        thick_seg(cx, cy + 9, cx, cy + 1, c);        /* 箭杆 */
        thick_seg(cx, cy - 10, cx - 7, cy + 1, c);   /* 箭头 */
        thick_seg(cx, cy - 10, cx + 7, cy + 1, c);
        break;
    case HICON_DOWN:
        thick_seg(cx, cy - 9, cx, cy - 1, c);
        thick_seg(cx, cy + 10, cx - 7, cy - 1, c);
        thick_seg(cx, cy + 10, cx + 7, cy - 1, c);
        break;
    case HICON_LEFT:
        thick_seg(cx + 9, cy, cx + 1, cy, c);
        thick_seg(cx - 10, cy, cx + 1, cy - 7, c);
        thick_seg(cx - 10, cy, cx + 1, cy + 7, c);
        break;
    case HICON_RIGHT:
        thick_seg(cx - 9, cy, cx - 1, cy, c);
        thick_seg(cx + 10, cy, cx - 1, cy - 7, c);
        thick_seg(cx + 10, cy, cx - 1, cy + 7, c);
        break;
    default:
        break;
    }
}

/* 底部按键提示条：图标 + 文字多段并排 */
void w_page_footer_segs(const HintSeg *segs, int n)
{
    Rect f = { 0, SCR_H - FOOTER_H, SCR_W, FOOTER_H };
    w_rect(f, theme->bg);
    w_rect((Rect){ 0, f.y, SCR_W, 2 }, theme->border);
    int x = 28, i;
    int cy = f.y + 18;   /* 图标中心，与文字(顶 y+12)的水平中心对齐，略高于条带中线 */
    for (i = 0; i < n; i++) {
        int has_icon = segs[i].icon != HICON_NONE;
        int adv = 0;
        if (has_icon) {
            w_icon(segs[i].icon, x + 11, cy);
            adv = 26;
        }
        if (segs[i].text && segs[i].text[0]) {
            int tw = 0, th = 0;
            w_text_w(1.0f, segs[i].text, &tw, &th);
            w_text(x + adv, f.y + 12, 1.0f, theme->text_dim, "%s", segs[i].text);
            x += adv + tw;
        } else {
            x += adv ? adv : 0;
        }
        x += 24;   /* 段间距 */
    }
}

void w_row(Rect r, const char *main_text, const char *sub_text, bool selected)
{
    uint32_t card = selected ? theme->accent : theme->card;
    uint32_t main_c = selected ? theme->accent_text : theme->text;
    uint32_t sub_c  = selected ? theme->accent_text : theme->text_dim;
    w_rect(r, card);
    int mh = 0;
    w_text_w(1.25f, main_text, NULL, &mh);
    w_text_clip(r.x + 24, r.y + (r.h - mh) / 2, 1.25f, main_c, main_text, r.w - 200);
    if (sub_text && *sub_text) {
        int sw = 0, sh = 0;
        w_text_w(1.0f, sub_text, &sw, &sh);
        w_text(r.x + r.w - sw - 24, r.y + (r.h - sh) / 2, 1.0f, sub_c, "%s", sub_text);
    }
}

void w_button(Rect r, const char *label, bool active)
{
    if (active) {
        w_rect(r, theme->accent);
        int w = 0, h = 0;
        w_text_w(1.3f, label, &w, &h);
        w_text(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, 1.3f,
               theme->accent_text, "%s", label);
    } else {
        w_rect(r, theme->card);
        w_rect_outline(r, theme->border);
        int w = 0, h = 0;
        w_text_w(1.3f, label, &w, &h);
        w_text(r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, 1.3f,
               theme->text, "%s", label);
    }
}

Rect w_modal_box(int content_h)
{
    w_rect((Rect){ 0, 0, SCR_W, SCR_H }, theme->overlay);
    Rect card = { (SCR_W - 780) / 2, (SCR_H - content_h) / 2, 780, content_h };
    w_rect(card, theme->card);
    w_rect_outline(card, theme->border);
    return card;
}

void w_human_size(SceOff size, char *out)
{
    if (size < 1024)            snprintf(out, 16, "%lld B", (long long)size);
    else if (size < 1024 * 1024) snprintf(out, 16, "%.1f KB", (double)size / 1024);
    else if (size < 1024LL * 1024 * 1024)
        snprintf(out, 16, "%.1f MB", (double)size / (1024 * 1024));
    else
        snprintf(out, 16, "%.2f GB", (double)size / (1024.0 * 1024 * 1024));
}

void w_clear_picked(void)
{
    g_app.picked_count = 0;
    g_app.picked_total = 0;
}
