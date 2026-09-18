/* 控件绘制：文字 / 矩形 / 列表行 / 按钮 / 进度条 / 弹窗 / 头尾栏 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/theme.h"

/* ---------- 文字 ----------
 * 字符路由：码点 <= 0xFF（ASCII / Latin-1）与通用标点 U+2000-206F（省略号、
 * 弯引号、破折号…——CJK 字库缺而拉丁字库含，见 cp_domain 注释）走拉丁字体，
 * 其余（CJK、全角符号、假名…）走 CJK 字体；同一字体的连续子串一次绘制，
 * 减少调用并保留 kerning。
 * 字号：ui_main.c 的 font_get 为每个像素字号建独立字体对象，保证每个
 * 字都按本字号原生光栅化（详见 ui_main.c 注释），draw_scale 恒为 1。
 * 尺寸语义（libvita2d freetype 后端，见 vita2d_font.c）：
 *  - `vita2d_font_draw_text` 的 y 是"基线"，size 是像素字号（em 盒高）；
 *  - w_text 的 y 保持"行首升部线"语义：基线 = y + FONT_ASC*size。
 *    FONT_ASC 取 CJK 满格字（中/文/日）字形顶比例：Noto Sans CJK 实测
 *    0.875~0.90em（16/20/26/34px 档分别 0.875/0.900/0.885/0.882，取 0.88；
 *    非 1.16 的盒 ascender——盒顶在 em 上方留行距空白，用它基线会被压得
 *    过低）。拉丁大写顶 ≈ 0.72em，同基线下略低于中文顶 ~0.16em，视觉协调；
 *  - scale→px：scale=1.0 → 20px（CJK 字形全高 ~0.91em + 少量下伸，
 *    行距观感与列表 26px 吻合）。整体嫌大/小只调 FONT_PX。 */
#define FONT_PX   20.0f
#define FONT_ASC  0.88f

/* scale → 像素字号。对外声明（ui.h）供 ui_main.c 由 scale 列表推字号档，
 * 保证"scale 与 px 的对应关系"全项目只有这一处定义。 */
int w_font_px(float scale)
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

/* 字体域判定：ASCII / Latin-1 与通用标点（U+2000-206F，含省略号 U+2026、
 * 弯引号 U+201C/201D、破折号 U+2014 等）走拉丁字体；其余（CJK 汉字、全角
 * 符号、假名…）走 CJK 字体。这类通用标点只放在拉丁字库里（早期按"码点
 * >0xFF 一律 CJK"路由，把省略号等发给了不带这些字形的 CJK 字库 → 中文文案
 * 渲染成方框，故单列此段）。 */
static int cp_domain(uint32_t cp)
{
    if (cp <= 0xFF) return 0;
    if (cp >= 0x2000 && cp <= 0x206F) return 0;
    return 1;
}

/* 对 p 指向的字符串做逐字符字体路由：每段是连续同字体域子串，
 * cjk_out 返回该段字体域（1=CJK、0=拉丁；实际字体对象在绘制时按
 * 当前字号经 font_get 取得）。返回段长度（字节）。 */
static int next_run(const char *p, int *cjk_out)
{
    uint32_t cp;
    const char *q = utf8_next_cp(p, &cp);
    int cjk = cp_domain(cp);
    *cjk_out = cjk;
    while (*q) {
        uint32_t cp2;
        const char *r = utf8_next_cp(q, &cp2);
        if (cp_domain(cp2) != cjk) break;
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
    int size = w_font_px(scale);
    int base_y = (int)(y + FONT_ASC * size + 0.5f);
    int pen = (int)(x + 0.5f);
    const char *p = buf;
    while (*p) {
        int cjk;
        int n = next_run(p, &cjk);
        vita2d_font *f = font_get(size, cjk);
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
    int size = w_font_px(scale);
    char seg[512];
    int pen = 0;
    const char *p = text;
    while (*p) {
        int cjk;
        int n = next_run(p, &cjk);
        vita2d_font *f = font_get(size, cjk);
        if (f) {
            /* 本函数对入参长度无约束（如超长目录路径），一个 run 可能长于 seg：
             * 按字符边界切成 ≤(sizeof seg - 1) 字节的片分别测宽累加，避免越界写栈
             * （段与段之间无 kerning，切开的代价只是相邻字符间距不再微调）。 */
            int off = 0;
            while (off < n) {
                int cn = 0, sw = 0;
                while (off + cn < n) {
                    uint32_t c2;
                    const char *nx = utf8_next_cp(p + off + cn, &c2);
                    if (cn + (int)(nx - (p + off + cn)) > (int)sizeof seg - 1) break;
                    cn += (int)(nx - (p + off + cn));
                }
                memcpy(seg, p + off, cn);
                seg[cn] = 0;
                vita2d_font_text_dimensions(f, size, seg, &sw, NULL);
                pen += sw;
                off += cn;
            }
        }
        p += n;
    }
    if (w) *w = pen;
    if (h) *h = size;   /* 行高 ≈ 字号 px */
}

void w_text_clip(float x, float y, float scale, uint32_t color,
                 const char *text, int max_w)
{
    char buf[512] = {0};      /* 必须初始化：一个字都放不下时不能把未初始化内容画出去 */
    char one[8];
    int cur_w = 0, n = 0;
    const char *p = text;
    while (*p) {
        uint32_t cp;
        /* 逐字符步进统一走 utf8_next_cp：它校验后继字节与 \0，非法/截断序列
         * 只前进 1 字节；早先这里另有一套"按首字节宽度盲进 2~4 字节"的实现，
         * 定长 name 数组被填满、结尾是半个汉字时它会越过 '\0' 继续读。 */
        const char *next = utf8_next_cp(p, &cp);
        int cl = (int)(next - p);
        if (n + cl >= (int)sizeof buf) break;   /* 文本过长：截断 */
        /* 单趟累加逐字宽度（同 w_text_mid 的取宽口径），不再每纳一字就重测整段前缀
         * （旧写法对 n 个字要测 n(n+1)/2 次宽、2n 次前缀 memcpy）。 */
        int cw = 0;
        memcpy(one, p, cl);
        one[cl] = 0;
        w_text_w(scale, one, &cw, NULL);
        if (cur_w + cw > max_w) break;
        memcpy(buf + n, p, cl);
        n += cl;
        buf[n] = 0;
        cur_w += cw;
        p = next;
    }
    if (cur_w > 0)        /* 放不下任何一个字形 → 不画（原"n==0 也画"会输出垃圾） */
        w_text(x, y, scale, color, "%s", buf);
}

/* 中间省略：整串放不下 max_w 时画成"头…尾"（尾部保留，超长文件名的扩展名
 * 才看得见），返回实际绘制宽度；放得下则整串照画。头尾各占可用宽的一半。 */
int w_text_mid(float x, float y, float scale, uint32_t color,
               const char *text, int max_w)
{
    const char *off[200], *tail;
    int cw[200];
    char cbuf[8], out[576];
    int ncp = 0, i, j, tw = 0, ew = 0, avail, lw = 0, rw = 0, o;
    const char *p = text;
    if (!text || !*text) return 0;
    w_text_w(scale, text, &tw, NULL);
    if (tw <= max_w) {
        w_text(x, y, scale, color, "%s", text);
        return tw;
    }
    if (strlen(text) >= sizeof out - 4) {          /* 极端长：退回尾部裁剪 */
        w_text_clip(x, y, scale, color, text, max_w);
        return max_w;
    }
    while (*p && ncp < (int)(sizeof off / sizeof off[0])) {
        uint32_t cp;
        const char *nx = utf8_next_cp(p, &cp);
        int n = (int)(nx - p), w = 0;
        if (n > (int)sizeof cbuf - 1) n = (int)sizeof cbuf - 1;
        memcpy(cbuf, p, n);
        cbuf[n] = 0;
        w_text_w(scale, cbuf, &w, NULL);
        off[ncp] = p;
        cw[ncp] = w;
        ncp++;
        p = nx;
    }
    w_text_w(scale, "…", &ew, NULL);
    avail = max_w - ew;
    if (avail <= 0) return 0;
    for (i = 0; i < ncp && lw + cw[i] <= avail / 2; i++) lw += cw[i];
    for (j = ncp - 1; j >= i && rw + cw[j] <= avail - lw; j--) rw += cw[j];
    tail = (j + 1 < ncp) ? off[j + 1] : text + strlen(text);
    o = (int)(off[i] - text);
    memcpy(out, text, (size_t)o);
    memcpy(out + o, "…", 3);
    o += 3;
    memcpy(out + o, tail, strlen(tail));
    o += (int)strlen(tail);
    out[o] = 0;
    w_text(x, y, scale, color, "%s", out);
    return lw + ew + rw;
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

void w_page_header(const char *title)
{
    w_text(28, 10, 1.7f, theme->text, "%s", title);
    w_rect((Rect){ 0, HEADER_H - 2, SCR_W, 2 }, theme->border);
}

/* w_row 实现（sub_c=0 用默认色；指定则覆盖——"检查更新"行发现新版时用 accent 强调） */
static void w_row_impl(Rect r, const char *main_text, const char *sub_text,
                       uint32_t sub_c, bool selected)
{
    uint32_t card = selected ? theme->accent : theme->card;
    uint32_t main_c = selected ? theme->accent_text : theme->text;
    w_rect(r, card);
    int mh = 0;
    w_text_w(1.25f, main_text, NULL, &mh);
    w_text_clip(r.x + 24, r.y + (r.h - mh) / 2, 1.25f, main_c, main_text, r.w - 200);
    if (sub_text && *sub_text) {
        uint32_t sc = sub_c ? sub_c
                            : (selected ? theme->accent_text : theme->text_dim);
        int sw = 0, sh = 0;
        w_text_w(1.0f, sub_text, &sw, &sh);
        w_text(r.x + r.w - sw - 24, r.y + (r.h - sh) / 2, 1.0f, sc, "%s", sub_text);
    }
}

void w_row(Rect r, const char *main_text, const char *sub_text, bool selected)
{
    w_row_impl(r, main_text, sub_text, 0, selected);
}

void w_row_c(Rect r, const char *main_text, const char *sub_text,
             uint32_t sub_c, bool selected)
{
    w_row_impl(r, main_text, sub_text, sub_c, selected);
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
