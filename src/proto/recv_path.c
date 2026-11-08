/* 接收文件夹的纯路径逻辑；实现说明见 recv_path.h。 */
#include "proto/recv_path.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool is_cont(unsigned char c)
{
    return (c & 0xC0) == 0x80;
}

const char *rp_strerror(RpError e)
{
    switch (e) {
    case RP_OK:             return "ok";
    case RP_E_EMPTY:        return "empty path";
    case RP_E_ABSOLUTE:     return "absolute path";
    case RP_E_DRIVE:        return "drive letter";
    case RP_E_DOT:          return "dot component";
    case RP_E_EMPTY_COMP:   return "empty component";
    case RP_E_DEPTH:        return "path too deep";
    case RP_E_REL_TOO_LONG: return "relative path too long";
    case RP_E_INVALID_UTF8: return "invalid utf-8";
    case RP_E_BUFFER:       return "buffer too small";
    }
    return "unknown";
}

bool rp_utf8_valid(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (!p) return false;
    while (*p) {
        unsigned c = *p;
        int n, i;
        unsigned cp;
        if (c < 0x80) {
            p++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            n = 2;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
            cp = c & 0x07;
        } else {
            return false;
        }
        for (i = 1; i < n; i++) {
            if (!p[i] || !is_cont(p[i])) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (n == 2 && cp < 0x80) return false;
        if (n == 3 && cp < 0x800) return false;
        if (n == 4 && cp < 0x10000) return false;
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;
        if (cp > 0x10FFFF) return false;
        p += n;
    }
    return true;
}

size_t rp_utf8_floor(const char *s, size_t n)
{
    size_t k;
    if (!s || n == 0) return 0;
    k = n;
    while (k > 0 && is_cont((unsigned char)s[k - 1])) k--;
    if (k > 0) {
        unsigned char c = (unsigned char)s[k - 1];
        size_t need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        if (k - 1 + need > n) return k - 1;
    }
    return n;
}

/* 与 receive.c 的 name_byte 保持同一口径：路径分隔符/控制字符 → '_' */
static unsigned char name_byte(unsigned char c)
{
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\' || c == ':' ||
        c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
        return '_';
    return c;
}

bool rp_sanitize_component(const char *in, char *out, size_t outsz,
                           bool *truncated)
{
    int lim, o = 0, i, keep, elen = 0;
    bool any = false, cut = false;
    const char *dot = NULL;
    size_t len;

    if (truncated) *truncated = false;
    if (!out || outsz == 0) return false;
    out[0] = 0;
    if (!in) return false;
    if (!rp_utf8_valid(in)) return false;

    while (*in == ' ') in++;
    lim = (int)(outsz - 1);
    if (lim > RP_COMP_MAX) lim = RP_COMP_MAX;
    if (lim < 1) return false;

    len = strlen(in);
    if (len <= (size_t)lim) {
        keep = (int)len;
    } else {
        size_t el = 0;
        cut = true;
        dot = strrchr(in, '.');
        if (dot && dot != in) {
            el = strlen(dot);
            if (el <= RP_EXT_MAX && el < (size_t)lim) elen = (int)el;
            else dot = NULL;
        }
        keep = lim - elen;
        if (keep < 0) keep = 0;
    }
    keep = (int)rp_utf8_floor(in, (size_t)keep);

    for (i = 0; i < keep; i++) {
        unsigned char c = name_byte((unsigned char)in[i]);
        out[o++] = (char)c;
        if (c != '_') any = true;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    if (dot) {
        for (i = 0; i < elen; i++) {
            unsigned char c = name_byte((unsigned char)dot[i]);
            out[o++] = (char)c;
            if (c != '_') any = true;
        }
    }
    out[o] = 0;
    if (!any || !out[0] || strcmp(out, ".") == 0 || strcmp(out, "..") == 0) {
        if (lim < 7) {
            out[0] = 0;
            return false;
        }
        memcpy(out, "unnamed", 8);
    }
    if (truncated) *truncated = cut;
    return true;
}

RpError rp_parse_relpath(const char *in, char *out, size_t outsz,
                         int *depth, bool *truncated)
{
    char norm[RP_REL_MAX + 1];
    char joined[RP_REL_MAX + 1];
    size_t inlen, olen = 0;
    const char *p, *end;
    int d = 0;
    bool cut_any = false;

    if (out && outsz) out[0] = 0;
    if (depth) *depth = 0;
    if (truncated) *truncated = false;
    if (!in || !*in) return RP_E_EMPTY;
    if (!out || outsz == 0) return RP_E_BUFFER;

    inlen = strlen(in);
    if (inlen > RP_REL_MAX) return RP_E_REL_TOO_LONG;
    {
        size_t i;
        for (i = 0; i < inlen; i++)
            norm[i] = (in[i] == '\\') ? '/' : in[i];
        norm[inlen] = 0;
    }
    if (norm[0] == '/') return RP_E_ABSOLUTE;
    if (isalpha((unsigned char)norm[0]) && norm[1] == ':') return RP_E_DRIVE;

    p = norm;
    end = norm + inlen;
    while (p < end) {
        const char *slash = memchr(p, '/', (size_t)(end - p));
        size_t clen = slash ? (size_t)(slash - p) : (size_t)(end - p);
        char raw[RP_REL_MAX + 1];
        char clean[RP_COMP_MAX + 1];
        size_t need, sep;
        bool cut = false;

        if (clen == 0) return RP_E_EMPTY_COMP;
        if ((clen == 1 && p[0] == '.') ||
            (clen == 2 && p[0] == '.' && p[1] == '.'))
            return RP_E_DOT;
        memcpy(raw, p, clen);
        raw[clen] = 0;
        if (!rp_utf8_valid(raw)) return RP_E_INVALID_UTF8;
        if (!rp_sanitize_component(raw, clean, sizeof clean, &cut))
            return RP_E_BUFFER;
        if (cut) cut_any = true;
        if (d >= RP_MAX_DEPTH) return RP_E_DEPTH;

        need = strlen(clean);
        sep = (d > 0) ? 1 : 0;
        if (olen + sep + need > RP_REL_MAX) return RP_E_REL_TOO_LONG;
        if (olen + sep + need >= outsz) return RP_E_BUFFER;
        if (sep) joined[olen++] = '/';
        memcpy(joined + olen, clean, need);
        olen += need;
        d++;

        if (!slash) break;
        p = slash + 1;
        if (p >= end) return RP_E_EMPTY_COMP;   /* 尾随 '/' */
    }
    if (d == 0) return RP_E_EMPTY;
    joined[olen] = 0;
    memcpy(out, joined, olen + 1);
    if (depth) *depth = d;
    if (truncated) *truncated = cut_any;
    return RP_OK;
}

int rp_name_cmp(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        int la = tolower(ca);
        int lb = tolower(cb);
        if (la != lb) return la < lb ? -1 : 1;
        if (!ca) return 0;
    }
}

static bool ends_with_ci(const char *s, const char *suffix)
{
    size_t ls, lf;
    if (!s || !suffix) return false;
    ls = strlen(s);
    lf = strlen(suffix);
    return ls >= lf && rp_name_cmp(s + ls - lf, suffix) == 0;
}

bool rp_reserved_name(const char *name)
{
    return ends_with_ci(name, RP_STAGE_SUFFIX) ||
           ends_with_ci(name, RP_PART_SUFFIX);
}

bool rp_unique_name(const char *name, int k, char *out, size_t outsz)
{
    char suffix[16];
    int sn;
    size_t lim, elen = 0, blen, need, base_lim;
    const char *dot;

    if (out && outsz) out[0] = 0;
    if (!name || !*name || !out || outsz == 0 || k < 1) return false;
    sn = snprintf(suffix, sizeof suffix, " (%d)", k);
    if (sn < 0 || (size_t)sn >= sizeof suffix) return false;
    need = (size_t)sn;

    lim = outsz - 1;
    if (lim > RP_COMP_MAX) lim = RP_COMP_MAX;
    if (need >= lim) return false;

    dot = strrchr(name, '.');
    if (dot && dot != name) {
        size_t el = strlen(dot);
        if (el <= RP_EXT_MAX && need + el < lim) elen = el;
    }
    if (elen == 0) dot = NULL;

    base_lim = lim - need - elen;
    if (base_lim == 0) return false;
    blen = strlen(name) - elen;
    if (blen > base_lim) {
        blen = rp_utf8_floor(name, base_lim);
        while (blen > 0 && (name[blen - 1] == ' ' || name[blen - 1] == '.'))
            blen--;
    }
    if (blen == 0 || blen + need + elen > lim) return false;

    memcpy(out, name, blen);
    memcpy(out + blen, suffix, need);
    if (elen) memcpy(out + blen + need, dot, elen);
    out[blen + need + elen] = 0;
    return true;
}

static bool path_fail(char *out, size_t outsz)
{
    if (out && outsz) out[0] = 0;
    return false;
}

static bool path_add(char *out, size_t outsz, size_t *len, const char *s)
{
    size_t limit, n;
    if (!out || outsz == 0 || !len || !s) return false;
    limit = outsz - 1;
    if (limit > RP_PATH_MAX) limit = RP_PATH_MAX;
    n = strlen(s);
    if (*len > limit || n > limit - *len) return path_fail(out, outsz);
    memcpy(out + *len, s, n);
    *len += n;
    out[*len] = 0;
    return true;
}

static bool path_add_sep(char *out, size_t outsz, size_t *len)
{
    if (*len > 0 && out[*len - 1] == '/') return true;
    return path_add(out, outsz, len, "/");
}

static bool valid_component_arg(const char *s)
{
    if (!s || !*s || strchr(s, '/') || strlen(s) > RP_COMP_MAX) return false;
    if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return false;
    return true;
}

static bool valid_inner_arg(const char *s)
{
    const char *p;
    size_t n;
    if (!s || !*s || s[0] == '/') return false;
    n = strlen(s);
    if (s[n - 1] == '/' || n > RP_REL_MAX) return false;
    p = s;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0) return false;
        if ((len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.'))
            return false;
        if (!slash) break;
        p = slash + 1;
    }
    return true;
}

bool rp_stage_root_path(const char *save_dir, const char *top,
                        char *out, size_t outsz)
{
    size_t len = 0;
    if (!out || outsz == 0) return false;
    out[0] = 0;
    if (!save_dir || !*save_dir || !valid_component_arg(top)) return false;
    if (!path_add(out, outsz, &len, save_dir)) return false;
    if (!path_add_sep(out, outsz, &len)) return false;
    if (!path_add(out, outsz, &len, top)) return false;
    return path_add(out, outsz, &len, RP_STAGE_SUFFIX);
}

bool rp_stage_file_path(const char *save_dir, const char *top,
                        const char *inner, char *out, size_t outsz)
{
    size_t len;
    if (!rp_stage_root_path(save_dir, top, out, outsz)) return false;
    if (!valid_inner_arg(inner)) return path_fail(out, outsz);
    len = strlen(out);
    if (!path_add_sep(out, outsz, &len)) return false;
    return path_add(out, outsz, &len, inner);
}

bool rp_stage_part_path(const char *save_dir, const char *top,
                        const char *inner, char *out, size_t outsz)
{
    size_t len;
    if (!rp_stage_file_path(save_dir, top, inner, out, outsz)) return false;
    len = strlen(out);
    return path_add(out, outsz, &len, RP_PART_SUFFIX);
}

bool rp_final_path(const char *save_dir, const char *top,
                   const char *inner, char *out, size_t outsz)
{
    size_t len = 0;
    if (!out || outsz == 0) return false;
    out[0] = 0;
    if (!save_dir || !*save_dir || !valid_component_arg(top)) return false;
    if (!path_add(out, outsz, &len, save_dir)) return false;
    if (!path_add_sep(out, outsz, &len)) return false;
    if (!path_add(out, outsz, &len, top)) return false;
    if (inner && *inner) {
        if (!valid_inner_arg(inner)) return path_fail(out, outsz);
        if (!path_add_sep(out, outsz, &len)) return false;
        if (!path_add(out, outsz, &len, inner)) return false;
    }
    return true;
}

bool rp_flat_part_path(const char *save_dir, const char *name,
                       char *out, size_t outsz)
{
    size_t len = 0;
    if (!out || outsz == 0) return false;
    out[0] = 0;
    if (!save_dir || !*save_dir || !valid_component_arg(name)) return false;
    if (!path_add(out, outsz, &len, save_dir)) return false;
    if (!path_add_sep(out, outsz, &len)) return false;
    if (!path_add(out, outsz, &len, name)) return false;
    return path_add(out, outsz, &len, RP_PART_SUFFIX);
}
