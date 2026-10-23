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
 * 本页只展示、不改任何传输状态：返回后回到进入前的页面。 */
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include "ui/ui.h"
#include "ui/pages_internal.h"
#include "ui/theme.h"
#include "core/config.h"
#include "core/i18n.h"
#include "net/identity.h"

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
    } else {
        g_app.verify_code[0] = 0;   /* 首次开机身份还在生成：页面给出说明 */
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
    int i;

    w_page_header(tr("Verify"));
    w_hdr_button(HDR_SLOT_R, WID_HDR_BACK_VER, HDRBTN_BACK, false);

    w_rect(card, theme->card);
    w_text_clip(48, 100, 1.4f, theme->text, g_app.verify_alias, lmax);

    if (!g_app.verify_code[0]) {
        w_text_clip(48, 168, 1.2f, theme->text_dim,
                    tr("Identity is still being generated."), lmax);
    } else {
        /* 128 hex 分 4 行（每行 32 字符），逐行居中；等宽排布便于逐字比对 */
        for (i = 0; i < 4; i++) {
            char chunk[33];
            int tw = 0, th = 0;
            memcpy(chunk, g_app.verify_code + i * 32, 32);
            chunk[32] = 0;
            w_text_w(1.25f, chunk, &tw, &th);
            w_text(card.x + (card.w - tw) / 2, 170 + i * 46, 1.25f,
                   theme->text, "%s", chunk);
        }
    }

    w_text_clip(48, 356, 1.0f, theme->text_dim,
                tr("Compare this code with the other device."), lmax);

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
