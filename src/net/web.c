/* web.c —— Web 网页端（见 web.h）。会话状态、PIN 鉴权、网页资源。
 *
 * 线程模型：http.c 的每个连接各有一个 worker 线程，这里的状态全部由 g_mtx
 * 串行保护（锁内只做字符串/整数操作，不做文件 IO）。
 *
 * 网页是编译进固件的单页（英文打底 + 运行时按 navigator.languages 选简中 /
 * 繁中 / 日文），不读 VPK 外部资源，也就没有"漏打包 assets"这种失败模式。
 * 下载页与上传页共用一套样式与脚本，只有面板不同（MODE 常量区分）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/processmgr.h>
#include "net/web.h"
#include "net/http.h"
#include "net/net.h"
#include "core/dlog.h"

/* ================= 会话状态 ================= */

/* 同一 IP 输错 PIN 3 次后的冷却时长：到点自动清零（对齐网页上"稍后再试"的提示，
 * 也让用户不必回 PSV 退出共享页才能重试）。 */
#define WEB_PIN_LOCK_US  (30ULL * 1000 * 1000)

/* 会话"在线"判定的空闲上限：页面每 2 秒发一次 /heartbeat（见 HTML_STRINGS 的
 * `beat()`），关页即停；这里留 3 个心跳的余量。只影响 PSV 运行页上的显示，
 * 不删会话记录——浏览器刷新回来仍可带 sessionId 免 PIN。后台标签页会被浏览器
 * 把定时器降频到 ~1 次/分钟，此时显示会先归零、切回前台即恢复。 */
#define WEB_SESS_TTL_US  (7ULL * 1000 * 1000)

static struct {
    int   mode;                       /* WebMode（0=未开启：所有 web 路由 403） */
    char  pin[WEB_PIN_LEN + 1];
    unsigned gen;                     /* 每次 start/stop 递增：下载循环据此收手 */
    WebEntry files[WEB_MAX_FILES];
    int   n;
    SceOff total;

    struct { char sid[40]; char ip[16]; uint64_t at; } sess[WEB_MAX_SESS];
    int   nsess;

    /* 同一 IP 的 PIN 错误计数（对齐官方 common/pin.rs：错 3 次 → 429）。
     * 封禁不是永久的：错满 3 次后冷却 WEB_PIN_LOCK_US，到点自动清零、重新给 3 次
     * （见 pin_ok_locked）。否则一旦输错 3 次，同一台浏览器就只能等用户回 PSV
     * 上退出共享再重开——而页面上并没有任何提示说这件事。 */
    struct { char ip[16]; int fail; uint64_t until_us; } bad[WEB_MAX_SESS * 2];

    char  client_ip[16];
    char  client_ua[64];
    char  client_browser[32];         /* UA 解析出的短标签（Chrome 131 …） */
    int   requests;

    int   cur;                        /* 当前下载文件下标（-1=无） */
    int   cur_sess;                   /* 正在下载的会话下标（-1=无）：刷新其在线时间 */
    char  cur_name[128];
    SceOff cur_size;
    SceOff cur_got;
    SceOff done;
    SceOff sp_bytes;                  /* 速度采样基线 */
    uint64_t sp_us;
    double speed;
    uint64_t last_us;
} W;

static SceUID g_mtx = -1;

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

static uint64_t now_us(void) { return (uint64_t)sceKernelGetSystemTimeWide(); }

void web_init(void)
{
    if (g_mtx >= 0) return;
    g_mtx = sceKernelCreateMutex("psvsend_web", 0, 0, NULL);
    if (g_mtx < 0) dlog("web: create mutex fail 0x%08X", (unsigned)g_mtx);
    memset(&W, 0, sizeof W);
    W.cur = -1;
    W.cur_sess = -1;
    W.sp_us = now_us();
}

/* 随机 hex（会话号）：优先 libc getentropy（与 receive.c 同一写法），失败退回时间混洗 */
static void gen_hex(char *out, int bytes)
{
    unsigned char buf[32];
    int i;
    if (bytes > (int)sizeof buf) bytes = (int)sizeof buf;
    if (getentropy(buf, (size_t)bytes) == 0) {
        for (i = 0; i < bytes; i++) snprintf(out + i * 2, 3, "%02x", buf[i]);
        out[bytes * 2] = 0;
        return;
    }
    {
        uint64_t t = now_us() ^ ((uint64_t)sceKernelGetProcessTimeWide() << 7);
        for (i = 0; i < bytes; i++) {
            if ((i % 8) == 0) t = t * 6364136223846793005ULL + 0x9E3779B97F4A7C15ULL;
            snprintf(out + i * 2, 3, "%02x",
                     (unsigned)((t >> ((unsigned)((i * 5) % 56))) & 0xFF));
        }
        out[bytes * 2] = 0;
    }
}

void web_new_pin(void)
{
    unsigned char b[2];
    unsigned v;
    web_init();
    if (getentropy(b, sizeof b) != 0) {
        uint64_t t = now_us();
        b[0] = (unsigned char)(t >> 13);
        b[1] = (unsigned char)(t >> 27);
    }
    v = ((unsigned)b[0] << 8 | b[1]) % 10000u;
    lock();
    snprintf(W.pin, sizeof W.pin, "%04u", v);
    unlock();
}

void web_start_download(const WebEntry *files, int n)
{
    int i, cnt;
    SceOff total = 0;
    web_init();
    lock();
    if (!files) n = 0;
    W.mode = WEB_MODE_DOWNLOAD;
    W.n = n > WEB_MAX_FILES ? WEB_MAX_FILES : (n < 0 ? 0 : n);
    W.total = 0;
    for (i = 0; i < W.n; i++) {
        snprintf(W.files[i].name, sizeof W.files[i].name, "%s", files[i].name);
        snprintf(W.files[i].path, sizeof W.files[i].path, "%s", files[i].path);
        W.files[i].size = files[i].size;
        W.total += files[i].size;
    }
    W.nsess = 0;
    memset(W.bad, 0, sizeof W.bad);
    W.client_ip[0] = W.client_ua[0] = W.client_browser[0] = 0;
    W.requests = 0;
    W.cur = -1;
    W.cur_sess = -1;
    W.cur_name[0] = 0;
    W.cur_size = W.cur_got = W.done = W.sp_bytes = 0;
    W.sp_us = now_us();
    W.speed = 0;
    W.last_us = 0;
    W.gen++;
    cnt = W.n;
    total = W.total;
    unlock();
    dlog("web: download share start, %d file(s), %lld bytes", cnt, (long long)total);
}

void web_start_upload(void)
{
    web_init();
    lock();
    W.mode = WEB_MODE_UPLOAD;
    W.n = 0;
    W.total = 0;
    W.nsess = 0;
    memset(W.bad, 0, sizeof W.bad);
    W.client_ip[0] = W.client_ua[0] = W.client_browser[0] = 0;
    W.requests = 0;
    W.cur = -1;
    W.cur_sess = -1;
    W.cur_name[0] = 0;
    W.cur_size = W.cur_got = W.done = W.sp_bytes = 0;
    W.sp_us = now_us();
    W.speed = 0;
    W.last_us = 0;
    W.gen++;
    unlock();
    dlog("web: upload mode start");
}

void web_stop(void)
{
    web_init();
    lock();
    W.mode = WEB_MODE_NONE;
    W.n = 0;
    W.total = 0;
    W.nsess = 0;
    memset(W.bad, 0, sizeof W.bad);
    W.pin[0] = 0;
    W.cur = -1;
    W.cur_sess = -1;
    W.cur_name[0] = 0;
    W.cur_size = W.cur_got = W.done = W.sp_bytes = 0;
    W.speed = 0;
    W.gen++;                          /* 正在跑的下载循环下一块就会看到代次变化 */
    unlock();
}

int web_mode(void)
{
    int m;
    web_init();
    lock();
    m = W.mode;
    unlock();
    return m;
}

void web_snapshot(WebSnap *out)
{
    uint64_t now;
    int i, live = 0;
    if (!out) return;
    web_init();
    lock();
    memset(out, 0, sizeof *out);
    now = now_us();
    /* 只把"最近还有请求"的会话算在线：浏览器关页 / 断开后 at 不再刷新，
     * 30 秒后会话数与连接来源自然归零（会话记录本身保留，刷新回来仍免 PIN）。 */
    for (i = 0; i < W.nsess; i++)
        if (now - W.sess[i].at < WEB_SESS_TTL_US) live++;
    out->mode = W.mode;
    snprintf(out->pin, sizeof out->pin, "%s", W.pin);
    snprintf(out->ip, sizeof out->ip, "%s", net_local_ip());
    out->port = http_port();
    out->file_count = W.n;
    out->total_bytes = W.total;
    /* 连接来源同样看"最近有没有活动"：只开过页面（还没输 PIN）也算，方便在
     * 运行页上看到是谁连上了；离开后一起变回"等待浏览器"。 */
    if (live > 0 || (W.last_us && now - W.last_us < WEB_SESS_TTL_US)) {
        snprintf(out->client_ip, sizeof out->client_ip, "%s", W.client_ip);
        snprintf(out->client_ua, sizeof out->client_ua, "%s", W.client_ua);
        snprintf(out->client_browser, sizeof out->client_browser, "%s", W.client_browser);
    }
    out->sessions = live;
    out->requests = W.requests;
    out->cur = W.cur;
    snprintf(out->cur_name, sizeof out->cur_name, "%s", W.cur_name);
    out->cur_size = W.cur_size;
    out->cur_got = W.cur_got;
    out->done_bytes = W.done;
    out->speed_bps = W.speed;
    out->last_us = W.last_us;
    unlock();
}

/* ================= 鉴权 / 会话 ================= */

/* URL query 取参数（含 %XX 解码，够 pin / sessionId 这种 ASCII 值用） */
static void query_get(const char *q, const char *key, char *out, int cap)
{
    int kl = (int)strlen(key), o = 0;
    if (cap <= 0) return;
    out[0] = 0;
    if (!q) return;
    while (*q) {
        const char *amp = strchr(q, '&');
        int len = amp ? (int)(amp - q) : (int)strlen(q);
        if (len > kl && q[kl] == '=' && strncmp(q, key, (size_t)kl) == 0) {
            const char *p = q + kl + 1, *end = q + len;
            while (p < end && o < cap - 1) {
                if (*p == '%' && p + 2 < end) {
                    int hi = p[1], lo = p[2];
                    hi = (hi >= '0' && hi <= '9') ? hi - '0'
                       : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
                       : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
                    lo = (lo >= '0' && lo <= '9') ? lo - '0'
                       : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
                       : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
                    if (hi >= 0 && lo >= 0) {
                        out[o++] = (char)(hi << 4 | lo);
                        p += 3;
                        continue;
                    }
                }
                out[o++] = *p++;
            }
            out[o] = 0;
            return;
        }
        if (!amp) break;
        q = amp + 1;
    }
}

/* 该 IP 的错误记录槽（找不到时 create=1 则占一个新槽） */
static int bad_slot_locked(const char *ip, int create)
{
    int i, free_i = -1;
    for (i = 0; i < (int)(sizeof W.bad / sizeof W.bad[0]); i++) {
        if (W.bad[i].ip[0] && strcmp(W.bad[i].ip, ip) == 0) return i;
        if (!W.bad[i].ip[0] && free_i < 0) free_i = i;
    }
    if (create && free_i >= 0) {
        snprintf(W.bad[free_i].ip, sizeof W.bad[free_i].ip, "%s", ip);
        W.bad[free_i].fail = 0;
        return free_i;
    }
    return -1;
}

/* 锁内校验 PIN：200 / 401 / 429。失败累加到该 IP 的槽位；错满 3 次进入冷却，
 * 冷却结束自动清零（重新给 3 次机会），不需要用户退出共享页。 */
static int pin_ok_locked(const char *pin, const char *ip)
{
    int s;
    if (!pin || strlen(pin) != WEB_PIN_LEN || !W.pin[0]) return 401;
    s = bad_slot_locked(ip, 1);
    if (s >= 0 && W.bad[s].fail >= 3) {
        if (now_us() < W.bad[s].until_us) return 429;   /* 冷却中 */
        W.bad[s].fail = 0;                              /* 冷却结束：重新计数 */
    }
    if (strcmp(pin, W.pin) == 0) {
        if (s >= 0) { W.bad[s].fail = 0; W.bad[s].ip[0] = 0; }
        return 200;
    }
    if (s >= 0) {
        if (++W.bad[s].fail >= 3) {
            W.bad[s].until_us = now_us() + WEB_PIN_LOCK_US;
            return 429;
        }
    }
    return 401;
}

int web_check_pin(const char *pin, const char *ip)
{
    int r;
    web_init();
    lock();
    r = W.mode == WEB_MODE_NONE ? 403
      : (W.mode != WEB_MODE_UPLOAD ? 403 : pin_ok_locked(pin, ip));
    unlock();
    return r;
}

/* User-Agent → "Chrome 131" 这样的短标签（PSV 页面展示"连接来源"用）。
 * 只取主版本号：PSV 页面一行放不下完整 UA，官方客户端也只显示浏览器名/版本。
 * 表序有讲究——Edge / Opera 的 UA 里同样含 "Chrome/"，Safari 用 "Version/"
 * 取版本，因此必须按下面顺序匹配，命中即返回。 */
static void ua_label(const char *ua, char *out, int cap)
{
    static const struct { const char *tok; const char *name; } tab[] = {
        { "Edg/",     "Edge"    }, { "EdgA/",   "Edge"    },
        { "EdgiOS/",  "Edge"    }, { "OPR/",    "Opera"   },
        { "Opera/",   "Opera"   }, { "Firefox/", "Firefox" },
        { "FxiOS/",   "Firefox" }, { "CriOS/",  "Chrome"  },
        { "Chrome/",  "Chrome"  }, { "Version/", "Safari" },
        { "MSIE ",    "IE"      }, { "Trident/", "IE"     },
    };
    int i;
    if (cap <= 0) return;
    out[0] = 0;
    if (!ua || !ua[0]) return;
    for (i = 0; i < (int)(sizeof tab / sizeof tab[0]); i++) {
        const char *p = strstr(ua, tab[i].tok);
        char ver[8];
        int vn = 0;
        if (!p) continue;
        p += strlen(tab[i].tok);
        while (p[vn] >= '0' && p[vn] <= '9' && vn < (int)sizeof ver - 1) {
            ver[vn] = p[vn];
            vn++;
        }
        ver[vn] = 0;
        if (vn > 0) snprintf(out, (size_t)cap, "%s %s", tab[i].name, ver);
        else        snprintf(out, (size_t)cap, "%s", tab[i].name);
        return;
    }
    snprintf(out, (size_t)cap, "%s", "Browser");
}

void web_note_client(const char *ip, const char *ua)
{
    web_init();
    lock();
    snprintf(W.client_ip, sizeof W.client_ip, "%s", ip ? ip : "");
    snprintf(W.client_ua, sizeof W.client_ua, "%s", ua ? ua : "");
    ua_label(ua, W.client_browser, sizeof W.client_browser);
    W.requests++;
    W.last_us = now_us();
    unlock();
}

void web_upload_note(const char *ip, const char *ua)
{
    web_note_client(ip, ua);
}

/* 前端心跳：页面每 2 秒打一次，关页即停（见 web.h）。
 * 带有效 sessionId 就续该会话的在线时间；没带（还没输 PIN）只续"最近活动"，
 * 这样 PSV 运行页在输 PIN 之前也能显示连接来源。不参与 requests 计数。 */
void web_heartbeat(const char *query, const char *ip)
{
    char q_sid[48] = "";
    int i;
    web_init();
    lock();
    if (W.mode == WEB_MODE_NONE) { unlock(); return; }
    W.last_us = now_us();
    query_get(query, "sessionId", q_sid, sizeof q_sid);
    if (q_sid[0]) {
        for (i = 0; i < W.nsess; i++) {
            if (strcmp(W.sess[i].sid, q_sid) == 0 &&
                strcmp(W.sess[i].ip, ip ? ip : "") == 0) {
                W.sess[i].at = W.last_us;
                break;
            }
        }
    }
    unlock();
}

int web_prepare_download(const char *query, const char *ip,
                         char *sid_out, int sidcap,
                         char *files_json, int fsz)
{
    char q_sid[48] = "", q_pin[16] = "";
    int i, s, code = 403, o = 0;

    if (sid_out && sidcap > 0) sid_out[0] = 0;
    if (files_json && fsz > 0) files_json[0] = 0;
    query_get(query, "sessionId", q_sid, sizeof q_sid);
    query_get(query, "pin", q_pin, sizeof q_pin);

    web_init();
    lock();
    if (W.mode != WEB_MODE_DOWNLOAD) { unlock(); return 403; }
    W.requests++;
    W.last_us = now_us();

    /* 刷新页面：带同一会话号 + 同来源 IP 直接放行（协议 §5.2） */
    if (q_sid[0]) {
        for (i = 0; i < W.nsess; i++) {
            if (strcmp(W.sess[i].sid, q_sid) == 0 &&
                strcmp(W.sess[i].ip, ip ? ip : "") == 0) {
                W.sess[i].at = now_us();       /* 刷新即视为在线 */
                snprintf(sid_out, (size_t)sidcap, "%s", W.sess[i].sid);
                code = 200;
                break;
            }
        }
        if (code == 200) goto build;
        /* 会话号不认识（重启过 / 已停止）→ 让浏览器回去输 PIN */
        unlock();
        return 401;
    }

    code = pin_ok_locked(q_pin, ip);
    if (code != 200) { unlock(); return code; }

    /* 新会话：槽满时淘汰最老的一条 */
    s = 0;
    if (W.nsess < WEB_MAX_SESS) {
        s = W.nsess++;
    } else {
        for (i = 1; i < WEB_MAX_SESS; i++)
            if (W.sess[i].at < W.sess[s].at) s = i;
    }
    gen_hex(W.sess[s].sid, 16);
    snprintf(W.sess[s].ip, sizeof W.sess[s].ip, "%s", ip ? ip : "");
    W.sess[s].at = now_us();
    snprintf(sid_out, (size_t)sidcap, "%s", W.sess[s].sid);
    dlog("web: download session %s from %s", W.sess[s].sid, W.sess[s].ip);

build:
    if (fsz > 1) { files_json[0] = '['; files_json[1] = 0; o = 1; }
    for (i = 0; i < W.n && fsz > 0 && o < fsz - 8; i++) {
        char en[512];
        int el = 0;
        const char *p = W.files[i].name;
        /* 名字里的 " 与 \ 转义（文件名可以包含任意字符） */
        while (*p && el < (int)sizeof en - 3) {
            if (*p == '"' || *p == '\\') en[el++] = '\\';
            en[el++] = *p++;
        }
        en[el] = 0;
        o += snprintf(files_json + o, (size_t)(fsz - o),
                      "%s{\"id\":\"f%d\",\"fileName\":\"%s\",\"size\":%lld}",
                      i ? "," : "", i, en, (long long)W.files[i].size);
    }
    if (fsz > 0)
        snprintf(files_json + (o < fsz ? o : fsz - 1),
                 (size_t)(o < fsz ? fsz - o : 1), "]");
    unlock();
    return 200;
}

int web_resolve_download(const char *query, const char *ip, int *idx,
                         char *name, int namesz, SceOff *size)
{
    char q_sid[48] = "", q_fid[16] = "";
    int i, k, ok = 0;

    query_get(query, "sessionId", q_sid, sizeof q_sid);
    query_get(query, "fileId", q_fid, sizeof q_fid);
    if (!q_sid[0] || q_fid[0] != 'f') return 400;
    if (!q_fid[1]) return 400;
    for (i = 1; q_fid[i]; i++)
        if (q_fid[i] < '0' || q_fid[i] > '9') return 400;
    k = atoi(q_fid + 1);

    web_init();
    lock();
    /* 共享已停止：回 410（Gone）而不是 403——Gopeed 这类下载器会把 403 当成
     * "服务器连接数限制"忽略，于是把中断的下载判成完成；410 会被当失败。 */
    if (W.mode != WEB_MODE_DOWNLOAD) { unlock(); return 410; }
    for (i = 0; i < W.nsess; i++) {
        if (strcmp(W.sess[i].sid, q_sid) == 0) {
            /* 会话还在但来源 IP 不符：保持 403（安全语义，不回 410 暴露会话存在） */
            if (strcmp(W.sess[i].ip, ip ? ip : "") != 0) { unlock(); return 403; }
            W.sess[i].at = now_us();      /* 有下载请求 = 会话在线 */
            W.cur_sess = i;               /* 传输期间的进度刷新也续这个会话 */
            ok = 1;
            break;
        }
    }
    if (!ok) { unlock(); return 410; }   /* 会话号不认识（共享已重开 / 已停止） */
    if (k < 0 || k >= W.n) { unlock(); return 404; }
    W.requests++;
    W.last_us = now_us();
    if (idx) *idx = k;
    if (name && namesz > 0) snprintf(name, (size_t)namesz, "%s", W.files[k].name);
    if (size) *size = W.files[k].size;
    unlock();
    return 200;
}

const char *web_entry_path(int idx)
{
    return (idx >= 0 && idx < WEB_MAX_FILES) ? W.files[idx].path : "";
}

unsigned web_download_gen(void)
{
    unsigned g;
    lock();
    g = W.gen;
    unlock();
    return g;
}

int web_download_begin(int idx, SceOff size)
{
    int g;
    lock();
    W.cur = idx;
    snprintf(W.cur_name, sizeof W.cur_name, "%s",
             (idx >= 0 && idx < W.n) ? W.files[idx].name : "");
    W.cur_size = size;
    W.cur_got = 0;
    W.sp_bytes = W.done;
    W.sp_us = now_us();
    W.last_us = W.sp_us;
    g = (int)W.gen;
    unlock();
    return g;
}

void web_download_progress(int gen, SceOff got)
{
    uint64_t t = now_us();
    lock();
    if ((unsigned)gen != W.gen) { unlock(); return; }
    W.cur_got = got;
    W.last_us = t;
    /* 大文件一传几分钟：按块续会话的在线时间，否则 PSV 运行页会在传输中途
     * 把"会话数 / 连接来源"清掉。 */
    if (W.cur_sess >= 0 && W.cur_sess < W.nsess)
        W.sess[W.cur_sess].at = t;
    /* 400ms 采样 + 一阶平滑：分辨率高到能显示"MB/s"就够了，不必更细 */
    if (t - W.sp_us >= 400000ULL && t > W.sp_us) {
        double inst = (double)(W.done - W.sp_bytes) * 1000000.0 / (double)(t - W.sp_us);
        W.speed = W.speed > 0 ? W.speed * 0.55 + inst * 0.45 : inst;
        W.sp_bytes = W.done;
        W.sp_us = t;
    }
    unlock();
}

void web_download_end(int gen)
{
    lock();
    if ((unsigned)gen == W.gen) {
        W.done += W.cur_got;
        W.cur = -1;
        W.cur_got = 0;
        W.speed = 0;
        W.sp_bytes = W.done;
        W.sp_us = now_us();
    }
    unlock();
}

/* ================= 网页资源 =================
 * 单页应用：下载 / 上传两套面板，靠 MODE 常量切换。文案四套（英/简/繁台/日），
 * 繁中台港共用——网页上的用词差异不值得再分一份。
 * 样式只用系统字体与纯 CSS，不引外部资源（离线热点也能开）。 */

#define HTML_HEAD \
"<!doctype html><html><head><meta charset=\"utf-8\">" \
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">" \
"<title>PSVSend</title><style>" \
":root{color-scheme:dark;--bg:#14161a;--card:#20242b;--line:#333944;--tx:#eef1f6;" \
"--dim:#9aa4b2;--ac:#3d7ef0;--ok:#3ecf8e;--bad:#f0605d}" \
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);" \
"font:16px/1.45 system-ui,-apple-system,\"Segoe UI\",Roboto,\"Noto Sans SC\",sans-serif}" \
".wrap{max-width:720px;margin:0 auto;padding:20px 16px 48px}" \
"h1{font-size:20px;margin:0 0 2px}.sub{color:var(--dim);font-size:13px;margin:0 0 20px}" \
".card{background:var(--card);border:1px solid var(--line);border-radius:12px;" \
"padding:16px;margin:0 0 14px}" \
"label{display:block;font-size:13px;color:var(--dim);margin:0 0 8px}" \
"input[type=text]{width:100%;padding:12px 14px;border-radius:10px;" \
"border:1px solid var(--line);background:#14161a;color:var(--tx);font-size:20px;" \
"letter-spacing:6px;text-align:center}" \
"input[type=file]{width:100%;font-size:14px;color:var(--tx)}" \
"button{width:100%;margin-top:12px;padding:12px;border:0;border-radius:10px;" \
"background:var(--ac);color:#fff;font-size:16px;font-weight:600}" \
"button:disabled{opacity:.5}.msg{min-height:20px;font-size:13px;color:var(--dim);margin-top:10px}" \
".msg.err{color:var(--bad)}.msg.ok{color:var(--ok)}" \
"ul{list-style:none;margin:0;padding:0}" \
"li{display:flex;align-items:center;gap:12px;padding:12px 0;border-top:1px solid var(--line)}" \
"li:first-child{border-top:0}" \
".nm{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}" \
".sz{color:var(--dim);font-size:13px;white-space:nowrap}" \
"a.dl{padding:8px 14px;border-radius:8px;background:var(--ac);color:#fff;" \
"text-decoration:none;font-size:14px;white-space:nowrap}" \
".bar{height:8px;background:#14161a;border-radius:6px;overflow:hidden;margin-top:10px;" \
"display:none}" \
".bar i{display:block;height:100%;width:0;background:var(--ac);transition:width .2s}" \
"footer{margin-top:20px;font-size:12px;color:var(--dim);display:flex;" \
"justify-content:space-between;gap:12px}" \
".tip{font-size:12px;color:var(--dim);margin:14px 2px 0;line-height:1.5}" \
"select{background:#14161a;color:var(--tx);border:1px solid var(--line);" \
"border-radius:8px;padding:6px}" \
"</style></head><body><div class=\"wrap\">" \
"<h1>PSVSend</h1><p class=\"sub\" id=\"sub\"></p>" \
"<div class=\"msg\" id=\"msg\"></div>"

#define HTML_TAIL \
"<footer><span id=\"foot\"></span><select id=\"lang\">" \
"<option value=\"en\">English</option><option value=\"zh\">简体中文</option>" \
"<option value=\"tw\">繁體中文</option><option value=\"ja\">日本語</option>" \
"</select></footer></div><script>"

#define HTML_STRINGS \
"const T={" \
"en:{d:'Download from PS Vita',u:'Send files to PS Vita',pin:'Enter the 4-digit PIN shown on PS Vita'," \
"go:'Connect',get:'Get',wrong:'Wrong PIN',many:'Too many attempts, wait 30 seconds'," \
"off:'Sharing is not active',files:'Shared files',empty:'No files shared'," \
"pick:'Choose files (up to 64)',send:'Send',wait:'Waiting for confirmation on PS Vita…'," \
"reject:'Declined or timed out',busy:'PS Vita is busy, try again later',done:'All files sent'," \
"part:'Some files failed',trunc:'Only the first 64 files are sent'," \
"foot:'Plain HTTP · only the files selected on PS Vita'," \
"con:'Each file may use up to 16 connections. Download one or two at a time — three or more may briefly block this page.'}, " \
"zh:{d:'从 PS Vita 下载文件',u:'把文件发送到 PS Vita',pin:'输入 PS Vita 上显示的 4 位 PIN'," \
"go:'连接',get:'下载',wrong:'PIN 不正确',many:'尝试次数过多，请 30 秒后再试'," \
"off:'共享未开启',files:'共享文件',empty:'没有共享文件'," \
"pick:'选择文件（最多 64 个）',send:'发送',wait:'等待 PS Vita 确认…'," \
"reject:'已拒绝或超时',busy:'PS Vita 正忙，请稍后再试',done:'全部发送完成'," \
"part:'部分文件发送失败',trunc:'只发送前 64 个文件'," \
"foot:'明文 HTTP · 仅本次在 PS Vita 上选定的文件'," \
"con:'每个文件最多占用 16 条连接；建议一次下 1~2 个，同时下三个以上可能让本页短暂无响应。'}, " \
"tw:{d:'從 PS Vita 下載檔案',u:'把檔案傳送到 PS Vita',pin:'輸入 PS Vita 上顯示的 4 位 PIN'," \
"go:'連線',get:'下載',wrong:'PIN 不正確',many:'嘗試次數過多，請 30 秒後再試'," \
"off:'共享尚未開啟',files:'共享檔案',empty:'沒有共享檔案'," \
"pick:'選擇檔案（最多 64 個）',send:'傳送',wait:'等待 PS Vita 確認…'," \
"reject:'已拒絕或逾時',busy:'PS Vita 忙碌中，請稍後再試',done:'全部傳送完成'," \
"part:'部分檔案傳送失敗',trunc:'只傳送前 64 個檔案'," \
"foot:'明文 HTTP · 僅本次在 PS Vita 上選定的檔案'," \
"con:'每個檔案最多佔用 16 條連線；建議一次下 1~2 個，同時下三個以上可能讓本頁短暫無回應。'}, " \
"ja:{d:'PS Vita からダウンロード',u:'PS Vita にファイルを送る',pin:'PS Vita に表示された 4 桁の PIN を入力'," \
"go:'接続',get:'取得',wrong:'PIN が違います',many:'試行回数が多すぎます。30 秒後にお試しください'," \
"off:'共有は開始されていません',files:'共有ファイル',empty:'共有ファイルはありません'," \
"pick:'ファイルを選択（最大 64 個）',send:'送信',wait:'PS Vita の確認を待っています…'," \
"reject:'拒否されたかタイムアウトしました',busy:'PS Vita は使用中です。後で再試行してください'," \
"done:'すべて送信しました',part:'一部のファイルを送信できませんでした'," \
"trunc:'最初の 64 個のファイルだけ送信します'," \
"foot:'平文 HTTP · PS Vita で選んだファイルのみ'," \
"con:'1 ファイルにつき最大 16 接続を使用します。同時に 1〜2 個までがおすすめです（3 個以上では本ページが一時的に応答しなくなる場合があります）。'}};" \
"const L=(function(){var q=null;try{q=new URLSearchParams(location.search).get('lang');}" \
"catch(e){}if(q&&T[q])return q;var ls=navigator.languages||[navigator.language||'en'];" \
"for(var i=0;i<ls.length;i++){var s=(ls[i]||'').toLowerCase();" \
"if(s.indexOf('zh')===0)return (s.indexOf('tw')>=0||s.indexOf('hk')>=0||" \
"s.indexOf('hant')>=0)?'tw':'zh';if(s.indexOf('ja')===0)return 'ja';" \
"if(s.indexOf('en')===0)return 'en';}return 'en';})();" \
"var T0=T[L]||T.en;function $(id){return document.getElementById(id);}" \
"function msg(s,e){var m=$('msg');if(!m)return;m.textContent=s||'';" \
"m.className='msg'+(e===1?' err':e===2?' ok':'');}" \
"function fmt(n){if(n<1024)return n+' B';if(n<1048576)return (n/1024).toFixed(1)+' KB';" \
"if(n<1073741824)return (n/1048576).toFixed(1)+' MB';" \
"return (n/1073741824).toFixed(2)+' GB';}" \
"document.documentElement.lang=L;var sel=$('lang');sel.value=L;" \
"sel.onchange=function(){var u=new URL(location.href);u.searchParams.set('lang',sel.value);" \
"location.href=u.toString();};" \
"function beat(){var s='';try{s=sessionStorage.getItem('psvsend_sid')||'';}catch(e){}" \
"fetch('/heartbeat?sessionId='+encodeURIComponent(s)," \
"{method:'POST',cache:'no-store'}).catch(function(){});}" \
"setInterval(beat,2000);beat();" \
"document.addEventListener('visibilitychange',function(){if(!document.hidden)beat();});"

/* 下载面板的脚本：PIN → prepare-download → 文件列表 → 直链下载 */
#define HTML_DL_JS \
"var sid=sessionStorage.getItem('psvsend_sid')||'';" \
"function render(files){$('pinCard').style.display='none';$('listCard').style.display='';" \
"var ul=$('list');ul.innerHTML='';if(!files||!files.length){msg(T0.empty);return;}" \
"files.forEach(function(f){var li=document.createElement('li');" \
"var nm=document.createElement('span');nm.className='nm';nm.textContent=f.fileName;" \
"var sz=document.createElement('span');sz.className='sz';sz.textContent=fmt(f.size);" \
"var a=document.createElement('a');a.className='dl';a.textContent=T0.get;" \
"a.href='/api/localsend/v2/download?sessionId='+encodeURIComponent(sid)+" \
"'&fileId='+encodeURIComponent(f.id);li.append(nm,sz,a);ul.appendChild(li);});}" \
"async function connect(pin){msg('');var q=pin?'pin='+encodeURIComponent(pin):" \
"'sessionId='+encodeURIComponent(sid);if(!pin&&!sid)return;" \
"try{var r=await fetch('/api/localsend/v2/prepare-download?'+q,{method:'POST'});" \
"if(r.status===200){var j=await r.json();sid=j.sessionId;" \
"sessionStorage.setItem('psvsend_sid',sid);$('pinCard').style.display='none';render(j.files);}" \
"else if(r.status===401){msg(T0.wrong,1);sessionStorage.removeItem('psvsend_sid');sid='';}" \
"else if(r.status===429){msg(T0.many,1);}else{msg(T0.off,1);}" \
"}catch(e){msg(T0.off,1);}}" \
"$('go').onclick=function(){connect($('pin').value.replace(/\\D/g,''));};" \
"$('pin').onkeydown=function(e){if(e.key==='Enter')$('go').click();};" \
"$('sub').textContent=T0.d;$('pinLabel').textContent=T0.pin;$('go').textContent=T0.go;" \
"$('filesTitle').textContent=T0.files;$('foot').textContent=T0.foot;" \
"$('tip').textContent=T0.con;" \
"if(sid)connect('');"

/* 上传面板的脚本：PIN → prepare-upload（等 PSV 确认）→ 逐文件 upload */
#define HTML_UP_JS \
"var pin=sessionStorage.getItem('psvsend_pin')||'';" \
"function progress(a,b){var p=b>0?Math.round(a*100/b):0;$('bar').style.display='';" \
"$('bar').firstElementChild.style.width=p+'%';}" \
"async function send(){var fl=$('files').files;if(!fl.length)return;" \
"var arr=Array.prototype.slice.call(fl,0,64);" \
"var files={};arr.forEach(function(f,i){files['f'+i]={fileName:f.name,size:f.size};});" \
"var body=JSON.stringify({info:{alias:'Web browser',deviceModel:'Browser',deviceType:'web'," \
"fingerprint:'',port:0,protocol:'http',download:false},files:files});" \
"msg(T0.wait);$('send').disabled=true;" \
"try{var r=await fetch('/api/localsend/v2/prepare-upload?pin='+encodeURIComponent(pin)," \
"{method:'POST',headers:{'Content-Type':'application/json'},body:body});" \
"if(r.status===401){msg(T0.wrong,1);return;}if(r.status===429){msg(T0.many,1);return;}" \
"if(r.status===409){msg(T0.busy,1);return;}if(r.status!==200){msg(T0.reject,1);return;}" \
"var j=await r.json();var total=0;arr.forEach(function(f){total+=f.size;});" \
"var done=0,bad=0;for(var i=0;i<arr.length;i++){var tok=j.files&&j.files['f'+i];" \
"if(!tok)continue;var u='/api/localsend/v2/upload?sessionId='+encodeURIComponent(j.sessionId)+" \
"'&fileId=f'+i+'&token='+encodeURIComponent(tok);" \
"try{var ur=await fetch(u,{method:'POST',headers:{'Content-Type':'application/octet-stream'}," \
"body:arr[i]});if(ur.status!==200)bad++;}catch(e){bad++;}" \
"done+=arr[i].size;progress(done,total);if(bad)msg(T0.part,1);}" \
"msg(bad?T0.part:T0.done,bad?1:2);" \
"}catch(e){msg(T0.reject,1);}finally{$('send').disabled=false;}}" \
"$('sub').textContent=T0.u;$('pinLabel').textContent=T0.pin;$('pinGo').textContent=T0.go;" \
"$('pickLabel').textContent=T0.pick;$('send').textContent=T0.send;" \
"$('foot').textContent=T0.foot;" \
"$('pinGo').onclick=function(){pin=$('pin').value.replace(/\\D/g,'');" \
"if(pin.length!==4){msg(T0.wrong,1);return;}sessionStorage.setItem('psvsend_pin',pin);" \
"$('pinCard').style.display='none';$('upCard').style.display='';msg('');};" \
"$('pin').onkeydown=function(e){if(e.key==='Enter')$('pinGo').click();};" \
"$('send').onclick=send;" \
"if(pin.length===4){$('pinCard').style.display='none';$('upCard').style.display='';}"

#define HTML_DL_BODY \
"<div class=\"card\" id=\"pinCard\"><label id=\"pinLabel\" for=\"pin\"></label>" \
"<input type=\"text\" id=\"pin\" maxlength=\"4\" inputmode=\"numeric\" " \
"autocomplete=\"one-time-code\" placeholder=\"••••\">" \
"<button id=\"go\"></button></div>" \
"<div class=\"card\" id=\"listCard\" style=\"display:none\"><label id=\"filesTitle\"></label>" \
"<ul id=\"list\"></ul></div><p class=\"tip\" id=\"tip\"></p>"

#define HTML_UP_BODY \
"<div class=\"card\" id=\"pinCard\"><label id=\"pinLabel\" for=\"pin\"></label>" \
"<input type=\"text\" id=\"pin\" maxlength=\"4\" inputmode=\"numeric\" " \
"autocomplete=\"one-time-code\" placeholder=\"••••\">" \
"<button id=\"pinGo\"></button></div>" \
"<div class=\"card\" id=\"upCard\" style=\"display:none\">" \
"<label id=\"pickLabel\" for=\"files\"></label>" \
"<input type=\"file\" id=\"files\" multiple> <button id=\"send\"></button>" \
"<div class=\"bar\" id=\"bar\"><i></i></div></div>"

static const char s_page_dl[] =
    HTML_HEAD HTML_DL_BODY HTML_TAIL HTML_STRINGS HTML_DL_JS "</script></body></html>";
static const char s_page_up[] =
    HTML_HEAD HTML_UP_BODY HTML_TAIL HTML_STRINGS HTML_UP_JS "</script></body></html>";

/* 未开启共享时的 403 页（极简：说明状态 + 语言提示） */
static const char s_page_off[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>PSVSend</title><style>body{background:#14161a;color:#eef1f6;"
    "font:16px/1.5 system-ui,sans-serif;padding:32px 20px;text-align:center}"
    "h1{font-size:20px}p{color:#9aa4b2;font-size:14px}</style></head><body>"
    "<h1>PSVSend</h1><p>Web sharing is not active on PS Vita.<br>"
    "网页未开启：请在 PS Vita 上打开「网页共享」。</p></body></html>";

const char *web_page(int *len)
{
    int m = web_mode();
    const char *p = m == WEB_MODE_DOWNLOAD ? s_page_dl
                  : m == WEB_MODE_UPLOAD   ? s_page_up
                  : s_page_off;
    if (len) *len = (int)strlen(p);
    return p;
}
