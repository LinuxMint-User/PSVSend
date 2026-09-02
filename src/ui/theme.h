/* PSVSend UI 主题
 * 所有界面颜色只从当前主题取，运行时可切换，下帧生效。
 * 深色基调；内置 OLED 与 Yaru 两套。 */
#ifndef PSVSEND_UI_THEME_H
#define PSVSEND_UI_THEME_H

#include <stdint.h>

typedef struct {
    uint32_t bg;         /* 页面背景 */
    uint32_t card;       /* 卡片 / 列表行 */
    uint32_t accent;     /* 主色: 选中高亮 / 按钮 / 进度条 */
    uint32_t accent_text;
    uint32_t text;       /* 主文字 */
    uint32_t text_dim;   /* 次要文字 */
    uint32_t border;
    uint32_t success;
    uint32_t danger;
    uint32_t warn;
    uint32_t overlay;    /* 弹窗遮罩（含 alpha） */
} Theme;

typedef enum {
    THEME_YARU,
    THEME_OLED,
    THEME_COUNT
} ThemeId;

extern const Theme *theme;
extern const char *theme_names[];

void theme_set(ThemeId id);

#endif
