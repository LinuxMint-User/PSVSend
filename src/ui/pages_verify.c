/* 页面：设备身份验证（PAGE_VERIFY）。
 *
 * 与官方 LocalSend 的 VerifyPage 同口径（app/lib/pages/verify_page.dart 的
 * CombinedFingerprint）：把【本机指纹】与【对端指纹】按字典序排序后直接拼接，
 * 不做哈希、不截断，两边各自算出的 128 hex 应当逐字符一致。
 *   - 指纹 = 设备自签证书 DER 的 SHA-256，大写 hex（官方 cert.rs 明确"uppercase
 *     hex, the format used for LocalSend fingerprints"），与本机 identity_fingerprint
 *     的格式一致，故直接拼即可，不需要统一大小写。
 *   - 只从"确认对方身份有意义"的入口进：发送等待页（对端协议 https，页面有「验证」
 *     按钮）与接收确认页（本次连接是 TLS，页面有「验证」按钮）。明文 http 对端的指纹是随机串，
 *     官方也是把验证按钮画成禁用态，我们不提供入口。
 * 显示两种等价形式（官方是二选一，我们并排放，省掉切换交互）：
 *   - 左半：16 个图标，图标 = SHA-256(128 hex) 前 16 字节做 base-260 的 16 位数字，
 *     下标直接取图集格子（官方 fingerprintToIcons 的完整口径）；
 *   - 右半：原 128 hex，改成 8 行 × 16 字符（字号不变，只是换行）。
 * 本页只展示、不改任何传输状态：返回后回到进入前的页面。 */
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include <mbedtls/sha256.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/i18n.h"
#include "net/identity.h"

/* 图集几何（与 scripts/gen-verify-icons.py、ui_main.c 的尺寸校验一致）：
 * 260 个格子 = 16 列 × 17 行，每格 48px，四周各留 1px 透明边 → 格距 50。 */
#define ICON_CELL   48
#define ICON_PAD    1
#define ICON_PITCH  (ICON_CELL + ICON_PAD * 2)
#define ICON_COLS   16
#define ICON_ROWS   17

/* 128 hex → SHA-256 → 取前 16 字节 → base-260 的 16 位数字（digits[0] 最高位）。
 * 与官方 convertFingerprintToAlphabet 逐位对齐：长度 = ceil(32*4*log2/log260) = 16，
 * 用大整数反复除 260，最低位落在 digits[15]。图集第 n 格就是 alphabet[n]，
 * 所以这里算出来的数字可以直接当格子号。 */
static void code_to_digits(const char *code, unsigned char digits[16])
{
    unsigned char v[16];
    int i, j;

    mbedtls_sha256((const unsigned char *)code, strlen(code), v, 0);
    for (i = 0; i < 16; i++) {
        unsigned rem = 0;
        for (j = 0; j < 16; j++) {
            unsigned cur = (rem << 8) | v[j];
            v[j] = (unsigned char)(cur / 260);
            rem = cur % 260;
        }
        digits[15 - i] = (unsigned char)rem;
    }
}

void open_verify(const char *peer_fp, const char *alias)
{
    char mine[65];
    const char *a, *b;

    if (!peer_fp || !peer_fp[0]) return;

    mine[0] = 0;
    if (identity_fingerprint(mine) != 0)
        config_get_fingerprint(mine, sizeof mine);   /* 身份未就绪：先看盘上旧值 */
    mine[64] = 0;

    if (mine[0]) {
        a = mine;
        b = peer_fp;
        if (strcmp(a, b) > 0) { const char *t = a; a = b; b = t; }
        snprintf(g_app.verify_code, sizeof g_app.verify_code, "%s%s", a, b);
        code_to_digits(g_app.verify_code, g_app.verify_digits);
    } else {
        g_app.verify_code[0] = 0;   /* 首次开机身份还在生成：页面给出说明 */
        memset(g_app.verify_digits, 0, sizeof g_app.verify_digits);
    }
    snprintf(g_app.verify_alias, sizeof g_app.verify_alias, "%s",
             alias && alias[0] ? alias : "?");
    g_app.verify_ret = (int)g_app.page;
    g_app.page = PAGE_VERIFY;
}

static void verify_back(void)
{
    if (g_app.verify_ret > 0 && g_app.verify_ret < PAGE_COUNT)
        g_app.page = (PageId)g_app.verify_ret;
    else
        g_app.page = PAGE_DEVICES;
}

void page_verify_render(void)
{
    Rect card = { 24, 80, SCR_W - 48, 316 };
    int lmax = card.w - 48;
    int div_x = card.x + card.w / 2;          /* 480：左右半的分界 */
    int right_w = card.x + card.w - div_x;    /* 456 */
    int i;
    struct vita2d_texture *icons = ui_verify_icons();

    w_page_header(tr("Verify"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_VER, HDRBTN_BACK, false);

    w_rect(card, theme->card);
    w_text_clip(48, 96, 1.2f, theme->text, g_app.verify_alias, lmax);

    if (!g_app.verify_code[0]) {
        w_text_clip(48, 168, 1.2f, theme->text_dim,
                    tr("Identity is still being generated."), lmax);
    } else {
        w_text_clip(48, 124, 1.0f, theme->text_dim,
                    tr("Compare this code with the other device."), lmax);

        /* 左半：4×4 图标。图集按 1:1 绘制（48px 格 + 1px 边），每格连同透明边
         * 一起贴，避免相邻图标在边缘互相渗色；颜色走主题前景色（图集是白字 +
         * alpha，tint 后即为当前主题的文字色）。图集没加载上就只画右半。 */
        if (icons) {
            int gx = card.x + (div_x - card.x - (4 * ICON_CELL + 3 * 12)) / 2;
            int gy = 158;
            for (i = 0; i < 16; i++) {
                int d = g_app.verify_digits[i];
                float sx = (float)((d % ICON_COLS) * ICON_PITCH);
                float sy = (float)((d / ICON_COLS) * ICON_PITCH);
                float x = (float)(gx + (i % 4) * (ICON_CELL + 12) - ICON_PAD);
                float y = (float)(gy + (i / 4) * (ICON_CELL + 12) - ICON_PAD);
                vita2d_draw_texture_tint_part(icons, x, y, sx, sy,
                                              ICON_PITCH, ICON_PITCH,
                                              theme->text);
            }
        }

        /* 中缝：只是视觉分隔，不占交互 */
        w_rect((Rect){ div_x - 1, 158, 2, 228 }, theme->border);

        /* 右半：128 hex 分 8 行 × 16 字符（字号与原来的文本模式一致，仅换行），
         * 逐行在本半区内居中，保留逐字符比对的用途 */
        for (i = 0; i < 8; i++) {
            char chunk[17];
            int tw = 0;
            memcpy(chunk, g_app.verify_code + i * 16, 16);
            chunk[16] = 0;
            w_text_w(SC_MAIN, chunk, &tw, NULL);
            w_text(div_x + (right_w - tw) / 2, 158 + i * 29, SC_MAIN,
                   theme->text, "%s", chunk);
        }
    }

    HintSeg segs[2] = { 0 };
    int ns = 0;
    segs[ns].key = HKEY_BACK; segs[ns++].text = tr("Back");
    w_page_footer_segs(segs, ns);
}

void page_verify_input(const Input *in)
{
    if (in->drag_start || in->dragging) return;
    if (in->tap) {
        if (w_hit(in->tap_x, in->tap_y) == WID_HDR_BACK_VER) verify_back();
        return;
    }
    if (in->back) verify_back();
}
