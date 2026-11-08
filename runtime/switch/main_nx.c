// MSR-nx: the Switch platform layer. Boots the game's translated code on its own threads, shows
// the two 3DS screens with deko3d, plays the DSP mix through audout and feeds the Joy-Cons to hid.
//
// Bring-up version: the 3DS GPU work is drawn by the reference renderer on the CPU (correct but
// slow); the deko3d PICA renderer replaces it next. Files live in sdmc:/switch/msr/.
#include <deko3d.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include "boot.h"
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "sched.h"

#define DIR "sdmc:/switch/msr"

void msr_run(Cpu* c, u32 entry, u32 stop);
const char* msr_trace_ring[64];
unsigned msr_trace_pos;
void msr_dump_trace(void) {}

extern int msr_full_log;
extern int sched_realtime;
extern u64 msr_time_limit_ns;
extern unsigned msr_svc_limit;
extern char msr_save_dir[512];
extern u32 msr_buttons;
extern s16 msr_circle_x, msr_circle_y;
extern int msr_touch_down;
extern u16 msr_touch_x, msr_touch_y;
extern s16 msr_audio_ring[];
extern volatile u32 msr_audio_write;
typedef struct { u32 left, right, stride, format; unsigned swaps; } MsrFramebuffer;
extern MsrFramebuffer msr_fb[2];

// ---- the game ------------------------------------------------------------------------------------

static Cpu* g_main_cpu;
static void* game_main(void* arg) {
    (void)arg;
    void sched_enter_main(void);
    sched_enter_main();
    msr_run(g_main_cpu, MSR_CODE_BASE, 0xFFFFFFF0u);
    msr_receipt("the game's entry function returned");
    msr_log_close();
    exit(0);
    return 0;
}

// ---- display ---------------------------------------------------------------------------------------

#define FB_W 1280
#define FB_H 720
static DkDevice g_dev;
static DkQueue g_queue;
static DkSwapchain g_swap;
static DkMemBlock g_fb_mem, g_cmd_mem, g_tex_mem, g_up_mem;
static DkImage g_fb[2], g_scr[2];
static DkCmdBuf g_cmd;
static u32* g_upload;   // CPU-visible staging: top 400x240 then bottom 320x240, RGBA8
static Mutex g_qlock;   // the game's renderer and the presenter share the queue
void dkr_init(DkDevice dev, DkQueue q, Mutex* qlock);

static void display_init(void) {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft;   // the PICA's conventions
    g_dev = dkDeviceCreate(&dm);

    DkImageLayoutMaker lm;
    DkImageLayout fbl;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = FB_W;
    lm.dimensions[1] = FB_H;
    dkImageLayoutInitialize(&fbl, &lm);
    u32 fsz = (u32)dkImageLayoutGetSize(&fbl), falign = dkImageLayoutGetAlignment(&fbl);
    fsz = (fsz + falign - 1) & ~(falign - 1);
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, g_dev, (2 * fsz + 0xFFFF) & ~0xFFFFu);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_fb_mem = dkMemBlockCreate(&mm);
    DkImage const* list[2];
    for (int k = 0; k < 2; k++) { dkImageInitialize(&g_fb[k], &fbl, g_fb_mem, k * fsz); list[k] = &g_fb[k]; }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev, nwindowGetDefault(), list, 2);
    g_swap = dkSwapchainCreate(&sm);

    // the two screen images, sources for the scaled blits
    DkImageLayout sl[2];
    u32 off = 0, offs[2];
    for (int k = 0; k < 2; k++) {
        dkImageLayoutMakerDefaults(&lm, g_dev);
        lm.flags = DkImageFlags_Usage2DEngine;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = k ? 320 : 400;
        lm.dimensions[1] = 240;
        dkImageLayoutInitialize(&sl[k], &lm);
        u32 a = dkImageLayoutGetAlignment(&sl[k]);
        off = (off + a - 1) & ~(a - 1);
        offs[k] = off;
        off += (u32)dkImageLayoutGetSize(&sl[k]);
    }
    dkMemBlockMakerDefaults(&mm, g_dev, (off + 0xFFFF) & ~0xFFFFu);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_tex_mem = dkMemBlockCreate(&mm);
    for (int k = 0; k < 2; k++) dkImageInitialize(&g_scr[k], &sl[k], g_tex_mem, offs[k]);

    dkMemBlockMakerDefaults(&mm, g_dev, 0x80000);   // (400 + 320) * 240 * 4 = 691200 > 0x80000? use 1 MiB
    mm.size = 0x100000;
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_up_mem = dkMemBlockCreate(&mm);
    g_upload = (u32*)dkMemBlockGetCpuAddr(g_up_mem);

    dkMemBlockMakerDefaults(&mm, g_dev, 0x10000);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_cmd_mem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    g_cmd = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(g_cmd, g_cmd_mem, 0, 0x10000);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics;
    g_queue = dkQueueCreate(&qm);
}

// An LCD framebuffer is 240 pixels per column, columns left to right, each column bottom to top.
static void convert_screen(int screen, u32* out) {
    MsrFramebuffer* f = &msr_fb[screen];
    u32 W = screen ? 320 : 400, H = 240, fmt = f->format & 7;
    if (!f->left) { memset(out, 0, W * H * 4); return; }
    u32 bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    u32 stride = f->stride ? f->stride : 240 * bpp;
    const u8* base = g_mem + f->left;
    for (u32 x = 0; x < W; x++) {
        const u8* col = base + x * stride;
        for (u32 y = 0; y < H; y++) {
            const u8* p = col + (H - 1 - y) * bpp;
            u32 r, g, b;
            u16 h = (u16)(p[0] | (bpp > 1 ? p[1] << 8 : 0));
            switch (fmt) {
            case 0: r = p[3]; g = p[2]; b = p[1]; break;
            case 1: r = p[2]; g = p[1]; b = p[0]; break;
            case 3: r = ((h >> 11) & 31) << 3; g = ((h >> 5) & 63) << 2; b = (h & 31) << 3; break;
            case 2: r = ((h >> 11) & 31) << 3; g = ((h >> 6) & 31) << 3; b = ((h >> 1) & 31) << 3; break;
            default: r = (h >> 12) << 4; g = ((h >> 8) & 15) << 4; b = ((h >> 4) & 15) << 4; break;
            }
            out[y * W + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
}

void dkr_frame_end(void);
static int g_gpu_render;
static void present(void) {
    if (g_gpu_render) dkr_frame_end();
    convert_screen(0, g_upload);
    convert_screen(1, g_upload + 400 * 240);
    mutexLock(&g_qlock);
    int slot = dkQueueAcquireImage(g_queue, g_swap);
    mutexUnlock(&g_qlock);
    DkImageView fbv, sv[2];
    dkImageViewDefaults(&fbv, &g_fb[slot]);
    dkCmdBufBindRenderTarget(g_cmd, &fbv, NULL);
    dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    DkGpuAddr up = dkMemBlockGetGpuAddr(g_up_mem);
    for (int k = 0; k < 2; k++) {
        dkImageViewDefaults(&sv[k], &g_scr[k]);
        u32 W = k ? 320 : 400;
        DkCopyBuf src = {up + (k ? 400 * 240 * 4 : 0), 0, 0};
        DkImageRect r = {0, 0, 0, W, 240, 1};
        dkCmdBufCopyBufferToImage(g_cmd, &src, &sv[k], &r, 0);
    }
    // layout: the top screen large on the left (2.4x, 960x576), the bottom screen on the right (1x)
    DkImageRect ts = {0, 0, 0, 400, 240, 1}, td = {0, 72, 0, 960, 576, 1};
    DkImageRect bs = {0, 0, 0, 320, 240, 1}, bd = {960, 240, 0, 320, 240, 1};
    dkCmdBufBlitImage(g_cmd, &sv[0], &ts, &fbv, &td, DkBlitFlag_FilterLinear, 0);
    dkCmdBufBlitImage(g_cmd, &sv[1], &bs, &fbv, &bd, DkBlitFlag_FilterLinear, 0);
    mutexLock(&g_qlock);
    dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(g_cmd));
    dkQueuePresentImage(g_queue, g_swap, slot);
    dkQueueWaitIdle(g_queue);
    mutexUnlock(&g_qlock);
    dkCmdBufClear(g_cmd);
}

// ---- audio: the DSP's 32728 Hz mix, resampled to 48 kHz -----------------------------------------

#define AUD_SAMPLES 1440   // 30 ms per buffer
#define RING (32768 * 2)
static AudioOutBuffer g_abuf[3];
static s16* g_adata[3];

static void* audio_thread(void* arg) {
    (void)arg;
    u32 size = (AUD_SAMPLES * 4 + 0xFFF) & ~0xFFFu;
    for (int k = 0; k < 3; k++) {
        g_adata[k] = (s16*)memalign(0x1000, size);
        memset(g_adata[k], 0, size);
        g_abuf[k].buffer = g_adata[k];
        g_abuf[k].buffer_size = size;
        g_abuf[k].data_size = AUD_SAMPLES * 4;
        g_abuf[k].data_offset = 0;
        audoutAppendAudioOutBuffer(&g_abuf[k]);
    }
    double pos = 0;   // read position in DSP samples
    for (;;) {
        AudioOutBuffer* done = 0;
        u32 n = 0;
        audoutWaitPlayFinish(&done, &n, UINT64_MAX);
        if (!done) continue;
        s16* d = (s16*)done->buffer;
        u32 w = msr_audio_write;
        // keep about two buffers of latency; jump if far behind or ahead
        double lag = (double)w - pos;
        if (lag > 16384 || lag < 0) pos = w > 3000 ? w - 3000 : 0;
        double step = 32728.0 / 48000.0;
        for (int i = 0; i < AUD_SAMPLES; i++) {
            u32 p = (u32)pos;
            if (p >= w) { d[2 * i] = d[2 * i + 1] = 0; continue; }   // starved: silence
            u32 q = p % RING;
            d[2 * i] = msr_audio_ring[2 * q];
            d[2 * i + 1] = msr_audio_ring[2 * q + 1];
            pos += step;
        }
        audoutAppendAudioOutBuffer(done);
    }
    return 0;
}

// ---- input ------------------------------------------------------------------------------------------

static PadState g_pad;

static void input_poll(void) {
    padUpdate(&g_pad);
    u64 k = padGetButtons(&g_pad);
    u32 b = 0;
    if (k & HidNpadButton_A) b |= 1 << 0;
    if (k & HidNpadButton_B) b |= 1 << 1;
    if (k & HidNpadButton_Minus) b |= 1 << 2;   // SELECT
    if (k & HidNpadButton_Plus) b |= 1 << 3;    // START
    if (k & HidNpadButton_Right) b |= 1 << 4;
    if (k & HidNpadButton_Left) b |= 1 << 5;
    if (k & HidNpadButton_Up) b |= 1 << 6;
    if (k & HidNpadButton_Down) b |= 1 << 7;
    if (k & HidNpadButton_R) b |= 1 << 8;
    if (k & HidNpadButton_L) b |= 1 << 9;
    if (k & HidNpadButton_X) b |= 1 << 10;
    if (k & HidNpadButton_Y) b |= 1 << 11;
    msr_buttons = b;
    HidAnalogStickState s = padGetStickPos(&g_pad, 0);
    msr_circle_x = (s16)(s.x * 156 / 32767);
    msr_circle_y = (s16)(s.y * 156 / 32767);
    // touch screen (handheld): the bottom screen is drawn at (960, 240), 1:1
    HidTouchScreenState ts = {0};
    msr_touch_down = 0;
    if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        int x = (int)ts.touches[0].x - 960, y = (int)ts.touches[0].y - 240;
        if (x >= 0 && x < 320 && y >= 0 && y < 240) { msr_touch_down = 1; msr_touch_x = (u16)x; msr_touch_y = (u16)y; }
    }
}

// ---- main -------------------------------------------------------------------------------------------

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    appletLockExit();
    mkdir(DIR, 0777);
    struct stat st;
    msr_full_log = stat(DIR "/fulllog", &st) == 0;   // create this file to get the detailed msr.log
    msr_log_open(DIR);
    int cpu_render = stat(DIR "/cpurender", &st) == 0;   // create this file to draw with the CPU reference renderer
    msr_receipt("MSR-nx Switch test build 1 (%s)", cpu_render ? "CPU reference renderer" : "deko3d PICA renderer");
    snprintf(msr_save_dir, sizeof msr_save_dir, DIR "/save");
    sched_realtime = 1;
    { extern size_t sched_thread_stack; sched_thread_stack = 4u << 20; }
    msr_time_limit_ns = ~0ull;
    msr_svc_limit = ~0u;

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
    mutexInit(&g_qlock);
    display_init();
    if (!cpu_render) { dkr_init(g_dev, g_queue, &g_qlock); g_gpu_render = 1; }
    if (R_SUCCEEDED(audoutInitialize()) && R_SUCCEEDED(audoutStartAudioOut())) {
        pthread_t at;
        pthread_create(&at, 0, audio_thread, 0);
    } else msr_receipt("audio: audout did not start");

    g_main_cpu = msr_boot(DIR);
    if (!g_main_cpu) {
        msr_log_close();
        appletUnlockExit();
        return 1;
    }
    // msr_boot took the baton for the main guest thread on this host thread; the game runs on its
    // own thread with a large stack, so hand the baton over by running it there.
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 16u << 20);
    pthread_t gt;
    void sched_handoff_main(void);
    sched_handoff_main();
    pthread_create(&gt, &at, game_main, 0);

    while (appletMainLoop()) {
        input_poll();
        present();
        if ((padGetButtons(&g_pad) & (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_R)) ==
            (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_R))
            break;   // L + R + plus + minus: quit
        svcSleepThread(16666667);
    }
    msr_receipt("quit");
    msr_log_close();
    appletUnlockExit();
    exit(0);
}
