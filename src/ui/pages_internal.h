/* pages 内部共享：布局常量 + 各页共用的小工具/跨页符号。
 * 仅供 src/ui/pages*.c 使用，不属于 ui.h 的公共页面接口。
 *
 * 拆分背景：原 pages.c 是单文件上帝模块，按"页面/职责"拆为
 *   pages.c            共享状态（进度行清单 xf_*）+ 开机初始化/预热
 *   pages_send.c       设备列表 + 文件浏览 + 发送确认
 *   pages_progress.c   进度页（发送/接收共用）
 *   pages_recv.c       接收确认/接收设置（含系统键盘改名）
 *   pages_settings.c   设置 + 目录选择
 * 各文件共享的常量与小工具集中于此（工具以 static inline 提供，避免额外符号）。 */
#ifndef PSVSEND_UI_PAGES_INTERNAL_H
#define PSVSEND_UI_PAGES_INTERNAL_H

#include <vita2d.h>
#include "ui/ui.h"

/* ---------- 布局常量 ---------- */
#define LIST_TOP     76                 /* 列表可视区顶 */
#define LIST_BOTTOM  (SCR_H - 46 - 6)   /* 列表可视区底（页脚上方留白） */
#define LIST_VIEW_H  (LIST_BOTTOM - LIST_TOP)
#define ROW_H        56
#define ROW_STRIDE   62
#define WID_FILES_DONE 0x8001    /* 文件选择页"完成"触摸按钮 */
#define WID_DIRPICK_SAVE 0x9001  /* 目录选择页"存到当前目录"触摸按钮 */
#define DIR_SUFFIX   "/"         /* 目录名后缀 */

/* ---------- 发送主页两栏布局 ----------
 * 主页 = 设备栏 + 已选文件栏并排（左右位置由 App.pane_swap 决定）；
 * 两栏共用一张 widget 命中表，故行 id 必须分区（每栏最多 512 行）。 */
#define PANE_TITLE_Y 78                       /* 栏标题行（其下 2px 焦点下划线） */
#define PANE_TOP     104                      /* 两栏列表可视区顶 */
#define PANE_BOTTOM  LIST_BOTTOM              /* 两栏列表可视区底（与整幅列表一致） */
#define PANE_VIEW_H  (PANE_BOTTOM - PANE_TOP)
#define PANE_GAP     16
#define PANE_X       24
#define PANE_W_DEV   430                      /* 设备栏宽（alias + 型号/类型，单行） */
#define PANE_W_FILE  (SCR_W - 2 * PANE_X - PANE_GAP - PANE_W_DEV)

#define WID_DEVS_BASE   0x1000                /* + 设备行下标 */
#define WID_PICKS_BASE  0x2000                /* + 已选文件行下标 */
#define WID_PICKX_BASE  0x3000                /* + 已选文件行尾"✕"下标 */
#define WID_FILES_PICK  0x8002                /* 空文件栏的"选择文件"按钮 */

/* ---------- 各页共用的小工具 ---------- */

/* 确认/返回键在当前布局下的提示名（随 X/O 布局即时变化） */
static inline const char *key_confirm(void) { return g_app.confirm_layout == 0 ? "X" : "O"; }
static inline const char *key_back(void)    { return g_app.confirm_layout == 0 ? "O" : "X"; }

static inline int list_max_scroll(int count)
{
    int m = count * ROW_STRIDE - LIST_VIEW_H;
    return m > 0 ? m : 0;
}

static inline void clamp_scroll(int *st, int count)
{
    int m = list_max_scroll(count);
    if (*st < 0) *st = 0;
    if (*st > m) *st = m;
}

/* 让选中行保持完整可见（键盘导航用；最小滚动量） */
static inline void keep_sel_visible(int *st, int count, int sel)
{
    int top = sel * ROW_STRIDE;
    int bot = top + ROW_H;
    if (top < *st) *st = top;
    if (bot > *st + LIST_VIEW_H) *st = bot - LIST_VIEW_H;
    clamp_scroll(st, count);
}

/* 触摸拖动：内容跟随手指；选中 = 手指压住的行（贴边时顺行移动） */
static inline void list_drag(const Input *in, int count, int *scroll, int *press, int *sel)
{
    int vis_top, vis_bot, row;
    if (in->drag_start) *press = *scroll;
    int ns = *press - in->drag_dy;      /* 手指下移(dy>0) → 内容下移 → 滚动减小 */
    clamp_scroll(&ns, count);
    *scroll = ns;
    vis_top = *scroll / ROW_STRIDE;
    vis_bot = (*scroll + LIST_VIEW_H) / ROW_STRIDE;
    row = (in->drag_y - LIST_TOP + *scroll) / ROW_STRIDE;
    if (row < vis_top) row = vis_top;
    if (row > vis_bot) row = vis_bot;
    if (row < 0) row = 0;
    if (row >= count) row = count - 1;
    *sel = row;
}

/* ---------- 带可视区的滚动工具（发送主页两栏用）----------
 * 与上面的整幅版本同理，但可视区（顶/高度）由调用方给出：主页两栏各自
 * 独立滚动，不能共用 LIST_TOP/LIST_VIEW_H。 */
static inline int area_max_scroll(int count, int view_h)
{
    int m = count * ROW_STRIDE - view_h;
    return m > 0 ? m : 0;
}

static inline void area_clamp(int *st, int count, int view_h)
{
    int m = area_max_scroll(count, view_h);
    if (*st < 0) *st = 0;
    if (*st > m) *st = m;
}

/* 让选中行保持完整可见（键盘导航用；最小滚动量） */
static inline void area_keep_visible(int *st, int count, int sel, int view_h)
{
    int top = sel * ROW_STRIDE;
    int bot = top + ROW_H;
    if (top < *st) *st = top;
    if (bot > *st + view_h) *st = bot - view_h;
    area_clamp(st, count, view_h);
}

/* 触摸拖动（可视区由参数给出）：内容跟随手指；选中 = 手指压住的行 */
static inline void area_drag(const Input *in, int count, int *scroll, int *press,
                             int *sel, int view_h)
{
    int vis_top, vis_bot, row;
    if (in->drag_start) *press = *scroll;
    int ns = *press - in->drag_dy;
    int m = area_max_scroll(count, view_h);
    if (ns < 0) ns = 0;
    if (ns > m) ns = m;
    *scroll = ns;
    vis_top = *scroll / ROW_STRIDE;
    vis_bot = (*scroll + view_h) / ROW_STRIDE;
    row = (in->drag_y - PANE_TOP + *scroll) / ROW_STRIDE;
    if (row < vis_top) row = vis_top;
    if (row > vis_bot) row = vis_bot;
    if (row < 0) row = 0;
    if (row >= count) row = count - 1;
    *sel = row;
}

/* 把一行注册成触摸区（越出可视区部分裁掉，避免误命中栏标题/页脚） */
static inline void area_row_hit(int id, int x, int w, int y)
{
    Rect h = { x, y, w, ROW_H };
    if (h.y < PANE_TOP) { h.h -= PANE_TOP - h.y; h.y = PANE_TOP; }
    if (h.y + h.h > PANE_BOTTOM) h.h = PANE_BOTTOM - h.y;
    if (h.h > 0) w_add(id, h);
}

/* 把一行注册成触摸区（可视区外部分裁掉，避免误命中头部/页脚） */
static inline void add_row_hit(int id, int top)
{
    Rect h = { 24, top, SCR_W - 48, ROW_H };
    if (h.y < LIST_TOP) { h.h -= LIST_TOP - h.y; h.y = LIST_TOP; }
    if (h.y + h.h > LIST_BOTTOM) h.h = LIST_BOTTOM - h.y;
    if (h.h > 0) w_add(id, h);
}

/* ---------- 跨页符号 ---------- */
void files_load(void);          /* pages_send.c：按 cur_dir 重载 g_app.files */
void enter_dir(const char *name);   /* pages_send.c：进入子目录并重载 */
void parent_dir(void);              /* pages_send.c：回上级目录（根则退回设备页） */
void pane_focus_default(void);      /* pages_send.c：按已选文件数定主页焦点栏（有文件→设备栏） */
void goto_devices(void);            /* pages_progress.c：回设备页并复位传输态（接收收尾/离开进度页用） */
void open_dir_pick(PageId origin, bool persist, const char *start_dir); /* pages_settings.c */
void open_color_pick(void);         /* pages_settings.c：进自定义色盘页（实时预览、确认落盘） */
void ask_ime_host(void);            /* pages_recv.c：打开系统键盘改本机设备名（设置页主机名行用） */
void settings_open(void);           /* pages_settings.c：进设置页（焦点回首项、滚动归顶） */

/* ---------- 跨页共享状态 ---------- */
/* 进度页当前文件清单：发送时每帧由 xfer 快照覆盖；接收由 start_recv 先以本地
 * 清单兜底首帧、再由 receive 会话快照覆盖（定义在 pages.c）。 */
extern int    xf_count;
extern char   xf_name[MAX_PICKED][192];
extern SceOff xf_size[MAX_PICKED];
extern int    xf_scroll;

#endif
