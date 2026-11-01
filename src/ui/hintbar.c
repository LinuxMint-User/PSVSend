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
#include <psp2/kernel/processmgr.h>
#include "ui/ui.h"
#include "ui/hintbar.h"
#include "ui/theme.h"
#include "core/i18n.h"

#define HINT_SEG_MAX 8   /* 排序缓冲区上限：一页最多几段提示 */

/* 提示条里用到的按键图形。页面不直接引用，只经 HintKey 推出（见 icon_of）。 */
typedef enum {
    HICON_NONE = 0,
    HICON_CROSS,     /* X 键 */
    HICON_CIRCLE,    /* O 键 */
    HICON_TRIANGLE,  /* 三角键 */
    HICON_SQUARE,    /* 方块键 */
    HICON_L,         /* L 肩键（带字母的肩键形） */
    HICON_R,         /* R 肩键 */
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

/* "🔍"图标：镜片环 + 右下斜柄 */
void w_icon_search(float cx, float cy, float r, uint32_t c)
{
    float k = r * 0.62f;              /* 柄起点落在环上（45° 处略往内，接缝不留缝） */
    vita2d_draw_fill_circle(cx, cy, r, c);
    vita2d_draw_fill_circle(cx, cy, r - 2.5f, theme->bg);
    thick_seg(cx + k, cy + k, cx + r + 3.5f, cy + r + 3.5f, c);
}

/* "←"图标：箭尖 + 一条横杆（<-- ）。尖头两臂 45° 张开，杆横穿到箭尾。 */
void w_icon_back(float cx, float cy, float r, uint32_t c)
{
    float tip = cx - r;
    float k   = r * 0.62f;            /* 尖头两臂的横/竖分量 */
    thick_seg(tip, cy, tip + k, cy - k, c);
    thick_seg(tip, cy, tip + k, cy + k, c);
    thick_seg(tip, cy, cx + r, cy, c);
}

/* "⚙"图标：外环 + 8 根齿（每 45° 一根，从环外探出 ~2.5px）。
 * 齿先画、环后画：环的挖空圆会把探进内圈的那截齿盖掉，接缝干净。 */
void w_icon_gear(float cx, float cy, float r, uint32_t c)
{
    int i;
    for (i = 0; i < 8; i++) {
        float a = 0.7853982f * (float)i;
        float ux = cosf(a), uy = sinf(a);
        thick_seg(cx + ux * (r - 1.0f), cy + uy * (r - 1.0f),
                  cx + ux * (r + 2.5f), cy + uy * (r + 2.5f), c);
    }
    vita2d_draw_fill_circle(cx, cy, r, c);
    vita2d_draw_fill_circle(cx, cy, r - 2.5f, theme->bg);
}

/* 地球：外圈 + 赤道 + 两条经线（窄椭圆的两半）。线宽与齿轮/放大镜同一口径。 */
void w_icon_globe(float cx, float cy, float r, uint32_t c)
{
    float rx = r * 0.45f;
    int k, i;
    vita2d_draw_fill_circle(cx, cy, r, c);
    vita2d_draw_fill_circle(cx, cy, r - 2.2f, theme->bg);
    thick_seg(cx - r + 1.2f, cy, cx + r - 1.2f, cy, c);      /* 赤道 */
    for (k = -1; k <= 1; k += 2) {                            /* 左右经线 */
        float px = cx, py = cy - r + 1.2f;
        for (i = 1; i <= 8; i++) {
            float t = 3.14159265f * (float)i / 8.0f;
            float x = cx + (float)k * rx * sinf(t);
            float y = cy - (r - 1.2f) * cosf(t);
            thick_seg(px, py, x, y, c);
            px = x;
            py = y;
        }
    }
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
    case HICON_L:
    case HICON_R: {
        /* 肩键：24x16 的空心梯形/圆角块 + 字母。内部用主题背景色挖空，
         * 与 □ 图标同一手法，只能画在纯色底（页脚）上。 */
        const char *ch = ic == HICON_L ? "L" : "R";
        int tw = 0, th = 0;
        w_rect((Rect){ (int)cx - 12, (int)cy - 8, 24, 16 }, c);
        w_rect((Rect){ (int)cx - 10, (int)cy - 6, 20, 12 }, theme->bg);
        w_text_w(0.7f, ch, &tw, &th);
        w_text(cx - tw / 2.0f, cy - th / 2.0f, 0.7f, c, "%s", ch);
        break;
    }
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

/* ---------- 蓄力环（页脚） ---------- */

/* 待画的蓄力环：w_page_footer_charge 设置，w_page_footer_segs 绘制时消费一次即清 */
static HintKey g_charge_key = HKEY_NONE;
static float   g_charge_pct;

/* 蓄力环：以 (cx,cy) 为心、半径 r，从 12 点顺时针填充 pct 比例；未满部分画
 * base（底环灰），用户能看出"还差多少"。24 段短线段拼弧（vita2d 没有画弧 API；
 * 禁用 draw_array，见文件头）。 */
static void w_hold_ring(float cx, float cy, float r, float pct,
                        uint32_t c, uint32_t base)
{
    const float step = 6.2831853f / 24.0f;
    int n = (int)(pct * 24.0f + 0.5f), i;
    if (n > 24) n = 24;
    for (i = 0; i < 24; i++) {
        float a0 = -1.5707963f + step * i;   /* -90° = 12 点方向；角度增即顺时针 */
        float a1 = a0 + step;
        thick_seg(cx + r * cosf(a0), cy + r * sinf(a0),
                  cx + r * cosf(a1), cy + r * sinf(a1),
                  i < n ? c : base);
    }
}

void w_page_footer_charge(HintKey key, float pct)
{
    g_charge_key = key;
    g_charge_pct = pct;
}

/* ---------- 键 → 图标 / 键序 ---------- */

/* 键位图形：确认/返回键按当前布局在 ○/✗ 之间取 */
static HintIcon icon_of(HintKey k)
{
    switch (k) {
    case HKEY_START:                       /* 无符号图标，靠文案说明（同 SELECT） */
    case HKEY_SELECT:   return HICON_NONE;
    case HKEY_L:        return HICON_L;
    case HKEY_R:        return HICON_R;
    case HKEY_DPAD:     return HICON_DPAD;
    case HKEY_CONFIRM:  return g_app.confirm_layout == 0 ? HICON_CROSS : HICON_CIRCLE;
    case HKEY_BACK:     return g_app.confirm_layout == 0 ? HICON_CIRCLE : HICON_CROSS;
    case HKEY_SQUARE:   return HICON_SQUARE;
    case HKEY_TRIANGLE: return HICON_TRIANGLE;
    default:            return HICON_NONE;   /* HKEY_SELECT 无符号图标，靠文案说明 */
    }
}

/* 默认键序：SELECT、START、L、R、方向键、○、✗、□、△。
 * ○/✗ 两项按**图标符号**定位（○ 在 ✗ 前），因此键位布局切换时图标位置不跳；
 * 反过来说，美式布局下"返回(○)"会排在"确认(✗)"之前——这是该口径的直接结果。 */
static int rank_of(HintKey k)
{
    switch (k) {
    case HKEY_SELECT:   return 0;
    case HKEY_START:    return 1;
    case HKEY_L:        return 2;
    case HKEY_R:        return 3;
    case HKEY_DPAD:     return 4;
    case HKEY_CONFIRM:
    case HKEY_BACK:     return icon_of(k) == HICON_CIRCLE ? 5 : 6;
    case HKEY_SQUARE:   return 7;
    case HKEY_TRIANGLE: return 8;
    default:            return 9;            /* HKEY_NONE：无键段，排最后 */
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

/* 提示条左右留白（左端起点 / 右端边界）、段间距 */
#define HINT_MARGIN   28
#define HINT_GAP      24

/* 按住 R 展开：页脚 46 → 92px，第二行从屏幕下沿滑入。动画与设置页选项面板
 * 同一口径（按真实时间线性推进，150ms 走完，每帧至少 1px 保证收敛）。
 * 展开只影响页脚自身的绘制区域（覆盖列表底部），不改列表布局常量。 */
#define FOOTER_EXPAND_MS 150
static float    expand_p;      /* 0..1 展开进度 */
static uint64_t expand_t;      /* 上次步进时刻（微秒；0 = 尚未起步） */

static void expand_step(float target)
{
    uint64_t now = (uint64_t)sceKernelGetSystemTimeWide();
    float step;
    if (!expand_t) expand_t = now;
    step = (float)(now - expand_t) / ((float)FOOTER_EXPAND_MS * 1000.0f);
    if (step < 1.0f / 60.0f) step = 1.0f / 60.0f;
    expand_t = now;
    if (expand_p < target) {
        expand_p += step;
        if (expand_p > target) expand_p = target;
    } else if (expand_p > target) {
        expand_p -= step;
        if (expand_p < target) expand_p = target;
    }
}

/* 一行能**完整**放下的段数：从前往后累加，放不下的整段丢弃（绝不裁字）。 */
static int fit_count(const HintSeg *s, int n, int gap);

/* 段的图标部分宽度：无图标 0、单键 26、合并段（双键）57。
 * 量宽与绘制两处都要用，抽成一处保证口径一致。 */
static int seg_icon_w(const HintSeg *s)
{
    if (icon_of(s->key) == HICON_NONE) return 0;
    return icon_of(s->key2) != HICON_NONE ? 57 : 26;
}

/* 段宽 = 图标宽 + 文字宽（文字按 sc 缩放）。
 * 量宽与绘制两处都要用，抽成一处保证口径一致。 */
static int seg_w(const HintSeg *s, float sc)
{
    int tw = 0;
    if (s->text && s->text[0]) w_text_w(sc, s->text, &tw, NULL);
    return seg_icon_w(s) + tw;
}

/* 一行能**完整**放下的段数：从前往后累加，放不下的整段丢弃（绝不裁字）。 */
static int fit_count(const HintSeg *s, int n, int gap)
{
    int i, x = HINT_MARGIN;
    for (i = 0; i < n; i++) {
        int need = seg_w(&s[i], 1.0f);
        if (x + need > SCR_W - HINT_MARGIN) break;
        x += need + gap;
    }
    return i;
}

/* 画一行提示：y_text = 文字顶（图标中心自动取 y_text + 8，与旧版 f.y+12 / f.y+20
 * 的对应关系一致），sc = 文字缩放。超出右边界时本行剩余段不再画 / 文字裁尾。 */
static void draw_row(const HintSeg *s, int n, int y_text, float sc, int gap)
{
    int i;
    int x = HINT_MARGIN;
    int cy = y_text + 8;
    for (i = 0; i < n; i++) {
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
            /* 蓄力环：只给单键段、且非 dim（列表已空时该段已灰，画环没意义） */
            if (g_charge_pct > 0.0f && s[i].key == g_charge_key && !s[i].dim)
                w_hold_ring(x + 11, cy, 14.0f, g_charge_pct,
                            theme->accent, theme->border);
            if (ic2 != HICON_NONE) {
                /* 第二个键：与第一个隔开，中间一个小号暗淡的 "/" 表示"或" */
                int sw = 0;
                float ss = sc * 0.9f;   /* "/" 比正文再小一档 */
                w_text_w(ss, "/", &sw, NULL);
                w_icon(ic2, x + 42, cy, ic_c, s[i].dir_off);
                w_text(x + 26 - sw / 2, y_text, ss, tx_c, "/");
            }
        }
        if (s[i].text && s[i].text[0]) {
            int tw = 0, th = 0, avail = room - adv;
            w_text_w(sc, s[i].text, &tw, &th);
            if (tw > avail) {                 /* 超出右边界：裁尾，宁可少几个字也不出屏 */
                if (avail > 0)
                    w_text_clip(x + adv, y_text, sc, tx_c, s[i].text, avail);
                x += room;
            } else {
                w_text(x + adv, y_text, sc, tx_c, "%s", s[i].text);
                x += adv + tw;
            }
        } else {
            x += adv;
        }
        x += gap;
    }
}

/* 底部按键提示条：页面给的段先排成默认键序再绘制。
 * 段的 key2 非 HKEY_NONE 时表示"两个键同一个动作"，画成「key / key2 文案」：
 * 两个键都显示出来（告知两条路子都能走），动作文案只写一次，避免同一句话写两遍；
 * 段内两个键之间也按同一默认键序排（○ 恒在 ✗ 前）。
 *
 * 排版（默认单行）：
 *   按默认键序从前往后填，**整段**放不下就丢弃该段及之后的所有段（绝不裁字）。
 *   如果存在被丢弃的段，自动插入一条 R 段（按默认键序排，不固定在右端），
 *   文案「按住显示更多提示」——按 R 可临时展开。
 *
 * 展开（按住 R）：
 *   页脚 46 → 92px，所有段按原顺序分成两行；R 段文案变为「松开显示更少提示」。
 *   动画口径与设置页选项面板相同（150ms、按真实时间步进）。页脚向上生长覆盖
 *   列表底部，列表布局常量不变；松开立即收回。 */
void w_page_footer_segs(const HintSeg *segs, int n)
{
    HintSeg s[HINT_SEG_MAX];
    int i, j, m = 0, total = 0;
    const int avail = SCR_W - HINT_MARGIN * 2;
    bool held, overflow;
    int h;
    Rect f;

    if (n > HINT_SEG_MAX) n = HINT_SEG_MAX;
    for (i = 0; i < n; i++)
        total += seg_w(&segs[i], 1.0f);      /* 总宽与顺序无关，先判是否溢出 */
    overflow = n > 0 && total + HINT_GAP * (n - 1) > avail;
    held = ui_input_r_down();
    if (overflow && n >= HINT_SEG_MAX) n--;  /* 给自动插入的 R 段留一格 */

    for (i = 0; i < n; i++) {                /* 插入排序：段数很少，够用且稳定 */
        j = m++;
        while (j > 0 && seg_before(&segs[i], &s[j - 1])) { s[j] = s[j - 1]; j--; }
        s[j] = segs[i];
    }
    if (overflow) {
        s[m].key    = HKEY_R;
        s[m].key2   = HKEY_NONE;
        s[m].text   = tr(held ? "Less hints" : "More hints");
        s[m].dim    = false;
        s[m].varies = false;
        s[m].dir_off = 0;
        j = m++;
        while (j > 0 && seg_before(&s[j], &s[j - 1])) {
            HintSeg t = s[j]; s[j] = s[j - 1]; s[j - 1] = t;
            j--;
        }
    }

    expand_step(overflow && held ? 1.0f : 0.0f);
    h = FOOTER_H + (int)((float)FOOTER_H * expand_p + 0.5f);
    f = (Rect){ 0, SCR_H - h, SCR_W, h };
    w_rect(f, theme->bg);
    w_rect((Rect){ 0, f.y, SCR_W, 2 }, theme->border);

    if (expand_p <= 0.0f) {
        draw_row(s, fit_count(s, m, HINT_GAP), f.y + 12, 1.0f, HINT_GAP);
    } else {
        /* 两行：第一行填满，余下进第二行（第二行 y 固定，展开时从屏幕下沿滑入） */
        int n1 = fit_count(s, m, HINT_GAP);
        draw_row(s, n1, f.y + 12, 1.0f, HINT_GAP);
        draw_row(s + n1, fit_count(s + n1, m - n1, HINT_GAP),
                 f.y + 50, 1.0f, HINT_GAP);
    }
    /* 一次性设置：本帧消费完即清（页面要画就每帧设一次） */
    g_charge_key = HKEY_NONE;
    g_charge_pct = 0.0f;
}
