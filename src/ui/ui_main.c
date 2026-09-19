/* UI 主循环：渲染 + 输入 → 页面调度。
 * widget 命中表由 w_clear/w_add 在本帧渲染时填充，输入处理时查询。 */
#include <string.h>
#include <stdlib.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/fcntl.h>
#include "ui/ui.h"
#include "ui/theme.h"
#include "app/api.h"
#include "app/update.h"
#include "core/dlog.h"
#include "core/i18n.h"

App g_app;
extern void pages_init(void);

/* ---------- 字体（按像素字号分槽，懒加载） ----------
 * libvita2d 的字体 atlas 按字形缓存、不分字号：同一字体对象若某字符
 * 先以小字号光栅化，之后大字号绘制会直接放大那份小位图（draw_scale
 * = size/缓存字号 > 1）。LINEAR 过滤会把 atlas 相邻槽的渗色随放大一起
 * 放大 → 大字顶/底出现横线；且同词中部分字是原生光栅化、部分字是
 * 放大/插值出来的，笔划粗细不一，视觉参差。对策：每个像素字号一个
 * 独立字体对象（独立 atlas），draw_scale 恒为 1，全部原生光栅化。
 * 首次用到某字号才 load；同字号 latin/cjk 各一份。 */
#define FONT_PATHS_BASE "app0:/fonts/"
#define FONT_LAT_FILE   FONT_PATHS_BASE "NotoSans-Regular.ttf"
#define FONT_SC_FILE    FONT_PATHS_BASE "NotoSansCJKsc-Regular.otf"
#define FONT_TC_FILE    FONT_PATHS_BASE "NotoSansCJKtc-Regular.otf"
#define FONT_JP_FILE    FONT_PATHS_BASE "NotoSansCJKjp-Regular.otf"
#define MAX_FONT_SIZES 24
static struct {
    int size;
    int cjk_slot;      /* 本项 cjk 字体建自哪个地区槽（见 cjk_slot_current） */
    vita2d_font *lat;
    vita2d_font *cjk;
} g_fs[MAX_FONT_SIZES];
static int g_fs_n;
static int g_font_lang = -1;   /* g_fs 的 cjk 字体是按哪个界面语言建的 */

/* ---------- 字体文件的一次性内存副本 ----------
 * libvita2d 默认用 FT_New_Face 开"文件 face"（app0:/fonts/*.ttf）。app0: 是
 * VPK 内的压缩文件系统，FreeType 每取一个**新**字形的轮廓都要在包内随机读
 * 一块并解压：实测 ~40ms/字形（d121 探针同字同号的对照：文件 face
 * 43.5ms/字形，内存 face 0.19ms/字形）。启动预热那 5.3s、以及切页时"该页新
 * 字越多越卡"，都出自这里；且成本与字号无关（轮廓数据量只跟字形复杂度有
 * 关），故砍字号档位、缩小标题字号都无效。
 * 机型是 UMA（无独立显存，无需把数据搬进显存），字体文件本身也不大，故启动
 * 时把字体文件各读一份到 RAM，字体对象改从内存建：取轮廓退化为内存访问。
 * 注：vita2d_load_font_mem 只存指针不拷贝数据，这些 buffer 必须常驻。
 *
 * 字体分四份：拉丁 NotoSans，CJK 用 Noto Sans CJK 的地区版本（简 sc / 繁 tc /
 * 日 jp）。各份都含全部 CJK 字形，差别只在"同一码位默认取哪个地区的字形"
 * （骨/道/直/繁…），故按当前界面语言只加载其中一份，换语言时才读另一份
 * （见 font_reload_cjk）。 */
static void *g_font_ram[4];        /* 0=拉丁 1=CJK简 2=CJK繁 3=CJK日 */
static int   g_font_ram_len[4];

/* 每槽读盘状态（g_font_ram[slot] 非 NULL ⇔ READY）。
 * 进设置页会起后台线程预读"另外两份" CJK，而主线程在页内切语言时可能同时要
 * 读同一份（切到槽序靠后的那份、线程还在读靠前的那份）。故用 CAS 抢读盘权：
 * 同一槽同时只有一个执行体在读——各读一遍会白占 16MB，且其中一份永远泄漏。 */
enum { FRAM_EMPTY = 0, FRAM_LOADING, FRAM_READY };
static volatile int g_fram[4];
static SceUID g_font_thid = -1;    /* 设置页后台预读线程（-1 = 没在跑） */
static volatile int g_font_stop;   /* 要求预读线程尽快退出（见 font_read 块间检查） */

static const char *font_path(int slot)
{
    switch (slot) {
    case 0:  return FONT_LAT_FILE;
    case 1:  return FONT_SC_FILE;
    case 2:  return FONT_TC_FILE;
    default: return FONT_JP_FILE;
    }
}

/* 当前界面语言对应的 CJK 地区槽（繁中台/港用 tc、日语用 jp，简体与英文用 sc） */
static int cjk_slot_current(void)
{
    int l = i18n_lang();
    if (l == I18N_LANG_ZH_TW || l == I18N_LANG_ZH_HK) return 2;
    if (l == I18N_LANG_JA) return 3;
    return 1;
}

/* 读盘块大小：块间查 g_font_stop。一份 16MB 要 ~1.6s，不查就没法及时退出设置页
 * （256KB ≈ 25ms 一块，即出页时最坏多等一块）。 */
#define FONT_RAM_CHUNK (256 * 1024)

/* 把一份字体文件整份读进 RAM：成功返回 buffer 并写 *out_len，失败/被中止返回
 * NULL。g_font_stop 只对预读线程有意义——主线程（切语言时同步读）调用时它恒为
 * 0，因为只有"出设置页"那一行会置起它，而那时主线程不会再读盘。 */
static void *font_read(const char *path, int *out_len)
{
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    void *buf;
    int sz, off = 0;

    if (fd < 0) {
        dlog("font ram: open failed %s", path);
        return NULL;
    }
    sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    buf = sz > 0 ? malloc((size_t)sz) : NULL;
    if (!buf) {
        dlog("font ram: malloc %d failed %s", sz, path);
        sceIoClose(fd);
        return NULL;
    }
    while (off < sz && !g_font_stop) {
        int n = sz - off;
        int r;
        if (n > FONT_RAM_CHUNK) n = FONT_RAM_CHUNK;
        r = sceIoRead(fd, (char *)buf + off, (unsigned)n);
        if (r <= 0) break;
        off += r;
    }
    sceIoClose(fd);
    if (off != sz) {              /* 读残 / 被叫停：整份作废，绝不留半份 buffer */
        dlog("font ram: %s got %d/%d, dropped", path, off, sz);
        free(buf);
        return NULL;
    }
    *out_len = sz;
    return buf;
}

/* 确保槽 slot 的字体副本已在 RAM 里。wait=1（主线程）：别的执行体正在读同一槽
 * 就等它读完；wait=0（预读线程）：直接放弃、交给对方。返回 1 = 已就绪。 */
static int font_ram_load(int slot, int wait)
{
    const char *path = font_path(slot);
    void *buf;
    int len = 0;
    long long t0;

    for (;;) {
        if (g_fram[slot] == FRAM_READY)
            return 1;
        if (__sync_bool_compare_and_swap((int *)&g_fram[slot], FRAM_EMPTY,
                                         FRAM_LOADING))
            break;                /* 抢到读盘权：由本执行体读并发布 */
        if (!wait)
            return 0;
        sceKernelDelayThread(2000);
    }
    t0 = (long long)sceKernelGetSystemTimeWide();
    buf = font_read(path, &len);
    if (buf) {
        g_font_ram[slot] = buf;   /* 整份成功才发布：读盘期间没人看得到它 */
        g_font_ram_len[slot] = len;
        __sync_synchronize();     /* 先发布数据，再公布 READY */
        g_fram[slot] = FRAM_READY;
        dlog("font ram: slot%d %s %d bytes in %lldus", slot, path, len,
             (long long)sceKernelGetSystemTimeWide() - t0);
        return 1;
    }
    g_fram[slot] = FRAM_EMPTY;
    return 0;
}

/* 取 slot 的字体副本（常驻，不释放）；主线程路径：已有就直接用，别处正在读就等
 * 它读完，否则自己读。 */
static void *font_ram(int slot)
{
    return font_ram_load(slot, 1) ? g_font_ram[slot] : NULL;
}

static vita2d_font *load_slot(int slot)
{
    void *buf = font_ram(slot);
    return buf ? vita2d_load_font_mem(buf, (unsigned)g_font_ram_len[slot]) : NULL;
}

/* 渲染 pass 中（vita2d_start_drawing..end_drawing 之间）置 1：此间禁止新建
 * 字体槽（见 font_get 的守卫），因为建槽会分配 512² 显存纹理，落在渲染 pass
 * 中途可触发 GPU crash 整机重启（见 font_preload_all 注释）。 */
static int g_in_frame;
static int g_font_warned;      /* 缺档/表满告警已打印次数（防 dlog 刷屏） */

/* 已建槽中与 size 最接近的字体（无槽则 NULL）。用于"帧内不许建槽/表满"时的
 * 降级：字号略有出入，但不会崩、也不丢字。 */
static vita2d_font *font_nearest(int size, int cjk)
{
    int i, best = -1, bd = 0;
    for (i = 0; i < g_fs_n; i++) {
        int d = g_fs[i].size > size ? g_fs[i].size - size : size - g_fs[i].size;
        if (best < 0 || d < bd) { best = i; bd = d; }
    }
    if (best < 0) return NULL;
    return cjk ? g_fs[best].cjk : g_fs[best].lat;
}

/* 返回 size 像素字号的字体：cjk=0 拉丁、1 CJK（地区随当前界面语言）；加载
 * 失败返回 NULL。
 * 注意：新槽一次建齐 lat+cjk 两份，故建槽可能在"要拉丁"的那次调用里发生——
 * CJK 槽必须按当前界面语言算，不能沿用该次调用的 cjk 参数（否则两种字体
 * 会被建成同一份）。 */
vita2d_font *font_get(int size, int cjk)
{
    int i;
    for (i = 0; i < g_fs_n; i++)
        if (g_fs[i].size == size)
            return cjk ? g_fs[i].cjk : g_fs[i].lat;
    /* 帧内禁建槽：UI 字号档本应在启动时按 UI_FONT_SCALES 建齐（font_preload_all），
     * 走到这里说明该字号漏在表外（页面新加了字号档没补表）。此时宁可降级到最接近
     * 的已建档，也不能在建槽时触发 GPU crash。 */
    if (g_in_frame) {
        if (g_font_warned < 8) {
            g_font_warned++;
            dlog("font: size %d requested in-frame but not preloaded -> degrade",
                 size);
        }
        return font_nearest(size, cjk);
    }
    if (g_fs_n < MAX_FONT_SIZES) {
        int slot = cjk_slot_current();
        g_fs[g_fs_n].size     = size;
        g_fs[g_fs_n].cjk_slot = slot;
        g_fs[g_fs_n].lat      = load_slot(0);
        g_fs[g_fs_n].cjk      = load_slot(slot);
        if (!g_fs[g_fs_n].lat || !g_fs[g_fs_n].cjk)
            dlog("font load failed size=%d lat=%p cjk=%p", size,
                 (void *)g_fs[g_fs_n].lat, (void *)g_fs[g_fs_n].cjk);
        g_fs_n++;
        return cjk ? g_fs[g_fs_n - 1].cjk : g_fs[g_fs_n - 1].lat;
    }
    if (g_font_warned < 8) {
        g_font_warned++;
        dlog("font: slot table full (%d), size %d -> degrade", MAX_FONT_SIZES, size);
    }
    return font_nearest(size, cjk);
}

/* UI 字号档 = 各页面 w_text / w_text_w / w_text_mid / w_text_clip 的 scale 取值
 * 集合，这里是**唯一真源**（scale→px 的换算只走 w_font_px，字号档由它生成，
 * 不再手工维护一张 px 表）。页面新增字号档时补进本列表即可；漏补不会崩——
 * font_get 的帧内守卫会拦下建槽并降级，同时 dlog 报出漏掉的字号。 */
#define UI_FONT_SCALES(X)                                                    \
    X(0.8f) X(0.9f) X(1.0f) X(1.05f) X(1.1f) X(1.15f) X(1.2f) X(1.25f)      \
    X(1.3f) X(1.5f) X(1.7f)
#define SCALE_ENTRY(s) s,
static const float g_font_scales[] = { UI_FONT_SCALES(SCALE_ENTRY) };
#define FONT_SCALES_N ((int)(sizeof g_font_scales / sizeof g_font_scales[0]))

/* 启动时帧外预加载全部字号档（见 ui_run 的调用点）。
 * 动机：字体对象是懒加载的，若某字号第一次被页面用到才创建，创建动作
 * （vita2d_load_font_mem：freetype 初始化 + 512x512 灰度纹理分配 + 显存
 * 映射）会落在渲染 pass 中途（start_drawing 与 end_drawing 之间）。GPU
 * 正异步执行上一批命令时 CPU 侧改显存管理状态，可触发 render GPU crash
 * （无 CPU 异常线程、纯 GPU 驱动报错，表现为撕裂后崩溃）。UI 字号档位
 * 有限，启动一次建齐后 font_get 运行时只命中缓存，此路径被整体消除；
 * font_get 另有帧内禁建槽守卫兜住"表漏了某档"的情况。
 * 字形 glyph 仍按需光栅化写进已建好的 atlas（纯 CPU memcpy，不创建 GPU
 * 资源，无此风险），故无需也不应全量光栅化字形。 */
static void font_preload_all(void)
{
    int i, ok = 0, sz;
    g_font_lang = i18n_lang();
    for (i = 0; i < FONT_SCALES_N; i++) {
        sz = w_font_px(g_font_scales[i]);
        if (font_get(sz, 0)) ok++;   /* latin */
        if (font_get(sz, 1)) ok++;   /* CJK  */
    }
    dlog("font preload done: %d/%d fonts, sizes %d..%d, lang=%d cjk_slot=%d "
         "lat=%p cjk=%p", ok, 2 * FONT_SCALES_N,
         w_font_px(g_font_scales[0]), w_font_px(g_font_scales[FONT_SCALES_N - 1]),
         g_font_lang, cjk_slot_current(), (void *)g_fs[0].lat, (void *)g_fs[0].cjk);
}

/* 界面语言换了地区（简↔繁↔日）时，把各字号的 CJK 字体对象换成对应地区那份：
 * 先建新的、成功后再释放旧的。必须帧外调用——vita2d_load_font_mem 会建 512x512
 * 显存纹理，落在渲染 pass 中途有 GPU crash 风险（见 font_preload_all 注释）；
 * ui_run 在每帧 start_drawing 之前检查一次。首次换到某地区会读盘 ~1.6s
 * （16MB），属一次性开销。
 * 失败处理：先删后建的老写法一旦读盘/建对象失败，该字号的 CJK 就永久为 NULL
 * （整屏汉字消失）且 cjk_slot 已记新区、后续帧不再重试。现在建失败就保留旧
 * 字体继续显示（字形地区不对，但汉字还在），且不推进 g_font_lang → 下一帧
 * 自动重试；连续失败 FONT_RELOAD_MAX 次才放弃（避免每帧都去读 16MB 卡住）。 */
#define FONT_RELOAD_MAX 3
static void font_reload_cjk(void)
{
    static int fails;
    int i, slot, ok = 1;
    slot = cjk_slot_current();
    for (i = 0; i < g_fs_n; i++) {
        vita2d_font *nf;
        if (g_fs[i].cjk_slot == slot)
            continue;
        nf = load_slot(slot);
        if (!nf) {                /* 建失败：保留旧对象（内容将就但可见） */
            ok = 0;
            continue;
        }
        if (g_fs[i].cjk)
            vita2d_free_font(g_fs[i].cjk);
        g_fs[i].cjk = nf;
        g_fs[i].cjk_slot = slot;
    }
    if (ok) {
        fails = 0;
        g_font_lang = i18n_lang();
    } else if (++fails >= FONT_RELOAD_MAX) {
        fails = 0;
        g_font_lang = i18n_lang();   /* 放弃：不再每帧重试，等下次换语言再说 */
    }
    dlog("font cjk reload: lang=%d slot=%d ok=%d fails=%d", g_font_lang, slot, ok,
         fails);
}

/* ---------- 设置页字体预读（进页起线程，出页停并释放） ----------
 * 设置页里可以切界面语言，切到还没读过的那一份要现读 16MB ≈ 1.6s，主线程冻结。
 * 故一进设置页就后台把"另外两份 CJK"读进 RAM（低优先级线程，不挡主线程）；出页
 * 只保留拉丁 + 当前语言那份，其余 free——页内峰值 ≈ 0.57 + 16.44×3 ≈ 49.9MB，是
 * 全项目最大一笔运行期内存，不在页外常驻（页外与从前一致 ≈ 17MB）。
 * ★ 进出用显式钩子，不能按"页边沿"判断：色盘页、目录选择页退出时会把
 *   g_app.page 再设回 PAGE_SETTINGS（pages_settings.c），按页判断会误判成"离开
 *   设置页"→ 掐掉预读、回来重读。 */
static int font_preload_thr(SceSize args, void *argp)
{
    int slot;
    (void)args;
    (void)argp;
    for (slot = 1; slot < 4; slot++) {
        if (g_font_stop)
            break;
        if (slot == cjk_slot_current() || g_font_ram[slot])
            continue;                 /* 当前语言那份常驻不动 */
        if (!font_ram_load(slot, 0))  /* 抢不到/失败：交给切语言时的同步读兜底 */
            dlog("font preload: slot%d skipped", slot);
    }
    dlog("font preload: thread exit stop=%d", g_font_stop);
    return 0;
}

/* 还有字体对象指着这一份副本吗（正常没有——切语言时对象已整体换到新地区那份；
 * 只有 font_reload_cjk 建新对象失败、保留旧地区对象时才为真）。 */
static int font_slot_referenced(int slot)
{
    int i;
    for (i = 0; i < g_fs_n; i++)
        if (g_fs[i].cjk && g_fs[i].cjk_slot == slot)
            return 1;
    return 0;
}

/* 进设置页：起后台预读（无预读线程时才起） */
void ui_font_preload_begin(void)
{
    if (g_font_thid >= 0)
        return;
    g_font_stop = 0;
    g_font_thid = sceKernelCreateThread("psvsend_fontram", font_preload_thr,
                                        0x50, 0x8000, 0, 0, NULL);
    if (g_font_thid < 0) {
        dlog("font preload: create failed 0x%08X", (unsigned)g_font_thid);
        g_font_thid = -1;
        return;
    }
    if (sceKernelStartThread(g_font_thid, 0, NULL) < 0) {
        dlog("font preload: start failed");
        sceKernelDeleteThread(g_font_thid);
        g_font_thid = -1;
        return;
    }
    dlog("font preload: started lang=%d cur_slot=%d", i18n_lang(),
         cjk_slot_current());
}

/* 出设置页：停预读线程，并释放"非当前语言"的 CJK 副本 */
void ui_font_preload_end(void)
{
    int i, slot;

    if (g_font_thid >= 0) {
        int stopped = 0;
        g_font_stop = 1;              /* 线程每块（256KB）查一次 → 最坏多等一块 */
        for (i = 0; i < 400; i++) {   /* 上限 2s：卡住也不把 UI 拖死 */
            SceUInt to = 5000;
            if (sceKernelWaitThreadEnd(g_font_thid, NULL, &to) == 0) {
                stopped = 1;
                break;
            }
        }
        if (stopped) {
            sceKernelDeleteThread(g_font_thid);
            g_font_stop = 0;
        } else {
            /* 没在 2s 内退出（不该发生）：保持停止标志让它自己尽快收，句柄交给
             * 系统——硬删运行中的线程会让它手里的 buffer 悬空。 */
            dlog("font preload: thread won't stop, handle leaked");
        }
        g_font_thid = -1;
    }
    for (slot = 1; slot < 4; slot++) {
        if (!g_font_ram[slot] || slot == cjk_slot_current())
            continue;                 /* 拉丁与当前语言那份常驻 */
        /* 兜底：对象还指着它就不能 free（见 font_slot_referenced），宁可多占一份 */
        if (font_slot_referenced(slot))
            continue;
        free(g_font_ram[slot]);
        g_font_ram[slot] = NULL;
        g_font_ram_len[slot] = 0;
        g_fram[slot] = FRAM_EMPTY;
        dlog("font ram: slot%d freed", slot);
    }
}

/* ---------- widget 命中表 ---------- */
#define MAX_W 96
static Rect g_widgets[MAX_W];
static int  g_ids[MAX_W];
static int  g_wcount;

void w_clear(void) { g_wcount = 0; }

void w_add(int id, Rect r)
{
    if (g_wcount < MAX_W) {
        g_ids[g_wcount] = id;
        g_widgets[g_wcount] = r;
        g_wcount++;
        return;
    }
    /* 表满：多出来的区域被丢掉，表现为"这块点不动"却又查不出原因。同一页只报
     * 一次（列表页每帧都会注册，逐帧刷屏会把日志冲没）。 */
    static int warned_page = -1;
    if (g_app.page != warned_page) {
        warned_page = g_app.page;
        dlog("w_add: hit table full (%d), drop id=%d rect=%d,%d,%d,%d page=%d",
             MAX_W, id, r.x, r.y, r.w, r.h, (int)g_app.page);
    }
}

int w_hit(int x, int y)
{
    int i;
    for (i = g_wcount - 1; i >= 0; i--) {
        Rect *r = &g_widgets[i];
        if (x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h)
            return g_ids[i];
    }
    return -1;
}

/* ---------- 页面调度 ---------- */
static void render_page(void)
{
    switch (g_app.page) {
    case PAGE_DEVICES:       page_devices_render(); break;
    case PAGE_FILES:         page_files_render(); break;
    case PAGE_SEND_WAIT:     page_send_wait_render(); break;
    case PAGE_RECV_CONFIRM:  page_recv_confirm_render(); break;
    case PAGE_RECV_SETUP:    page_recv_setup_render(); break;
    case PAGE_DIR_PICK:      page_dir_pick_render(); break;
    case PAGE_PROGRESS:      page_progress_render(); break;
    case PAGE_SETTINGS:      page_settings_render(); break;
    case PAGE_COLOR_PICK:    page_color_pick_render(); break;
    default: break;
    }
}

static void input_page(const Input *in)
{
    switch (g_app.page) {
    case PAGE_DEVICES:       page_devices_input(in); break;
    case PAGE_FILES:         page_files_input(in); break;
    case PAGE_SEND_WAIT:     page_send_wait_input(in); break;
    case PAGE_RECV_CONFIRM:  page_recv_confirm_input(in); break;
    case PAGE_RECV_SETUP:    page_recv_setup_input(in); break;
    case PAGE_DIR_PICK:      page_dir_pick_input(in); break;
    case PAGE_PROGRESS:      page_progress_input(in); break;
    case PAGE_SETTINGS:      page_settings_input(in); break;
    case PAGE_COLOR_PICK:    page_color_pick_input(in); break;
    default: break;
    }
}

/* ---------- 主循环 ---------- */
void ui_run(void)
{
    static uint64_t last_be = 0;
    static bool ime_was_busy;      /* 上一帧键盘是否打开（用于解冻时复位输入边沿） */
    uint64_t run0 = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
    ui_input_init();           /* 开启触摸采样 */
    pages_init();
    update_init();             /* 更新检查状态机（自动周期由 config 决定） */
    font_preload_all();        /* 建齐字号字体对象（帧外；首次触发 ttf 读进 RAM） */

    while (!g_app.done) {
        /* 主线程心跳（诊断用）：开机头 12s 或"网络未就绪"期间每秒打一行，
         * 记录主循环存活及它读到的后端状态（disc 启动与否、链路缓存、IP）。
         * 定位"后端已 up 但 UI 卡 not ready"类问题：心跳持续 = 主线程活着、
         * 状态机问题；心跳停 = 主线程卡死在某帧路径。就绪后自动静默。 */
        {
            uint64_t bn = (uint64_t)sceKernelGetSystemTimeWide() / 1000;
            if (bn - last_be >= 1000 &&
                (bn - run0 < 12000 || !api_network_ready() || !api_link_up())) {
                last_be = bn;
                dlog("ui: beat disc=%d ctl=%d up=%d ip=%s",
                     api_discovery_state(), api_ctl_state(),
                     api_link_up() ? 1 : 0, api_local_ip());
            }
        }
        api_tick();          /* announce 节奏（500ms 节流；sendto 只在主线程可靠） */
        pages_tick();        /* 检测新到待决定的接收请求 → 弹接收确认页 */
        update_tick();       /* 自动检查更新：到周期且网络就绪时后台触发 */
        page_ime_pump();     /* 系统键盘改名事务：帧间打开挂起键盘 / 轮询收尾
                              * （须非绘制中调用 + 打开期间持续出帧，见 ime.c d56） */

        vita2d_start_drawing();
        g_in_frame = 1;              /* 此间 font_get 禁止新建字体槽（GPU crash 风险） */
        vita2d_set_clear_color(theme->bg);
        vita2d_clear_screen();

        w_clear();
        render_page();

        if (!page_ime_busy()) {          /* 系统键盘打开期间按键/触摸归键盘，页面输入暂停 */
            Input in;
            if (ime_was_busy)
                ui_input_resync();       /* 冻结期结束：先对齐边沿，别把"还在按着的键"当新按下 */
            ui_input_poll(&in);
            if (in.tap || in.up || in.down || in.left || in.right ||
                in.confirm || in.back || in.menu || in.alt || in.alt_long ||
                in.square || in.drag_start || in.dragging)
                input_page(&in);
        }
        ime_was_busy = page_ime_busy();

        vita2d_end_drawing();
        g_in_frame = 0;
        /* 系统对话框（IME 键盘/消息框等）由应用每帧把 dialog 合成进显示缓冲，
         * 由 vita2d_common_dialog_update() 完成（内部自检有无 dialog 在运行，
         * 无则空转）。缺此调用时 dialog 引擎卡在 RUNNING、画面永不出现——
         * d55-d57 真机"卡死不弹键盘"根因。须在 swap 前、end_drawing 后调用。 */
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        vita2d_wait_rendering_done();  /* sceGxmFinish：等 GPU 本帧命令全部执行完再开下一帧。
                                        * 缺此调用时渲染/显示队列长期高速超前回绕，可出现画面
                                        * 撕裂进而 GPU render crash（跨版本偶发、撕裂先兆）。 */
        /* 设置页刚改过界面语言 → 帧外换 CJK 地区字体（见 font_reload_cjk）。放在
         * swap 之后：本帧已按新语言渲染完毕，换字体等待期间屏上是新语言界面，
         * 而不是停在旧语言。须在渲染 pass 外（建字体对象会分配显存纹理）。 */
        if (g_font_lang != i18n_lang())
            font_reload_cjk();
        api_poke();          /* 断网活性刺激（d65 起非阻塞）：促使系统快速重连 Wi-Fi。
                              * 旧的阻塞实现在重连过渡态会卡住主循环约 2s（UI 连同
                              * 按键一起停摆），详见 net_poke / api_poke 注释。 */
    }
}
