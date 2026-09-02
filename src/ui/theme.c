#include "theme.h"
#include <vita2d.h>

static const Theme themes[THEME_COUNT] = {
    /* Yaru 深色: Ubuntu 橙 */
    [THEME_YARU] = {
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
    /* OLED 深色: 纯黑高对比（PSV1000 最佳） */
    [THEME_OLED] = {
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

const Theme *theme = &themes[THEME_YARU];

void theme_set(ThemeId id)
{
    if (id >= 0 && id < THEME_COUNT)
        theme = &themes[id];
}
