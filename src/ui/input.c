/* 输入抽象：SceCtrl(按键) + SceTouch(触摸) → 统一动作。
 * 页面只消费 Input 动作；确认键 X/O 随布局设置切换。
 * 按键均为"沿触发"（按下瞬间一次）；方向键按住 300ms 后自动重复。 */
#include <string.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/kernel/threadmgr/thread.h>
#include "ui.h"

int g_confirm_key = SCE_CTRL_CROSS;
int g_back_key    = SCE_CTRL_CIRCLE;

static uint32_t prev_buttons = 0;

/* 方向键长按重复状态（up/down/left/right） */
static uint64_t rep_press[4];   /* 按下时刻（微秒） */
static uint64_t rep_last[4];    /* 上次触发时刻 */

#define REP_DELAY_US   300000   /* 长按 300ms 开始重复 */
#define REP_INTERVAL_US 80000   /* 之后每 80ms 一次 */

/* 触摸拖动状态机：
 *   按下            → 记录起点
 *   位移 < 阈值 抬起 → tap（单击）
 *   位移 >= 阈值     → 进入拖动，此后每帧发 drag_start/dragging + 累计位移
 * 拖动中抬起不会产生 tap（避免滑动误触）。 */
#define TOUCH_DRAG_THRESHOLD 24   /* 像素，超过才算滑动 */

static bool   t_down;            /* 本帧是否有手指按住 */
static bool   t_drag;            /* 当前按住的手是否已判定为滑动 */
static int    t_x0, t_y0;        /* 按下起点（屏幕坐标） */
static int    t_lastx, t_lasty;  /* 最近一次触摸点 */
static bool   t_have_last;       /* 是否已有坐标可参考（抬起时用于距离判断） */

static void update_keys(void)
{
    if (g_app.confirm_layout == 0) {   /* 美式: X=确认 O=返回 */
        g_confirm_key = SCE_CTRL_CROSS;
        g_back_key    = SCE_CTRL_CIRCLE;
    } else {                            /* 日式: O=确认 X=返回 */
        g_confirm_key = SCE_CTRL_CIRCLE;
        g_back_key    = SCE_CTRL_CROSS;
    }
}

/* 方向键事件：按下沿触发一次；长按到重复期后按间隔持续触发 */
static bool dir_event(uint32_t cur, uint32_t key, int idx, uint64_t now)
{
    if (!(cur & key)) return false;
    if (!(prev_buttons & key)) {
        rep_press[idx] = now;
        rep_last[idx] = now;
        return true;
    }
    if (now - rep_press[idx] >= REP_DELAY_US &&
        now - rep_last[idx] >= REP_INTERVAL_US) {
        rep_last[idx] = now;
        return true;
    }
    return false;
}

static bool touch_to_screen(int px, int py, int *sx, int *sy)
{
    /* 面板坐标 → 屏幕坐标：优先按面板信息缩放 */
    SceTouchPanelInfo p;
    if (sceTouchGetPanelInfo(SCE_TOUCH_PORT_FRONT, &p) >= 0 &&
        p.maxDispX > p.minDispX) {
        *sx = (px - p.minDispX) * SCR_W / (p.maxDispX - p.minDispX + 1);
        *sy = (py - p.minDispY) * SCR_H / (p.maxDispY - p.minDispY + 1);
        return true;
    }
    *sx = px / 2;   /* 面板 1920x1088 的兜底换算 */
    *sy = py / 2;
    return false;
}

static void touch_poll(Input *in, SceTouchData *touch)
{
    bool down = touch->reportNum > 0;
    int cx = 0, cy = 0;

    if (down)
        touch_to_screen(touch->report[0].x, touch->report[0].y, &cx, &cy);

    if (down) {
        if (!t_down) {
            /* 手指刚按下 */
            t_x0 = cx; t_y0 = cy;
            t_lastx = cx; t_lasty = cy;
            t_have_last = true;
            t_drag = false;
        } else {
            t_lastx = cx; t_lasty = cy;
            if (!t_drag) {
                int dx = cx - t_x0, dy = cy - t_y0;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (dx >= TOUCH_DRAG_THRESHOLD || dy >= TOUCH_DRAG_THRESHOLD) {
                    t_drag = true;
                    in->drag_start = true;
                    in->drag_x0 = t_x0; in->drag_y0 = t_y0;
                    in->drag_x = cx;   in->drag_y = cy;
                    in->drag_dx = cx - t_x0; in->drag_dy = cy - t_y0;
                }
            }
            if (t_drag) {
                in->dragging = true;
                in->drag_x = cx;   in->drag_y = cy;
                in->drag_dx = cx - t_x0; in->drag_dy = cy - t_y0;
            }
        }
    } else if (t_down) {
        /* 手指抬起 */
        if (!t_drag) {
            int ex = t_have_last ? t_lastx : t_x0;
            int ey = t_have_last ? t_lasty : t_y0;
            int dx = ex - t_x0, dy = ey - t_y0;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx < TOUCH_DRAG_THRESHOLD && dy < TOUCH_DRAG_THRESHOLD) {
                in->tap = true;          /* 单击：按起点坐标 */
                in->tap_x = t_x0;
                in->tap_y = t_y0;
            }
        }
        t_down = false;
        t_drag = false;
        t_have_last = false;
    }
    t_down = down;
}

void ui_input_poll(Input *in)
{
    SceCtrlData pad;
    SceTouchData touch;
    uint64_t now;

    memset(in, 0, sizeof *in);
    update_keys();
    now = sceKernelGetSystemTimeWide();

    if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0) {
        uint32_t b = pad.buttons;
        /* 沿触发动作：确认/返回/菜单/功能键按住不重复 */
        in->confirm = (b & g_confirm_key) && !(prev_buttons & g_confirm_key);
        in->back    = (b & g_back_key) && !(prev_buttons & g_back_key);
        in->menu    = (b & SCE_CTRL_SELECT) && !(prev_buttons & SCE_CTRL_SELECT);
        in->alt     = (b & SCE_CTRL_TRIANGLE) && !(prev_buttons & SCE_CTRL_TRIANGLE);
        in->square  = (b & SCE_CTRL_SQUARE) && !(prev_buttons & SCE_CTRL_SQUARE);
        /* 方向键：沿触发 + 长按重复 */
        in->up    = dir_event(b, SCE_CTRL_UP, 0, now);
        in->down  = dir_event(b, SCE_CTRL_DOWN, 1, now);
        in->left  = dir_event(b, SCE_CTRL_LEFT, 2, now);
        in->right = dir_event(b, SCE_CTRL_RIGHT, 3, now);
        prev_buttons = b;
    }

    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
        touch_poll(in, &touch);
    else
        t_down = false;   /* 读取失败视为抬手，避免状态卡住 */
}

/* 启动触摸采样（PSV 默认关闭，必须显式开启才能收到事件） */
void ui_input_init(void)
{
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
}
