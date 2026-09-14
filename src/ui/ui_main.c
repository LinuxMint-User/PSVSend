/* UI 主循环：渲染 + 输入 → 页面调度。
 * widget 命中表由 w_clear/w_add 在本帧渲染时填充，输入处理时查询。 */
#include <string.h>
#include <stdlib.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
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

static void *font_ram(int slot)
{
    const char *path = font_path(slot);
    SceUID fd;
    int sz, got = 0;
    long long t0;

    if (g_font_ram[slot])
        return g_font_ram[slot];
    fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        dlog("font ram: open failed %s", path);
        return NULL;
    }
    sz = (int)sceIoLseek(fd, 0, SCE_SEEK_END);
    sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz > 0) {
        void *buf = malloc((size_t)sz);
        t0 = (long long)sceKernelGetSystemTimeWide();
        if (buf)
            got = sceIoRead(fd, buf, (unsigned)sz);
        if (!buf || got != sz) {
            dlog("font ram: read failed %s size=%d got=%d", path, sz, got);
            free(buf);
        } else {
            g_font_ram[slot] = buf;
            g_font_ram_len[slot] = sz;
            dlog("font ram: slot%d %s %d bytes in %lldus", slot, path, sz,
                 (long long)sceKernelGetSystemTimeWide() - t0);
        }
    }
    sceIoClose(fd);
    return g_font_ram[slot];
}

static vita2d_font *load_slot(int slot)
{
    void *buf = font_ram(slot);
    return buf ? vita2d_load_font_mem(buf, (unsigned)g_font_ram_len[slot]) : NULL;
}

/* 返回 size 像素字号的字体：cjk=0 拉丁、1 CJK（地区随当前界面语言）；加载
 * 失败返回 NULL。表满（UI 档位应远少于 MAX_FONT_SIZES）时回退 0 号槽。
 * 注意：新槽一次建齐 lat+cjk 两份，故建槽可能在"要拉丁"的那次调用里发生——
 * CJK 槽必须按当前界面语言算，不能沿用该次调用的 cjk 参数（否则两种字体
 * 会被建成同一份）。 */
vita2d_font *font_get(int size, int cjk)
{
    int i;
    for (i = 0; i < g_fs_n; i++)
        if (g_fs[i].size == size)
            return cjk ? g_fs[i].cjk : g_fs[i].lat;
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
    return cjk ? g_fs[0].cjk : g_fs[0].lat;
}

/* UI 字号档位表 = 现有页面全部 w_text scale 经 font_px 取整的集合；
 * font_preload_all 启动建齐。日后新增字号档请同步补进此表。 */
static const int g_font_sizes[] = {
    16, 18, 20, 21, 22, 23, 24, 25, 26, 30, 34,
};
#define FONT_SIZES_N ((int)(sizeof g_font_sizes / sizeof g_font_sizes[0]))

/* 启动时帧外预加载全部字号档（见 ui_run 的调用点）。
 * 动机：字体对象是懒加载的，若某字号第一次被页面用到才创建，创建动作
 * （vita2d_load_font_mem：freetype 初始化 + 512x512 灰度纹理分配 + 显存
 * 映射）会落在渲染 pass 中途（start_drawing 与 end_drawing 之间）。GPU
 * 正异步执行上一批命令时 CPU 侧改显存管理状态，可触发 render GPU crash
 * （无 CPU 异常线程、纯 GPU 驱动报错，表现为撕裂后崩溃）。UI 字号档位
 * 有限，启动一次建齐后 font_get 运行时只命中缓存，此路径被整体消除。
 * 字形 glyph 仍按需光栅化写进已建好的 atlas（纯 CPU memcpy，不创建 GPU
 * 资源，无此风险），故无需也不应全量光栅化字形。 */
static void font_preload_all(void)
{
    int i, ok = 0;
    g_font_lang = i18n_lang();
    for (i = 0; i < FONT_SIZES_N; i++) {
        if (font_get(g_font_sizes[i], 0)) ok++;   /* latin */
        if (font_get(g_font_sizes[i], 1)) ok++;   /* CJK  */
    }
    dlog("font preload done: %d/%d fonts, sizes %d..%d, lang=%d cjk_slot=%d "
         "lat=%p cjk=%p", ok, 2 * FONT_SIZES_N, g_font_sizes[0],
         g_font_sizes[FONT_SIZES_N - 1], g_font_lang, cjk_slot_current(),
         (void *)g_fs[0].lat, (void *)g_fs[0].cjk);
}

/* 界面语言换了地区（简↔繁；日后含日文）时，把各字号的 CJK 字体对象换成对应
 * 地区那份：释放旧的、按新槽重建。必须帧外调用——vita2d_load_font_mem 会建
 * 512x512 显存纹理，落在渲染 pass 中途有 GPU crash 风险（见 font_preload_all
 * 注释）；ui_run 在每帧 start_drawing 之前检查一次。首次换到某地区会读盘
 * ~1.6s（16MB），属一次性开销。 */
static void font_reload_cjk(void)
{
    int i, slot;
    g_font_lang = i18n_lang();
    slot = cjk_slot_current();
    for (i = 0; i < g_fs_n; i++) {
        if (g_fs[i].cjk_slot == slot)
            continue;
        if (g_fs[i].cjk)
            vita2d_free_font(g_fs[i].cjk);
        g_fs[i].cjk = load_slot(slot);
        g_fs[i].cjk_slot = slot;
    }
    dlog("font cjk reload: lang=%d slot=%d", g_font_lang, slot);
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

        /* 设置页刚改过界面语言 → 帧外换 CJK 地区字体（见 font_reload_cjk） */
        if (g_font_lang != i18n_lang())
            font_reload_cjk();

        vita2d_start_drawing();
        vita2d_set_clear_color(theme->bg);
        vita2d_clear_screen();

        w_clear();
        render_page();

        if (!page_ime_busy()) {          /* 系统键盘打开期间按键/触摸归键盘，页面输入暂停 */
            Input in;
            ui_input_poll(&in);
            if (in.tap || in.up || in.down || in.left || in.right ||
                in.confirm || in.back || in.menu || in.alt || in.square ||
                in.drag_start || in.dragging)
                input_page(&in);
        }

        vita2d_end_drawing();
        /* 系统对话框（IME 键盘/消息框等）由应用每帧把 dialog 合成进显示缓冲，
         * 由 vita2d_common_dialog_update() 完成（内部自检有无 dialog 在运行，
         * 无则空转）。缺此调用时 dialog 引擎卡在 RUNNING、画面永不出现——
         * d55-d57 真机"卡死不弹键盘"根因。须在 swap 前、end_drawing 后调用。 */
        vita2d_common_dialog_update();
        vita2d_swap_buffers();
        vita2d_wait_rendering_done();  /* sceGxmFinish：等 GPU 本帧命令全部执行完再开下一帧。
                                        * 缺此调用时渲染/显示队列长期高速超前回绕，可出现画面
                                        * 撕裂进而 GPU render crash（跨版本偶发、撕裂先兆）。 */
        api_poke();          /* 断网活性刺激（d65 起非阻塞）：促使系统快速重连 Wi-Fi。
                              * 旧的阻塞实现在重连过渡态会卡住主循环约 2s（UI 连同
                              * 按键一起停摆），详见 net_poke / api_poke 注释。 */
    }
}
