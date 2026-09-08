/* 配置读写实现（见 config.h）。 */
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/processmgr.h>
#include "config.h"
#include "json_util.h"
#include <psp2/kernel/threadmgr/mutex.h>

Config g_cfg = { DEFAULT_ALIAS, "", DEFAULT_PORT, 0, 0, 0, 0, {{0}} };
static SceUID g_mtx = -1;          /* 保护 g_cfg：UI 改设置 / 发现·扫描记 IP 并发 */
static uint64_t g_last_save_us = 0;

static void cfg_lock(void)
{
    if (g_mtx < 0) g_mtx = sceKernelCreateMutex("psvsend_cfg", 0, 0, NULL);
    if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
}
static void cfg_unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

static void ensure_dir(const char *path)
{
    sceIoMkdir(path, 0777);   /* 已存在返回 0x80410011，忽略 */
}

/* 身份串：时间 + 进程时间混出 64 位十六进制，足够区分设备与避免自发现 */
static void gen_fingerprint(char *out, int n)
{
    uint64_t t = (uint64_t)sceKernelGetSystemTimeWide();
    uint64_t p = (uint64_t)sceKernelGetProcessTimeWide();
    snprintf(out, n, "psvsend-%016llx%016llx",
             (unsigned long long)(t ^ (p << 1)),
             (unsigned long long)p);
}

static void cfg_defaults(void)
{
    strncpy(g_cfg.alias, DEFAULT_ALIAS, sizeof g_cfg.alias - 1);
    g_cfg.alias[sizeof g_cfg.alias - 1] = 0;
    g_cfg.fingerprint[0] = 0;
    g_cfg.port = DEFAULT_PORT;
    g_cfg.theme_id = 0;
    g_cfg.confirm_layout = 0;
    g_cfg.lang = 0;                    /* 语言偏好默认跟随系统 */
    g_cfg.known_n = 0;
    g_cfg.upd_auto = 2;                /* 自动检查更新：默认每周 */
    g_cfg.upd_last = 0;                /* 从未查过 → 启动联网后自动授时评估即首查 */
}

void config_init(void)
{
    char buf[2048];
    int n;
    long long v;

    ensure_dir(PSVSEND_DATA_DIR);
    ensure_dir(PSVSEND_DL_DIR);
    cfg_defaults();

    SceUID fd = sceIoOpen(PSVSEND_CONFIG, SCE_O_RDONLY, 0);
    if (fd >= 0) {
        n = sceIoRead(fd, buf, sizeof buf - 1);
        sceIoClose(fd);
        if (n > 0) {
            buf[n] = 0;
            json_get_str(buf, "alias", g_cfg.alias, sizeof g_cfg.alias);
            json_get_str(buf, "fingerprint", g_cfg.fingerprint,
                         sizeof g_cfg.fingerprint);
            if (json_get_int(buf, "port", &v)) g_cfg.port = (int)v;
            if (json_get_int(buf, "theme", &v)) g_cfg.theme_id = (int)v;
            if (json_get_int(buf, "confirmLayout", &v)) g_cfg.confirm_layout = (int)v;
            if (json_get_int(buf, "lang", &v)) g_cfg.lang = (int)v;
            if (json_get_int(buf, "updateAuto", &v)) g_cfg.upd_auto = (int)v;
            if (json_get_int(buf, "updateLast", &v)) g_cfg.upd_last = (int)v;
            {   /* knownIps: "ip,ip,..."（逗号分隔，最新在前） */
                char k[512];
                if (json_get_str(buf, "knownIps", k, sizeof k) && k[0]) {
                    char *s = k, *comma;
                    g_cfg.known_n = 0;
                    while (g_cfg.known_n < KNOWN_MAX && *s) {
                        comma = strchr(s, ',');
                        if (comma) *comma = 0;
                        if (s[0]) {
                            strncpy(g_cfg.known_ips[g_cfg.known_n], s,
                                    sizeof g_cfg.known_ips[0] - 1);
                            g_cfg.known_ips[g_cfg.known_n]
                                [sizeof g_cfg.known_ips[0] - 1] = 0;
                            g_cfg.known_n++;
                        }
                        if (!comma) break;
                        s = comma + 1;
                    }
                }
            }
        }
    }

    if (!g_cfg.fingerprint[0]) gen_fingerprint(g_cfg.fingerprint,
                                               sizeof g_cfg.fingerprint);
    if (g_cfg.port <= 0 || g_cfg.port > 65535) g_cfg.port = DEFAULT_PORT;
    if (!g_cfg.alias[0]) strncpy(g_cfg.alias, DEFAULT_ALIAS, sizeof g_cfg.alias - 1);
    if (g_cfg.upd_auto < 0 || g_cfg.upd_auto > 3) g_cfg.upd_auto = 2;
    if (g_cfg.upd_last < 0) g_cfg.upd_last = 0;
}

void config_save(void)
{
    char a[2 * sizeof g_cfg.alias];
    char f[2 * sizeof g_cfg.fingerprint];
    char k[KNOWN_MAX * 17];        /* "ip,ip,...,ip" 最长 = 24*(15+1)-1 */
    char out[2048];
    int len, i;
    cfg_lock();
    json_escape(g_cfg.alias, a, sizeof a);
    json_escape(g_cfg.fingerprint, f, sizeof f);
    k[0] = 0;
    for (i = 0; i < g_cfg.known_n; i++) {
        if (i) strncat(k, ",", sizeof k - strlen(k) - 1);
        strncat(k, g_cfg.known_ips[i], sizeof k - strlen(k) - 1);
    }
    len = snprintf(out, sizeof out,
                   "{\n"
                   "  \"alias\": \"%s\",\n"
                   "  \"fingerprint\": \"%s\",\n"
                   "  \"port\": %d,\n"
                   "  \"theme\": %d,\n"
                   "  \"confirmLayout\": %d,\n"
                   "  \"lang\": %d,\n"
                   "  \"updateAuto\": %d,\n"
                   "  \"updateLast\": %d,\n"
                   "  \"knownIps\": \"%s\"\n"
                   "}\n",
                   a, f, g_cfg.port, g_cfg.theme_id, g_cfg.confirm_layout,
                   g_cfg.lang, g_cfg.upd_auto, g_cfg.upd_last, k);
    cfg_unlock();
    if (len < 0 || len >= (int)sizeof out) return;
    SceUID fd = sceIoOpen(PSVSEND_CONFIG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                          0777);
    if (fd < 0) return;
    sceIoWrite(fd, out, (unsigned)len);
    sceIoClose(fd);
}

/* 记录最近在线的设备 IP：去重、最新在前；写盘节流避免扫描一轮狂写 */
void config_note_ip(const char *ip)
{
    unsigned a, b, c, d;
    int i;
    if (!ip || !ip[0]) return;
    if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 ||
        a > 255 || b > 255 || c > 255 || d > 255 || d == 0 || d > 254)
        return;                          /* 非点分 IPv4 / 本网段边界值，忽略 */
    cfg_lock();
    for (i = 0; i < g_cfg.known_n; i++)
        if (strcmp(g_cfg.known_ips[i], ip) == 0) break;
    if (i < g_cfg.known_n) {
        char tmp[16];
        memcpy(tmp, g_cfg.known_ips[i], sizeof tmp);       /* 已存在：移到最前 */
        memmove(&g_cfg.known_ips[1], &g_cfg.known_ips[0],
                sizeof g_cfg.known_ips[0] * i);
        memcpy(g_cfg.known_ips[0], tmp, sizeof tmp);
    } else {
        if (g_cfg.known_n >= KNOWN_MAX) g_cfg.known_n = KNOWN_MAX - 1;
        memmove(&g_cfg.known_ips[1], &g_cfg.known_ips[0],
                sizeof g_cfg.known_ips[0] * g_cfg.known_n);
        strncpy(g_cfg.known_ips[0], ip, sizeof g_cfg.known_ips[0] - 1);
        g_cfg.known_ips[0][sizeof g_cfg.known_ips[0] - 1] = 0;
        g_cfg.known_n++;
    }
    cfg_unlock();

    if (g_last_save_us == 0 ||
        (uint64_t)sceKernelGetSystemTimeWide() - g_last_save_us >= 5000000ull) {
        g_last_save_us = (uint64_t)sceKernelGetSystemTimeWide();
        config_save();
    }
}

/* 扫描种子：返回与 a.b.c. 前缀匹配的历史主机号（保留"最新在前"次序） */
int config_known_hosts(unsigned a, unsigned b, unsigned c, int *out, int max)
{
    int n = 0, i;
    if (!out || max <= 0) return 0;
    cfg_lock();
    for (i = 0; i < g_cfg.known_n && n < max; i++) {
        unsigned ha, hb, hc, hd;
        if (sscanf(g_cfg.known_ips[i], "%u.%u.%u.%u", &ha, &hb, &hc, &hd) == 4 &&
            ha == a && hb == b && hc == c && hd >= 1 && hd <= 254)
            out[n++] = (int)hd;
    }
    cfg_unlock();
    return n;
}
