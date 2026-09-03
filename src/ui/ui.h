/* PSVSend UI 公共定义：App 状态、输入结构、控件接口 */
#ifndef PSVSEND_UI_UI_H
#define PSVSEND_UI_UI_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/types.h>

#define SCR_W 960
#define SCR_H 544

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

/* 底部提示用的按键图标 */
typedef enum {
    HICON_NONE = -1,
    HICON_CROSS,    /* X 键 */
    HICON_CIRCLE,   /* O 键 */
    HICON_TRIANGLE, /* 三角键 */
    HICON_SQUARE,   /* 方块键 */
    HICON_START,    /* START */
    HICON_DPAD,     /* 方向键（十字） */
    HICON_UP, HICON_DOWN, HICON_LEFT, HICON_RIGHT, /* 单个方向箭头 */
    HICON_COUNT
} HintIcon;

typedef struct { HintIcon icon; const char *text; } HintSeg;  /* text 可空：只画图标 */

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

#define MAX_INF 32
typedef struct {
    char  name[128];    /* 原始文件名 */
    char  rname[128];   /* 接收时的保存名（重命名功能 TODO，尚未实现） */
    bool  inc;          /* 勾选接收（默认勾上） */
    SceOff size;
} InFile;

typedef enum {
    PAGE_DEVICES,      /* 设备列表（主页面） */
    PAGE_FILES,        /* 文件浏览（发文件流程） */
    PAGE_SEND_CONFIRM, /* 发送确认 */
    PAGE_RECV_CONFIRM, /* 接收请求确认 */
    PAGE_RECV_SETUP,   /* 接收设置：本次保存目录 + 逐文件勾选/改名 */
    PAGE_PROGRESS,     /* 传输进度 */
    PAGE_SETTINGS,     /* 设置 */
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

    /* 文件浏览 */
    char    cur_dir[512];
    FsEntry files[MAX_FILES];
    int     file_count;
    int     file_sel;
    PickedFile picked[MAX_PICKED];
    int     picked_count;
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
    char    recv_dir[64];  /* 本次保存目录（暂固定默认值；目录选择 TODO） */
    bool    recv_cancel;   /* 发送方已取消请求 */

    /* 设置 */
    int     theme_id;      /* 0=Yaru 1=OLED */
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

/* ---------- 初始化（pages.c） ---------- */
void pages_init(void);

/* ---------- UI 模块入口（ui_main.c） ---------- */
void ui_run(void);

/* ---------- 页面绘制/输入（pages.c，每页 render 时注册控件） ---------- */
void page_devices_render(void);
void page_devices_input(const Input *in);
void page_files_render(void);
void page_files_input(const Input *in);
void page_send_confirm_render(void);
void page_send_confirm_input(const Input *in);
void page_recv_confirm_render(void);
void page_recv_confirm_input(const Input *in);
void page_recv_setup_render(void);
void page_recv_setup_input(const Input *in);
void page_progress_render(void);
void page_progress_input(const Input *in);
void page_settings_render(void);
void page_settings_input(const Input *in);

/* ---------- 控件绘制（widgets.c） ---------- */
void w_text(float x, float y, float scale, uint32_t color, const char *fmt, ...);
void w_text_w(float scale, const char *text, int *w, int *h);
void w_text_clip(float x, float y, float scale, uint32_t color,
                 const char *text, int max_w);
void w_rect(Rect r, uint32_t color);
void w_rect_outline(Rect r, uint32_t color);
void w_bar(Rect r, uint32_t bg, uint32_t fg, int pct);
void w_page_header(const char *title);
void w_page_footer(const char *hint);
void w_page_footer_segs(const HintSeg *segs, int n);   /* 图标 + 文字的按键提示条 */
void w_row(Rect r, const char *main_text, const char *sub_text, bool selected);
void w_button(Rect r, const char *label, bool active);
void w_modal_begin(int content_h);          /* 绘制遮罩 + 弹窗卡片，返回卡片区域置顶布局起点 */
Rect w_modal_box(int content_h);
void w_human_size(SceOff size, char *out);
void w_clear_picked(void);

#endif
