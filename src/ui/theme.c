#include "ui/theme.h"
#include <math.h>
#include <vita2d.h>

/* 色系 × 明暗 二维表：行 = 色系，列 = [深色, 浅色]。
 * 注意 border 兼作"禁用 / 未聚焦"色（页脚灭臂、未聚焦卡片底），
 * 深色系里它比 text_dim 更暗，浅色系里必须比 text_dim 更浅，
 * 否则"禁用"会读成"更黑更抢眼"，语义反转。
 * [THEME_CUSTOM] 一行不在此表：其色值运行时由主色推导（见 build_custom）。 */
static const Theme themes[THEME_COUNT][2] = {
    /* Yaru：Ubuntu 橙 */
    [THEME_YARU][THEME_DARK] = {
        .bg = RGBA8(32, 31, 28, 255),
        .card = RGBA8(46, 44, 40, 255),
        .accent = RGBA8(233, 84, 32, 255),
        .accent_text = RGBA8(26, 26, 26, 255),
        .text = RGBA8(255, 255, 255, 255),
        .text_dim = RGBA8(184, 181, 174, 255),
        .border = RGBA8(62, 59, 53, 255),
        .success = RGBA8(63, 185, 80, 255),
        .danger = RGBA8(229, 72, 77, 255),
        .warn = RGBA8(245, 166, 35, 255),
        .overlay = RGBA8(0, 0, 0, 170),
    },
    /* Yaru 浅色：白卡片 + 压深的橙主色；success/danger/warn 一并压深，
     * 否则亮绿/亮红/黄在白底上对比不足（黄几乎看不见） */
    [THEME_YARU][THEME_LIGHT] = {
        .bg = RGBA8(242, 241, 239, 255),
        .card = RGBA8(255, 255, 255, 255),
        .accent = RGBA8(198, 62, 20, 255),
        .accent_text = RGBA8(255, 255, 255, 255),
        .text = RGBA8(28, 27, 25, 255),
        .text_dim = RGBA8(108, 105, 99, 255),
        .border = RGBA8(213, 210, 204, 255),
        .success = RGBA8(21, 122, 47, 255),
        .danger = RGBA8(190, 26, 31, 255),
        .warn = RGBA8(158, 98, 0, 255),
        .overlay = RGBA8(0, 0, 0, 110),
    },
    /* OLED 深色：纯黑高对比（PSV1000 最佳）。
     * 无浅色变体——theme_set 对 OLED 恒定走这一格。 */
    [THEME_OLED][THEME_DARK] = {
        .bg = RGBA8(0, 0, 0, 255),
        .card = RGBA8(17, 17, 17, 255),
        .accent = RGBA8(255, 255, 255, 255),
        .accent_text = RGBA8(0, 0, 0, 255),
        .text = RGBA8(255, 255, 255, 255),
        .text_dim = RGBA8(138, 138, 138, 255),
        .border = RGBA8(42, 42, 42, 255),
        .success = RGBA8(48, 209, 88, 255),
        .danger = RGBA8(255, 69, 58, 255),
        .warn = RGBA8(255, 214, 10, 255),
        .overlay = RGBA8(0, 0, 0, 200),
    },
};

const char *theme_names[THEME_COUNT] = { "Yaru", "OLED", "Custom" };

const Theme *theme = &themes[THEME_YARU][THEME_DARK];

/* ---------- 自定义色系：主色（HSV）推导整套 ---------- */
/* 主色由用户在色盘页选（h 0-359 / s 0-100 / v 0-100），其余色值按
 * "背景=主色压暗、卡片/边线逐级提亮、高亮=主色、文字按对比度取白/深"
 * 推出，保证同一主色下的整套配色协调。暗/浅各一套，明暗轴照常正交。 */
static int  custom_h = 210, custom_s = 65, custom_v = 90;
static Theme custom[2];   /* [THEME_DARK] / [THEME_LIGHT]，theme_set_custom 重建 */
static int  custom_ready = 0;   /* 防止在首次注入主色前选中 Custom 时读到全零色值 */

uint32_t theme_hsv(int h, int s, int v)
{
    float H = (float)(h % 360); if (H < 0) H += 360.0f;   /* 色相环 */
    float S = (float)(s < 0 ? 0 : s > 100 ? 100 : s) / 100.0f;
    float V = (float)(v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
    float c = V * S;
    float x = c * (1.0f - fabsf(fmodf(H / 60.0f, 2.0f) - 1.0f));
    float m = V - c;
    float r = 0, g = 0, b = 0;
    int seg = (int)(H / 60.0f) % 6;
    switch (seg) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    int R = (int)((r + m) * 255.0f + 0.5f);
    int G = (int)((g + m) * 255.0f + 0.5f);
    int B = (int)((b + m) * 255.0f + 0.5f);
    return RGBA8(R, G, B, 255);
}

/* 感知亮度（0~1）：决定主色上放白字还是深字 */
static float lum_of(uint32_t c)
{
    float r = (float)(c & 0xFF) / 255.0f;
    float g = (float)((c >> 8) & 0xFF) / 255.0f;
    float b = (float)((c >> 16) & 0xFF) / 255.0f;
    return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

static void build_custom(void)
{
    Theme *d = &custom[THEME_DARK];
    Theme *l = &custom[THEME_LIGHT];
    uint32_t accent = theme_hsv(custom_h, custom_s, custom_v);

    /* 深色：背景 = 主色压暗 + 降饱和（避免大面积高饱和刺眼），
     * 卡片 / 边线逐级提亮；边线必须比 text_dim 更暗（禁用=更弱）。 */
    d->bg        = theme_hsv(custom_h, custom_s * 32 / 100, 13);
    d->card      = theme_hsv(custom_h, custom_s * 28 / 100, 20);
    d->border    = theme_hsv(custom_h, custom_s * 22 / 100, 28);
    d->accent    = accent;
    d->accent_text = lum_of(accent) > 0.55f ? RGBA8(20, 20, 20, 255)
                                            : RGBA8(255, 255, 255, 255);
    d->text      = theme_hsv(custom_h, custom_s * 5 / 100, 98);
    d->text_dim  = theme_hsv(custom_h, custom_s * 12 / 100, 72);
    d->success   = RGBA8(63, 185, 80, 255);
    d->danger    = RGBA8(229, 72, 77, 255);
    d->warn      = RGBA8(245, 166, 35, 255);
    d->overlay   = RGBA8(0, 0, 0, 170);

    /* 浅色：白卡片 + 主色压深（保证白底对比）；状态色一并压深。
     * 边线必须比 text_dim 更浅（禁用=更弱，浅色系里"更弱"=更浅）。 */
    uint32_t accent_l = theme_hsv(custom_h, custom_s, custom_v * 82 / 100);
    l->bg        = theme_hsv(custom_h, custom_s * 10 / 100, 95);
    l->card      = theme_hsv(custom_h, custom_s * 4 / 100, 100);
    l->border    = theme_hsv(custom_h, custom_s * 10 / 100, 83);
    l->accent    = accent_l;
    l->accent_text = lum_of(accent_l) > 0.55f ? RGBA8(20, 20, 20, 255)
                                              : RGBA8(255, 255, 255, 255);
    l->text      = theme_hsv(custom_h, custom_s * 12 / 100, 11);
    l->text_dim  = theme_hsv(custom_h, custom_s * 8 / 100, 42);
    l->success   = RGBA8(21, 122, 47, 255);
    l->danger    = RGBA8(190, 26, 31, 255);
    l->warn      = RGBA8(158, 98, 0, 255);
    l->overlay   = RGBA8(0, 0, 0, 110);
}

void theme_set_custom(int h, int s, int v)
{
    custom_h = h < 0 ? 0 : h > 359 ? 359 : h;
    custom_s = s < 0 ? 0 : s > 100 ? 100 : s;
    custom_v = v < 0 ? 0 : v > 100 ? 100 : v;
    build_custom();
    custom_ready = 1;
}

void theme_set(ThemeId id, int mode)
{
    if (id < 0 || id >= THEME_COUNT) return;
    if (id == THEME_OLED) mode = THEME_DARK;   /* OLED 无浅色变体 */
    if (mode != THEME_LIGHT) mode = THEME_DARK;
    if (id == THEME_CUSTOM) {
        if (!custom_ready) theme_set_custom(custom_h, custom_s, custom_v);
        theme = &custom[mode];
        return;
    }
    theme = &themes[id][mode];
}
