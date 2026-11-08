/* 接收文件夹的纯路径逻辑（不依赖 VitaSDK，可在开发机单测）。
 *
 * 协议事实：文件夹不是目录对象，而是若干 fileName 带相对路径的普通文件，
 * 例如 "Photos/2026/a.jpg"。本模块只负责把对端给的相对路径变成可安全落盘的
 * 组件序列，并计算暂存/最终路径；真正的 sceIo* 调用留给 receive.c。
 *
 * 口径（与 TODO「文件夹接收执行方案」一致）：
 *   - `\` 归一化为 `/`；拒绝绝对路径、盘符、`.`/`..`、空组件；
 *   - 每个组件独立 sanitize（非法字符→`_`、去尾点/空格、非法 UTF-8 拒绝）；
 *   - 组件上限 191 字节（与现有 recv_sanitize_name / UI 缓冲一致）；
 *   - 相对路径上限 512 字节；深度上限 16；
 *   - 路径总长按**暂存内的 part 路径**算（含 .psvsend-part / .psvsend.tmp），
 *     上限 1023 字节，溢出必须报错，绝不静默截断。
 *
 * 大小写：FAT/exFAT 大小写不敏感但保留拼写。这里的 rp_name_cmp 只做 ASCII
 * 大小写折叠；非 ASCII 的碰撞由 write 时的 sceIoGetstat 兜底（M1 实现）。
 */
#ifndef PSVSEND_PROTO_RECV_PATH_H
#define PSVSEND_PROTO_RECV_PATH_H

#include <stdbool.h>
#include <stddef.h>

#define RP_COMP_MAX    191      /* 单组件字节上限（与 RECV_NAME_MAX 一致） */
#define RP_REL_MAX     512      /* 相对路径字节上限 */
#define RP_MAX_DEPTH   16       /* 目录深度上限（防递归/栈溢出） */
#define RP_PATH_MAX    1023     /* 完整路径字节上限（含 ux0: 前缀） */
#define RP_EXT_MAX     16       /* 截断时视作“值得保留的后缀”的最大长度 */

#define RP_STAGE_SUFFIX ".psvsend-part"   /* 顶层目录的暂存后缀 */
#define RP_PART_SUFFIX  ".psvsend.tmp"    /* 单文件写入中的临时后缀 */

typedef enum {
    RP_OK = 0,
    RP_E_EMPTY,          /* 空路径 */
    RP_E_ABSOLUTE,       /* 以 '/' 开头 */
    RP_E_DRIVE,          /* 形如 C: / C:foo */
    RP_E_DOT,            /* 含 '.' 或 '..' 组件 */
    RP_E_EMPTY_COMP,     /* 空组件：a//b、a/、/a */
    RP_E_DEPTH,          /* 组件数超过 RP_MAX_DEPTH */
    RP_E_REL_TOO_LONG,   /* 相对路径超过 RP_REL_MAX */
    RP_E_INVALID_UTF8,   /* 非法 UTF-8 序列 */
    RP_E_BUFFER          /* 输出缓冲不足 */
} RpError;

/* 稳定的错误名（测试/日志用） */
const char *rp_strerror(RpError e);

/* 严格 UTF-8 校验：拒绝 overlong、代理区、>U+10FFFF、截断序列。 */
bool rp_utf8_valid(const char *s);

/* 返回 s 中不超过 n 字节的最大“完整 UTF-8 字符”前缀长度。
 * 调用者保证 n <= strlen(s)。 */
size_t rp_utf8_floor(const char *s, size_t n);

/* 把对端相对路径解析成可落盘的相对路径。
 * - 成功：out 为净化后的相对路径（'/' 分隔），depth 为组件数；
 *   truncated=true 表示至少一个组件因超长被缩短（确认页要提示）。
 * - 失败：返回错误码，out 置空。 */
RpError rp_parse_relpath(const char *in, char *out, size_t outsz,
                         int *depth, bool *truncated);

/* 单组件净化（UI 改名 / 顶层名也用它）。
 * 与现有 recv_sanitize_name 的差异：输入必须是单组件，不在这里拆 basename；
 * 非法 UTF-8 直接返回 false（旧实现是默默按字节截断）。
 * truncated=true 表示因超过 RP_COMP_MAX 被缩短。 */
bool rp_sanitize_component(const char *in, char *out, size_t outsz,
                           bool *truncated);

/* ASCII 大小写不敏感比较（返回 <0/0/>0，语义同 strcmp）。 */
int rp_name_cmp(const char *a, const char *b);
static inline bool rp_name_eq(const char *a, const char *b)
{
    return rp_name_cmp(a, b) == 0;
}

/* 顶层名是否以保留后缀结尾（.psvsend-part / .psvsend.tmp，大小写不敏感）。 */
bool rp_reserved_name(const char *name);

/* 生成第 k 个不重名候选：base (k).ext，结果仍 <= RP_COMP_MAX 且 < outsz。
 * k 从 1 开始；无法容纳（缓冲太小/长度不够）返回 false。 */
bool rp_unique_name(const char *name, int k, char *out, size_t outsz);

/* ---- 路径拼接：全部返回 false 表示超长/参数非法，out 置空 ---- */

/* <save_dir>/<top>.psvsend-part */
bool rp_stage_root_path(const char *save_dir, const char *top,
                        char *out, size_t outsz);

/* <save_dir>/<top>.psvsend-part/<inner> （暂存内某文件的最终名） */
bool rp_stage_file_path(const char *save_dir, const char *top,
                        const char *inner, char *out, size_t outsz);

/* <save_dir>/<top>.psvsend-part/<inner>.psvsend.tmp （实际写入路径） */
bool rp_stage_part_path(const char *save_dir, const char *top,
                        const char *inner, char *out, size_t outsz);

/* <save_dir>/<top>[/<inner>] （最终保存位置） */
bool rp_final_path(const char *save_dir, const char *top,
                   const char *inner, char *out, size_t outsz);

/* <save_dir>/<name>.psvsend.tmp （顶层平铺文件的临时路径） */
bool rp_flat_part_path(const char *save_dir, const char *name,
                       char *out, size_t outsz);

#endif
