/* 底部按键提示条：键位图标绘制 + 段排序 + 排版。
 *
 * 页面按任意顺序追加 HintSeg，本模块负责：
 *   1) 按默认键序（SELECT、START、方向键、○、✗、□、△）稳定排序；
 *   2) 把"动作随焦点/状态变"的段（varies）整体排到固定段右侧；
 *   3) 图标按 HintKey 推出（页面不写图标），再逐段绘制。
 * 排序规则只在这里实现一处——加新按键只需补 HintKey 与下面的图标/键序表，
 * 各页面无需改动。 */
#include <stddef.h>
#include <math.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/hintbar.h"
#include "ui/theme.h"

#define FOOTER_H     46
#define HINT_SEG_MAX 8   /* 排序缓冲区上限：一页最多几段提示 */

/* 提示条里用到的按键图形。页面不直接引用，只经 HintKey 推出（见 icon_of）。 */
typedef enum {
    HICON_NONE = 0,
    HICON_CROSS,     /* X 键 */
    HICON_CIRCLE,    /* O 键 */
    HICON_TRIANGLE,  /* 三角键 */
    HICON_SQUARE,    /* 方块键 */
    HICON_START,     /* START */
    HICON_DPAD       /* 方向键（十字）：按 dir_off 分亮臂/灰臂 */
} HintIcon;

/* ---------- 图标绘制 ---------- */
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

/* "✕"图标：以 (cx,cy) 为中心、半径 r 的两条斜线（列表行尾删除按钮用） */
void w_icon_cross(float cx, float cy, float r, uint32_t c)
{
    thick_seg(cx - r, cy - r, cx + r, cy + r, c);
    thick_seg(cx - r, cy + r, cx + r, cy - r, c);
}

/* "✓"图标：以 (cx,cy) 为中心的两段折线（设置页选项面板标记当前档用） */
void w_icon_check(float cx, float cy, float r, uint32_t c)
{
    thick_seg(cx - r * 0.9f, cy + r * 0.05f, cx - r * 0.2f, cy + r * 0.7f, c);
    thick_seg(cx - r * 0.2f, cy + r * 0.7f, cx + r * 0.95f, cy - r * 0.75f, c);
}

static void w_icon(HintIcon ic, float cx, float cy, uint32_t c, uint8_t dir_off)
{
    switch (ic) {
    case HICON_CROSS:
        w_icon_cross(cx, cy, 8.0f, c);
        break;
    case HICON_CIRCLE:
        /* 环宽 2.5px：与 thick_seg 画的叉/三角线宽（≈2.6px）一致 */
        vita2d_draw_fill_circle(cx, cy, 9.0f, c);
        vita2d_draw_fill_circle(cx, cy, 6.5f, theme->bg);
        break;
    case HICON_TRIANGLE:
        /* 几何中心对准 (cx,cy)：原 -10..+7 上下不对称，视觉偏上 1.5px */
        thick_seg(cx, cy - 9, cx - 9, cy + 8, c);
        thick_seg(cx, cy - 9, cx + 9, cy + 8, c);
        thick_seg(cx - 9, cy + 8, cx + 9, cy + 8, c);
        break;
    case HICON_SQUARE:
        /* 边框 3px：与 thick_seg 线宽接近（原 4px 明显偏粗） */
        w_rect((Rect){ (int)cx - 9, (int)cy - 9, 18, 18 }, c);
        w_rect((Rect){ (int)cx - 6, (int)cy - 6, 12, 12 }, theme->bg);
        break;
    case HICON_START:
        /* 横向胶囊：填充 + 两条竖向挖空 */
        w_rect((Rect){ (int)cx - 12, (int)cy - 7, 24, 14 }, c);
        w_rect((Rect){ (int)cx - 9, (int)cy - 7, 3, 14 }, theme->bg);
        w_rect((Rect){ (int)cx + 6, (int)cy - 7, 3, 14 }, theme->bg);
        break;
    case HICON_DPAD: {
        /* 旋转 45° 的空心菱形（描边），在四条边中点附近切开成四份、各占一个角，
         * 中心留空。切缝从中点向两侧各让出 s，四份之间才有明显空隙。
         * 每份按 dir_off 分色——可用份主色，灭份 border 灰，
         * 灭份用来表示"这个方向当前按不动"（列表滚到顶/到底等）。 */
        uint32_t offc = theme->border;
        const float R = 11.0f;   /* 顶点到中心 */
        const float q = 5.5f;    /* 边中点相对中心的坐标（= R/2），切缝基准 */
        const float s = 2.0f;    /* 切缝：每份沿边从中点让出的距离 */
        uint32_t uc = (dir_off & HDIR_UP)    ? offc : c;
        uint32_t dc = (dir_off & HDIR_DOWN)  ? offc : c;
        uint32_t lc = (dir_off & HDIR_LEFT)  ? offc : c;
        uint32_t rc = (dir_off & HDIR_RIGHT) ? offc : c;
        thick_seg(cx, cy - R, cx - (q - s), cy - (q + s), uc);   /* 上角 */
        thick_seg(cx, cy - R, cx + (q - s), cy - (q + s), uc);
        thick_seg(cx + R, cy, cx + (q + s), cy - (q - s), rc);   /* 右角 */
        thick_seg(cx + R, cy, cx + (q + s), cy + (q - s), rc);
        thick_seg(cx, cy + R, cx - (q - s), cy + (q + s), dc);   /* 下角 */
        thick_seg(cx, cy + R, cx + (q - s), cy + (q + s), dc);
        thick_seg(cx - R, cy, cx - (q + s), cy - (q - s), lc);   /* 左角 */
        thick_seg(cx - R, cy, cx - (q + s), cy + (q - s), lc);
        break;
    }
    default:
        break;
    }
}

/* ---------- 键 → 图标 / 键序 ---------- */

/* 键位图形：确认/返回键按当前布局在 ○/✗ 之间取 */
static HintIcon icon_of(HintKey k)
{
    switch (k) {
    case HKEY_START:    return HICON_START;
    case HKEY_DPAD:     return HICON_DPAD;
    case HKEY_CONFIRM:  return g_app.confirm_layout == 0 ? HICON_CROSS : HICON_CIRCLE;
    case HKEY_BACK:     return g_app.confirm_layout == 0 ? HICON_CIRCLE : HICON_CROSS;
    case HKEY_SQUARE:   return HICON_SQUARE;
    case HKEY_TRIANGLE: return HICON_TRIANGLE;
    default:            return HICON_NONE;   /* HKEY_SELECT 无符号图标，靠文案说明 */
    }
}

/* 默认键序：SELECT、START、方向键、○、✗、□、△。
 * ○/✗ 两项按**图标符号**定位（○ 在 ✗ 前），因此键位布局切换时图标位置不跳；
 * 反过来说，美式布局下"返回(○)"会排在"确认(✗)"之前——这是该口径的直接结果。 */
static int rank_of(HintKey k)
{
    switch (k) {
    case HKEY_SELECT:   return 0;
    case HKEY_START:    return 1;
    case HKEY_DPAD:     return 2;
    case HKEY_CONFIRM:
    case HKEY_BACK:     return icon_of(k) == HICON_CIRCLE ? 3 : 4;
    case HKEY_SQUARE:   return 5;
    case HKEY_TRIANGLE: return 6;
    default:            return 7;            /* HKEY_NONE：无键段，排最后 */
    }
}

/* 一段的排序位：合并段（key + key2）取两键中靠前的那个，
 * 位置口径与段内图标一致，不随键位布局左右跳 */
static int seg_rank(const HintSeg *s)
{
    int r = rank_of(s->key);
    if (s->key2 != HKEY_NONE) {
        int r2 = rank_of(s->key2);
        if (r2 < r) r = r2;
    }
    return r;
}

/* 排序比较：固定段在前，其次按默认键序；两者都相同则保持页面给的先后（稳定） */
static bool seg_before(const HintSeg *a, const HintSeg *b)
{
    if (a->varies != b->varies) return !a->varies;
    return seg_rank(a) < seg_rank(b);
}

/* ---------- 提示条 ---------- */

void w_page_footer(const char *hint)
{
    Rect f = { 0, SCR_H - FOOTER_H, SCR_W, FOOTER_H };
    w_rect(f, theme->bg);
    w_rect((Rect){ 0, f.y, SCR_W, 2 }, theme->border);
    w_text(28, f.y + 12, 1.0f, theme->text_dim, "%s", hint);
}

/* 提示条左右留白（左端起点 / 右端边界）、段间距（充裕时 / 压缩后） */
#define HINT_MARGIN   28
#define HINT_GAP      24
#define HINT_GAP_MIN  12

/* 段的图标部分宽度：无图标 0、单键 26、合并段（双键）57。
 * 量宽与绘制两处都要用，抽成一处保证口径一致。 */
static int seg_icon_w(const HintSeg *s)
{
    if (icon_of(s->key) == HICON_NONE) return 0;
    return icon_of(s->key2) != HICON_NONE ? 57 : 26;
}

/* 底部按键提示条：页面给的段先排成默认键序再绘制。
 * 段的 key2 非 HKEY_NONE 时表示"两个键同一个动作"，画成「key / key2 文案」：
 * 两个键都显示出来（告知两条路子都能走），动作文案只写一次，避免同一句话写两遍；
 * 段内两个键之间也按同一默认键序排（○ 恒在 ✗ 前）。
 * 排版有右边界：段的起点出屏就不再画；文字超宽则裁尾（w_text_clip）——此前
 * 只管把 x 往右推，段数多/文案长（长语言）时尾段会被推出屏幕、整段看不见。 */
void w_page_footer_segs(const HintSeg *segs, int n)
{
    HintSeg s[HINT_SEG_MAX];
    int i, j, m = 0, total = 0, gap = HINT_GAP;
    if (n > HINT_SEG_MAX) n = HINT_SEG_MAX;
    for (i = 0; i < n; i++) {                /* 插入排序：段数很少，够用且稳定 */
        j = m++;
        while (j > 0 && seg_before(&segs[i], &s[j - 1])) { s[j] = s[j - 1]; j--; }
        s[j] = segs[i];
    }

    Rect f = { 0, SCR_H - FOOTER_H, SCR_W, FOOTER_H };
    w_rect(f, theme->bg);
    w_rect((Rect){ 0, f.y, SCR_W, 2 }, theme->border);
    /* 先量总宽（段宽 = 图标宽 + 文字宽）：装不下就先把段间距压到最小，仍装不下由
     * 下面的绘制循环裁尾兜底。 */
    for (i = 0; i < m; i++) {
        int tw = 0;
        if (s[i].text && s[i].text[0]) w_text_w(1.0f, s[i].text, &tw, NULL);
        total += seg_icon_w(&s[i]) + tw;
    }
    if (HINT_MARGIN * 2 + total + HINT_GAP * (m > 0 ? m - 1 : 0) > SCR_W)
        gap = HINT_GAP_MIN;

    int x = HINT_MARGIN;
    /* 图标中心对准文字的字形视觉中线：1.0 号字 20px、升部 0.81em，文字顶在
     * y+12（= f.y+12）→ 基线 f.y+28.2，满格字形视觉中心 ≈ f.y+20.1
     * （拉丁大写略低约 1px，取 20 折中）。 */
    int cy = f.y + 20;
    for (i = 0; i < m; i++) {
        HintIcon ic = icon_of(s[i].key);
        int adv = seg_icon_w(&s[i]);
        int room = SCR_W - HINT_MARGIN - x;   /* 本段可用宽（图标 + 文字） */
        if (room < adv) break;                /* 连图标都放不下：本段及之后不画 */
        /* dim 段：图标/文字都用 border 灰，表示该动作当前不可用 */
        uint32_t ic_c = s[i].dim ? theme->border : theme->text;
        uint32_t tx_c = s[i].dim ? theme->border : theme->text_dim;
        if (ic != HICON_NONE) {
            HintIcon ic2 = icon_of(s[i].key2);
            /* 合并段内部也按默认键序（○ 在 ✗ 前），与段间排序同口径：
             * 美式布局下确认键是 ✗，这里要把它换到 ○ 后面 */
            if (ic2 != HICON_NONE && rank_of(s[i].key2) < rank_of(s[i].key)) {
                HintIcon t = ic; ic = ic2; ic2 = t;
            }
            w_icon(ic, x + 11, cy, ic_c, s[i].dir_off);
            if (ic2 != HICON_NONE) {
                /* 第二个键：与第一个隔开，中间一个小号暗淡的 "/" 表示"或" */
                int sw = 0;
                w_text_w(0.9f, "/", &sw, NULL);
                w_icon(ic2, x + 42, cy, ic_c, s[i].dir_off);
                w_text(x + 26 - sw / 2, f.y + 12, 0.9f, tx_c, "/");
            }
        }
        if (s[i].text && s[i].text[0]) {
            int tw = 0, th = 0, avail = room - adv;
            w_text_w(1.0f, s[i].text, &tw, &th);
            if (tw > avail) {                 /* 超出右边界：裁尾，宁可少几个字也不出屏 */
                if (avail > 0)
                    w_text_clip(x + adv, f.y + 12, 1.0f, tx_c, s[i].text, avail);
                x += room;
            } else {
                w_text(x + adv, f.y + 12, 1.0f, tx_c, "%s", s[i].text);
                x += adv + tw;
            }
        } else {
            x += adv;
        }
        x += gap;
    }
}
