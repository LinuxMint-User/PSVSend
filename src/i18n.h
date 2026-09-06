/* i18n —— 界面文案中英（详见 i18n.c）。
 * 语言偏好存 Config.lang（0=跟随系统 1=English 2=中文）；
 * 运行时当前语言由 i18n_init 解析（偏好 AUTO 时用 SceAppUtil 读系统语言，
 * 只把简/繁中文映射成中文，其余语言暂回退英文）。
 * 译文用 tr() 取：key 就是英文文案（含 %s/%d 占位符），中文本按格式串翻译，
 * 调用点照旧传参，未收录或当前语言为英文时原样返回 key。 */
#ifndef PSVSEND_I18N_H
#define PSVSEND_I18N_H

/* 语言偏好（g_cfg.lang 存这个值，设置页语言行也用） */
enum {
    I18N_LANG_AUTO = 0,    /* 跟随系统 */
    I18N_LANG_EN,          /* 强制 English */
    I18N_LANG_ZH,          /* 简体中文 */
    I18N_LANG_COUNT
};

/* config_init 之后调用：按 g_cfg.lang 解析当前语言（AUTO 尝试读系统参数） */
void i18n_init(void);

/* 当前实际语言：I18N_LANG_EN / I18N_LANG_ZH（渲染前查一次即可） */
int i18n_lang(void);

/* 改语言偏好（写 g_cfg + 存盘 + 重解析），立即生效 */
void i18n_set_lang(int pref);

/* 当前偏好（g_cfg.lang，设置页高亮用） */
int i18n_lang_pref(void);

/* 语言选项显示名（AUTO/EN/ZH，各自语种自述） */
const char *i18n_lang_name(int id);

/* 取 key 在当前语言下的译文；未收录时原样返回 key（英文兜底） */
const char *tr(const char *key);

#endif
