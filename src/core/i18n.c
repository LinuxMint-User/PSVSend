/* i18n.c —— 界面文案多语言切换（见 i18n.h）。
 *
 * 语言来源优先级：Config.lang 显式偏好 > 跟随系统。跟随系统时用 SceAppUtil
 * 的 sceAppUtilSystemParamGetInt 读 SCE_SYSTEM_PARAM_ID_LANG（该 API 要求先
 * sceAppUtilInit——本文件是唯一使用者，故在此惰性初始化一次；读失败/未知语言
 * 一律回退英文，绝不因查语言失败影响启动）。
 * 系统语言只区分"简体 / 繁体中文"，繁体无法再分台 / 港；PSV 的中文（繁體）
 * 本地化基底是台灣用語（網路 / 設定 / 儲存…，Sony 从未单独做港式繁中），故系统
 * 为繁体时默认取「繁中（台灣）」，想用港式用词（網絡 / 文件夾）可在设置页语言行
 * 手选「繁中（香港）」（系统语言推不出来，只能手选）。
 *
 * 译文表：一行一个 key，含 简体 / 繁中（台灣）/ 繁中（香港）/ 日本語 四列译文，
 * key 即英文文案。条目少、每帧调用有限，用线性扫描（不要求表有序，插新词条
 * 无需保证位置）。
 * 两列繁中按各自地区用词习惯分别翻译（如 網路 / 網絡、資料夾 / 文件夾），
 * 不是繁简字符一对一转写；日语同理是意译而非汉字直搬（如 Devices → デバイス、
 * Up → 上へ），避免"中味"。
 * 格式串翻译注意：译文保留与英文 key 一致的 %s/%d 占位符顺序，调用点传参
 * 无需改动；译文可省略某个占位符（多传的实参无害），但绝不能新增英文里没有
 * 的占位符。 */
#include <string.h>
#include <psp2/apputil.h>
#include <psp2/system_param.h>
#include "i18n.h"
#include "config.h"
#include "dlog.h"

typedef struct {
    const char *en;   /* 英文 key（也是兜底文案） */
    const char *zh;   /* 简体中文译文 */
    const char *tw;   /* 繁中（台灣）译文 */
    const char *hk;   /* 繁中（香港）译文 */
    const char *ja;   /* 日本語译文 */
} TrEntry;

/* 词条表（按页面分组便于维护；查表用线性扫描，不要求有序）。
 * key 必须与调用点字符串逐字节一致（含开头空格/标点），
 * 否则查不到 → 该语言界面里该条回退成英文。 */
static const TrEntry s_tr[] = {
    /* ---- 设备页 / 手动扫描横幅 ---- */
    { "Scanning... %d of %d possible hosts, %d found, you could proceed",
      "扫描中…已探测 %d/%d 个可能的主机，找到 %d 台，可继续操作",
      "掃描中…已探測 %d/%d 個可能的主機，找到 %d 台，可繼續操作",
      "掃描中…已探測 %d/%d 個可能的主機，找到 %d 台，可繼續操作",
      "スキャン中… 候補ホスト %d/%d、検出 %d 台。このまま続行できます" },
    { "Scanning... %d of %d possible hosts",
      "扫描中…已探测 %d/%d 个可能的主机",
      "掃描中…已探測 %d/%d 個可能的主機",
      "掃描中…已探測 %d/%d 個可能的主機",
      "スキャン中… 候補ホスト %d/%d" },
    { "Scan complete, %d device%s found",
      "扫描完成，发现 %d 台设备",
      "掃描完成，找到 %d 台裝置",
      "掃描完成，找到 %d 台裝置",
      "スキャン完了、%d 台のデバイスを検出" },
    { "Scan complete, no devices",
      "扫描完成，未发现设备",
      "掃描完成，未找到裝置",
      "掃描完成，未找到裝置",
      "スキャン完了、デバイスは見つかりません" },
    { "Network not ready.",
      "网络未就绪。",
      "網路尚未就緒。",
      "網絡尚未就緒。",
      "ネットワークの準備ができていません。" },
    { "Waiting for Wi-Fi to come back up...",
      "正在等待 Wi-Fi 恢复…",
      "正在等待 Wi-Fi 恢復…",
      "正在等待 Wi-Fi 恢復…",
      "Wi-Fi の復帰を待っています…" },
    { "Wi-Fi link is down.",
      "Wi-Fi 连接已断开。",
      "Wi-Fi 連線已中斷。",
      "Wi-Fi 連線已中斷。",
      "Wi-Fi 接続が切断されています。" },
    { "Scanning network...",
      "正在扫描网络…",
      "正在掃描網路…",
      "正在掃描網絡…",
      "ネットワークをスキャン中…" },
    { "No devices found.",
      "未发现设备。",
      "未找到裝置。",
      "未找到裝置。",
      "デバイスが見つかりません。" },
    { "Press Triangle to scan this network.",
      "按三角键扫描当前网络。",
      "按三角鍵掃描此網路。",
      "按三角鍵掃描此網絡。",
      "△ ボタンでこのネットワークをスキャンします。" },
    { "Choose", "选择", "選擇", "選擇", "選択" },
    { "Send", "发送", "傳送", "傳送", "送信" },
    { "Scan", "扫描", "掃描", "掃描", "スキャン" },
    { "SELECT Settings", "SELECT 设置", "SELECT 設定", "SELECT 設定", "SELECT 設定" },

    /* ---- 发送主页两栏（设备栏 / 已选文件栏） ---- */
    { "Devices (%d)", "设备（%d）", "裝置（%d）", "裝置（%d）", "デバイス（%d）" },
    { "Selected files (%d)", "已选文件（%d）", "已選檔案（%d）", "已選檔案（%d）",
      "選択したファイル（%d）" },
    { "Select files", "选择文件", "選擇檔案", "選擇檔案", "ファイルを選択" },
    { "Select files first", "请先选择文件", "請先選擇檔案", "請先選擇檔案",
      "先にファイルを選択してください" },
    { "Switch", "切换", "切換", "切換", "切り替え" },
    { "Remove", "移除", "移除", "移除", "削除" },
    { "Remove all", "全部删除", "全部移除", "全部移除", "すべて削除" },
    { "Select all", "全选", "全選", "全選", "すべて選択" },
    { "Deselect all", "取消全选", "取消全選", "取消全選", "すべて解除" },
    { "%d selected", "已选 %d 个", "已選 %d 個", "已選 %d 個", "%d 件を選択中" },

    /* ---- 文件浏览 / 发送确认 ---- */
    { "Send to %s", "发送到 %s", "傳送到 %s", "傳送到 %s", "%s に送信" },
    { "folder", "文件夹", "資料夾", "文件夾", "フォルダー" },
    { "(empty folder)", "（空文件夹）", "（空資料夾）", "（空文件夾）",
      "（空のフォルダー）" },
    { "Send(%d)", "发送(%d)", "傳送(%d)", "傳送(%d)", "送信(%d)" },
    { "Open/Pick", "打开/勾选", "開啟/挑選", "開啟/挑選", "開く/選択" },
    { "Up", "上级", "上層", "上層", "上へ" },
    { "Send (%d)", "发送（%d）", "傳送（%d）", "傳送（%d）", "送信（%d）" },
    { "Confirm Send", "确认发送", "確認傳送", "確認傳送", "送信の確認" },
    { "Target: %s", "目标：%s", "目標：%s", "目標：%s", "送信先：%s" },
    { "%d file(s)  total %s", "%d 个文件，共 %s", "%d 個檔案，共 %s", "%d 個檔案，共 %s",
      "%d 個のファイル  合計 %s" },
    { "  <file %d>", "  （文件 %d）", "  （檔案 %d）", "  （檔案 %d）",
      "  （ファイル %d）" },
    { "  ... %d more", "  … 还有 %d 个", "  … 還有 %d 個", "  … 還有 %d 個",
      "  … 他 %d 件" },
    { "Cancel", "取消", "取消", "取消", "キャンセル" },

    /* ---- 接收请求确认 / 接收设置 ---- */
    { "Incoming files", "收到文件", "收到檔案", "收到檔案", "受信ファイル" },
    { "This request has expired.", "此请求已过期。", "此請求已過期。", "此請求已過期。",
      "このリクエストは期限切れです。" },
    { "No response was sent to the sender.",
      "未向发送方发出响应。",
      "未向傳送方送出回應。",
      "未向傳送方送出回應。",
      "送信元へ応答を返していません。" },
    { "Close", "关闭", "關閉", "關閉", "閉じる" },
    { "%s wants to send you %d file(s).",
      "「%s」想向你发送 %d 个文件。",
      "「%s」想傳送 %d 個檔案給你。",
      "「%s」想傳送 %d 個檔案給你。",
      "%s が %d 個のファイルを送信しようとしています。" },
    { "Total %s", "共 %s", "共 %s", "共 %s", "合計 %s" },
    { "Total %s  (%d of %d selected)",
      "共 %s（已选 %d/%d）",
      "共 %s（已選 %d/%d）",
      "共 %s（已選 %d/%d）",
      "合計 %s（%d/%d を選択中）" },
    { "%d more file(s) exceed the receive limit and will be skipped.",
      "%d 个文件超出接收上限，将自动跳过。",
      "%d 個檔案超出接收上限，將自動略過。",
      "%d 個檔案超出接收上限，將自動略過。",
      "%d 個のファイルが受信上限を超えるためスキップされます。" },
    { "%d file name(s) too long, will be shortened",
      "%d 个文件名过长，将自动缩短（保留后缀）",
      "%d 個檔案名稱過長，將自動縮短（保留副檔名）",
      "%d 個檔案名稱過長，將自動縮短（保留副檔名）",
      "%d 個のファイル名が長すぎるため短縮されます（拡張子は保持）" },
    { "Reject", "拒绝", "拒絕", "拒絕", "拒否" },
    { "Setup", "设置", "設定", "設定", "受信設定" },
    { "Accept", "接受", "接受", "接受", "受信" },
    { "Switch", "切换", "切換", "切換", "切り替え" },
    { "Receive setup", "接收设置", "接收設定", "接收設定", "受信設定" },
    { "Save to", "保存到", "儲存到", "儲存到", "保存先" },
    { "default", "默认", "預設", "預設", "既定" },
    { "Rename", "重命名", "重新命名", "重新命名", "名前を変更" },
    { "Toggle", "勾选", "勾選", "勾選", "チェック" },
    { "Back", "返回", "返回", "返回", "戻る" },
    { "Rename file", "重命名文件", "重新命名檔案", "重新命名檔案", "ファイル名を変更" },

    /* ---- 传输进度页 ---- */
    { "Transfer failed", "传输失败", "傳輸失敗", "傳輸失敗", "転送に失敗しました" },
    { "Waiting for receiver to accept...", "等待对方接受…", "等待對方接受…", "等待對方接受…",
      "相手が受信を許可するのを待っています…" },
    { "Please accept on the other device.",
      "请在对方设备上确认接收。",
      "請在對方裝置上確認接收。",
      "請在對方裝置上確認接收。",
      "相手のデバイスで受信を許可してください。" },
    { "Sending...", "发送中…", "傳送中…", "傳送中…", "送信中…" },
    { "Sending", "发送", "傳送", "傳送", "送信中" },
    { "Receive failed", "接收失败", "接收失敗", "接收失敗", "受信に失敗しました" },
    { "Cancelled", "已取消", "已取消", "已取消", "キャンセルしました" },
    { "Waiting for sender to start...", "等待发送方开始…", "等待傳送方開始…", "等待傳送方開始…",
      "送信側の開始を待っています…" },
    { "Receiving...", "接收中…", "接收中…", "接收中…", "受信中…" },
    { "Receiving", "接收", "接收", "接收", "受信中" },
    { "Session was not created (request expired).",
      "未能建立会话（请求已过期）。",
      "未能建立工作階段（請求已過期）。",
      "未能建立工作階段（請求已過期）。",
      "セッションを作成できませんでした（リクエストの期限切れ）。" },
    { "Total   %d%%", "总计   %d%%", "總計   %d%%", "總計   %d%%", "合計   %d%%" },
    { "Complete", "已完成", "已完成", "已完成", "完了" },
    { "Complete, %d failed", "已完成，%d 个失败", "已完成，%d 個失敗",
      "已完成，%d 個失敗", "完了、%d 件失敗" },
    { "Failed", "失败", "失敗", "失敗", "失敗" },
    { "Files %d/%d    Elapsed %d:%02d    Speed %.2f MB/s",
      "文件 %d/%d    用时 %d:%02d    速度 %.2f MB/s",
      "檔案 %d/%d    已用時間 %d:%02d    速度 %.2f MB/s",
      "檔案 %d/%d    已用時間 %d:%02d    速度 %.2f MB/s",
      "ファイル %d/%d    経過 %d:%02d    速度 %.2f MB/s" },
    { "Advanced", "高级", "進階", "進階", "詳細" },
    { "Done", "完成", "完成", "完成", "完了" },

    /* ---- 设置页 ---- */
    { "Settings", "设置", "設定", "設定", "設定" },
    { "Theme", "主题", "主題", "主題", "テーマ" },
    { "Appearance", "外观", "外觀", "外觀", "外観" },
    { "Dark", "深色", "深色", "深色", "ダーク" },
    { "Light", "浅色", "淺色", "淺色", "ライト" },
    { "Dark (fixed)", "深色（固定）", "深色（固定）", "深色（固定）", "ダーク（固定）" },
    { "Custom", "自定义", "自訂", "自訂", "カスタム" },
    { "Primary color", "主色", "主色", "主色", "メインカラー" },
    { "Choose color", "选择颜色", "選擇顏色", "選擇顏色", "色を選択" },
    { "Hue", "色相", "色相", "色相", "色相" },
    { "Saturation", "饱和度", "飽和度", "飽和度", "彩度" },
    { "Brightness", "明度", "明度", "明度", "明度" },
    { "Adjust", "调整", "調整", "調整", "調整" },
    { "Save", "保存", "儲存", "儲存", "保存" },
    { "Language", "语言", "語言", "語言", "言語" },
    { "Confirm key", "确认键", "確認鍵", "確認鍵", "決定キー" },
    { "Layout", "布局", "版面配置", "版面", "レイアウト" },
    { "Devices left", "设备在左", "裝置在左", "裝置在左", "デバイスを左" },
    { "Files left", "文件在左", "檔案在左", "檔案在左", "ファイルを左" },
    { "Hostname", "主机名", "主機名稱", "主機名稱", "ホスト名" },
    { "%s confirm / %s back (%s)",
      "%s 确认 / %s 返回（%s）",
      "%s 確認 / %s 返回（%s）",
      "%s 確認 / %s 返回（%s）",
      "%s 決定 / %s 戻る（%s）" },
    { "Device", "设备", "裝置", "裝置", "デバイス" },
    { "Display", "显示", "顯示", "顯示", "表示" },
    { "Controls", "操作", "操作", "操作", "操作" },
    { "Tap row halves to change",
      "点击行左右半边切换当前项",
      "點擊列的左右半邊切換目前項目",
      "點擊列的左右半邊切換目前項目",
      "行の左右をタップして切り替え" },
    { "Change", "更改", "變更", "變更", "変更" },
    { "About", "关于", "關於", "關於", "このアプリについて" },

    /* ---- 存储组 / 目录选择（设置页默认保存目录 & 接收 Setup 本次目录共用） ---- */
    { "Storage", "存储", "儲存空間", "儲存空間", "ストレージ" },
    { "Save folder", "保存目录", "儲存目錄", "儲存目錄", "保存先フォルダー" },
    { "Choose folder", "选择文件夹", "選擇資料夾", "選擇文件夾", "フォルダーを選択" },
    { "Open", "打开", "開啟", "開啟", "開く" },
    { "Save here", "存到此处", "存到此處", "存到此處", "ここに保存" },
    { "Exit", "退出", "退出", "退出", "終了" },
    { "Change folder", "更改目录", "變更目錄", "變更目錄", "フォルダーを変更" },
    { "LocalSend client v1.15+ compatible (protocol v2.0)",
      "适配 LocalSend 客户端 v1.15+（协议 v2.0）",
      "相容 LocalSend 用戶端 v1.15+（通訊協定 v2.0）",
      "相容 LocalSend 用戶端 v1.15+（通訊協定 v2.0）",
      "LocalSend クライアント v1.15+ 対応（プロトコル v2.0）" },

    /* ---- 更新检查（设置页 Update 组） ---- */
    { "Update", "更新", "更新", "更新", "更新" },
    { "Check for updates", "检查更新", "檢查更新", "檢查更新", "更新を確認" },
    { "Auto check", "自动检查", "自動檢查", "自動檢查", "自動チェック" },
    { "%s available", "新版本 %s 可用", "新版本 %s 可用", "新版本 %s 可用",
      "新しいバージョン %s があります" },
    { "Up to date", "已是最新", "已是最新", "已是最新", "最新版です" },
    { "Checking...", "检查中…", "檢查中…", "檢查中…", "確認中…" },
    { "Check failed", "检查失败", "檢查失敗", "檢查失敗", "確認に失敗しました" },
    { "Off", "关闭", "關閉", "關閉", "オフ" },
    { "Daily", "每天", "每天", "每天", "毎日" },
    { "Weekly", "每周", "每週", "每週", "毎週" },
    { "Monthly", "每月", "每月", "每月", "毎月" },

    /* ---- 其它 ---- */
    /* 语言项"跟随系统"：列表其余项都是各语言自述，而 AUTO 不是语言名、没有自述
     * 形态，只能跟随界面语言（见 i18n_lang_name）。 */
    { "System", "跟随系统", "跟隨系統", "跟隨系統", "システム" },
    { "unknown", "未知", "未知", "未知", "不明" },
};

static int s_lang = I18N_LANG_EN;   /* 当前实际语言 */

/* sceAppUtilSystemParamGetInt 要求先 sceAppUtilInit；本文件是唯一使用者，
 * 惰性初始化一次（失败不致命：随后读参数会失败 → 英文兜底）。 */
static void apputil_ensure_init(void)
{
    static int done = 0;
    SceAppUtilInitParam ip;
    SceAppUtilBootParam bp;
    int r;
    if (done) return;
    done = 1;
    memset(&ip, 0, sizeof ip);
    memset(&bp, 0, sizeof bp);
    r = sceAppUtilInit(&ip, &bp);
    if (r < 0) dlog("i18n: sceAppUtilInit failed 0x%08X", r);
}

static int lang_of_system(void)
{
    int v = -1, r;
    apputil_ensure_init();
    r = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &v);
    dlog("i18n: system lang read r=0x%08X v=%d", r, r < 0 ? -1 : v);
    if (r != 0) return I18N_LANG_EN;           /* 读不到 → 英文兜底 */
    if (v == SCE_SYSTEM_PARAM_LANG_CHINESE_S)
        return I18N_LANG_ZH;                   /* 简体中文 */
    if (v == SCE_SYSTEM_PARAM_LANG_CHINESE_T)
        return I18N_LANG_ZH_TW;                /* 繁体：PSV 繁中本地化基底为台灣用語 */
    if (v == SCE_SYSTEM_PARAM_LANG_JAPANESE)
        return I18N_LANG_JA;                   /* 日本語 */
    return I18N_LANG_EN;                       /* 其它语言暂回退英文 */
}

void i18n_init(void)
{
    int pref = g_cfg.lang;
    if (pref < I18N_LANG_AUTO || pref >= I18N_LANG_COUNT) {
        pref = I18N_LANG_AUTO;
        config_set_lang(pref);   /* 钳制结果写回 g_cfg + 盘：旧实现只钳局部变量，
                                  * 非法值会一直留在配置里被反复读回 */
    }
    s_lang = (pref == I18N_LANG_AUTO) ? lang_of_system() : pref;
}

void i18n_set_lang(int pref)
{
    config_set_lang(pref);       /* 写 g_cfg + 落盘（内部持锁、含越界钳制） */
    i18n_init();
}

int i18n_lang(void)      { return s_lang; }
int i18n_lang_pref(void) { return g_cfg.lang; }

const char *i18n_lang_name(int id)
{
    switch (id) {
    /* 其余项用各语言自述（界面语言看不懂也能认出自己那项）；AUTO 不是语言名、
     * 没有自述形态，只能跟随界面语言 —— 选 AUTO 时 s_lang 即系统探到的语言，
     * 所以显示的正好是"该系统语言自己的说法"，不会出现语言错配。 */
    case I18N_LANG_AUTO:  return tr("System");
    case I18N_LANG_EN:    return "English";
    case I18N_LANG_ZH:    return "中文";
    case I18N_LANG_ZH_TW: return "繁體中文（台灣）";
    case I18N_LANG_ZH_HK: return "繁體中文（香港）";
    case I18N_LANG_JA:    return "日本語";
    default:              return "?";
    }
}

const char *tr(const char *key)
{
    int i, n = (int)(sizeof s_tr / sizeof s_tr[0]);
    if (!key) return key;
    if (s_lang == I18N_LANG_EN) return key;    /* 英文原样（省查找） */
    for (i = 0; i < n; i++) {
        if (strcmp(key, s_tr[i].en) != 0) continue;
        switch (s_lang) {
        case I18N_LANG_ZH:    return s_tr[i].zh;
        case I18N_LANG_ZH_TW: return s_tr[i].tw;
        case I18N_LANG_ZH_HK: return s_tr[i].hk;
        case I18N_LANG_JA:    return s_tr[i].ja;
        }
        return key;
    }
    return key;                                /* 未收录 → 英文兜底 */
}
