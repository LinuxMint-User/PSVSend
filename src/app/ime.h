/* ime.h —— PSV 系统键盘（SceImeDialog）封装，见 ime.c。
 * 说明：vitasdk 自带 libSceIme_stub.a + psp2/ime_dialog.h，调用系统 IME
 * 弹全屏键盘（支持简中/繁中/日文等多语言），替代自绘键盘（无中文输入引擎）。
 *
 * 调用模型（重要，参考 VitaShell / mGBA-psp2 实现）：
 * 系统键盘弹出后，应用**必须保持渲染循环持续出帧**，键盘 UI 才会出现并响应。
 * 因此本封装是"打开 → 每帧轮询"的非阻塞状态机，不能阻塞等待：
 *   1) ime_ask_begin() 在页面 input 处理中调用（打开对话框，立即返回）；
 *   2) 之后主循环每帧正常渲染，并在渲染完成后调用 ime_ask_poll()；
 *   3) poll 返回非 0（确认/取消）表示对话框已关闭，可处理结果。
 */
#ifndef PSVSEND_APP_IME_H
#define PSVSEND_APP_IME_H

#include <stdint.h>

/* 打开系统键盘（不阻塞）。参数语义见 ime.c。out 缓冲必须存活到
 * ime_ask_poll() 返回结果之后（调用方持有）。
 * limit_us：键盘打开后多久无结束（确认/取消）即强制放弃（微秒），
 * <=0 表示不限制。仅"有业务时效窗口"的输入才需要限时（如接收改名须赶在
 * prepare 等 UI 决定的 60s 窗口内，取 50s）；纯设置项（如主机名）传 0 不限，
 * 正常结束（确认/取消）本就走不到超时，它只是防键盘引擎假死的保险丝。
 * 返回 1 = 已打开；0/-1 = 打开失败（dlog 记录原因），此时不会进入轮询态。 */
int ime_ask_begin(const char *title_utf8, const char *initial_utf8,
                  char *out_utf8, int cap, int64_t limit_us);

/* 键盘是否处于打开/进行中。用于 UI 判断：打开期间应跳过正常按键输入，
 * 并把渲染循环交给 poll 收尾。 */
int ime_active(void);

/* 每帧调用一次（渲染完成后）。
 * 返回 0 = 仍在进行（继续渲染下一帧再调）；
 * 返回 1 = 已确认：out_utf8 已写入输入文本，对话框已关闭；
 * 返回 2 = 已取消/关闭（未写入），对话框已关闭。 */
int ime_ask_poll(void);

#endif
