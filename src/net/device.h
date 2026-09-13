/* device.h —— 一台被发现的对端设备（LocalSend announce / register 的关键字段）。
 *
 * 刻意定义在 net 层而非 app/api.h：发现、扫描、注册表与上层契约都要用它，
 * 若留在 app 层，net/（底层）就得 include app/api.h 才能拿到类型——分层倒置。
 * 容量 DEVICE_MAX 与 UI 侧 MAX_DEVICES（ui/ui.h）保持一致。 */
#ifndef PSVSEND_NET_DEVICE_H
#define PSVSEND_NET_DEVICE_H

#include <stdbool.h>

#define DEVICE_MAX 32

typedef struct {
    char ip[16];             /* 发送方 IP（announce 来源） */
    int  port;               /* 该设备 HTTP 服务端口 */
    char alias[64];          /* 设备名 */
    char model[64];          /* 设备型号（可空） */
    char dtype[16];          /* 平台类型 mobile/desktop/web/... */
    char fingerprint[96];    /* 身份串/HTTPS 证书 SHA-256（hex，防自发现 + TLS pin） */
    char protocol[8];        /* http / https */
    bool download;           /* 是否开了下载 API（能否主动收） */
} Device;

#endif
