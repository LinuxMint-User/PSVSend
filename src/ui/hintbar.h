/* 底部按键提示条（页脚）模块。
 *
 * 页面只声明"本页用到哪些键、文案、状态"（HintSeg 数组），**不必关心顺序**：
 * 段由本模块按默认键序排好再绘制，顺序规则只在 hintbar.c 实现一处。
 * 默认键序：SELECT、START、方向键、○、✗、□、△；其中"动作随焦点/状态变化"
 * 的段（HintSeg.varies）统一靠右，固定段靠左。
 * 排列顺序与文案来自页面，与"哪个物理键是确认键"解耦：页面写 HKEY_CONFIRM /
 * HKEY_BACK，图标与排序位由本模块按当前键位布局推出。 */
#ifndef PSVSEND_UI_HINTBAR_H
#define PSVSEND_UI_HINTBAR_H

#include <stdbool.h>
#include <stdint.h>

/* 段里用到的键。页面只写键，不写图标（图标由键推出，见 hintbar.c icon_of）。 */
typedef enum {
    HKEY_NONE = 0,  /* 无键（不画图标，纯文案段）；取 0 使 `= { 0 }` 零初始化安全 */
    HKEY_SELECT,    /* SELECT：无符号图标，靠文案说明 */
    HKEY_START,     /* START */
    HKEY_DPAD,      /* 方向键（十字）：按 dir_off 分亮臂/灰臂 */
    HKEY_CONFIRM,   /* 确认键：图标随布局在 ○/✗ 间切 */
    HKEY_BACK,      /* 返回键：与确认键互补 */
    HKEY_SQUARE,    /* SQUARE */
    HKEY_TRIANGLE,  /* TRIANGLE */
    HKEY_COUNT
} HintKey;

/* 方向键提示的"灭臂"掩码位（HintSeg.dir_off）：标出当前按不动的方向，画成灰臂。
 * 0 = 四向全亮（默认）——把"全亮"取作 0，使 `= { 0 }` 零初始化安全：
 * 页面忘了设也只是多亮几臂，不会凭空把方向键画暗。 */
#define HDIR_UP    0x1
#define HDIR_DOWN  0x2
#define HDIR_LEFT  0x4
#define HDIR_RIGHT 0x8
#define HDIR_VERT  (HDIR_UP | HDIR_DOWN)
#define HDIR_HORZ  (HDIR_LEFT | HDIR_RIGHT)

/* 一段按键提示。dim=true：该动作当前不可用（画成灰字灰图标，避免误导）。
 * key2 != HKEY_NONE：该段是"两个键同一个动作"，画成「键 / 键 文案」，
 * 两个键都显示出来，动作文案只写一次。
 * varies=true：本段动作随焦点/运行状态改变（如 Send↔Remove、Cancel↔Done），
 * 排到固定段右侧——变动的只会推自己右侧的东西，左边固定提示不会跟着乱跳。
 * dir_off 仅对 HKEY_DPAD 有意义，见上方 HDIR_* 说明。
 * 注意：HintSeg 数组请用 `= { 0 }` 初始化，否则各字段是未初始化值。 */
typedef struct {
    HintKey key, key2;
    const char *text;   /* 可空：只画图标 */
    bool dim;
    bool varies;
    uint8_t dir_off;    /* 方向键灰掉的方向（0 = 全亮） */
} HintSeg;

/* 底部按键提示条：用 HintSeg 描述（推荐，顺序由本模块排） */
void w_page_footer_segs(const HintSeg *segs, int n);
/* 底部提示条：单行纯文案（无键位图标） */
void w_page_footer(const char *hint);

/* 页脚蓄力环：在下一次 w_page_footer_segs 中，给 key 所在段的图标周围画一圈
 * 进度环（pct 0..1，从 12 点顺时针；底环灰、进度弧主色）。
 * 与 w_clear/w_add 同模式——一次性设置，绘制后自动清除：页面每帧设一次即可，
 * 不设就不画（0 或未设为不画）。dim 段不画。 */
void w_page_footer_charge(HintKey key, float pct);

/* 独立"✕"图标（列表行尾删除按钮，不用在提示条里） */
void w_icon_cross(float cx, float cy, float r, uint32_t c);

/* 独立"✓"图标（设置页选项面板标记当前档；与 w_icon_cross 同一套线宽口径） */
void w_icon_check(float cx, float cy, float r, uint32_t c);

#endif /* PSVSEND_UI_HINTBAR_H */
