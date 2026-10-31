/* cam_bench —— 后摄采集 + QR 解码基准（实验室页手动触发）。
 *
 * 为什么放后台线程：采集/解码的耗时就是被测对象，若放在渲染回调里，测到的
 * 吞吐会被 UI 帧率（60fps 上限）截断，也看不出"解码拖慢采集"的真实程度。
 * 线程里跑 sceCameraRead + quirc 解码，UI 每帧只读一份快照显示。
 *
 * 一次「开始」连跑三档（每档 CAM_BENCH_FRAMES 帧）：
 *   阶段 1 纯采集 → 阶段 2 采集 + 解码 → 阶段 3 采集 + 解码 + 预览。
 * 三档之间不重开摄像头（同一路流，只改每帧要做的额外工作），这样差异只来自
 * 解码/预览本身。整表写 log.txt（dlog），结果摘要回实验室列表行。 */
#ifndef PSVSEND_APP_CAM_BENCH_H
#define PSVSEND_APP_CAM_BENCH_H

#include <stdbool.h>

enum {
    CAM_BENCH_IDLE = 0,     /* 未运行（从未跑过 / 已跑完） */
    CAM_BENCH_RUNNING,
    CAM_BENCH_DONE,
    CAM_BENCH_ERROR         /* 摄像头打不开 / 启动失败（err 里是 sce 错误码） */
};

#define CAM_BENCH_STAGES   3
#define CAM_BENCH_FRAMES   60   /* 每档固定帧数（640x480@30fps 约 2s） */
#define CAM_BENCH_WARMUP   10   /* 开流后丢弃的预热帧（等自动曝光稳定） */

/* 分辨率档位（0=640x480 1=320x240） */
#define CAM_BENCH_RES_N    2

/* UI 每帧取的状态快照（线程写、主线程读；字段都是 4 字节对齐，单次读不会撕裂） */
typedef struct {
    int   state;            /* CAM_BENCH_* */
    int   res;              /* 0=640x480 1=320x240 */
    int   stage;            /* 运行中 0..2；否则 -1 */
    int   frame;            /* 当前档已跑帧数 */
    int   width, height;
    float cap_fps;          /* 当前档吞吐：帧数 / 该档墙钟 */
    float read_avg_ms, read_max_ms;   /* sceCameraRead 平均 / 最大耗时 */
    float dec_avg_ms, dec_max_ms;     /* quirc 解码平均 / 最大耗时 */
    int   dec_ok, dec_fail;           /* 当前档识别成功 / 失败帧数 */
    float prev_avg_ms, prev_max_ms;   /* 预览帧上传平均 / 最大耗时（主线程测） */
    int   err;                        /* ERROR 时的 sce 错误码 */
    /* 每档最终结果（跑完或中途停止时都有已跑部分） */
    float stage_fps[CAM_BENCH_STAGES];
    float stage_dec_avg[CAM_BENCH_STAGES];
    float stage_dec_max[CAM_BENCH_STAGES];
    int   stage_dec_ok[CAM_BENCH_STAGES];
    int   stage_dec_fail[CAM_BENCH_STAGES];
    int   stage_done_frames[CAM_BENCH_STAGES];
} CamBenchSnap;

/* 起一次基准测试（res：0=640x480 1=320x240）。已有在跑 → 返回 false。 */
bool cam_bench_start(int res);
/* 请求停止并等线程收尾（关摄像头、释放缓冲）；未在跑时是空操作 */
void cam_bench_stop(void);
bool cam_bench_running(void);

/* 读当前快照 */
void cam_bench_snapshot(CamBenchSnap *out);

/* 列表行摘要（最近一次结果；从未跑过返回 NULL，由页面显示 "Not run"） */
const char *cam_bench_summary(void);

/* 预览：pre_frame 在渲染 pass 之外调用（建纹理 / 上传最新 Y 帧），
 * preview_tex 在渲染里调用（返回纹理，NULL=暂无预览）。 */
void cam_bench_pre_frame(void);
struct vita2d_texture;
struct vita2d_texture *cam_bench_preview_tex(void);

#endif
