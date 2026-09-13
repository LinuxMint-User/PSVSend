#include "ui/theme.h"
#include <vita2d.h>

/* 色系 × 明暗 二维表：行 = 色系，列 = [深色, 浅色]。
 * 注意 border 兼作"禁用 / 未聚焦"色（页脚灭臂、未聚焦卡片底），
 * 深色系里它比 text_dim 更暗，浅色系里必须比 text_dim 更浅，
 * 否则"禁用"会读成"更黑更抢眼"，语义反转。 */
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

const char *theme_names[THEME_COUNT] = { "Yaru", "OLED" };

const Theme *theme = &themes[THEME_YARU][THEME_DARK];

void theme_set(ThemeId id, int mode)
{
    if (id < 0 || id >= THEME_COUNT) return;
    if (id == THEME_OLED) mode = THEME_DARK;   /* OLED 无浅色变体 */
    if (mode != THEME_LIGHT) mode = THEME_DARK;
    theme = &themes[id][mode];
}
