/* cam_bench.c —— 后摄采集 + QR 解码基准（见 cam_bench.h）。
 *
 * 实现要点：
 *   - 摄像头帧缓冲必须是 CDRAM（摄像头 DMA 写入；参照 VitaSDK camera 例程，
 *     普通 malloc 拿到的内存驱动未必认），故用 sceKernelAllocMemBlock 分配一块，
 *     Y/U/V 三个平面按 4KB 对齐切分。
 *   - 读帧用 mode=0（阻塞）：每次 read 拿到一帧新数据，线程自然地跟着 30fps 走，
 *     不会空转；这一档的"捕获吞吐"就是真实流水线吞吐。
 *   - 解码只喂 Y 平面（YUV420 的亮度即灰度，正是 quirc 要的输入），按宽度逐行
 *     拷进 quirc 的图缓冲，避免把 pitch/stride 差异带进库。
 *   - 阶段 3 的预览同样从 Y 平面来：线程降采样到 320x240 灰度写共享缓冲，主线程
 *     在渲染 pass 外上传纹理（vita2d 纹理上传不能落在绘制中途）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vita2d.h>
#include <psp2/camera.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include "app/cam_bench.h"
#include "core/dlog.h"
#include "quirc/quirc.h"

#define CAM_DEV       SCE_CAMERA_DEVICE_BACK
#define CAM_FRAMERATE SCE_CAMERA_FRAMERATE_30_FPS

/* 各档分辨率（与 SceCameraResolution 枚举对应） */
static const struct {
    int res;      /* SCE_CAMERA_RESOLUTION_* */
    int w, h;
} s_res[CAM_BENCH_RES_N] = {
    { SCE_CAMERA_RESOLUTION_640_480, 640, 480 },
    { SCE_CAMERA_RESOLUTION_320_240, 320, 240 },
};

/* 摄像头帧缓冲：最大 640x480 YUV420 + 每平面 4KB 对齐的余量 → 512KB 足够 */
#define CAM_MEM_SIZE (512 * 1024)
#define CAM_PLANE_ALIGN 4096

/* 预览纹理固定 320x240 灰度（ABGR 里 R=G=B=Y） */
#define PREV_W 320
#define PREV_H 240

/* ---------- 共享状态（线程写 / 主线程读） ---------- */
static volatile int g_state = CAM_BENCH_IDLE;
static volatile int g_res;
static volatile int g_stage = -1;
static volatile int g_frame;
static volatile int g_width, g_height;
static volatile float g_cap_fps;
static volatile float g_read_avg, g_read_max;
static volatile float g_dec_avg, g_dec_max;
static volatile int g_dec_ok, g_dec_fail;
static volatile float g_prev_avg, g_prev_max;
static volatile int g_err;

static volatile float g_stage_fps[CAM_BENCH_STAGES];
static volatile float g_stage_dec_avg[CAM_BENCH_STAGES];
static volatile float g_stage_dec_max[CAM_BENCH_STAGES];
static volatile int g_stage_ok[CAM_BENCH_STAGES];
static volatile int g_stage_fail[CAM_BENCH_STAGES];
static volatile int g_stage_frames[CAM_BENCH_STAGES];

static volatile int g_stop;
static SceUID g_thid = -1;
static int  g_stuck;            /* 线程停不下来（已泄漏句柄）：不再允许重开 */
static char g_summary[128];     /* 跑完/停止后填；列表行读 */
static int  g_has_result;

/* 预览共享缓冲：线程写 g_prev_buf 后 seq++；主线程按 seq 上传纹理 */
static unsigned char g_prev_buf[PREV_W * PREV_H];
static volatile int g_prev_seq;
static struct vita2d_texture *g_prev_tex;
static int g_prev_tex_seq;
static int g_prev_n;            /* 预览上传采样数（主线程） */
static double g_prev_sum;

bool cam_bench_running(void)
{
    return g_state == CAM_BENCH_RUNNING;
}

const char *cam_bench_summary(void)
{
    return g_has_result ? g_summary : NULL;
}

void cam_bench_snapshot(CamBenchSnap *out)
{
    int i;
    if (!out) return;
    out->state = g_state;
    out->res = g_res;
    out->stage = g_stage;
    out->frame = g_frame;
    out->width = g_width;
    out->height = g_height;
    out->cap_fps = g_cap_fps;
    out->read_avg_ms = g_read_avg;
    out->read_max_ms = g_read_max;
    out->dec_avg_ms = g_dec_avg;
    out->dec_max_ms = g_dec_max;
    out->dec_ok = g_dec_ok;
    out->dec_fail = g_dec_fail;
    out->prev_avg_ms = g_prev_avg;
    out->prev_max_ms = g_prev_max;
    out->err = g_err;
    for (i = 0; i < CAM_BENCH_STAGES; i++) {
        out->stage_fps[i] = g_stage_fps[i];
        out->stage_dec_avg[i] = g_stage_dec_avg[i];
        out->stage_dec_max[i] = g_stage_dec_max[i];
        out->stage_dec_ok[i] = g_stage_ok[i];
        out->stage_dec_fail[i] = g_stage_fail[i];
        out->stage_done_frames[i] = g_stage_frames[i];
    }
}

static uint64_t now_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

/* 一帧解码：成功识别出至少一个可解的二维码 → 1，否则 0。
 * 耗时由调用方包含 quirc_begin/end（拷贝 + 定位 + 解码全算进去）。 */
static int decode_frame(struct quirc *qr, const unsigned char *y, int w, int h)
{
    unsigned char *qb = quirc_begin(qr, NULL, NULL);
    int i, n, ok = 0;

    for (i = 0; i < h; i++)
        memcpy(qb + i * w, y + i * w, (size_t)w);

    quirc_end(qr);
    n = quirc_count(qr);
    for (i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(qr, i, &code);
        if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
            ok = 1;
            break;
        }
    }
    return ok;
}

/* 降采样一帧到预览缓冲（最近邻；640 宽隔行取点，320 宽整行拷） */
static void preview_fill(const unsigned char *y, int w, int h)
{
    int x, yy;
    for (yy = 0; yy < PREV_H; yy++) {
        const unsigned char *src = y + (size_t)(yy * h / PREV_H) * w;
        unsigned char *dst = g_prev_buf + yy * PREV_W;
        if (w == PREV_W) {
            memcpy(dst, src, PREV_W);
        } else {
            for (x = 0; x < PREV_W; x++)
                dst[x] = src[x * w / PREV_W];
        }
    }
    __sync_synchronize();
    g_prev_seq++;
}

/* 把结果摘要写进 g_summary（列表行显示） */
static void summary_build(int res, int state)
{
    if (state == CAM_BENCH_ERROR) {
        snprintf(g_summary, sizeof g_summary, "Error 0x%08X", (unsigned)g_err);
    } else {
        int total = 0, ok = 0, s;
        for (s = 0; s < CAM_BENCH_STAGES; s++) {
            total += g_stage_frames[s];
            ok += g_stage_ok[s];
        }
        snprintf(g_summary, sizeof g_summary,
                 "%dx%d  %.1ffps  dec %.1fms  QR %d/%d",
                 s_res[res].w, s_res[res].h,
                 (double)g_stage_fps[0], (double)g_stage_dec_avg[1], ok, total);
    }
    g_has_result = 1;
}

static int cam_thread(SceSize args, void *argp)
{
    int res = g_res;
    int w = s_res[res].w, h = s_res[res].h;
    SceUID mbid;
    void *base = NULL;
    unsigned char *pI, *pU, *pV;
    size_t ysize, usize;
    struct quirc *qr = NULL;
    SceCameraInfo info;
    int ret, i, s;
    int read_err = 0;
    uint64_t bench_t0 = now_us();
    int stopped = 0;

    (void)args;
    (void)argp;

    mbid = sceKernelAllocMemBlock("psvsend_cam", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                  CAM_MEM_SIZE, NULL);
    if (mbid < 0) {
        g_err = (int)mbid;
        g_state = CAM_BENCH_ERROR;
        dlog("cambench: memblock alloc failed 0x%08X", (unsigned)mbid);
        goto done;
    }
    if (sceKernelGetMemBlockBase(mbid, &base) < 0 || !base) {
        g_err = -1;
        g_state = CAM_BENCH_ERROR;
        dlog("cambench: memblock base failed");
        sceKernelFreeMemBlock(mbid);
        goto done;
    }

    ysize = (size_t)w * h;
    usize = ysize / 4;
    pI = (unsigned char *)base;
    pU = (unsigned char *)base + ((ysize + CAM_PLANE_ALIGN - 1) / CAM_PLANE_ALIGN) * CAM_PLANE_ALIGN;
    pV = (unsigned char *)pU + ((usize + CAM_PLANE_ALIGN - 1) / CAM_PLANE_ALIGN) * CAM_PLANE_ALIGN;

    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.resolution = (uint16_t)s_res[res].res;
    info.framerate = CAM_FRAMERATE;
    info.width = (uint16_t)w;
    info.height = (uint16_t)h;
    info.sizeIBase = (SceSize)ysize;
    info.sizeUBase = (SceSize)usize;
    info.sizeVBase = (SceSize)usize;
    info.pIBase = pI;
    info.pUBase = pU;
    info.pVBase = pV;
    info.pitch = 0;          /* 行间无额外填充 */
    info.buffer = 0;

    ret = sceCameraOpen(CAM_DEV, &info);
    dlog("cambench: open dev=%d res=%dx%d ret=0x%08X", CAM_DEV, w, h, (unsigned)ret);
    if (ret < 0) {
        g_err = ret;
        g_state = CAM_BENCH_ERROR;
        sceKernelFreeMemBlock(mbid);
        goto done;
    }
    ret = sceCameraStart(CAM_DEV);
    dlog("cambench: start ret=0x%08X", (unsigned)ret);
    if (ret < 0) {
        g_err = ret;
        g_state = CAM_BENCH_ERROR;
        sceCameraClose(CAM_DEV);
        sceKernelFreeMemBlock(mbid);
        goto done;
    }

    g_width = w;
    g_height = h;

    qr = quirc_new();
    if (qr && quirc_resize(qr, w, h) < 0) {
        quirc_destroy(qr);
        qr = NULL;
    }
    if (!qr)
        dlog("cambench: quirc init failed (decode stages will report 0)");

    /* 预热：等自动曝光/白平衡稳定，这几十毫秒不计入任何一档 */
    for (i = 0; i < CAM_BENCH_WARMUP && !g_stop; i++) {
        SceCameraRead rd;
        memset(&rd, 0, sizeof rd);
        rd.size = sizeof rd;
        rd.mode = 0;
        sceCameraRead(CAM_DEV, &rd);
    }

    for (s = 0; s < CAM_BENCH_STAGES; s++) {
        uint64_t t_stage = now_us();
        double read_sum = 0.0, dec_sum = 0.0;
        float read_max = 0.0f, dec_max = 0.0f;
        int ok = 0, fail = 0, n = 0;

        g_stage = s;
        g_frame = 0;
        g_cap_fps = 0;
        g_read_avg = 0; g_read_max = 0;
        g_dec_avg = 0; g_dec_max = 0;
        g_dec_ok = 0; g_dec_fail = 0;

        for (i = 0; i < CAM_BENCH_FRAMES; i++) {
            SceCameraRead rd;
            uint64_t t0, t1;
            const unsigned char *y;
            float ms;

            if (g_stop) { stopped = 1; break; }

            memset(&rd, 0, sizeof rd);
            rd.size = sizeof rd;
            rd.mode = 0;                 /* 阻塞：拿一帧新数据 */
            t0 = now_us();
            ret = sceCameraRead(CAM_DEV, &rd);
            t1 = now_us();
            if (ret < 0) {
                if (read_err < 3)
                    dlog("cambench: read fail 0x%08X stage=%d frame=%d",
                         (unsigned)ret, s, i);
                read_err++;
                g_frame = i + 1;
                continue;
            }

            y = rd.pIBase ? (const unsigned char *)rd.pIBase : pI;
            ms = (float)(t1 - t0) / 1000.0f;
            read_sum += ms;
            if (ms > read_max) read_max = ms;

            if (s >= 1 && qr) {
                uint64_t d0 = now_us();
                int got = decode_frame(qr, y, w, h);
                uint64_t d1 = now_us();
                float dms = (float)(d1 - d0) / 1000.0f;
                dec_sum += dms;
                if (dms > dec_max) dec_max = dms;
                if (got) ok++; else fail++;
            }
            if (s >= 2)
                preview_fill(y, w, h);

            n++;
            g_frame = i + 1;
            /* 运行中的实时值：吞吐按"本档开始到现在"算，读数会随解码变慢而下降 */
            {
                uint64_t el = now_us() - t_stage;
                g_cap_fps = el > 0 ? (float)n * 1000000.0f / (float)el : 0.0f;
            }
            g_read_avg = n ? (float)(read_sum / n) : 0.0f;
            g_read_max = read_max;
            if (s >= 1 && n) {
                g_dec_avg = (float)(dec_sum / n);
                g_dec_max = dec_max;
            }
            g_dec_ok = ok;
            g_dec_fail = fail;
        }

        g_stage_frames[s] = n;
        g_stage_ok[s] = ok;
        g_stage_fail[s] = fail;
        g_stage_dec_avg[s] = n ? (float)(dec_sum / n) : 0.0f;
        g_stage_dec_max[s] = dec_max;
        {
            uint64_t el = now_us() - t_stage;
            g_stage_fps[s] = el > 0 ? (float)n * 1000000.0f / (float)el : 0.0f;
            g_cap_fps = g_stage_fps[s];
        }
        dlog("cambench: stage%d %s frames=%d wall=%llums fps=%.1f "
             "read_avg=%.2f read_max=%.2f dec_avg=%.2f dec_max=%.2f qr_ok=%d qr_fail=%d",
             s + 1,
             s == 0 ? "capture" : (s == 1 ? "capture+decode" : "capture+decode+preview"),
             n, (unsigned long long)((now_us() - t_stage) / 1000),
             (double)g_stage_fps[s], (double)g_read_avg, (double)g_read_max,
             (double)g_stage_dec_avg[s], (double)dec_max, ok, fail);
        if (stopped) break;
    }

    g_stage = -1;
    dlog("cambench: end stopped=%d read_err=%d total=%llums prev_avg=%.2f prev_max=%.2f",
         stopped, read_err, (unsigned long long)((now_us() - bench_t0) / 1000),
         (double)g_prev_avg, (double)g_prev_max);

    if (qr) quirc_destroy(qr);
    sceCameraStop(CAM_DEV);
    sceCameraClose(CAM_DEV);
    sceKernelFreeMemBlock(mbid);

    summary_build(res, CAM_BENCH_DONE);
    dlog("cambench: summary %s", g_summary);
    g_state = CAM_BENCH_DONE;
    return 0;

done:
    if (qr) quirc_destroy(qr);
    if (!g_has_result) summary_build(res, CAM_BENCH_ERROR);
    if (g_state != CAM_BENCH_ERROR) g_state = CAM_BENCH_ERROR;
    return 0;
}

bool cam_bench_start(int res)
{
    if (g_state == CAM_BENCH_RUNNING)
        return false;                /* 这一轮还在跑 */
    if (g_thid >= 0)
        cam_bench_stop();            /* 上一轮已结束但句柄没回收 */
    if (g_stuck)
        return false;                /* 线程卡死过：别再动摄像头 */
    if (res < 0 || res >= CAM_BENCH_RES_N) res = 0;

    g_res = res;
    g_state = CAM_BENCH_RUNNING;
    g_has_result = 0;            /* 旧结果作废：跑完/失败时再填 */
    g_stage = -1;
    g_frame = 0;
    g_width = s_res[res].w;
    g_height = s_res[res].h;
    g_cap_fps = 0;
    g_read_avg = g_read_max = 0;
    g_dec_avg = g_dec_max = 0;
    g_dec_ok = g_dec_fail = 0;
    g_prev_avg = g_prev_max = 0;
    g_err = 0;
    g_stop = 0;
    g_prev_seq = 0;
    g_prev_n = 0;
    g_prev_sum = 0.0;
    memset((void *)g_stage_fps, 0, sizeof g_stage_fps);
    memset((void *)g_stage_dec_avg, 0, sizeof g_stage_dec_avg);
    memset((void *)g_stage_dec_max, 0, sizeof g_stage_dec_max);
    memset((void *)g_stage_ok, 0, sizeof g_stage_ok);
    memset((void *)g_stage_fail, 0, sizeof g_stage_fail);
    memset((void *)g_stage_frames, 0, sizeof g_stage_frames);

    g_thid = sceKernelCreateThread("psvsend_cambench", cam_thread, 0x40, 0x40000,
                                   0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (g_thid < 0) {
        dlog("cambench: thread create failed 0x%08X", (unsigned)g_thid);
        g_err = (int)g_thid;
        g_thid = -1;
        g_state = CAM_BENCH_ERROR;
        summary_build(res, CAM_BENCH_ERROR);
        return false;
    }
    if (sceKernelStartThread(g_thid, 0, NULL) < 0) {
        dlog("cambench: thread start failed");
        sceKernelDeleteThread(g_thid);
        g_thid = -1;
        g_err = -1;
        g_state = CAM_BENCH_ERROR;
        summary_build(res, CAM_BENCH_ERROR);
        return false;
    }
    dlog("cambench: begin res=%dx%d frames=%d warmup=%d",
         s_res[res].w, s_res[res].h, CAM_BENCH_FRAMES, CAM_BENCH_WARMUP);
    return true;
}

void cam_bench_stop(void)
{
    int i;
    if (g_thid < 0 || g_stuck)
        return;
    g_stop = 1;
    /* 读帧是阻塞的，最坏要等一帧（~33ms）；给足 3s，超时就不硬删线程
     * （硬删会让它手里的 memblock / 摄像头句柄悬空）。 */
    for (i = 0; i < 600; i++) {
        SceUInt to = 5000;
        if (sceKernelWaitThreadEnd(g_thid, NULL, &to) == 0) {
            sceKernelDeleteThread(g_thid);
            g_thid = -1;
            break;
        }
    }
    if (g_thid >= 0) {
        dlog("cambench: thread won't stop, handle leaked");
        g_stuck = 1;                 /* 句柄留着：线程可能还在用摄像头/缓冲 */
    }
    if (g_stage >= 0) g_stage = -1;
}

/* ---------- 预览 ---------- */

void cam_bench_pre_frame(void)
{
    int seq = g_prev_seq;
    int stride, x, y;
    uint32_t *dp;
    uint64_t t0;

    if (seq <= 0 || seq == g_prev_tex_seq) return;
    if (!g_prev_tex) {
        g_prev_tex = vita2d_create_empty_texture_format(
            PREV_W, PREV_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!g_prev_tex) {
            dlog("cambench: preview texture create failed");
            g_prev_tex_seq = seq;    /* 别再每帧重试 */
            return;
        }
        /* 1:1 贴图：点采样保持锐利（缩放/线性过滤会把 QR 模块边缘糊掉） */
        vita2d_texture_set_filters(g_prev_tex, SCE_GXM_TEXTURE_FILTER_POINT,
                                   SCE_GXM_TEXTURE_FILTER_POINT);
    }
    stride = vita2d_texture_get_stride(g_prev_tex);
    dp = (uint32_t *)vita2d_texture_get_datap(g_prev_tex);
    t0 = now_us();
    for (y = 0; y < PREV_H; y++) {
        uint32_t *row = (uint32_t *)((unsigned char *)dp + (size_t)y * stride);
        const unsigned char *s = g_prev_buf + y * PREV_W;
        for (x = 0; x < PREV_W; x++) {
            uint32_t v = s[x];
            row[x] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    }
    g_prev_tex_seq = seq;
    /* 预览开销 = 主线程把 Y 帧转成纹理的耗时（不是线程里的降采样） */
    {
        float ms = (float)(now_us() - t0) / 1000.0f;
        g_prev_n++;
        g_prev_sum += ms;
        g_prev_avg = (float)(g_prev_sum / g_prev_n);
        if (ms > g_prev_max) g_prev_max = ms;
    }
}

struct vita2d_texture *cam_bench_preview_tex(void)
{
    return g_prev_tex;
}
