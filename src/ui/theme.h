/* PSVSend UI 主题
 * 所有界面颜色只从当前主题取，运行时可切换，下帧生效。
 * 两个正交轴：色系（ThemeId：Yaru / OLED / Custom）× 明暗（THEME_DARK / THEME_LIGHT）。
 * "浅色版 Yaru" = Yaru × LIGHT；OLED 定位纯黑高对比，无浅色变体（恒定深色）。
 * Custom（自定义）不写死色值：由用户所选主色经 HSV 推导整套，暗/浅各一套
 * （见 theme_set_custom），设置页色盘页选色、下帧生效。 */
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
    uint32_t border;     /* 分隔线 / 未聚焦卡片底 / 禁用态——兼作"更弱"色，
                          * 深色系里比 text_dim 更暗，浅色系里必须比它更浅 */
    uint32_t success;
    uint32_t danger;
    uint32_t warn;
    uint32_t overlay;    /* 弹窗遮罩（含 alpha） */
} Theme;

typedef enum {
    THEME_YARU,
    THEME_OLED,
    THEME_CUSTOM,   /* 自定义：整套色值由主色（HSV）推导 */
    THEME_COUNT
} ThemeId;

/* 明暗轴（与色系正交） */
enum {
    THEME_DARK = 0,
    THEME_LIGHT = 1
};

extern const Theme *theme;
extern const char *theme_names[];

/* 切主题：色系 id + 明暗 mode。id 非法则保持当前主题；
 * mode 非法按深色处理；OLED 恒定深色（无浅色变体）。 */
void theme_set(ThemeId id, int mode);

/* HSV（h 0-359 / s 0-100 / v 0-100）→ RGBA8。
 * 自定义主题推导与色盘页渐变条共用，避免两处各写一份换算。 */
uint32_t theme_hsv(int h, int s, int v);

/* 设自定义主色并重建 Custom 色系的两套明暗（越界自动夹取）。
 * 若当前正用 Custom，因色值原地重建，下帧即生效。 */
void theme_set_custom(int h, int s, int v);

#endif
