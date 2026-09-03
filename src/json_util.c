/* 迷你 JSON 工具实现（见 json_util.h）。
 * 防御式：任何位置解析失败都返回 false/跳过，不信任输入。 */
#include <string.h>
#include <stdlib.h>
#include "json_util.h"

/* 解析游标 */
typedef struct { const char *s; } P;

static void push(char *out, int *n, int outsz, char c)
{
    if (*n + 1 < outsz) out[*n] = c;   /* 留下结尾 0 的位置 */
    (*n)++;
}

static void skip_ws(P *p)
{
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n' || *p->s == '\r')
        p->s++;
}

static bool hex4(const char *h, unsigned *v)
{
    unsigned x = 0;
    int i;
    if (!h) return false;
    for (i = 0; i < 4; i++) {
        char c = h[i];
        x <<= 4;
        if (c >= '0' && c <= '9')      x |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') x |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') x |= (unsigned)(c - 'A' + 10);
        else return false;
    }
    *v = x;
    return true;
}

/* 把 Unicode 码点按 UTF-8 追加到输出 */
static void push_cp(char *out, int *n, int outsz, unsigned cp)
{
    if (cp < 0x80) {
        push(out, n, outsz, (char)cp);
    } else if (cp < 0x800) {
        push(out, n, outsz, (char)(0xC0 | (cp >> 6)));
        push(out, n, outsz, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        push(out, n, outsz, (char)(0xE0 | (cp >> 12)));
        push(out, n, outsz, (char)(0x80 | ((cp >> 6) & 0x3F)));
        push(out, n, outsz, (char)(0x80 | (cp & 0x3F)));
    } else {
        push(out, n, outsz, (char)(0xF0 | (cp >> 18)));
        push(out, n, outsz, (char)(0x80 | ((cp >> 12) & 0x3F)));
        push(out, n, outsz, (char)(0x80 | ((cp >> 6) & 0x3F)));
        push(out, n, outsz, (char)(0x80 | (cp & 0x3F)));
    }
}

/* 读取一个 JSON 字符串字面量（p 指向开引号），解码到 out（至多 outsz-1 字节） */
static bool read_string(P *p, char *out, int outsz)
{
    int n = 0;
    if (*p->s != '"') return false;
    p->s++;
    while (*p->s) {
        char c = *p->s;
        if (c == '"') {
            p->s++;
            if (out && outsz > 0) out[n < outsz - 1 ? n : outsz - 1] = 0;
            return true;
        }
        if (c == '\\') {
            p->s++;
            c = *p->s;
            if (!c) return false;
            switch (c) {
            case '"': case '\\': case '/': push(out, &n, outsz, c); break;
            case 'b': push(out, &n, outsz, '\b'); break;
            case 'f': push(out, &n, outsz, '\f'); break;
            case 'n': push(out, &n, outsz, '\n'); break;
            case 'r': push(out, &n, outsz, '\r'); break;
            case 't': push(out, &n, outsz, '\t'); break;
            case 'u': {
                unsigned cp;
                const char *h = p->s + 1;
                if (!hex4(h, &cp)) return false;
                p->s += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    /* 高代理后紧跟 \uDC00..DFFF 低代理 → 合成码点 */
                    if (h[4] == '\\' && h[5] == 'u') {
                        unsigned lo;
                        if (hex4(h + 6, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p->s += 6;
                        }
                    }
                    push_cp(out, &n, outsz, cp);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    push(out, &n, outsz, '?');   /* 孤立低代理 */
                } else {
                    push_cp(out, &n, outsz, cp);
                }
                break;
            }
            default:
                push(out, &n, outsz, c);
                break;
            }
            p->s++;
            continue;
        }
        push(out, &n, outsz, c);
        p->s++;
    }
    return false;   /* 未闭合 */
}

/* 只前进、不输出的字符串跳读（读值略过时用） */
static void skip_string(P *p)
{
    if (*p->s != '"') return;
    p->s++;
    while (*p->s) {
        if (*p->s == '\\') { p->s++; if (*p->s) p->s++; }
        else if (*p->s == '"') { p->s++; return; }
        else p->s++;
    }
}

/* 跳过一个值（字符串 / 嵌套对象数组 / 裸 token） */
static void skip_value(P *p)
{
    int d;
    skip_ws(p);
    if (!*p->s) return;
    if (*p->s == '"') { skip_string(p); return; }
    if (*p->s == '{' || *p->s == '[') {
        d = 1;
        p->s++;
        while (*p->s && d > 0) {
            char x = *p->s;
            if (x == '"') { skip_string(p); continue; }
            if (x == '{' || x == '[') d++;
            else if (x == '}' || x == ']') d--;
            p->s++;
        }
        return;
    }
    while (*p->s && *p->s != ',' && *p->s != '}' && *p->s != ']')
        p->s++;
}

/* 找到 key 并把游标定位到其值的起始处；返回值类型字符：'"' / 数字 '-' 或 '0'-'9' / 't' / 'f' */
static bool find_value(const char *doc, const char *key, P *val)
{
    P p = { doc };
    char kbuf[128];
    skip_ws(&p);
    if (*p.s != '{') return false;
    p.s++;
    while (*p.s) {
        P q = p;
        skip_ws(&q);
        if (*q.s == '}') return false;
        if (!read_string(&q, kbuf, sizeof kbuf)) return false;
        p = q;
        skip_ws(&p);
        if (*p.s != ':') return false;
        p.s++;
        skip_ws(&p);
        if (strcmp(kbuf, key) == 0) { *val = p; return true; }
        skip_value(&p);
        skip_ws(&p);
        if (*p.s == ',') p.s++;
        else if (*p.s == '}') return false;
    }
    return false;
}

bool json_get_str(const char *doc, const char *key, char *out, int outsz)
{
    P v;
    if (!out || outsz <= 0) return false;
    out[0] = 0;
    if (!find_value(doc, key, &v)) return false;
    skip_ws(&v);
    if (*v.s != '"') return false;
    return read_string(&v, out, outsz);
}

bool json_get_int(const char *doc, const char *key, long long *out)
{
    P v;
    char tmp[40];
    int i = 0;
    if (!find_value(doc, key, &v)) return false;
    skip_ws(&v);
    if (*v.s != '-' && (*v.s < '0' || *v.s > '9')) return false;
    while (*v.s && i < (int)sizeof tmp - 1 &&
           *v.s != ',' && *v.s != '}' && *v.s != ']')
        tmp[i++] = *v.s++;
    tmp[i] = 0;
    if (i == 0) return false;
    *out = strtoll(tmp, NULL, 10);
    return true;
}

bool json_get_bool(const char *doc, const char *key, bool *out)
{
    P v;
    char tmp[8];
    int i = 0;
    if (!find_value(doc, key, &v)) return false;
    skip_ws(&v);
    while (*v.s && i < (int)sizeof tmp - 1 &&
           *v.s != ',' && *v.s != '}' && *v.s != ']')
        tmp[i++] = *v.s++;
    tmp[i] = 0;
    if (strcmp(tmp, "true") == 0) { *out = true; return true; }
    if (strcmp(tmp, "false") == 0) { *out = false; return true; }
    return false;
}

/* UTF-8 前导字节 → 序列长度（非法字节按 1 处理，原样透传） */
static int utf8_len(unsigned char c)
{
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int json_escape(const char *in, char *out, int outsz)
{
    int n = 0;
    if (outsz <= 0) return 0;
    while (in && *in) {
        unsigned char c = (unsigned char)*in;
        if (c == '"')      { push(out, &n, outsz, '\\'); push(out, &n, outsz, '"'); }
        else if (c == '\\'){ push(out, &n, outsz, '\\'); push(out, &n, outsz, '\\'); }
        else if (c == '\n'){ push(out, &n, outsz, '\\'); push(out, &n, outsz, 'n'); }
        else if (c == '\r'){ push(out, &n, outsz, '\\'); push(out, &n, outsz, 'r'); }
        else if (c == '\t'){ push(out, &n, outsz, '\\'); push(out, &n, outsz, 't'); }
        else if (c < 0x20) { push(out, &n, outsz, ' '); }
        else if (c < 0x80) { push(out, &n, outsz, (char)c); }
        else {
            /* 多字节 UTF-8 原样透传 */
            int l = utf8_len(c), k;
            for (k = 0; k < l && *in; k++)
                push(out, &n, outsz, *in++);
            continue;
        }
        in++;
    }
    out[n < outsz - 1 ? n : outsz - 1] = 0;
    return n;
}
