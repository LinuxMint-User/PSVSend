/* 应用配置：存到 ux0:data/psvsend/config（JSON）。
 * 前端(设置页)与后端(发现/将来 HTTP)共享这份运行时配置。
 * alias 为设备名（LocalSend 里称 alias），默认 "PS Vita"；
 * 文字输入方案未定，改名入口暂做壳子，值暂不可编辑。 */
#ifndef PSVSEND_CONFIG_H
#define PSVSEND_CONFIG_H

#define PSVSEND_DATA_DIR  "ux0:data/psvsend"
#define PSVSEND_CONFIG    PSVSEND_DATA_DIR "/config"
#define PSVSEND_DL_DIR    PSVSEND_DATA_DIR "/downloads"
#define DEFAULT_ALIAS     "PS Vita"
#define DEFAULT_PORT      53317

/* 历史发现的设备 IP 上限（跨启动"最近在线优先扫描"的持久化列表） */
#define KNOWN_MAX         24

/* 客户端发布版本（运行时显示 / 设置页"关于"）。
 * 与 CMakeLists.txt 的 project(VERSION 2.1.0) 保持一致——升级版本号时
 * 两处一起改，SFO APP_VER 由 CMake 从 VERSION 派生，无需手改。 */
#define PSVSEND_APP_VERSION "2.1.0"

typedef struct {
    char alias[64];          /* 设备名，广播给其他 LocalSend 设备 */
    char fingerprint[64];    /* 随机身份串：防自发现；首次生成后持久化 */
    int  port;               /* HTTP 服务端口（默认 53317，与协议一致） */
    int  theme_id;           /* 0=Yaru 1=OLED */
    int  confirm_layout;     /* 0=美式 1=日式 */
    int  lang;               /* 界面语言偏好：0=跟随系统 1=English 2=中文 */
    int  known_n;            /* 历史设备 IP 条数（最近发现优先，作扫描种子） */
    char known_ips[KNOWN_MAX][16]; /* 历史设备 IP，最新在前 */
    int  upd_auto;   /* 自动检查更新：0=关 1=每天 2=每周(默认) 3=每月 */
    int  upd_last;   /* 上次检查更新时刻的"网络"unix 时间戳（授时来自远端
                      * 响应头 Date；自动周期判定用，0=从未成功查过） */
} Config;

extern Config g_cfg;

/* 建数据目录 + 填默认值 + 读盘覆盖；失败静默用默认 */
void config_init(void);
/* 把当前配置写回磁盘（幂等，失败静默） */
void config_save(void);
/* 记录一个最近在线的设备 IP（去重滚动、最新在前；内部节流落盘） */
void config_note_ip(const char *ip);
/* 取与 a.b.c. 前缀匹配的历史主机号列表（供扫描优先），返回数量 */
int config_known_hosts(unsigned a, unsigned b, unsigned c, int *out, int max);

#endif
