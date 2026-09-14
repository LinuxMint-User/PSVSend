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
#define FONT_LAT_FILE   FONT_PATHS_BASE "DroidSans.ttf"
#define FONT_CJK_FILE   FONT_PATHS_BASE "DroidSansFallbackFull.ttf"
#define MAX_FONT_SIZES 24
static struct { int size; vita2d_font *lat; vita2d_font *cjk; } g_fs[MAX_FONT_SIZES];
static int g_fs_n;

/* ---------- 字体文件的一次性内存副本 ----------
 * libvita2d 默认用 FT_New_Face 开"文件 face"（app0:/fonts/*.ttf）。app0: 是
 * VPK 内的压缩文件系统，FreeType 每取一个**新**字形的轮廓都要在包内随机读
 * 一块并解压：实测 ~40ms/字形（d121 探针同字同号的对照：文件 face
 * 43.5ms/字形，内存 face 0.19ms/字形）。启动预热那 5.3s、以及切页时"该页新
 * 字越多越卡"，都出自这里；且成本与字号无关（轮廓数据量只跟字形复杂度有
 * 关），故砍字号档位、缩小标题字号都无效。
 * 机型是 UMA（无独立显存，无需把数据搬进显存），字体文件本身也不大，故启动
 * 时把两个 ttf 各读一份到 RAM，字体对象改从内存建：取轮廓退化为内存访问。
 * 注：vita2d_load_font_mem 只存指针不拷贝数据，这两个 buffer 必须常驻。 */
static void *g_font_ram[2];        /* [0]=latin, [1]=CJK */
static int   g_font_ram_len[2];

static void *font_ram(int cjk)
{
    const char *path = cjk ? FONT_CJK_FILE : FONT_LAT_FILE;
    SceUID fd;
    int sz, got = 0;
    long long t0;

    if (g_font_ram[cjk])
        return g_font_ram[cjk];
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
            g_font_ram[cjk] = buf;
            g_font_ram_len[cjk] = sz;
            dlog("font ram: %s %d bytes in %lldus", cjk ? "cjk" : "lat", sz,
                 (long long)sceKernelGetSystemTimeWide() - t0);
        }
    }
    sceIoClose(fd);
    return g_font_ram[cjk];
}

/* 返回 size 像素字号的字体：cjk=0 拉丁、1 CJK；加载失败返回 NULL。
 * 表满（UI 档位应远少于 MAX_FONT_SIZES）时回退 0 号槽。 */
vita2d_font *font_get(int size, int cjk)
{
    int i;
    for (i = 0; i < g_fs_n; i++)
        if (g_fs[i].size == size)
            return cjk ? g_fs[i].cjk : g_fs[i].lat;
    if (g_fs_n < MAX_FONT_SIZES) {
        vita2d_font *lat, *cj;
        font_ram(0);
        font_ram(1);        /* 首次进来时把两个 ttf 读进 RAM，之后命中缓存 */
        lat = g_font_ram[0]
            ? vita2d_load_font_mem(g_font_ram[0], (unsigned)g_font_ram_len[0])
            : NULL;
        cj = g_font_ram[1]
            ? vita2d_load_font_mem(g_font_ram[1], (unsigned)g_font_ram_len[1])
            : NULL;
        if (!lat || !cj)
            dlog("font load failed size=%d lat=%p cjk=%p", size,
                 (void *)lat, (void *)cj);
        g_fs[g_fs_n].size = size;
        g_fs[g_fs_n].lat  = lat;
        g_fs[g_fs_n].cjk  = cj;
        g_fs_n++;
        return cjk ? cj : lat;
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
    for (i = 0; i < FONT_SIZES_N; i++) {
        if (font_get(g_font_sizes[i], 0)) ok++;   /* latin */
        if (font_get(g_font_sizes[i], 1)) ok++;   /* CJK  */
    }
    dlog("font preload done: %d/%d fonts, sizes %d..%d", ok, 2 * FONT_SIZES_N,
         g_font_sizes[0], g_font_sizes[FONT_SIZES_N - 1]);
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
