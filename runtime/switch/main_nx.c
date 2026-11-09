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
    // scratch memory for the interpreter vertex shader's spills (43520 bytes per warp); the translated
    // shaders need none, but a configuration missing from the shader pack falls back to it
    qm.perWarpScratchMemorySize = 0xAA00;
    g_queue = dkQueueCreate(&qm);
}

// An LCD framebuffer is 240 pixels per column, columns left to right, each column bottom to top.
static void convert_screen(int screen, u32* out, int flip) {
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
            // Rows go in bottom to top: the device uses OriginLowerLeft (the PICA's convention), so
            // deko3d tells the compositor to flip the whole window vertically (HAL_TRANSFORM_FLIP_V,
            // deko3d source/dk_swapchain.cpp). Our 2D-engine blits copy rows in memory order, so
            // the picture has to be stored upside down to come out the right way up.
            out[(flip ? H - 1 - y : y) * W + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
}

void dkr_frame_end(void);
static int g_gpu_render;
static void present(void) {
    if (g_gpu_render) dkr_frame_end();
    // only convert the screens when the game showed a new picture on one of them
    static unsigned seen[2] = {~0u, ~0u};
    if (msr_fb[0].swaps == seen[0] && msr_fb[1].swaps == seen[1]) return;
    seen[0] = msr_fb[0].swaps; seen[1] = msr_fb[1].swaps;
    MSR_PERF_BEGIN(PERF_PRESENT);
    convert_screen(0, g_upload, 1);
    convert_screen(1, g_upload + 400 * 240, 1);
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
    MSR_PERF_END(PERF_PRESENT);
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
static char g_run_dir[128];
// The shader pack, built into msr.nro by tools/switch/nxbuild.sh when the build has one.
extern const u8 msr_pack_data[] __attribute__((weak));
extern const u8 msr_pack_end[] __attribute__((weak));

// ---- the comparison cycle ---------------------------------------------------------------------------
// Test builds switch the renderer's shader choice every 20 seconds by themselves, so one launch
// measures and pictures every combination: no files to move between runs.
typedef struct { int vs_interp, fs_general; const char* name; } Mode;
static const Mode k_modes[] = {
    {0, 0, "translated vertex shaders, specialized fragment shaders"},
    {1, 0, "interpreter vertex shader, specialized fragment shaders"},
    {0, 1, "translated vertex shaders, general fragment shader"},
    {1, 1, "interpreter vertex shader, general fragment shader (as test 2)"},
};
#define MODE_SECONDS 20

int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h);
static u32 g_shot_buf[400 * 240];
static u8 g_shot_rgb[400 * 240 * 3];
static void convert_screen(int screen, u32* out, int flip);
// Both screens as the game drew them, straight from its framebuffers.
static void save_shots(const char* tag) {
    for (int sc = 0; sc < 2; sc++) {
        u32 W = sc ? 320 : 400;
        convert_screen(sc, g_shot_buf, 0);
        for (u32 i = 0; i < W * 240; i++) {
            g_shot_rgb[i * 3] = g_shot_buf[i] & 0xFF;
            g_shot_rgb[i * 3 + 1] = (g_shot_buf[i] >> 8) & 0xFF;
            g_shot_rgb[i * 3 + 2] = (g_shot_buf[i] >> 16) & 0xFF;
        }
        char path[300];
        snprintf(path, sizeof path, "%s/%s_%s.png", g_run_dir, tag, sc ? "bottom" : "top");
        msr_write_png(path, g_shot_rgb, W, 240);
    }
    msr_receipt("  pictures saved: %s_top.png, %s_bottom.png", tag, tag);
}

// Clock speeds as the system has them now (sys-clk changes included).
static void clocks(u32* cpu, u32* gpu, u32* mem) {
    *cpu = *gpu = *mem = 0;
    if (hosversionAtLeast(8, 0, 0)) {
        static int ok = -1;
        if (ok < 0) ok = R_SUCCEEDED(clkrstInitialize());
        if (!ok) return;
        PcvModuleId ids[3] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
        u32* outs[3] = {cpu, gpu, mem};
        for (int k = 0; k < 3; k++) {
            ClkrstSession s;
            if (R_SUCCEEDED(clkrstOpenSession(&s, ids[k], 3))) {
                clkrstGetClockRate(&s, outs[k]);
                clkrstCloseSession(&s);
            }
        }
    } else {
        static int ok = -1;
        if (ok < 0) ok = R_SUCCEEDED(pcvInitialize());
        if (!ok) return;
        pcvGetClockRate(PcvModule_CpuBus, cpu);
        pcvGetClockRate(PcvModule_GPU, gpu);
        pcvGetClockRate(PcvModule_EMC, mem);
    }
}
static unsigned long long g_start_ns;

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
    // the first presses go to the receipt, to check that the game is being told about them
    static u32 last_b;
    static unsigned logged;
    if (b != last_b && logged < 60) {
        logged++;
        msr_receipt("input: buttons %03X at %.1f s (game has shown %u top-screen pictures)", b,
                    (msr_now_ns() - g_start_ns) / 1e9, msr_fb[0].swaps);
    }
    last_b = b;
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
    // every launch gets its own folder for the receipt, pictures and anything else it writes
    mkdir(DIR "/switchlogs", 0777);
    for (int n = 1; n < 1000; n++) {
        snprintf(g_run_dir, sizeof g_run_dir, DIR "/switchlogs/run%03d", n);
        if (mkdir(g_run_dir, 0777) == 0) break;
    }
    msr_log_open(g_run_dir);
    int cpu_render = stat(DIR "/cpurender", &st) == 0;   // create this file to draw with the CPU reference renderer
    msr_receipt("MSR-nx Switch test build 4b (%s)", cpu_render ? "CPU reference renderer" : "deko3d PICA renderer");
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
    if (!cpu_render) {
        dkr_init(g_dev, g_queue, &g_qlock);
        void dkr_load_pack(const u8* data, size_t size, const char* new_path, const char* fs_new_path);
        char vs_new[300], fs_new[300];
        snprintf(vs_new, sizeof vs_new, "%s/vs_new.bin", g_run_dir);
        snprintf(fs_new, sizeof fs_new, "%s/fs_new.bin", g_run_dir);
        dkr_load_pack(msr_pack_data, msr_pack_data ? (size_t)(msr_pack_end - msr_pack_data) : 0, vs_new, fs_new);
        g_gpu_render = 1;
    }
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

    g_start_ns = msr_now_ns();
    unsigned long long next_report = g_start_ns + 5000000000ull;
    unsigned last_swaps = 0;
    int mode = 0, reports = 0;
    unsigned long long shot_at = g_start_ns + 5000000000ull;
    void dkr_force_general(int vs, int fs);
    if (g_gpu_render) {
        msr_receipt("mode 1 of %d: %s (modes change every %d s)", (int)(sizeof k_modes / sizeof k_modes[0]),
                    k_modes[0].name, MODE_SECONDS);
        dkr_force_general(0, 0);
    }
    while (appletMainLoop()) {
        input_poll();
        present();
        unsigned long long now = msr_now_ns();
        if (now >= next_report) {
            // the speed meter: the real 3DS shows up to 60 pictures a second on the top screen
            double secs = (now - next_report + 5000000000ull) / 1e9;
            unsigned sw = msr_fb[0].swaps;
            u32 ccpu, cgpu, cmem;
            clocks(&ccpu, &cgpu, &cmem);
            msr_receipt("speed at %.0f s (mode %d): %.1f top-screen pictures per second; clocks CPU %u MHz, GPU %u MHz, memory %u MHz",
                        (now - g_start_ns) / 1e9, mode + 1, (sw - last_swaps) / secs, ccpu / 1000000, cgpu / 1000000,
                        cmem / 1000000);
            msr_perf_report(secs);
            if (g_gpu_render) {
                void dkr_vs_stats(unsigned* hits, unsigned* misses);
                unsigned h, m;
                dkr_vs_stats(&h, &m);
                msr_receipt("  vertex shaders: %u draws translated, %u on the interpreter", h, m);
                void dkr_fs_stats(unsigned* hits, unsigned* misses);
                dkr_fs_stats(&h, &m);
                msr_receipt("  fragment shaders: %u draws specialized, %u on the general one", h, m);
            }
            last_swaps = sw;
            next_report = now + 5000000000ull;
            if (g_gpu_render && ++reports % (MODE_SECONDS / 5) == 0) {
                mode = (mode + 1) % (int)(sizeof k_modes / sizeof k_modes[0]);
                dkr_force_general(k_modes[mode].vs_interp, k_modes[mode].fs_general);
                msr_receipt("mode %d: %s", mode + 1, k_modes[mode].name);
                shot_at = now + 5000000000ull;
            }
        }
        if (shot_at && now >= shot_at) {
            char tag[48];
            snprintf(tag, sizeof tag, "t%03d_mode%d", (int)((now - g_start_ns) / 1000000000ull), mode + 1);
            save_shots(tag);
            shot_at = 0;
        }
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
