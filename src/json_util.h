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

/* 把 in 转义成可嵌入 JSON 双引号字符串的内容（不含引号）。
 * 返回写入字节数（不含结尾 0），outsz>0 时保证结尾 0。 */
int json_escape(const char *in, char *out, int outsz);

#endif
