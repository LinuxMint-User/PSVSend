/* ime.c —— PSV 系统键盘输入：封装 vitasdk 的 SceImeDialog（全屏系统键盘）。
 *
 * 选型：接收改名 / 主机名等文字输入用系统 IME，而不是自绘键盘——
 * 自绘键盘没有中文输入法（拼音/手写），中文文件名无法输入；系统键盘自带
 * 简中/繁中/日文等多语言软键盘。接口由 vitasdk 直接提供（ime_dialog.h +
 * libSceIme_stub.a），无新增依赖。
 *
 * 调用模型（d56 修正，真机 d55 曾整页卡死）：
 * d55 把等待循环放在 input 阶段阻塞主线程、期间不再出帧——系统键盘 UI
 * 依赖应用持续渲染（参考 VitaShell 主循环每帧 updateImeDialog()、mGBA-psp2
 * 在 while(RUNNING){ _drawStart(); poll; _drawEnd(); } 中轮询），停帧后
 * dialog 永远弹不出来、GetStatus 恒 RUNNING，120s 防御超时才恢复，观感即
 * "点击重命名后界面卡死"。故改为"打开 → 渲染循环内每帧 poll"的状态机。
 *
 * 参数姿势对齐 VitaShell ime_dialog.c：supportedLanguages=0x1FFFF、
 * languagesForced=TRUE（键盘语言强制为我们声明的集合）、单行输入用
 * DIALOG_MODE_DEFAULT（右下键/Enter 确认，X 关闭=取消）。
 *
 * 真机注意：调用前 sceSysmoduleLoadModule(SCE_SYSMODULE_IME)
 * （已加载会返回 0x80020001，忽略）。
 *
 * d58 根因补充：d57 实测 dialog init 成功、GetStatus 恒 RUNNING、主循环
 * 出帧正常，但键盘永不显示——系统对话框的 UI（键盘/消息框等）不是自己
 * 上屏的，须由应用每帧在 swap 前调用 libvita2d 的
 * vita2d_common_dialog_update() 把 dialog 合成进显示缓冲（内部按
 * sceCommonDialogUpdate 填本应用 display surface）。此前 ui_main.c 主循环
 * 缺这一调用，dialog 引擎一直等合成帧 → 键盘不出现、状态卡 RUNNING、
 * 输入无响应，观感即"卡死不弹键盘"（d55-d58 反复复现的根因）。已在
 * ui_main.c 主循环 end_drawing 与 swap_buffers 之间每帧调用修复。
 */
#include <string.h>
#include "ime.h"
#include "core/dlog.h"

#include <psp2/sysmodule.h>
#include <psp2/common_dialog.h>
#include <psp2/libime.h>
#include <psp2/ime_dialog.h>
#include <psp2/kernel/threadmgr/thread.h>   /* sceKernelGetSystemTimeWide（微秒） */

#define IME_MAX_U16     255       /* 可输入上限（UTF-16 单元数；含截断余量） */
#define IME_BUF_U16     (IME_MAX_U16 + 1)
/* 强制放弃的时长由调用方按业务窗口传入 ime_ask_begin(limit_us)：
 * 接收改名贴 prepare 等 UI 决定窗口取 50s（见 pages.c RN_IME_LIMIT_US）；
 * 纯设置项（如主机名）传 0 不限。正常结束（确认/取消）本就走不到超时，
 * 它只是防键盘引擎假死把界面永久冻结的保险丝。 */
/* Abort 后等引擎回空闲的最长时间（Abort 异步收尾，收完才可安全 Term） */
#define IME_ABANDON_WAIT_US (2ll * 1000 * 1000)

/* dialog 生命周期内的参数缓冲（初值/结果在打开期间由系统读写，须保持存活） */
static int g_active;                          /* 0=空闲 1=对话框进行中 */
static int64_t g_t0;                          /* 打开时刻（微秒），poll 超时放弃用 */
static SceWChar16 g_title[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static SceWChar16 g_init[64];
static SceWChar16 g_buf[IME_BUF_U16];
static char *g_out;                           /* 结果写回目标（调用方持有） */
static int g_cap;
static int64_t g_abandon_us;                  /* 打开时限（微秒）；<=0 不限 */

/* ---------- UTF-8 <-> UTF-16 转码（BMP + 代理对均处理，自包含无依赖） ---------- */

/* UTF-8 一码点 → UTF-16（1 或 2 个单元，写满 maxu 即停）。返回用掉 u16 单元数。 */
static int cp_to_utf16(unsigned cp, SceWChar16 *o, int maxu)
{
    if (cp < 0x10000) {
        if (maxu < 1) return 0;
        o[0] = (SceWChar16)cp;
        return 1;
    }
    if (cp <= 0x10FFFF && maxu >= 2) {          /* 转代理对 */
        cp -= 0x10000;
        o[0] = (SceWChar16)(0xD800 + (cp >> 10));
        o[1] = (SceWChar16)(0xDC00 + (cp & 0x3FF));
        return 2;
    }
    return 0;
}

/* UTF-8 串 → UTF-16 缓冲（截断安全，不越界）。返回 u16 单元数（不含结尾 \0），
 * 恒以 \0 收尾。 */
static int utf8_to_utf16(const char *s, SceWChar16 *o, int maxu)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < maxu) {
        unsigned cp;
        unsigned c = *p++;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && (*p & 0xC0) == 0x80) {
            cp = ((c & 0x1F) << 6) | (*p++ & 0x3F);
        } else if ((c & 0xF0) == 0xE0 && (*p & 0xC0) == 0x80
                   && (p[1] & 0xC0) == 0x80) {
            cp = ((c & 0x0F) << 12) | ((*p++ & 0x3F) << 6) | (*p++ & 0x3F);
        } else if ((c & 0xF8) == 0xF0 && (*p & 0xC0) == 0x80
                   && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((c & 0x07) << 18) | ((*p++ & 0x3F) << 12)
                 | ((*p++ & 0x3F) << 6) | (*p++ & 0x3F);
        } else {                                  /* 坏字节，跳过 */
            cp = 0xFFFD;
        }
        n += cp_to_utf16(cp, o + n, maxu - n);
    }
    o[n] = 0;
    return n;
}

/* UTF-16（len 个单元）→ UTF-8 缓冲（cap 字节含结尾 \0，截断安全）。 */
static void utf16_to_utf8(const SceWChar16 *s, int len, char *o, int cap)
{
    int wi = 0, bi = 0;
    while (wi < len && bi < cap - 1) {
        unsigned cp = s[wi];
        if (cp >= 0xD800 && cp <= 0xDBFF && wi + 1 < len
            && s[wi + 1] >= 0xDC00 && s[wi + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[wi + 1] - 0xDC00);
            wi++;
        }
        wi++;
        if (cp < 0x80) {
            o[bi++] = (char)cp;
        } else if (cp < 0x800 && bi + 1 < cap) {
            o[bi++] = (char)(0xC0 | (cp >> 6));
            o[bi++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000 && bi + 2 < cap) {
            o[bi++] = (char)(0xE0 | (cp >> 12));
            o[bi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            o[bi++] = (char)(0x80 | (cp & 0x3F));
        } else if (bi + 3 < cap) {
            o[bi++] = (char)(0xF0 | (cp >> 18));
            o[bi++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            o[bi++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            o[bi++] = (char)(0x80 | (cp & 0x3F));
        } else {
            break;                                /* 缓冲满 */
        }
    }
    o[bi] = 0;
}

/* ---------- 打开（不阻塞） ---------- */

int ime_ask_begin(const char *title_utf8, const char *initial_utf8,
                  char *out_utf8, int cap, int64_t limit_us)
{
    SceImeDialogParam prm;
    int r;

    if (!out_utf8 || cap <= 0 || g_active) return -1;

    dlog("ime: begin (title len=%d init len=%d)",
         title_utf8 ? (int)strlen(title_utf8) : 0,
         initial_utf8 ? (int)strlen(initial_utf8) : 0);
    {
        int rl = sceSysmoduleLoadModule(SCE_SYSMODULE_IME);
        dlog("ime: sysmodule load IME -> 0x%08X", (unsigned)rl);
    }

    sceImeDialogParamInit(&prm);
    utf8_to_utf16(title_utf8 ? title_utf8 : "", g_title,
                  SCE_IME_DIALOG_MAX_TITLE_LENGTH);
    utf8_to_utf16(initial_utf8 ? initial_utf8 : "", g_init, 63);
    prm.supportedLanguages = 0x0001FFFF; /* 全语言位，同 VitaShell：键盘可用语言齐 */
    prm.languagesForced = SCE_TRUE;      /* 强制用上述集合，不随系统语言缩水 */
    prm.type = SCE_IME_TYPE_DEFAULT;
    prm.title = g_title;
    prm.maxTextLength = IME_MAX_U16;
    prm.initialText = g_init;
    prm.inputTextBuffer = g_buf;
    /* 单行输入（VitaShell 同款）：DEFAULT 模式，右下键/Enter 确认、X 关闭=取消 */

    r = sceImeDialogInit(&prm);
    dlog("ime: dialog init -> 0x%08X", (unsigned)r);
    if (r < 0) {
        dlog("ime: dialog init failed 0x%08X", (unsigned)r);
        return -1;
    }
    g_active = 1;
    g_t0 = sceKernelGetSystemTimeWide();
    g_out = out_utf8;
    g_cap = cap;
    g_abandon_us = limit_us;
    return 1;
}

int ime_active(void)
{
    return g_active;
}

/* 每帧（渲染完成后）调用：0=进行中；1=确认已写 out；2=取消/关闭。 */
int ime_ask_poll(void)
{
    SceImeDialogResult res;
    int r, n;

    if (!g_active) return 0;                  /* 不应发生：UI 仅在 active 时调用 */
    if (sceImeDialogGetStatus() == SCE_COMMON_DIALOG_STATUS_RUNNING) {
        /* 防御（仅对调用方限时的事务生效，见 ime.h limit_us）：键盘打开极久
         * 仍无结束（系统无响应/引擎假死）则强制放弃，避免界面永久停在输入态。 */
        if (g_abandon_us > 0 &&
            sceKernelGetSystemTimeWide() - g_t0 > g_abandon_us) {
            SceCommonDialogStatus st;
            int64_t w0 = sceKernelGetSystemTimeWide();
            dlog("ime: abandon: dialog stuck RUNNING >%llds",
                 (long long)(g_abandon_us / 1000000));
            sceImeDialogAbort();
            /* Abort 异步收尾：必须等引擎回 NONE/FINISHED 再 Term。d58 曾在此
             * 直接 Abort+Term，引擎残留占用 → 之后每次 init 返回 0x80020401
             * BUSY，键盘再也弹不出（本次运行内）。等待期每 10ms 轮询。 */
            for (;;) {
                st = sceImeDialogGetStatus();
                if (st == SCE_COMMON_DIALOG_STATUS_NONE ||
                    st == SCE_COMMON_DIALOG_STATUS_FINISHED)
                    break;
                if (sceKernelGetSystemTimeWide() - w0 > IME_ABANDON_WAIT_US) {
                    dlog("ime: abandon: still st=%d after %lldms, force term",
                         (int)st,
                         (long long)(IME_ABANDON_WAIT_US / 1000));
                    break;
                }
                sceKernelDelayThread(10 * 1000);   /* 10ms */
            }
            sceImeDialogTerm();
            g_active = 0;
            return 2;
        }
        return 0;
    }

    dlog("ime: poll finished (was active %d ms)",
         (int)((sceKernelGetSystemTimeWide() - g_t0) / 1000));
    memset(&res, 0, sizeof res);
    r = sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    g_active = 0;
    dlog("ime: result r=0x%08X button=%d", (unsigned)r, (int)res.button);
    if (r < 0) return 2;                      /* 失败按取消处理 */
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) return 2;  /* 关闭/X = 取消 */

    n = 0;
    while (g_buf[n] && n < IME_MAX_U16) n++;
    utf16_to_utf8(g_buf, n, g_out, g_cap);
    return 1;
}
