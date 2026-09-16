/* 应用配置：存到 ux0:data/psvsend/config（JSON）。
 * 前端(设置页)与后端(发现/将来 HTTP)共享这份运行时配置。
 * alias 为设备名（LocalSend 里称 alias），默认 "PS Vita"；
 * 文字输入用 PSV 系统键盘（app/ime.c）：接收改名与设置页主机名均已接入，
 * 改名即时生效（alias 各广播点现读，下一轮 announce 自动带新名，无需重启）。 */
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
 * 与 CMakeLists.txt 的 project(VERSION 2.2.0) 保持一致——升级版本号时
 * 两处一起改，SFO APP_VER 由 CMake 从 VERSION 派生，无需手改。 */
#define PSVSEND_APP_VERSION "2.2.0"

typedef struct {
    char alias[64];          /* 设备名，广播给其他 LocalSend 设备 */
    char fingerprint[64];    /* 随机身份串：防自发现；首次生成后持久化 */
    int  port;               /* HTTP 服务端口（默认 53317，与协议一致） */
    int  theme_id;           /* 色系：0=Yaru 1=OLED 2=Custom（自定义） */
    int  light_mode;         /* 明暗（与色系正交）：0=深色(默认) 1=浅色；OLED 固定深色 */
    int  custom_h;           /* 自定义主色 HSV：色相 0-359 */
    int  custom_s;           /* 自定义主色 HSV：饱和度 0-100 */
    int  custom_v;           /* 自定义主色 HSV：明度 0-100 */
    int  confirm_layout;     /* 0=美式 1=日式 */
    int  pane_swap;          /* 发送主页两栏布局：0=设备在左 1=文件在左（照顾左撇子） */
    int  lang;               /* 界面语言偏好：I18N_LANG_*（0=跟随系统，见 core/i18n.h） */
    int  known_n;            /* 历史设备 IP 条数（最近发现优先，作扫描种子） */
    char known_ips[KNOWN_MAX][16]; /* 历史设备 IP，最新在前 */
    char save_dir[512];  /* 保存目录（设置页可选，持久化；默认 downloads）。
                          * 接收前的"本次目录"是内存态临时覆盖，不进 config。 */
    int  upd_auto;   /* 自动检查更新：0=关 1=每天 2=每周(默认) 3=每月 */
    int  upd_last;   /* 上次检查更新时刻的"网络"unix 时间戳（授时来自远端
                      * 响应头 Date；自动周期判定用，0=从未成功查过） */
} Config;

extern Config g_cfg;

/* 建数据目录 + 填默认值 + 读盘覆盖 + 值域钳制；失败静默用默认 */
void config_init(void);
/* 把当前配置写回磁盘：内部持锁，写临时文件 + rename 原子替换；
 * 写失败保留盘上旧配置并记 dlog（不再静默丢设置） */
void config_save(void);
/* 记录一个最近在线的设备 IP（去重滚动、最新在前；内部节流落盘） */
void config_note_ip(const char *ip);
/* 取与 a.b.c. 前缀匹配的历史主机号列表（供扫描优先），返回数量 */
int config_known_hosts(unsigned a, unsigned b, unsigned c, int *out, int max);

/* 跨线程共享字段的加锁访问：alias/fingerprint/saveDir 由 UI 线程改写、
 * worker 线程（HTTP 广播、扫描、接收落盘目录）读取，直接访问会读到改到
 * 一半的字符串（广播里发出半截设备名、接收落到半截目录名）；一律走这里。
 * setter 内部同时落盘，返回即已持久化。
 * 仅供 UI 线程读写的字段（theme/light/custom/confirmLayout/paneSwap）与
 * int 项（updAuto/updLast）可直接访问 g_cfg。 */
void config_get_alias(char *out, int n);
void config_set_alias(const char *alias);        /* 空串 → DEFAULT_ALIAS */
void config_get_fingerprint(char *out, int n);
void config_get_save_dir(char *out, int n);
void config_set_save_dir(const char *dir);
void config_set_lang(int lang);                  /* 越界 → AUTO */
void config_set_upd_auto(int v);                 /* 越界 → 每周 */
void config_set_upd_last(int t);

#endif
