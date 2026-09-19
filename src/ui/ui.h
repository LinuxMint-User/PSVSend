/* PSVSend UI 公共定义：App 状态、输入结构、控件接口 */
#ifndef PSVSEND_UI_UI_H
#define PSVSEND_UI_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/types.h>

/* 字体句柄不透明类型（完整定义在 vita2d.h；控件层只传递指针） */
typedef struct vita2d_font vita2d_font;
/* 取 size 像素字号的字体对象（ui_main.c 按字号分槽懒加载；cjk=0/1） */
vita2d_font *font_get(int size, int cjk);

#define SCR_W 960
#define SCR_H 544

/* ---------- 布局常量（唯一来源）----------
 * 列表页的几何口径都从这里取：页头/页脚高、列表可视区、行高与步进、行内边距、
 * 行内字号档。页面不再各写一份数值；要调间距只改这里。 */
#define HDR_H         52            /* 页头高（底沿 2px 分隔线） */
#define FOOTER_H      46            /* 页脚提示条高 */
#define LIST_GAP_TOP  24            /* 页头下沿 → 列表顶 */
#define LIST_GAP_BOT  6             /* 列表底 → 页脚留白 */
#define LIST_TOP      (HDR_H + LIST_GAP_TOP)              /* 76 */
#define LIST_BOTTOM   (SCR_H - FOOTER_H - LIST_GAP_BOT)   /* 492 */
#define LIST_VIEW_H   (LIST_BOTTOM - LIST_TOP)

#define ROW_H         56            /* 行可见高 */
#define ROW_STRIDE    62            /* 行步进 */
#define ROW_PAD_X     24            /* 行内容左内边距 */
#define ROW_PAD_R     24            /* 行内容右内边距 */
#define ROW_GAP       12            /* 主文本与右端元素的最小间距 */

/* 行内字号档（scale；像素字号 = scale * 20，见 w_font_px） */
#define SC_TITLE      1.7f          /* 页头标题 */
#define SC_GROUP      1.05f         /* 分组标题 */
#define SC_MAIN       1.25f         /* 行主文本 */
#define SC_SUB        1.0f          /* 行副文本 / 右端值 */
#define SC_DENSE      1.1f          /* 密集行（进度页）主文本 */

/* ---------- 输入抽象：页面只处理动作，不判断物理键 ---------- */
typedef struct {
    bool confirm;   /* 确认（映射到 X 或 O，随布局设置） */
    bool back;      /* 返回（映射到另一键） */
    bool up, down, left, right;
    bool menu;      /* SELECT（打开设置） */
    bool alt;       /* TRIANGLE */
    bool square;    /* SQUARE（接收页：模拟发送方取消请求） */
    bool tap;       /* 触摸单击（按住即抬起、无明显移动；屏幕坐标） */
    int  tap_x, tap_y;
    bool drag_start;              /* 触摸判定为"滑动"的首帧 */
    bool dragging;                /* 滑动进行中（本帧仍按住） */
    int  drag_x0, drag_y0;        /* 滑动起始点（屏幕坐标） */
    int  drag_x,  drag_y;         /* 当前触摸点（屏幕坐标） */
    int  drag_dx, drag_dy;        /* 距起始点的累计位移 */
} Input;

/* ---------- 控件：注册命中区域供触摸 ---------- */
typedef struct { int x, y, w, h; } Rect;

/* 底部按键提示条（HintKey/HintSeg/w_page_footer_segs）见 ui/hintbar.h */
#include "ui/hintbar.h"

void w_clear(void);                  /* 每帧开始清空命中表 */
void w_add(int id, Rect r);          /* 页面绘制时注册可触摸区域 */
int  w_hit(int x, int y);            /* 命中返回 widget id，未命中 -1 */

/* ---------- 数据 ---------- */
#define MAX_DEVICES  32
#define MAX_FILES    512
#define MAX_PICKED   64

typedef struct {
    char  name[256];    /* 文件名 */
    bool  is_dir;
    SceOff size;
} FsEntry;

typedef struct {
    char  name[128];    /* 文件名 */
    char  path[512];    /* 完整路径 */
    SceOff size;
} PickedFile;

#define MAX_INF 64
typedef struct {
    char  name[192];    /* 名字（后端已规整：净化 + 超长保后缀截断，即落盘名） */
    char  rname[128];   /* 接收时的保存名：Setup 页系统键盘改名后非空，否则沿用原名 */
    bool  trunc;        /* 名字过长被缩短过（确认页提示用；用户改名后清掉） */
    bool  inc;          /* 勾选接收（默认勾上） */
    SceOff size;
} InFile;

typedef enum {
    PAGE_DEVICES,      /* 设备列表（主页面） */
    PAGE_FILES,        /* 文件浏览（发文件流程） */
    PAGE_SEND_WAIT,    /* 发送等待：等接收方接受；接受后自动切进度页 */
    PAGE_RECV_CONFIRM, /* 接收请求确认 */
    PAGE_RECV_SETUP,   /* 接收设置：本次保存目录 + 逐文件勾选/改名 */
    PAGE_DIR_PICK,     /* 目录选择（设置页默认保存目录 / 接收本次目录共用） */
    PAGE_PROGRESS,     /* 传输进度 */
    PAGE_SETTINGS,     /* 设置 */
    PAGE_COLOR_PICK,   /* 色盘：自定义主题选主色（设置页子页） */
    PAGE_COUNT
} PageId;

typedef struct {
    /* 页面 */
    PageId page;
    int    sel;         /* 通用选中 */

    /* 设备（发现快照：设备页渲染前从后端拷贝；下标当帧有效） */
    char   dev_alias[MAX_DEVICES][64];  /* 别名（列表主文本） */
    char   dev_sub[MAX_DEVICES][40];    /* 副文本：型号或平台类型 */
    char   dev_kind[MAX_DEVICES][16];   /* 平台类型 mobile/desktop/...（接收页展示） */
    char   dev_ip[MAX_DEVICES][16];     /* 设备 IP（发送流程目标） */
    int    dev_port[MAX_DEVICES];       /* 设备 HTTP 端口（发送流程目标） */
    char   dev_proto[MAX_DEVICES][8];   /* 设备协议 http/https（是否走 TLS） */
    char   dev_fp[MAX_DEVICES][96];     /* 设备指纹：https 时为证书 SHA-256（TLS pin） */
    int    dev_count;
    int    dev_sel;
    int    dev_target;                  /* 发送流程选择的目标（快照下标） */

    /* 发送主页两栏（设备栏 / 已选文件栏） */
    int    pane_focus;                  /* 焦点栏：0=设备 1=文件（与左右位置无关） */
    int    pane_swap;                   /* 布局：0=设备在左 1=文件在左（设置项） */

    /* 文件浏览 */
    char    cur_dir[512];
    FsEntry files[MAX_FILES];
    int     file_count;
    int     file_sel;
    PickedFile picked[MAX_PICKED];
    int     picked_count;
    int     picked_sel;                 /* 已选文件栏的选中行（picked 下标） */
    SceOff  picked_total;

    /* 传输（mock 进度） */
    int     prog_dir;      /* 0=发送 1=接收 */
    int     prog_pct;
    bool    prog_done;     /* 传输已完成 */
    bool    prog_cancel;   /* 用户已取消（页面停留显示"已取消"，按钮变 Done） */
    bool    prog_info;     /* 高级面板展开 */
    bool    prog_running;
    int     prog_ms;       /* 已用时长（毫秒，结束后冻结） */
    char    prog_name[128];
    SceOff  prog_size;
    uint64_t prog_start;   /* 微秒 */

    /* 接收请求（mock，多文件；发送方取消用方块键模拟） */
    char    recv_alias[64];
    char    recv_type[24];
    InFile  inc_files[MAX_INF];
    int     inc_count;
    int     inc_sel;       /* 接收设置页焦点：0=目录行，1..=文件行 */
    char    recv_dir[512]; /* 本次保存目录（新请求来时=config saveDir；Setup 页可临时改） */
    bool    recv_cancel;   /* 发送方已取消请求 */
    int     recv_overflow; /* 发送方文件数超出上限被丢弃的个数（确认页提示） */

    /* 设置 */
    int     theme_id;      /* 色系：0=Yaru 1=OLED */
    int     light_mode;    /* 明暗：0=深色 1=浅色（OLED 固定深色） */
    int     confirm_layout;/* 0=美式(X确认/O返回) 1=日式(O确认/X返回) */
    int     set_sel;

    bool    done;
} App;

extern App g_app;
extern int g_confirm_key;   /* 解析后当前确认键位（SCE_CTRL_*） */
extern int g_back_key;

/* ---------- 输入（input.c） ---------- */
void ui_input_init(void);
void ui_input_poll(Input *in);
/* 输入被冻结（系统键盘打开期间整段不轮询）后调用一次：把边沿状态对齐到
 * "此刻的真实按键/触摸"，避免解冻首帧凭空合成一次确认/返回或 tap。 */
void ui_input_resync(void);

/* ---------- 初始化（pages.c） ---------- */
void pages_init(void);
/* 每帧主循环调用：发送等待页→进度页的自动切换；发现新的"待决定接收请求"
 * 时自动弹接收确认页。 */
void pages_tick(void);

/* ---------- UI 模块入口（ui_main.c） ---------- */
void ui_run(void);

/* ---------- 页面绘制/输入（pages.c，每页 render 时注册控件） ---------- */
void page_devices_render(void);
void page_devices_input(const Input *in);
void page_files_render(void);
void page_files_input(const Input *in);
void page_send_wait_render(void);
void page_send_wait_input(const Input *in);
void page_recv_confirm_render(void);
void page_recv_confirm_input(const Input *in);
void page_recv_setup_render(void);
void page_recv_setup_input(const Input *in);
void page_dir_pick_render(void);
void page_dir_pick_input(const Input *in);
void page_progress_render(void);
void page_progress_input(const Input *in);
void page_settings_render(void);
void page_settings_input(const Input *in);
void page_color_pick_render(void);
void page_color_pick_input(const Input *in);

/* ---------- 系统键盘（IME）改名事务（pages.c + app/ime.c） ---------- */
bool page_ime_busy(void);   /* 改名事务进行中：主循环应跳过页面按键/触摸 */
void page_ime_pump(void);   /* 主循环每帧帧间调用：打开挂起键盘 / 收尾写回 */

/* ---------- 控件绘制（widgets.c） ---------- */
/* scale → 像素字号的唯一换算点（widgets.c 内定义 FONT_PX）；ui_main.c 用它
 * 由"UI 用到的 scale 列表"推出启动预加载的字号档，两边不会再各存一份。 */
int  w_font_px(float scale);
void w_text(float x, float y, float scale, uint32_t color, const char *fmt, ...);
void w_text_w(float scale, const char *text, int *w, int *h);
/* 中间省略绘制（放不下 max_w 时画"头…尾"，尾部保留以露出扩展名），
 * 返回实际绘制宽度 */
int  w_text_mid(float x, float y, float scale, uint32_t color,
                const char *text, int max_w);
void w_text_clip(float x, float y, float scale, uint32_t color,
                 const char *text, int max_w);
/* 右对齐绘制：文本右边缘落在 x_right（内部先测量再定位） */
void w_text_right(int x_right, float y, float scale, uint32_t color,
                  const char *fmt, ...);

/* ---------- 行几何（行型 → 锚点/字号）----------
 * 页面按行型取几何，再自己画内容；要偏离默认只改取回的字段（如
 * g.x_text = 20），不做第二套"默认表 + 覆写"。reserve_right = 右端要留出的
 * 宽度（右端元素宽 + 间距），主文本可用宽由它扣出。 */
typedef enum {
    ROW_VALUE,   /* 单行主文本 + 右端（文字/色块/✕/勾选） */
    ROW_2LINE,   /* 主上副下两行排 */
    ROW_DENSE    /* 单行 + 下方附属进度条（进度页） */
} RowKind;

typedef struct {
    int   x_text;    /* 内容左锚点 */
    int   x_right;   /* 右端元素右边缘（w_text_right 用） */
    int   w_text;    /* 主文本可用宽（已扣 reserve_right，不为负） */
    int   y_main;    /* 主文本 y（行首升部线语义） */
    int   y_sub;     /* 副文本 y；ROW_DENSE 时 = 附属进度条顶 */
    float main_sc;   /* 主文本字号 */
    float sub_sc;    /* 副文本 / 右端值字号 */
    int   row_h;     /* 行可见高（= 传入 r.h） */
} RowGeom;

void row_geom(Rect r, RowKind kind, int reserve_right, RowGeom *g);
void w_rect(Rect r, uint32_t color);
void w_rect_outline(Rect r, uint32_t color);
void w_bar(Rect r, uint32_t bg, uint32_t fg, int pct);
void w_page_header(const char *title);
void w_row(Rect r, const char *main_text, const char *sub_text, bool selected);
/* 同 w_row，但右值文本颜色指定（0=默认：选中 accent_text / 未选 text_dim） */
void w_row_c(Rect r, const char *main_text, const char *sub_text,
             uint32_t sub_c, bool selected);
void w_button(Rect r, const char *label, bool active);
void w_modal_begin(int content_h);          /* 绘制遮罩 + 弹窗卡片，返回卡片区域置顶布局起点 */
Rect w_modal_box(int content_h);
void w_human_size(SceOff size, char *out);

#endif
