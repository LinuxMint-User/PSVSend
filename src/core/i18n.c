/* i18n.c —— 界面文案中英切换（见 i18n.h）。
 *
 * 语言来源优先级：Config.lang 显式偏好 > 跟随系统。跟随系统时用 SceAppUtil
 * 的 sceAppUtilSystemParamGetInt 读 SCE_SYSTEM_PARAM_ID_LANG（本套 SDK 的
 * apputil.h 没声明它，但符号在 libSceAppUtil_stub 里，这里手工 extern 声明；
 * 读失败/未知语言一律回退英文——绝不因查语言失败影响启动）。
 *
 * 译文表：只存"中文"一列，key 即英文文案。条目少、每帧调用有限，
 * 用线性扫描（不要求表有序，插新词条无需保证位置）。
 * 格式串翻译注意：中文本保留与英文 key 一致的 %s/%d 占位符顺序，调用点
 * 传参无需改动；中文本可省略某个占位符（多传的实参无害），但绝不能新增
 * 英文里没有的占位符。 */
#include <string.h>
#include <psp2/system_param.h>
#include "i18n.h"
#include "config.h"

/* SceAppUtil 系统参数读取（本 SDK apputil.h 未声明，手工声明链接 stub 符号） */
int sceAppUtilSystemParamGetInt(int id, int *value);

typedef struct {
    const char *en;   /* 英文 key（也是兜底文案） */
    const char *zh;   /* 简体中文译文（保留英文的 printf 占位符） */
} TrEntry;

/* 词条表（按页面分组便于维护；查表用线性扫描，不要求有序）。
 * key 必须与 pages.c 调用点字符串逐字节一致（含开头空格/标点），
 * 否则查不到 → 中文界面里该条回退成英文。 */
static const TrEntry s_zh[] = {
    /* ---- 设备页 / 手动扫描横幅 ---- */
    { "Scanning... %d/%d hosts, %d found",
      "扫描中… %d/%d 台主机，已发现 %d 台" },
    { "Scan complete, %d device%s found", "扫描完成，发现 %d 台设备" },
    { "Scan complete, no devices", "扫描完成，未发现设备" },
    { "Network not ready.", "网络未就绪。" },
    { "Waiting for Wi-Fi to come back up...", "正在等待 Wi-Fi 恢复…" },
    { "Wi-Fi link is down.", "Wi-Fi 连接已断开。" },
    { "Scanning network...", "正在扫描网络…" },
    { "No devices found.", "未发现设备。" },
    { "Press Triangle to scan this network.", "按三角键扫描当前网络。" },
    { "Choose", "选择" },
    { "Send", "发送" },
    { "Scan", "扫描" },
    { "SELECT Settings", "SELECT 设置" },

    /* ---- 文件浏览 / 发送确认 ---- */
    { "Send to %s", "发送到 %s" },
    { "folder", "文件夹" },
    { "(empty folder)", "（空文件夹）" },
    { "Send(%d)", "发送(%d)" },
    { "Open/Pick", "打开/勾选" },
    { "Up", "上级" },
    { "Send (%d)", "发送（%d）" },
    { "Confirm Send", "确认发送" },
    { "Target: %s", "目标：%s" },
    { "%d file(s)  total %s", "%d 个文件，共 %s" },
    { "  <file %d>", "  （文件 %d）" },
    { "  ... %d more", "  … 还有 %d 个" },
    { "Cancel", "取消" },

    /* ---- 接收请求确认 / 接收设置 ---- */
    { "Incoming files", "收到文件" },
    { "This request has expired.", "此请求已过期。" },
    { "No response was sent to the sender.", "未向发送方发出响应。" },
    { "Close", "关闭" },
    { "%s wants to send you %d file(s).", "「%s」想向你发送 %d 个文件。" },
    { "Total %s", "共 %s" },
    { "Total %s  (%d of %d selected)", "共 %s（已选 %d/%d）" },
    { "%d more file(s) exceed the receive limit and will be skipped.",
      "%d 个文件超出接收上限，将自动跳过。" },
    { "Reject", "拒绝" },
    { "Setup", "设置" },
    { "Accept", "接受" },
    { "Switch", "切换" },
    { "Receive setup", "接收设置" },
    { "Save to", "保存到" },
    { "default", "默认" },
    { "Rename", "重命名" },
    { "Toggle", "勾选" },
    { "Back", "返回" },
    { "Rename file", "重命名文件" },

    /* ---- 传输进度页 ---- */
    { "Transfer failed", "传输失败" },
    { "Waiting for receiver to accept...", "等待对方接受…" },
    { "Sending...", "发送中…" },
    { "Sending", "发送" },
    { "Receive failed", "接收失败" },
    { "Cancelled", "已取消" },
    { "Waiting for sender to start...", "等待发送方开始…" },
    { "Receiving...", "接收中…" },
    { "Receiving", "接收" },
    { "Session was not created (request expired).",
      "未能建立会话（请求已过期）。" },
    { "Total   %d%%", "总计   %d%%" },
    { "Complete", "已完成" },
    { "Failed", "失败" },
    { "Files %d/%d    Elapsed %d:%02d    Speed %.2f MB/s",
      "文件 %d/%d    用时 %d:%02d    速度 %.2f MB/s" },
    { "Advanced", "高级" },
    { "Done", "完成" },

    /* ---- 设置页 ---- */
    { "Settings", "设置" },
    { "Theme", "主题" },
    { "Language", "语言" },
    { "Confirm key", "确认键" },
    { "Hostname", "主机名" },
    { "%s confirm / %s back (%s)", "%s 确认 / %s 返回（%s）" },
    { "Device", "设备" },
    { "Display", "显示" },
    { "Controls", "操作" },
    { "Tap row halves to change", "点击行左右半边切换当前项" },
    { "Change", "更改" },
    { "About", "关于" },

    /* ---- 存储组 / 目录选择（设置页默认保存目录 & 接收 Setup 本次目录共用） ---- */
    { "Storage", "存储" },
    { "Save folder", "保存目录" },
    { "Choose folder", "选择文件夹" },
    { "Open", "打开" },
    { "Save here", "存到此处" },
    { "LocalSend client v1.15+ compatible (protocol v2.0)",
      "适配 LocalSend 客户端 v1.15+（协议 v2.0）" },

    /* ---- 更新检查（设置页 Update 组） ---- */
    { "Update", "更新" },
    { "Check for updates", "检查更新" },
    { "Auto check", "自动检查" },
    { "%s available", "新版本 %s 可用" },
    { "Up to date", "已是最新" },
    { "Checking...", "检查中…" },
    { "Check failed", "检查失败" },
    { "Off", "关闭" },
    { "Daily", "每天" },
    { "Weekly", "每周" },
    { "Monthly", "每月" },

    /* ---- 其它 ---- */
    { "unknown", "未知" },
};

static int s_lang = I18N_LANG_EN;   /* 当前实际语言 */

static int lang_of_system(void)
{
    int v = -1;
    if (sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &v) != 0)
        return I18N_LANG_EN;                   /* 读不到 → 英文兜底 */
    if (v == SCE_SYSTEM_PARAM_LANG_CHINESE_S ||
        v == SCE_SYSTEM_PARAM_LANG_CHINESE_T)
        return I18N_LANG_ZH;                   /* 先做中英，繁中归到中文 */
    return I18N_LANG_EN;                       /* 其它语言暂回退英文 */
}

void i18n_init(void)
{
    int pref = g_cfg.lang;
    if (pref < I18N_LANG_AUTO || pref >= I18N_LANG_COUNT) pref = I18N_LANG_AUTO;
    s_lang = (pref == I18N_LANG_AUTO) ? lang_of_system()
                                      : (pref == I18N_LANG_ZH ? I18N_LANG_ZH
                                                              : I18N_LANG_EN);
}

void i18n_set_lang(int pref)
{
    if (pref < I18N_LANG_AUTO || pref >= I18N_LANG_COUNT) pref = I18N_LANG_AUTO;
    g_cfg.lang = pref;
    config_save();
    i18n_init();
}

int i18n_lang(void)      { return s_lang; }
int i18n_lang_pref(void) { return g_cfg.lang; }

const char *i18n_lang_name(int id)
{
    switch (id) {
    case I18N_LANG_AUTO: return "System";
    case I18N_LANG_EN:   return "English";
    case I18N_LANG_ZH:   return "中文";
    default:             return "?";
    }
}

static const TrEntry *find_zh(const char *en)
{
    int i, n = (int)(sizeof s_zh / sizeof s_zh[0]);
    for (i = 0; i < n; i++)
        if (strcmp(en, s_zh[i].en) == 0)
            return &s_zh[i];
    return NULL;
}

const char *tr(const char *key)
{
    const TrEntry *e;
    if (!key) return key;
    if (s_lang == I18N_LANG_EN) return key;    /* 英文原样（省查找） */
    e = find_zh(key);
    return e ? e->zh : key;
}
