/* cam_scan —— 取景 + 二维码识别（实验室「扫码取景」页用）。
 *
 * 与 cam_bench 的区别：基准是"跑一遍出几个数字"，这里是"一直开着取景、
 * 按住 R 才解码"。所以拆成两个线程：
 *   采集线程：sceCameraRead 阻塞读，稳定 30fps；拷贝 Y 帧 + 降采样出预览。
 *   解码线程：只在按住 R 且未命中时取最新帧跑 quirc（640×480 ≈147ms → 6~8fps）。
 * 解码若和采集同线程，预览会被拖到 7fps（见 cam_bench 的单线程实现）。
 *
 * 命中一次就冻结：停止解码、停止刷新预览（画面定格），候选与包围盒交给 UI
 * 选择；UI 调 cam_scan_clear_hit() 后重新开始。 */
#ifndef PSVSEND_APP_CAM_SCAN_H
#define PSVSEND_APP_CAM_SCAN_H

#include <stdbool.h>

enum {
    CAM_SCAN_IDLE = 0,      /* 未启动 */
    CAM_SCAN_RUNNING,       /* 取景中（相机可能还在启动：看 preview 是否为空） */
    CAM_SCAN_ERROR          /* 相机打不开 / 启动失败（err 里是 sce 错误码） */
};

#define CAM_SCAN_MAX      8     /* 一帧最多收几个候选（够选就行，多了排不下） */
#define CAM_SCAN_TEXT     192   /* payload 截断长度：放得下分享 URL，也放得下提示行 */

#define CAM_SCAN_FRAME_W  640   /* 解码用的原帧尺寸（候选包围盒坐标系） */
#define CAM_SCAN_FRAME_H  480
/* 预览直接用原帧（d231 起）：此前降到 320×240 再放大画到 533×400，
 * 取景画面只有相机 1/4 的像素、发糊。纹理上传的代价见 cam_scan_pre_frame。 */
#define CAM_SCAN_PREV_W   CAM_SCAN_FRAME_W
#define CAM_SCAN_PREV_H   CAM_SCAN_FRAME_H

typedef struct {
    char text[CAM_SCAN_TEXT];   /* payload 文本（已 NUL 结尾） */
    int  box[4];                /* 包围盒 x0,y0,x1,y1（640×480 相机坐标） */
} CamScanItem;

typedef struct {
    int  state;                 /* CAM_SCAN_* */
    int  err;                   /* state=ERROR 时的 sce 错误码 */
    int  frozen;                /* 1=命中后冻结（画面定格、解码停止） */
    int  n;                     /* 候选数（frozen 时有意义） */
    int  frame;                 /* 已采集帧数（诊断） */
    int  dec_ms;                /* 最近一次解码耗时（诊断） */
    CamScanItem items[CAM_SCAN_MAX];
} CamScanSnap;

/* 开相机并起线程；已在运行 → 返回 false。 */
bool cam_scan_start(void);
/* 停止并等线程收尾（关相机、释放缓冲）；未运行时是空操作。 */
void cam_scan_stop(void);
bool cam_scan_running(void);

/* UI 每帧调用：按住 R 期间才解码。 */
void cam_scan_set_hold(bool hold);
/* 重新扫描：清候选并解冻（预览恢复刷新）。 */
void cam_scan_clear_hit(void);

void cam_scan_snapshot(CamScanSnap *out);

/* 预览：pre_frame 在渲染 pass 之外调用（建纹理 / 上传最新 Y 帧），
 * preview_tex 在渲染里调用（NULL=暂无预览）。 */
void cam_scan_pre_frame(void);
struct vita2d_texture;
struct vita2d_texture *cam_scan_preview_tex(void);

#endif
