/* Web 网页端（官方 web share 口径）：浏览器经明文 HTTP 与 PSV 互传文件。
 *
 * 两种互斥模式（同一时刻 `/` 只服务一种）：
 *   WEB_MODE_DOWNLOAD  PSV → 浏览器：分享本次已选的 ≤64 个文件（不做目录浏览）
 *   WEB_MODE_UPLOAD    浏览器 → PSV：走 LocalSend 既有的 prepare-upload / upload；
 *                      可逐文件选，也可整文件夹上传（相对路径随 fileName 还原）
 *                      通道，PSV 侧照旧弹接收确认页（写盘闸门不变）
 *
 * 鉴权：4 位 PIN（每次进共享页生成，不落 config，退出即失效）；同一 IP 错 3 次
 * 回 429。下载会话绑定首次输对 PIN 的来源 IP，刷新页面可带 sessionId 免 PIN。
 *
 * 本模块只管会话状态与网页资源；socket/TLS/流式读写住在 net/http.c（见那里的
 * web 路由）。文件路径只在 PSV 内部使用，浏览器只拿 fileName / size。 */
#ifndef PSVSEND_WEB_H
#define PSVSEND_WEB_H

#include <stdbool.h>
#include <stdint.h>
#include <psp2/types.h>

#define WEB_MAX_FILES 64        /* 下载分享清单上限（与 UI 的 MAX_PICKED 同口径）；上传走接收侧 2048 */
#define WEB_MAX_SESS  4         /* 并发下载会话上限（多台设备各一条） */
#define WEB_MAX_CLIENTS 4       /* 运行页"连接来源"最多列出的浏览器数（同 IP 只留一条） */
#define WEB_PIN_LEN   4

typedef enum {
    WEB_MODE_NONE = 0,
    WEB_MODE_DOWNLOAD,
    WEB_MODE_UPLOAD
} WebMode;

/* 分享清单条目：由 UI 从 g_app.picked 拷进来（web 层不依赖 ui 层类型） */
typedef struct {
    char   name[128];
    char   path[512];
    SceOff size;
} WebEntry;

/* 运行页"连接来源"的一行：IP + UA 解析出的短标签 */
typedef struct {
    char ip[16];
    char browser[32];            /* 如 "Chrome 131"；解析失败为空 */
} WebClient;

/* UI 渲染用快照（锁内拷贝） */
typedef struct {
    int    mode;                  /* WebMode */
    char   pin[WEB_PIN_LEN + 1];
    char   ip[16];                /* 本机 IP（URL 用） */
    int    port;                  /* HTTP 端口（URL 用） */
    int    file_count;
    SceOff total_bytes;
    /* 最近访问的浏览器（按最近活动倒序；同一 IP 合并成一条，多开浏览器只更新标签）。
     * 之前只留"最后一次"，多台设备同时下载时页面上只会看到最后一台。 */
    int    client_count;
    WebClient clients[WEB_MAX_CLIENTS];
    int    sessions;              /* 已建立下载会话数 */
    int    active_dl;             /* 正在进行的下载数（空闲会话不算） */
    int    requests;              /* 本次运行累计请求数 */
    /* 当前/最近一次下载 */
    int    cur;                   /* 文件下标（-1=无） */
    char   cur_name[128];
    SceOff cur_size;
    SceOff cur_got;
    SceOff done_bytes;            /* 本次运行累计发出的字节 */
    double speed_bps;             /* 最近采样速度（字节/秒） */
    uint64_t last_us;             /* 最近活动时刻（微秒；0=从未） */
} WebSnap;

/* 建锁（api_start 调用一次；幂等） */
void web_init(void);

/* 进入共享页：生成一枚新 PIN。运行中重进页面不要调用（否则浏览器会话失效）。 */
void web_new_pin(void);

/* 启动共享（会清空上一轮的会话与统计，生成新 PIN）。n 会被夹到 WEB_MAX_FILES。 */
void web_start_download(const WebEntry *files, int n);
void web_start_upload(void);
/* 停止共享：mode 归零、PIN/会话/统计清空 */
void web_stop(void);

int  web_mode(void);
void web_snapshot(WebSnap *out);

/* ---------- 以下供 net/http.c 的路由使用 ---------- */

/* 当前模式的网页（NONE 时返回 403 页）。返回静态缓冲指针，*len 为字节数。 */
const char *web_page(int *len);

/* PIN 校验：返回 200 / 401（不对）/ 429（该 IP 错误次数已满）。
 * 仅 WEB_MODE_UPLOAD 的 prepare-upload 预校验用；下载走 web_prepare_download。 */
int web_check_pin(const char *pin, const char *ip);

/* 记下浏览器来源（IP + User-Agent），PSV 页面显示用；UA 会截断到 63 字节，
 * 另解析出 "Chrome 131" 这样的短标签（见 WebSnap.client_browser）。 */
void web_note_client(const char *ip, const char *ua);

/* 前端心跳（POST/GET /heartbeat?sessionId=）：页面每 2 秒打一次，关页即停。
 * 有有效 sessionId 就续该会话的在线时间；没有就只续"最近活动"（输 PIN 前
 * 也能在运行页显示连接来源）。不计数、不建会话。 */
void web_heartbeat(const char *query, const char *ip);

/* POST /api/localsend/v2/prepare-download?pin=|sessionId=
 * 成功：200，sid_out=会话号（32 hex），files_json=`[{"id":"f0","fileName":…,"size":…}]`
 * 失败：401 PIN 不对 / 429 尝试过多 / 403 未开启下载模式 */
int web_prepare_download(const char *query, const char *ip,
                         char *sid_out, int sidcap,
                         char *files_json, int fsz);

/* GET /api/localsend/v2/download?sessionId=&fileId=
 * 成功：200，*idx=文件下标（同时记为"当前下载"），name/size 回填
 * 失败：400 缺参 / 403 来源 IP 不符 / 404 文件不存在 / 410 共享已停止或会话已失效
 * （410 而非 403：下载器会把 403 当"服务器连接数限制"忽略，从而把中断当完成） */
int web_resolve_download(const char *query, const char *ip, int *idx,
                         char *name, int namesz, SceOff *size);

/* 文件路径（仅在 resolve 成功后有效；web_stop 后不要再用） */
const char *web_entry_path(int idx);

/* 下载统计打点（http.c 流式写盘时调用）。begin 返回本次传输的代次，
 * 循环里用 web_download_gen() 比对：变了（用户退出共享）应立即收手。 */
int      web_download_begin(int idx, SceOff size);
void     web_download_progress(int gen, SceOff got);
void     web_download_end(int gen);
unsigned web_download_gen(void);

/* 浏览器上传侧打点（prepare/upload 到达时） */
void web_upload_note(const char *ip, const char *ua);

#endif /* PSVSEND_WEB_H */
