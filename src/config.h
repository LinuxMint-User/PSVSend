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

typedef struct {
    char alias[64];          /* 设备名，广播给其他 LocalSend 设备 */
    char fingerprint[64];    /* 随机身份串：防自发现；首次生成后持久化 */
    int  port;               /* HTTP 服务端口（默认 53317，与协议一致） */
    int  theme_id;           /* 0=Yaru 1=OLED */
    int  confirm_layout;     /* 0=美式 1=日式 */
} Config;

extern Config g_cfg;

/* 建数据目录 + 填默认值 + 读盘覆盖；失败静默用默认 */
void config_init(void);
/* 把当前配置写回磁盘（幂等，失败静默） */
void config_save(void);

#endif
