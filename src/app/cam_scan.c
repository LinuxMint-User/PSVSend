/* cam_scan.c —— 取景 + 二维码识别（见 cam_scan.h）。
 *
 * 采集与解码分两个线程：采集线程按 30fps 阻塞读帧（顺带把 Y 平面拷给预览），
 * 解码线程只在按住 R 且未命中时解最新一帧。若像 cam_bench 那样串在一个线程里，
 * 每帧多花 ~147ms 解码，预览会掉到 6~8fps——取景器就不能用了。
 *
 * 帧一致性：采集写 g_y、解码读 g_y 都在 g_mtx 内（各自只做一次 307KB 拷贝，
 * 亚毫秒级），所以解码拿到的一定是完整帧，不会撕裂。
 *
 * 命中即冻结：解码线程写好候选后置 g_frozen，采集线程随即停止刷新预览
 * （画面定格），解码线程自己也不再继续。 */
#include <stdio.h>
#include <string.h>
#include <vita2d.h>
#include <psp2/camera.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/mutex.h>
#include <psp2/kernel/processmgr.h>
#include "app/cam_scan.h"
#include "core/dlog.h"
#include "quirc/quirc.h"

#define CAM_DEV        SCE_CAMERA_DEVICE_BACK
#define CAM_FRAMERATE  SCE_CAMERA_FRAMERATE_30_FPS
#define CAM_W          CAM_SCAN_FRAME_W  /* 解码用原帧：QVGA 只快 ~24%，不值 */
#define CAM_H          CAM_SCAN_FRAME_H
#define CAM_WARMUP     10               /* 开流后丢弃的预热帧（等自动曝光稳定） */

/* 摄像头帧缓冲必须是 CDRAM（DMA 写入；普通堆内存驱动未必认），
 * 640×480 YUV420 + 每平面 4KB 对齐的余量 → 512KB 足够 */
#define CAM_MEM_SIZE   (512 * 1024)
#define CAM_PLANE_ALIGN 4096

enum { CS_IDLE = 0, CS_RUNNING, CS_ERROR };

static volatile int g_state = CS_IDLE;
static volatile int g_err;
static volatile int g_stop;
static volatile int g_hold;         /* 按住 R：允许解码 */
static volatile int g_frozen;       /* 已命中：停止解码 + 定格预览 */
static volatile int g_n;
static volatile int g_frame;
static volatile int g_dec_ms;

static CamScanItem g_items[CAM_SCAN_MAX];
static unsigned char g_y[CAM_W * CAM_H];            /* 最新一帧 Y 平面 */
static unsigned char g_prev[CAM_SCAN_PREV_W * CAM_SCAN_PREV_H];
static volatile int g_prev_seq;

static SceUID g_mtx = -1;
static SceUID g_cap_thid = -1, g_dec_thid = -1;
static int g_stuck;                 /* 线程停不下来（句柄有悬空风险）：不再重开 */

static struct vita2d_texture *g_prev_tex;
static int g_prev_tex_seq;

static uint64_t now_us(void)
{
    return (uint64_t)sceKernelGetSystemTimeWide();
}

static void lock(void)   { if (g_mtx >= 0) sceKernelLockMutex(g_mtx, 1, NULL); }
static void unlock(void) { if (g_mtx >= 0) sceKernelUnlockMutex(g_mtx, 1); }

bool cam_scan_running(void)
{
    return g_state == CS_RUNNING;
}

/* 预览缓冲 = 原帧 Y 平面（640×480，全分辨率）。只做一次 307KB 拷贝：
 * 渲染线程按 seq 取走，拷贝期间不会读到半新半旧的帧。 */
static void preview_fill(const unsigned char *y)
{
    memcpy(g_prev, y, sizeof g_prev);
    __sync_synchronize();
    g_prev_seq++;
}

/* ---------- 采集线程 ---------- */

static void cam_fail(int err)
{
    g_err = err;
    g_state = CS_ERROR;
    g_stop = 1;                     /* 让解码线程一起收摊 */
}

static int cap_thread(SceSize args, void *argp)
{
    SceUID mbid = -1;
    void *base = NULL;
    unsigned char *pI, *pU, *pV;
    size_t ysize = (size_t)CAM_W * CAM_H;
    size_t usize = ysize / 4;
    SceCameraInfo info;
    int ret, i, read_err = 0;

    (void)args;
    (void)argp;

    mbid = sceKernelAllocMemBlock("psvsend_scan", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                  CAM_MEM_SIZE, NULL);
    if (mbid < 0) {
        dlog("camscan: memblock alloc failed 0x%08X", (unsigned)mbid);
        cam_fail((int)mbid);
        return 0;
    }
    if (sceKernelGetMemBlockBase(mbid, &base) < 0 || !base) {
        dlog("camscan: memblock base failed");
        sceKernelFreeMemBlock(mbid);
        cam_fail(-1);
        return 0;
    }

    pI = (unsigned char *)base;
    pU = pI + ((ysize + CAM_PLANE_ALIGN - 1) / CAM_PLANE_ALIGN) * CAM_PLANE_ALIGN;
    pV = pU + ((usize + CAM_PLANE_ALIGN - 1) / CAM_PLANE_ALIGN) * CAM_PLANE_ALIGN;

    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = CAM_FRAMERATE;
    info.width = CAM_W;
    info.height = CAM_H;
    info.sizeIBase = (SceSize)ysize;
    info.sizeUBase = (SceSize)usize;
    info.sizeVBase = (SceSize)usize;
    info.pIBase = pI;
    info.pUBase = pU;
    info.pVBase = pV;
    info.pitch = 0;
    info.buffer = 0;

    ret = sceCameraOpen(CAM_DEV, &info);
    dlog("camscan: open ret=0x%08X", (unsigned)ret);
    if (ret < 0) {
        sceKernelFreeMemBlock(mbid);
        cam_fail(ret);
        return 0;
    }
    ret = sceCameraStart(CAM_DEV);
    dlog("camscan: start ret=0x%08X", (unsigned)ret);
    if (ret < 0) {
        sceCameraClose(CAM_DEV);
        sceKernelFreeMemBlock(mbid);
        cam_fail(ret);
        return 0;
    }

    /* 预热：自动曝光/白平衡稳定前的帧不参与预览与解码 */
    for (i = 0; i < CAM_WARMUP && !g_stop; i++) {
        SceCameraRead rd;
        memset(&rd, 0, sizeof rd);
        rd.size = sizeof rd;
        rd.mode = 0;
        sceCameraRead(CAM_DEV, &rd);
    }

    while (!g_stop) {
        SceCameraRead rd;
        const unsigned char *y;

        memset(&rd, 0, sizeof rd);
        rd.size = sizeof rd;
        rd.mode = 0;                        /* 阻塞：拿到一帧新数据再往下走 */
        ret = sceCameraRead(CAM_DEV, &rd);
        if (ret < 0) {
            if (read_err < 3)
                dlog("camscan: read fail 0x%08X", (unsigned)ret);
            if (++read_err > 30) {
                dlog("camscan: read keeps failing, stop");
                cam_fail(ret);
                break;
            }
            continue;
        }
        read_err = 0;
        y = rd.pIBase ? (const unsigned char *)rd.pIBase : pI;

        lock();
        memcpy(g_y, y, ysize);
        unlock();

        if (!g_frozen)
            preview_fill(y);
        g_frame++;
    }

    sceCameraStop(CAM_DEV);
    sceCameraClose(CAM_DEV);
    sceKernelFreeMemBlock(mbid);
    dlog("camscan: capture thread exit");
    return 0;
}

/* ---------- 解码线程 ---------- */

/* 解一帧：返回候选数，文本/包围盒写进 out（最多 max 个）。 */
static int decode_frame(struct quirc *qr, CamScanItem *out, int max)
{
    unsigned char *qb;
    int n, i, cnt = 0;
    uint64_t t0;

    if (!qr) return 0;

    qb = quirc_begin(qr, NULL, NULL);
    lock();
    memcpy(qb, g_y, sizeof g_y);
    unlock();

    t0 = now_us();
    quirc_end(qr);
    n = quirc_count(qr);
    for (i = 0; i < n && cnt < max; i++) {
        struct quirc_code code;
        struct quirc_data data;
        int k, len, dup = 0;

        quirc_extract(qr, i, &code);
        if (quirc_decode(&code, &data) != QUIRC_SUCCESS) continue;
        if (data.payload_len <= 0) continue;

        /* 同一个码可能被重复识别（多候选），按 payload 去重 */
        for (k = 0; k < cnt; k++) {
            size_t l = strlen(out[k].text);
            if (l == (size_t)data.payload_len &&
                memcmp(out[k].text, data.payload, l) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) continue;

        len = data.payload_len;
        if (len > CAM_SCAN_TEXT - 1) len = CAM_SCAN_TEXT - 1;
        memcpy(out[cnt].text, data.payload, (size_t)len);
        out[cnt].text[len] = 0;

        /* 包围盒：quirc 的四个角点（左上起顺时针），给 UI 画框用 */
        {
            int x0 = code.corners[0].x, y0 = code.corners[0].y;
            int x1 = x0, y1 = y0, c;
            for (c = 1; c < 4; c++) {
                if (code.corners[c].x < x0) x0 = code.corners[c].x;
                if (code.corners[c].y < y0) y0 = code.corners[c].y;
                if (code.corners[c].x > x1) x1 = code.corners[c].x;
                if (code.corners[c].y > y1) y1 = code.corners[c].y;
            }
            out[cnt].box[0] = x0;
            out[cnt].box[1] = y0;
            out[cnt].box[2] = x1;
            out[cnt].box[3] = y1;
        }
        cnt++;
    }
    g_dec_ms = (int)((now_us() - t0) / 1000);
    return cnt;
}

static int dec_thread(SceSize args, void *argp)
{
    struct quirc *qr;
    CamScanItem tmp[CAM_SCAN_MAX];

    (void)args;
    (void)argp;

    qr = quirc_new();
    if (qr && quirc_resize(qr, CAM_W, CAM_H) < 0) {
        quirc_destroy(qr);
        qr = NULL;
    }
    if (!qr)
        dlog("camscan: quirc init failed (decoding disabled)");

    while (!g_stop) {
        int n;
        if (!g_hold || g_frozen) {
            sceKernelDelayThread(10 * 1000);
            continue;
        }
        n = decode_frame(qr, tmp, CAM_SCAN_MAX);
        if (n > 0) {
            lock();
            memcpy(g_items, tmp, sizeof tmp[0] * (size_t)n);
            g_n = n;
            g_frozen = 1;
            unlock();
        }
    }
    if (qr) quirc_destroy(qr);
    dlog("camscan: decode thread exit");
    return 0;
}

/* ---------- 对外接口 ---------- */

bool cam_scan_start(void)
{
    if (g_state == CS_RUNNING)
        return false;
    if (g_stuck) {
        /* 上一轮线程卡死、相机句柄还悬着：明确报错，别让页面停在"启动中" */
        g_err = -2;
        g_state = CS_ERROR;
        return false;
    }
    if (g_cap_thid >= 0 || g_dec_thid >= 0)
        cam_scan_stop();

    if (g_mtx < 0) {
        g_mtx = sceKernelCreateMutex("psvsend_scan", 0, 0, NULL);
        if (g_mtx < 0)
            dlog("camscan: create mutex fail 0x%08X", (unsigned)g_mtx);
    }

    g_stop = 0;
    g_hold = 0;
    g_frozen = 0;
    g_n = 0;
    g_frame = 0;
    g_dec_ms = 0;
    g_err = 0;
    g_prev_seq = 0;
    g_prev_tex_seq = 0;     /* 纹理还留着上一轮的画面：让第一帧必须重新上传 */
    g_state = CS_RUNNING;

    g_dec_thid = sceKernelCreateThread("psvsend_scan_dec", dec_thread, 0x40, 0x40000,
                                       0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    g_cap_thid = sceKernelCreateThread("psvsend_scan_cap", cap_thread, 0x40, 0x40000,
                                       0, SCE_KERNEL_CPU_MASK_USER_ALL, NULL);
    if (g_dec_thid < 0 || g_cap_thid < 0) {
        dlog("camscan: thread create failed cap=0x%08X dec=0x%08X",
             (unsigned)g_cap_thid, (unsigned)g_dec_thid);
        if (g_cap_thid >= 0) { sceKernelDeleteThread(g_cap_thid); g_cap_thid = -1; }
        if (g_dec_thid >= 0) { sceKernelDeleteThread(g_dec_thid); g_dec_thid = -1; }
        g_err = -1;
        g_state = CS_ERROR;
        return false;
    }
    sceKernelStartThread(g_dec_thid, 0, NULL);
    sceKernelStartThread(g_cap_thid, 0, NULL);
    dlog("camscan: begin 640x480");
    return true;
}

void cam_scan_stop(void)
{
    SceUID *ids[2];
    int i, k;

    if (g_cap_thid < 0 && g_dec_thid < 0)
        return;

    g_stop = 1;
    ids[0] = &g_cap_thid;
    ids[1] = &g_dec_thid;
    /* 读帧阻塞最多等一帧（~33ms），解码一帧最多 ~150ms；给足 3s。
     * 超时就不硬删（线程还握着相机/缓冲句柄），标记 stuck 不再重开。 */
    for (k = 0; k < 2; k++) {
        if (*ids[k] < 0) continue;
        for (i = 0; i < 600; i++) {
            SceUInt to = 5000;
            if (sceKernelWaitThreadEnd(*ids[k], NULL, &to) == 0) {
                sceKernelDeleteThread(*ids[k]);
                *ids[k] = -1;
                break;
            }
        }
        if (*ids[k] >= 0) {
            dlog("camscan: thread won't stop, handle leaked");
            g_stuck = 1;
        }
    }
    g_hold = 0;
    g_frozen = 0;
    g_n = 0;
    g_state = CS_IDLE;
}

void cam_scan_set_hold(bool hold)
{
    g_hold = hold ? 1 : 0;
}

void cam_scan_clear_hit(void)
{
    lock();
    g_n = 0;
    g_frozen = 0;
    unlock();
}

void cam_scan_snapshot(CamScanSnap *out)
{
    if (!out) return;
    lock();
    out->state = g_state;
    out->err = g_err;
    out->frozen = g_frozen;
    out->n = g_n;
    out->frame = g_frame;
    out->dec_ms = g_dec_ms;
    memcpy(out->items, g_items, sizeof g_items);
    unlock();
}

/* ---------- 预览 ---------- */

void cam_scan_pre_frame(void)
{
    /* 灰度 → ABGR 的查表（307k 像素/帧，省掉每像素的移位/或运算） */
    static uint32_t g_lut[256];
    static int g_lut_ready;
    int seq = g_prev_seq;
    int stride, x, y;
    uint32_t *dp;

    if (seq <= 0 || seq == g_prev_tex_seq) return;
    if (!g_lut_ready) {
        for (x = 0; x < 256; x++)
            g_lut[x] = 0xFF000000u | ((uint32_t)x << 16) |
                       ((uint32_t)x << 8) | (uint32_t)x;
        g_lut_ready = 1;
    }
    if (!g_prev_tex) {
        g_prev_tex = vita2d_create_empty_texture_format(
            CAM_SCAN_PREV_W, CAM_SCAN_PREV_H, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
        if (!g_prev_tex) {
            dlog("camscan: preview texture create failed");
            g_prev_tex_seq = seq;
            return;
        }
        /* 预览是 640×480 原帧、画到 533×400（缩小），线性过滤更稳；
         * 二维码识别用原帧，与这里的显示过滤无关。 */
        vita2d_texture_set_filters(g_prev_tex, SCE_GXM_TEXTURE_FILTER_LINEAR,
                                   SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    stride = vita2d_texture_get_stride(g_prev_tex);
    dp = (uint32_t *)vita2d_texture_get_datap(g_prev_tex);
    for (y = 0; y < CAM_SCAN_PREV_H; y++) {
        uint32_t *row = (uint32_t *)((unsigned char *)dp + (size_t)y * stride);
        const unsigned char *s = g_prev + y * CAM_SCAN_PREV_W;
        for (x = 0; x < CAM_SCAN_PREV_W; x++)
            row[x] = g_lut[s[x]];
    }
    g_prev_tex_seq = seq;
}

struct vita2d_texture *cam_scan_preview_tex(void)
{
    return g_prev_tex;
}
