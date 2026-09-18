/* 配置读写实现（见 config.h）。 */
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr/thread.h>
#include <psp2/kernel/processmgr.h>
#include "config.h"
#include "i18n.h"          /* I18N_LANG_*（读盘值域钳制用；i18n.h 不反向依赖本头） */
#include "dlog.h"          /* 写盘失败的诊断（不再静默） */
#include "json_util.h"
#include <psp2/kernel/threadmgr/mutex.h>

Config g_cfg = { 0 };          /* 默认值见 cfg_defaults()（逐字段赋值，防结构体顺序耦合） */
static SceUID g_mtx = -1;          /* 保护 g_cfg：UI 改设置 / 发现·扫描记 IP 并发 */
static uint64_t g_last_save_us = 0;

#define CFG_TMP    PSVSEND_CONFIG ".tmp"   /* 原子替换用临时文件（同目录，rename 不跨卷） */
#define CFG_THEME_MAX 2                    /* = ui/theme.h 的 THEME_COUNT-1；
                                            * core 层不引用 ui 头，加色系时两处同步 */

/* 锁由 config_init() 预先建立（惰性创建在首次并发下可能两个线程各建一把 →
 * 一把锁形同虚设 + SceUID 泄漏）；下面仅作兜底，正常路径不会再建。 */
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
    g_cfg.light_mode = 0;              /* 明暗：默认深色 */
    g_cfg.custom_h = 210;              /* 自定义主色默认：偏冷的蓝（用户可改） */
    g_cfg.custom_s = 65;
    g_cfg.custom_v = 90;
    g_cfg.confirm_layout = 0;
    g_cfg.pane_swap = 0;               /* 主页两栏：默认设备在左 */
    g_cfg.max_parallel = PARALLEL_DEFAULT;  /* 发送侧并发上传数：默认 3 */
    g_cfg.lang = 0;                    /* 语言偏好默认跟随系统 */
    g_cfg.known_n = 0;
    g_cfg.upd_auto = 2;                /* 自动检查更新：默认每周 */
    g_cfg.upd_last = 0;                /* 从未查过 → 启动联网后自动授时评估即首查 */
    strncpy(g_cfg.save_dir, PSVSEND_DL_DIR, sizeof g_cfg.save_dir - 1);
    g_cfg.save_dir[sizeof g_cfg.save_dir - 1] = 0;
}

void config_init(void)
{
    char buf[2048];
    int n;
    long long v;

    /* 锁在这里预建（见 cfg_lock 说明）：本函数在启动早期单线程执行 */
    if (g_mtx < 0) g_mtx = sceKernelCreateMutex("psvsend_cfg", 0, 0, NULL);
    ensure_dir(PSVSEND_DATA_DIR);
    ensure_dir(PSVSEND_DL_DIR);
    cfg_defaults();

    {   /* 自愈：上次保存死在"删旧配置 → 改名"两步之间时，盘上没有 config，却有
         * 写完并已 sync 的 config.tmp（见 config_write_locked）——那就是完整的
         * 新配置，改名回来即恢复。写盘走的始终是 tmp，config 只可能"完整"或
         * "不存在"，故只需判读不到的情况。 */
        SceIoStat st;
        if (sceIoGetstat(PSVSEND_CONFIG, &st) < 0 &&
            sceIoGetstat(CFG_TMP, &st) == 0 &&
            sceIoRename(CFG_TMP, PSVSEND_CONFIG) == 0)
            dlog("cfg: recovered from tmp (config was missing)");
    }

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
            if (json_get_int(buf, "lightMode", &v)) g_cfg.light_mode = (int)v ? 1 : 0;
            if (json_get_int(buf, "customH", &v)) g_cfg.custom_h = (int)v;
            if (json_get_int(buf, "customS", &v)) g_cfg.custom_s = (int)v;
            if (json_get_int(buf, "customV", &v)) g_cfg.custom_v = (int)v;
            if (json_get_int(buf, "confirmLayout", &v)) g_cfg.confirm_layout = (int)v;
            if (json_get_int(buf, "paneSwap", &v)) g_cfg.pane_swap = (int)v;
            if (json_get_int(buf, "maxParallel", &v)) g_cfg.max_parallel = (int)v;
            if (json_get_int(buf, "lang", &v)) g_cfg.lang = (int)v;
            if (json_get_int(buf, "updateAuto", &v)) g_cfg.upd_auto = (int)v;
            if (json_get_int(buf, "updateLast", &v)) g_cfg.upd_last = v;
            json_get_str(buf, "saveDir", g_cfg.save_dir, sizeof g_cfg.save_dir);
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
    if (g_cfg.pane_swap != 0 && g_cfg.pane_swap != 1) g_cfg.pane_swap = 0;
    if (g_cfg.max_parallel < PARALLEL_MIN || g_cfg.max_parallel > PARALLEL_MAX)
        g_cfg.max_parallel = PARALLEL_DEFAULT;
    if (g_cfg.custom_h < 0 || g_cfg.custom_h > 359) g_cfg.custom_h = 210;
    if (g_cfg.custom_s < 0 || g_cfg.custom_s > 100) g_cfg.custom_s = 65;
    if (g_cfg.custom_v < 0 || g_cfg.custom_v > 100) g_cfg.custom_v = 90;
    if (!g_cfg.alias[0]) strncpy(g_cfg.alias, DEFAULT_ALIAS, sizeof g_cfg.alias - 1);
    if (g_cfg.upd_auto < 0 || g_cfg.upd_auto > 3) g_cfg.upd_auto = 2;
    if (g_cfg.upd_last < 0) g_cfg.upd_last = 0;
    /* 值域钳制收在"读盘入口"统一做：config 是外部可编辑的文件，非法值会直达
     * theme_names[g_cfg.theme_id] 等索引。UI 侧不必再各自防（那里的钳制会被
     * 本函数按盘上原值覆盖回来，等于没钳）。 */
    if (g_cfg.theme_id < 0 || g_cfg.theme_id > CFG_THEME_MAX) g_cfg.theme_id = 0;
    if (g_cfg.lang < I18N_LANG_AUTO || g_cfg.lang >= I18N_LANG_COUNT)
        g_cfg.lang = I18N_LANG_AUTO;
    /* saveDir：去尾斜杠（但保留 ux0:/ 根的自带斜杠，勿剥成 "ux0:"）；
     * 空/非法（非 ux0: 开头或不足 ux0:/）回退默认 downloads */
    {
        size_t sl = strlen(g_cfg.save_dir);
        while (sl > 5 && g_cfg.save_dir[sl - 1] == '/') g_cfg.save_dir[--sl] = 0;
        if (sl < 5 || strncmp(g_cfg.save_dir, "ux0:", 4) != 0) {
            strncpy(g_cfg.save_dir, PSVSEND_DL_DIR, sizeof g_cfg.save_dir - 1);
            g_cfg.save_dir[sizeof g_cfg.save_dir - 1] = 0;
        }
    }
}

/* 调用者必须已持锁：构造 JSON → 写临时文件 → rename 原子替换。
 * 直接 O_TRUNC 写目标文件的话，与另一线程的写并发会落出半截 JSON（下次启动
 * 解析失败即回退默认值，丢 alias/knownIps）；写失败则保留盘上旧配置——旧实现
 * 不看 sceIoWrite 返回值，一次磁盘满就把全部设置清空。 */
static void config_write_locked(void)
{
    char a[2 * sizeof g_cfg.alias];
    char f[2 * sizeof g_cfg.fingerprint];
    char d[2 * sizeof g_cfg.save_dir];
    char k[KNOWN_MAX * 17];        /* "ip,ip,...,ip" 最长 = 24*(15+1)-1 */
    char out[4096];
    int len, i, w;
    SceUID fd;

    json_escape(g_cfg.alias, a, sizeof a);
    json_escape(g_cfg.fingerprint, f, sizeof f);
    json_escape(g_cfg.save_dir, d, sizeof d);
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
                   "  \"lightMode\": %d,\n"
                   "  \"customH\": %d,\n"
                   "  \"customS\": %d,\n"
                   "  \"customV\": %d,\n"
                   "  \"confirmLayout\": %d,\n"
                   "  \"paneSwap\": %d,\n"
                   "  \"maxParallel\": %d,\n"
                   "  \"lang\": %d,\n"
                   "  \"updateAuto\": %d,\n"
                   "  \"updateLast\": %lld,\n"
                   "  \"saveDir\": \"%s\",\n"
                   "  \"knownIps\": \"%s\"\n"
                   "}\n",
                   a, f, g_cfg.port, g_cfg.theme_id, g_cfg.light_mode,
                   g_cfg.custom_h, g_cfg.custom_s, g_cfg.custom_v,
                   g_cfg.confirm_layout, g_cfg.pane_swap, g_cfg.max_parallel,
                   g_cfg.lang, g_cfg.upd_auto, g_cfg.upd_last, d, k);
    if (len < 0 || len >= (int)sizeof out) {
        dlog("cfg: json overflow (%d)", len);
        return;
    }
    fd = sceIoOpen(CFG_TMP, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0) {
        dlog("cfg: tmp open fail 0x%08X", (unsigned)fd);
        return;
    }
    w = sceIoWrite(fd, out, (unsigned)len);
    /* 先把内容真正刷到卡上再动旧配置：启动自愈的前提是 tmp 里的字节已完整落盘，
     * 否则"改名完成但数据还在缓存里就掉电"仍会留下 0 长度/半截的 config。 */
    if (w == len) sceIoSyncByFd(fd, 0);
    sceIoClose(fd);
    if (w != len) {
        dlog("cfg: write %d/%d -> keep old config", w, len);
        sceIoRemove(CFG_TMP);
        return;
    }
    /* uX0 的 sceIoRename 不支持覆盖已存在文件（真机 d135：恒返失败、每次保存都
     * 落进下面的直写兜底），故先删旧配置再改名 —— FAT 上没有原子替换可用。两次
     * 调用之间断电留下的局面是"config 缺失 + 完整 tmp"，由 config_init 的自愈逻辑
     * 改名恢复，所以设置不会丢；而写 tmp 期间旧配置始终完好（旧实现 O_TRUNC 直写
     * 时写一半断电，落的是半截 JSON，更难察觉）。 */
    sceIoRemove(PSVSEND_CONFIG);
    if (sceIoRename(CFG_TMP, PSVSEND_CONFIG) < 0) {
        /* rename 仍失败（占用/只读等）时兜底直写：宁可非原子也要把设置存下来 */
        dlog("cfg: rename fail -> direct write");
        fd = sceIoOpen(PSVSEND_CONFIG, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC,
                       0777);
        if (fd >= 0) {
            w = sceIoWrite(fd, out, (unsigned)len);
            sceIoClose(fd);
            if (w != len) dlog("cfg: direct write %d/%d fail", w, len);
        } else {
            dlog("cfg: direct open fail 0x%08X", (unsigned)fd);
        }
        sceIoRemove(CFG_TMP);
    }
}

void config_save(void)
{
    cfg_lock();
    config_write_locked();
    cfg_unlock();
}

/* ---- 跨线程共享字段的加锁访问（说明见 config.h） ---- */

void config_get_alias(char *out, int n)
{
    cfg_lock();
    snprintf(out, (size_t)n, "%s", g_cfg.alias);
    cfg_unlock();
}

void config_set_alias(const char *alias)
{
    cfg_lock();
    snprintf(g_cfg.alias, sizeof g_cfg.alias, "%s", alias ? alias : "");
    if (!g_cfg.alias[0])
        snprintf(g_cfg.alias, sizeof g_cfg.alias, "%s", DEFAULT_ALIAS);
    config_write_locked();
    cfg_unlock();
}

void config_get_fingerprint(char *out, int n)
{
    cfg_lock();
    snprintf(out, (size_t)n, "%s", g_cfg.fingerprint);
    cfg_unlock();
}

void config_get_save_dir(char *out, int n)
{
    cfg_lock();
    snprintf(out, (size_t)n, "%s", g_cfg.save_dir);
    cfg_unlock();
}

void config_set_save_dir(const char *dir)
{
    cfg_lock();
    snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", dir ? dir : "");
    config_write_locked();
    cfg_unlock();
}

void config_set_lang(int lang)
{
    cfg_lock();
    if (lang < I18N_LANG_AUTO || lang >= I18N_LANG_COUNT) lang = I18N_LANG_AUTO;
    g_cfg.lang = lang;
    config_write_locked();
    cfg_unlock();
}

void config_set_upd_auto(int v)
{
    cfg_lock();
    if (v < 0 || v > 3) v = 2;
    g_cfg.upd_auto = v;
    config_write_locked();
    cfg_unlock();
}

void config_set_max_parallel(int v)
{
    cfg_lock();
    if (v < PARALLEL_MIN || v > PARALLEL_MAX) v = PARALLEL_DEFAULT;
    g_cfg.max_parallel = v;
    config_write_locked();
    cfg_unlock();
}

void config_set_upd_last(long long t)
{
    cfg_lock();
    g_cfg.upd_last = t < 0 ? 0 : t;
    config_write_locked();
    cfg_unlock();
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
