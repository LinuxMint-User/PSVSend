/* i18n —— 界面文案多语言（英文 / 简体中文 / 繁中（台灣）/ 繁中（香港）/ 日本語，
 * 详见 i18n.c）。
 * 语言偏好存 Config.lang（0=跟随系统 1=English 2=简体 3=繁中（台灣）4=繁中（香港）
 * 5=日本語）；运行时当前语言由 i18n_init 解析（偏好 AUTO 时用 SceAppUtil 读系统
 * 语言，系统简/繁中文分别映射到简体 / 繁中（台灣）——系统只区分简繁、无法再分
 * 台港，而 PSV 的中文（繁體）本地化基底是台灣用語，故繁体默认取台灣；港式用词
 * 可在设置页语言行手选「繁中（香港）」；系统日语映射到日本語；其余语言回退英文）。
 * 译文用 tr() 取：key 就是英文文案（含 %s/%d 占位符），译文按格式串翻译，
 * 调用点照旧传参，未收录或当前语言为英文时原样返回 key。 */
#ifndef PSVSEND_I18N_H
#define PSVSEND_I18N_H

/* 语言偏好（g_cfg.lang 存这个值，设置页语言行也用）。
 * 新增语言项一律加在 I18N_LANG_COUNT 之前，保证老配置里的数值含义不变。 */
enum {
    I18N_LANG_AUTO = 0,    /* 跟随系统 */
    I18N_LANG_EN,          /* 强制 English */
    I18N_LANG_ZH,          /* 简体中文 */
    I18N_LANG_ZH_TW,       /* 繁中（台灣）：繁体 + 台灣用词习惯 */
    I18N_LANG_ZH_HK,       /* 繁中（香港）：繁体 + 香港用词习惯 */
    I18N_LANG_JA,          /* 日本語 */
    I18N_LANG_COUNT
};

/* config_init 之后调用：按 g_cfg.lang 解析当前语言（AUTO 尝试读系统参数） */
void i18n_init(void);

/* 当前实际语言：I18N_LANG_EN / ZH / ZH_TW / ZH_HK / JA（渲染前查一次即可） */
int i18n_lang(void);

/* 改语言偏好（写 g_cfg + 存盘 + 重解析），立即生效 */
void i18n_set_lang(int pref);

/* 当前偏好（g_cfg.lang，设置页高亮用） */
int i18n_lang_pref(void);

/* 语言选项显示名（AUTO/EN/简/繁台/繁港/日，各自语种自述） */
const char *i18n_lang_name(int id);

/* 取 key 在当前语言下的译文；未收录时原样返回 key（英文兜底） */
const char *tr(const char *key);

#endif
