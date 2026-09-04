/* 控件绘制：文字 / 矩形 / 列表行 / 按钮 / 进度条 / 弹窗 / 头尾栏 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vita2d.h>
#include "ui.h"
#include "theme.h"

extern vita2d_font *g_font_lat;   /* Droid Sans（拉丁）        */
extern vita2d_font *g_font_cjk;   /* Droid Sans Fallback Full（CJK） */

/* ---------- 文字 ----------
 * 字符路由：码点 <= 0xFF（ASCII / Latin-1）走拉丁字体，其余（CJK、
 * 全角符号、假名…）走 CJK 字体；同一字体的连续子串一次绘制，减少
 * 调用并保留 kerning。
 * 尺寸语义（libvita2d freetype 后端，见 vita2d_font.c）：
 *  - `vita2d_font_draw_text` 的 y 是"基线"，size 是像素字号（em 盒高）；
 *  - w_text 的 y 保持"行首升部线"语义：基线 = y + FONT_ASC*size。
 *    FONT_ASC 取 CJK 满格字（中/文/日）字形顶比例 ≈ 0.81em（实测
 *    glyf yMax/upem，非 1.043 的盒 ascender——盒顶在 em 上方留行距
 *    空白，用它基线会被压得过低）。拉丁大写顶 ≈ 0.717em，同基线
 *    下略低于中文顶 ~0.09em，视觉协调；
 *  - scale→px：scale=1.0 → 20px（CJK 字形全高 ~0.91em + 少量下伸，
 *    行距观感与列表 26px 吻合）。整体嫌大/小只调 FONT_PX。 */
#define FONT_PX   20.0f
#define FONT_ASC  0.81f

static int font_px(float scale)
{
    return (int)(scale * FONT_PX + 0.5f);
}

/* 从 UTF-8 串读一个码点，返回下一字符起点；非法/截断字节兜底按单字节。
 * p[i] 必须检查非 0，避免越界读 buf 结尾。 */
static const char *utf8_next_cp(const char *p, uint32_t *cp)
{
    unsigned char c = (unsigned char)p[0];
    if (c < 0x80) { *cp = c; return p + 1; }
    if ((c & 0xE0) == 0xC0 && p[1] &&
        (p[1] & 0xC0) == 0x80) {
        *cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
        return p + 2;
    }
    if ((c & 0xF0) == 0xE0 && p[1] && p[2] &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        *cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return p + 3;
    }
    if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3] &&
        (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
        (p[3] & 0xC0) == 0x80) {
        *cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) |
              ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        return p + 4;
    }
    *cp = c;
    return p + 1;
}

/* 对 p 指向的字符串做逐字符字体路由：每段是连续同字体子串，
 * f_out 返回该段字体。返回段长度（字节）。 */
static int next_run(const char *p, vita2d_font **f_out)
{
    uint32_t cp;
    const char *q = utf8_next_cp(p, &cp);
    vita2d_font *f = (cp <= 0xFF) ? g_font_lat : g_font_cjk;
    *f_out = f;
    while (*q) {
        uint32_t cp2;
        const char *r = utf8_next_cp(q, &cp2);
        if (((cp2 <= 0xFF) ? g_font_lat : g_font_cjk) != f) break;
        q = r;
    }
    return (int)(q - p);
}

void w_text(float x, float y, float scale, uint32_t color, const char *fmt, ...)
{
    char buf[512], seg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (!*buf) return;
    if (!g_font_lat && !g_font_cjk) return;
    int size = font_px(scale);
    int base_y = (int)(y + FONT_ASC * size + 0.5f);
    int pen = (int)(x + 0.5f);
    const char *p = buf;
    while (*p) {
        vita2d_font *f;
        int n = next_run(p, &f);
        if (f) {
            memcpy(seg, p, n);
            seg[n] = 0;
            pen += vita2d_font_draw_text(f, pen, base_y, color, size, seg);
        }
        p += n;
    }
}

void w_text_w(float scale, const char *text, int *w, int *h)
{
    if (w) *w = 0;
    if (h) *h = 0;
    if (!text || !*text) return;
    if (!g_font_lat && !g_font_cjk) return;
    int size = font_px(scale);
    char seg[512];
    int pen = 0;
    const char *p = text;
    while (*p) {
        vita2d_font *f;
        int n = next_run(p, &f);
        if (f) {
            memcpy(seg, p, n);
            seg[n] = 0;
            int sw = 0;
            vita2d_font_text_dimensions(f, size, seg, &sw, NULL);
            pen += sw;
        }
        p += n;
    }
    if (w) *w = pen;
    if (h) *h = size;   /* 行高 ≈ 字号 px */
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
