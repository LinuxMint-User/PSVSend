/* 配置读写实现（见 config.h）。 */
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/processmgr.h>
#include "config.h"
#include "json_util.h"

Config g_cfg = { DEFAULT_ALIAS, "", DEFAULT_PORT, 0, 0 };

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
        }
    }

    if (!g_cfg.fingerprint[0]) gen_fingerprint(g_cfg.fingerprint,
                                               sizeof g_cfg.fingerprint);
    if (g_cfg.port <= 0 || g_cfg.port > 65535) g_cfg.port = DEFAULT_PORT;
    if (!g_cfg.alias[0]) strncpy(g_cfg.alias, DEFAULT_ALIAS, sizeof g_cfg.alias - 1);
}

void config_save(void)
{
    char a[2 * sizeof g_cfg.alias];
    char f[2 * sizeof g_cfg.fingerprint];
    char out[1024];
    int len;
    json_escape(g_cfg.alias, a, sizeof a);
    json_escape(g_cfg.fingerprint, f, sizeof f);
    len = snprintf(out, sizeof out,
                   "{\n"
                   "  \"alias\": \"%s\",\n"
                   "  \"fingerprint\": \"%s\",\n"
                   "  \"port\": %d,\n"
                   "  \"theme\": %d,\n"
                   "  \"confirmLayout\": %d\n"
                   "}\n",
                   a, f, g_cfg.port, g_cfg.theme_id, g_cfg.confirm_layout);
    if (len < 0 || len >= (int)sizeof out) return;
    SceUID fd = sceIoOpen(PSVSEND_CONFIG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                          0777);
    if (fd < 0) return;
    sceIoWrite(fd, out, (unsigned)len);
    sceIoClose(fd);
}
