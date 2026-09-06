/* 落盘日志实现：append 模式写 ux0:data/psvsend/log.txt。
 * 前置：PSVSEND_DATA_DIR 目录已由 config_init() 建好。 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include "dlog.h"

#define DLOG_PATH "ux0:data/psvsend/log.txt"

static SceUID g_fd = -1;    /* -1 未开 / -2 打不开（永久放弃） */
static SceUID g_mtx = -1;   /* 多线程（http/watch/scan worker/主线程）并发写保护 */

void dlog_init(void)
{
    if (g_fd >= 0) sceIoClose(g_fd);
    g_fd = sceIoOpen(DLOG_PATH,
                     SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (g_fd < 0) g_fd = -2;
    if (g_mtx < 0) g_mtx = sceKernelCreateMutex("psvsend_dlog", 0, 0, NULL);
}

void dlog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int n;
    if (g_fd == -2) return;
    if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL);
    if (g_fd < 0) {
        /* 未 init 时的兜底：直接以 append 方式打开 */
        g_fd = sceIoOpen(DLOG_PATH,
                         SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
        if (g_fd < 0) { g_fd = -2; goto out; }
    }
    n = snprintf(buf, sizeof buf, "[%llu] ",
                 (unsigned long long)sceKernelGetSystemTimeWide() / 1000);
    if (n < 0 || n >= (int)sizeof buf) n = 0;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - (unsigned)n, fmt, ap);
    va_end(ap);
    n = (int)strlen(buf);
    if (n + 2 < (int)sizeof buf) { buf[n] = '\n'; buf[n + 1] = 0; n++; }
    sceIoWrite(g_fd, buf, (SceSize)n);
out:
    if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1);
}
