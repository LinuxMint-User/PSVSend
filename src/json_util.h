/* 迷你 JSON 工具：只支持"顶层对象按键取值"，够 config / 发现消息用。
 * 不引入 SceLibJson（系统库对 homebrew 可用性/复杂度权衡），自写小型解析器：
 *  - 逐字节扫描，忽略嵌套结构（{} []）与字符串，只在根对象层匹配键
 *  - 字符串值做转义解码（含 \uXXXX / 代理对 → UTF-8）
 * 写 JSON 用 json_escape 转义后 snprintf 拼装。 */
#ifndef PSVSEND_JSON_UTIL_H
#define PSVSEND_JSON_UTIL_H

#include <stdbool.h>

/* 在 doc 的顶层对象中找 key 的字符串值（无则 false；out 至少 outsz 字节） */
bool json_get_str(const char *doc, const char *key, char *out, int outsz);
/* 找整数（无/非数字 → false） */
bool json_get_int(const char *doc, const char *key, long long *out);
/* 找布尔（无/非 true|false → false） */
bool json_get_bool(const char *doc, const char *key, bool *out);

/* ---- 对象成员遍历（prepare-upload 的 files map 等嵌套结构解析用） ----
 * 迭代器：obj 指向任意对象起始 '{'（可从 val 指针取嵌套对象，见下）。
 * 用法：for (ok = json_iter_first(obj, &it); ok; ok = json_iter_next(&it)) … */
typedef struct {
    const char *p;        /* 内部游标（迭代间移动，勿直接改） */
    char key[192];        /* 当前成员键（已做转义解码） */
    const char *val;      /* 当前成员值的原始起点（'"' '{' '[' 或裸字面量） */
} JsonIter;
bool json_iter_first(const char *obj, JsonIter *it);
bool json_iter_next(JsonIter *it);

/* 把 val（迭代器/上一级拿到的值指针，须指向 '"'）解码为字符串；非字符串 → false */
bool json_val_str(const char *val, char *out, int outsz);
/* 把 val（指向数字）读出整数；非数字 → false */
bool json_val_int(const char *val, long long *out);

/* 在 obj（任意对象起点 '{'）中找 key，返回其值起点指针（字符串 '"'、对象 '{'、
 * 数字、true/false/null 等）。只遍历 obj 的直接成员，不递归进嵌套值；
 * 找不到返回 NULL。用于先定位 info/files 等嵌套对象，再交给 json_iter_first/
 * json_get_str 继续解析。 */
const char *json_get_val(const char *obj, const char *key);

/* 把 in 转义成可嵌入 JSON 双引号字符串的内容（不含引号）。
 * 返回写入字节数（不含结尾 0），outsz>0 时保证结尾 0。 */
int json_escape(const char *in, char *out, int outsz);

#endif
