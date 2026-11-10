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
#include "frame_plan.h"
#include "audio_stretch.h"
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "sched.h"
#include "port_menu.h"
#include "dk_shaders.h"   // generated (tools/switch/mkshaders.py): hud_vsh, hud_fsh

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
static DkMemBlock g_fb_mem, g_cmd_mem, g_tex_mem, g_up_mem, g_panel_mem, g_fps_mem;
static DkImage g_fb[2], g_scr[2], g_panel_img, g_fps_img;
// single-screen HUD: its shaders, a uniform block, and the bottom screen's descriptors
static DkMemBlock g_hud_code, g_hud_data;
static DkShader g_hud_vsh, g_hud_fsh;
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

    // the port menu's panel: CPU-drawn pixels (staging, then an image for the blit)
    {
        DkImageLayout pl;
        dkImageLayoutMakerDefaults(&lm, g_dev);
        lm.flags = DkImageFlags_Usage2DEngine;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = PANEL_W;
        lm.dimensions[1] = PANEL_H;
        dkImageLayoutInitialize(&pl, &lm);
        u32 psz = (u32)dkImageLayoutGetSize(&pl), pal = dkImageLayoutGetAlignment(&pl);
        u32 img_off = (PANEL_W * PANEL_H * 4 + pal - 1) & ~(pal - 1);
        dkMemBlockMakerDefaults(&mm, g_dev, (img_off + psz + 0xFFFF) & ~0xFFFFu);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
        g_panel_mem = dkMemBlockCreate(&mm);
        dkImageInitialize(&g_panel_img, &pl, g_panel_mem, img_off);
        // and the frame counter's
        dkImageLayoutMakerDefaults(&lm, g_dev);
        lm.flags = DkImageFlags_Usage2DEngine;
        lm.format = DkImageFormat_RGBA8_Unorm;
        lm.dimensions[0] = FPS_W;
        lm.dimensions[1] = FPS_H;
        dkImageLayoutInitialize(&pl, &lm);
        psz = (u32)dkImageLayoutGetSize(&pl);
        pal = dkImageLayoutGetAlignment(&pl);
        img_off = (FPS_W * FPS_H * 4 + pal - 1) & ~(pal - 1);
        dkMemBlockMakerDefaults(&mm, g_dev, (img_off + psz + 0xFFFF) & ~0xFFFFu);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
        g_fps_mem = dkMemBlockCreate(&mm);
        dkImageInitialize(&g_fps_img, &pl, g_fps_mem, img_off);
    }

    {   // the single-screen HUD's shaders and data
        u32 vsz = (sizeof hud_vsh_dksh + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
        dkMemBlockMakerDefaults(&mm, g_dev, (vsz + sizeof hud_fsh_dksh + DK_SHADER_CODE_UNUSABLE_SIZE + 0xFFFF) & ~0xFFFFu);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
        g_hud_code = dkMemBlockCreate(&mm);
        u8* code = (u8*)dkMemBlockGetCpuAddr(g_hud_code);
        memcpy(code, hud_vsh_dksh, sizeof hud_vsh_dksh);
        memcpy(code + vsz, hud_fsh_dksh, sizeof hud_fsh_dksh);
        DkShaderMaker sm2;
        dkShaderMakerDefaults(&sm2, g_hud_code, 0);
        dkShaderInitialize(&g_hud_vsh, &sm2);
        dkShaderMakerDefaults(&sm2, g_hud_code, vsz);
        dkShaderInitialize(&g_hud_fsh, &sm2);
        // 0x000 uniforms (32 bytes), 0x100 image descriptor, 0x200 sampler descriptor
        dkMemBlockMakerDefaults(&mm, g_dev, 0x1000);
        mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        g_hud_data = dkMemBlockCreate(&mm);
        u8* d = (u8*)dkMemBlockGetCpuAddr(g_hud_data);
        DkImageView iv;
        dkImageViewDefaults(&iv, &g_scr[1]);
        dkImageDescriptorInitialize((DkImageDescriptor*)(d + 0x100), &iv, false, false);
        DkSampler smp;
        dkSamplerDefaults(&smp);   // nearest: the readouts stay as sharp as the game drew them
        dkSamplerDescriptorInitialize((DkSamplerDescriptor*)(d + 0x200), &smp);
    }

    dkMemBlockMakerDefaults(&mm, g_dev, 0x10000);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_cmd_mem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev);
    g_cmd = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(g_cmd, g_cmd_mem, 0, 0x10000);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_Compute;   // the VRAM kernels and the screen conversion are compute work
    // scratch memory for the interpreter vertex shader's spills (43520 bytes per warp); the translated
    // shaders need none, but a configuration missing from the shader pack falls back to it
    qm.perWarpScratchMemorySize = 0xE000;   // the interpreter vertex shader spills 57344 bytes per warp (check the .dksh when it changes)
    g_queue = dkQueueCreate(&qm);
}

// The game's bottom-screen HUD is up (not a menu, map screen or cutscene): the light blue frame line
// under the missile count at y 40, dark just above it. Read from the last bottom-screen picture.
static int hud_on_bottom(void) {
    const u32* b = g_upload + 400 * 240;
    int ok = 0;
    for (int x = 90; x <= 140; x += 25) {
        u32 line = b[(239 - 40) * 320 + x], above = b[(239 - 37) * 320 + x];
        int r = line & 0xFF, g = (line >> 8) & 0xFF, bl = (line >> 16) & 0xFF;
        int ab = (above >> 16) & 0xFF;
        if (bl > 150 && g > 100 && r < 120 && ab < 80) ok++;
    }
    return ok == 3;
}

// The file select screen ("Samus Files") is on the bottom screen: the light blue lines of its title
// frame (y 10 at x 60 and 260, y 24 from x 110 to 210), dark just above the top one. Checked against
// run013's picture of it and the other bottom-screen pictures (gameplay, title).
static int file_select_on_bottom(void) {
    const u32* b = g_upload + 400 * 240;
    static const int pts[5][2] = {{110, 24}, {160, 24}, {210, 24}, {60, 10}, {260, 10}};
    for (int k = 0; k < 5; k++) {
        u32 c = b[(239 - pts[k][1]) * 320 + pts[k][0]];
        int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
        if (!(bl > 150 && g > 100 && r < 130)) return 0;
    }
    return ((b[(239 - 9) * 320 + 60] >> 16) & 0xFF) < 80;
}

// The game's pause menu frame is on the bottom screen: the light blue line under its header (y 24,
// x 76 to 94 and 223 to 227), dark just above. The map and the Samus screens of the pause menu both
// have it, and so does the Samus screen the game brings up to explain a new ability; nothing else
// in runs 10 to 18's bottom-screen pictures does (gameplay HUD, title, file select, logo).
static int pause_frame_on_bottom(void) {
    const u32* b = g_upload + 400 * 240;
    static const int xs[5] = {76, 80, 94, 223, 227};
    for (int k = 0; k < 5; k++) {
        u32 c = b[(239 - 24) * 320 + xs[k]];
        int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
        if (!(bl > 150 && g > 100 && r < 120)) return 0;
    }
    return ((b[(239 - 23) * 320 + 67] >> 16) & 0xFF) < 80;
}

// The beam column on the right of the bottom screen: three slots (power beam, then the beams the
// game adds). The selected one is lit (bright pixels, every second one counted); the others are dim. -1: none lit.
static const int k_beam_y[3] = {8, 84, 160};   // slot tops, 70 rows each, x 250 to 319
static int selected_beam_slot(void) {
    const u32* b = g_upload + 400 * 240;
    int best = -1, best_n = 150;   // lit: 218 to 381 in runs 10 to 18, menus 47 to 123
    for (int k = 0; k < 3; k++) {
        int n = 0;
        for (int y = k_beam_y[k]; y < k_beam_y[k] + 68; y += 2)
            for (int x = 252; x < 318; x += 2) {
                u32 c = b[(239 - y) * 320 + x];
                int r = c & 0xFF, g = (c >> 8) & 0xFF, bl = (c >> 16) & 0xFF;
                if (r > 180 || g > 180 || bl > 180) n++;
            }
        if (n > best_n) { best_n = n; best = k; }
    }
    return best;
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
static int g_shot_request, g_shots_left = 6;   // pictures of both screens when the pause view first comes up
static void present(void) {
    if (g_gpu_render) dkr_frame_end();
    // only convert the screens when the game showed a new picture on one of them; the menu (open,
    // or just closed) and a layout change redraw the output from the last pictures
    static unsigned seen[2] = {~0u, ~0u};
    static int last_layout = -1, menu_was_open;
    int fresh = !(msr_fb[0].swaps == seen[0] && msr_fb[1].swaps == seen[1]);
    int redraw = g_menu_open || menu_was_open || last_layout != g_set.layout;
    if (!fresh && !redraw) return;
    menu_was_open = g_menu_open;
    last_layout = g_set.layout;
    seen[0] = msr_fb[0].swaps; seen[1] = msr_fb[1].swaps;
    MSR_PERF_BEGIN(PERF_PRESENT);
    // the GPU renderer converts the screens on the GPU from its copy of VRAM; otherwise (or for a
    // framebuffer outside VRAM) the CPU does it
    DkGpuAddr up = dkMemBlockGetGpuAddr(g_up_mem);
    int done0 = 0, done1 = 0;
    if (fresh && g_gpu_render) {
        int dkr_record_screen(DkCmdBuf cmd, int screen, u32 addr, u32 stride, u32 format, DkGpuAddr out, u32 out_word);
        done0 = dkr_record_screen(g_cmd, 0, msr_fb[0].left, msr_fb[0].stride, msr_fb[0].format, up, 0);
        done1 = dkr_record_screen(g_cmd, 1, msr_fb[1].left, msr_fb[1].stride, msr_fb[1].format, up, 400 * 240);
    }
    if (fresh && !done0) convert_screen(0, g_upload, 1);
    if (fresh && !done1) convert_screen(1, g_upload + 400 * 240, 1);
    mutexLock(&g_qlock);
    int slot = dkQueueAcquireImage(g_queue, g_swap);
    mutexUnlock(&g_qlock);
    DkImageView fbv, sv[2];
    dkImageViewDefaults(&fbv, &g_fb[slot]);
    dkCmdBufBindRenderTarget(g_cmd, &fbv, NULL);
    // the whole output: the scissor is GPU state the game's draws leave behind (test 11's clear
    // missed the corners, and the closed menu stayed there)
    DkScissor fsc = {0, 0, FB_W, FB_H};
    dkCmdBufSetScissors(g_cmd, 0, &fsc, 1);
    dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 0, 0, 0, 1);
    for (int k = 0; k < 2; k++) {
        dkImageViewDefaults(&sv[k], &g_scr[k]);
        if (!fresh) continue;   // the images still hold the last pictures
        u32 W = k ? 320 : 400;
        DkCopyBuf src = {up + (k ? 400 * 240 * 4 : 0), 0, 0};
        DkImageRect r = {0, 0, 0, W, 240, 1};
        dkCmdBufCopyBufferToImage(g_cmd, &src, &sv[k], &r, 0);
    }
    // single-screen mode: the file select screen comes up by itself (after A on the title screen)
    // and goes away when the game loads; a few pictures in a row decide, so transitions do not flicker
    if (fresh) {
        static int on_count, off_count;
        if (g_set.layout == LAYOUT_TOP_ONLY && file_select_on_bottom()) { off_count = 0; if (++on_count >= 3 && !g_auto_bottom) { g_auto_bottom = 1; msr_receipt("single screen: file select up, showing the bottom screen"); } }
        else { on_count = 0; if (++off_count >= 10 && g_auto_bottom) { g_auto_bottom = 0; msr_receipt("single screen: file select gone, back to the top screen"); } }
        // the pause menu (Start, minus or plus) and the screens explaining a new ability: shown while
        // the game has them up, so closing them any way (B too) goes back to the top screen
        static int p_on, p_off;
        if (g_set.layout == LAYOUT_TOP_ONLY && pause_frame_on_bottom()) {
            p_off = 0;
            if (++p_on >= 2 && !g_map_view) { g_map_view = 1; msr_receipt("single screen: pause menu up, showing the bottom screen"); if (g_shots_left > 0) { g_shots_left--; g_shot_request = 1; } }
        } else {
            p_on = 0;
            if (++p_off >= 3 && g_map_view) { g_map_view = 0; msr_receipt("single screen: pause menu gone, back to the top screen"); }
        }
    }
    // the screens where the port menu's layout puts them
    Rect tr, br;
    port_menu_layout(&tr, &br);
    // (the output image is shown upside down under OriginLowerLeft: its row 0 is the bottom of the TV)
    DkImageRect ts = {0, 0, 0, 400, 240, 1}, td = {(u32)tr.x, (u32)(FB_H - tr.y - tr.h), 0, (u32)tr.w, (u32)tr.h, 1};
    DkImageRect bs = {0, 0, 0, 320, 240, 1}, bd = {(u32)br.x, (u32)(FB_H - br.y - br.h), 0, (u32)br.w, (u32)br.h, 1};
    if (tr.w) dkCmdBufBlitImage(g_cmd, &sv[0], &ts, &fbv, &td, DkBlitFlag_FilterLinear, 0);
    if (br.w) dkCmdBufBlitImage(g_cmd, &sv[1], &bs, &fbv, &bd, DkBlitFlag_FilterLinear, 0);
    if (g_set.layout == LAYOUT_TOP_ONLY && !g_map_view && !g_auto_bottom && hud_on_bottom()) {
        // single-screen mode: the bottom screen's readouts, cut out of it and drawn at 2x on the top
        // one (energy, missiles, Aeion gauge and the ability cross; the selected ability; the
        // Metroid DNA and Metroids left). Bottom-screen coordinates, rows counted from the top.
        static struct { int sx, sy, w, h, dx, dy; } k_hud[3] = {
            {0, 0, 160, 60, 48, 8},              // top left
            {250, 8, 70, 70, 1240 - 8 - 140, 8},    // top right: the selected beam (its slot, below)
            {60, 212, 185, 28, 48, 720 - 8 - 56},   // bottom left
        };
        // Drawn (not blitted) so the dark backgrounds can be see-through: hud_fsh.glsl.
        DkGpuAddr hd = dkMemBlockGetGpuAddr(g_hud_data);
        DkViewport vp = {0, 0, FB_W, FB_H, 0.0f, 1.0f};
        dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
        DkRasterizerState rs;
        dkRasterizerStateDefaults(&rs);
        rs.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(g_cmd, &rs);
        DkColorState cs;
        dkColorStateDefaults(&cs);
        dkColorStateSetBlendEnable(&cs, 0, true);
        dkCmdBufBindColorState(g_cmd, &cs);
        DkBlendState bls;
        dkBlendStateDefaults(&bls);
        dkBlendStateSetFactors(&bls, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_Zero, DkBlendFactor_One);
        dkCmdBufBindBlendStates(g_cmd, 0, &bls, 1);
        DkColorWriteState cw;
        dkColorWriteStateDefaults(&cw);
        dkCmdBufBindColorWriteState(g_cmd, &cw);
        DkDepthStencilState dss;
        dkDepthStencilStateDefaults(&dss);
        dss.depthTestEnable = false;
        dss.depthWriteEnable = false;
        dkCmdBufBindDepthStencilState(g_cmd, &dss);
        dkCmdBufBindImageDescriptorSet(g_cmd, hd + 0x100, 1);
        dkCmdBufBindSamplerDescriptorSet(g_cmd, hd + 0x200, 1);
        DkResHandle th = dkMakeTextureHandle(0, 0);
        dkCmdBufBindTextures(g_cmd, DkStage_Fragment, 0, &th, 1);
        DkShader const* hs[2] = {&g_hud_vsh, &g_hud_fsh};
        dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, hs, 2);
        dkCmdBufBarrier(g_cmd, DkBarrier_Full, DkInvalidateFlags_Image);   // the blits and uploads into the images are done
        { int sl = selected_beam_slot(); k_hud[1].sy = k_beam_y[sl < 0 ? 0 : sl]; }
        for (int k = 0; k < 3; k++) {
            float rect[8] = {(float)k_hud[k].dx, (float)(FB_H - k_hud[k].dy - 2 * k_hud[k].h), (float)(2 * k_hud[k].w), (float)(2 * k_hud[k].h),
                             (float)k_hud[k].sx, (float)(240 - k_hud[k].sy - k_hud[k].h), (float)k_hud[k].w, (float)k_hud[k].h};
            dkCmdBufPushConstants(g_cmd, hd, 32, 0, 32, rect);
            DkBufExtents ub = {hd, 32};
            dkCmdBufBindUniformBuffers(g_cmd, DkStage_Vertex, 0, &ub, 1);
            dkCmdBufDraw(g_cmd, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
        }
        dkCmdBufBarrier(g_cmd, DkBarrier_Fragments, 0);   // before the blits that follow
    }
    if (g_set.show_fps) {   // pictures the game showed on the top screen in the last half second or so
        static unsigned long long t0;
        static unsigned s0;
        static int fps10;
        unsigned long long t = msr_now_ns();
        if (!t0) { t0 = t; s0 = msr_fb[0].swaps; }
        if (t - t0 >= 500000000ull) {
            fps10 = (int)((msr_fb[0].swaps - s0) * 10000000000ull / (t - t0));
            if (fps10 > 999) fps10 = 999;
            t0 = t;
            s0 = msr_fb[0].swaps;
        }
        DkImageView fv;
        dkImageViewDefaults(&fv, &g_fps_img);
        const u32* px = port_menu_fps(fps10);
        if (px) {
            u32* dst = (u32*)dkMemBlockGetCpuAddr(g_fps_mem);
            for (u32 y = 0; y < FPS_H; y++) memcpy(dst + y * FPS_W, px + (FPS_H - 1 - y) * FPS_W, FPS_W * 4);
            DkCopyBuf src = {dkMemBlockGetGpuAddr(g_fps_mem), 0, 0};
            DkImageRect r = {0, 0, 0, FPS_W, FPS_H, 1};
            dkCmdBufCopyBufferToImage(g_cmd, &src, &fv, &r, 0);
        }
        DkImageRect fs = {0, 0, 0, FPS_W, FPS_H, 1}, fd = {FB_W - 8 - FPS_W, 8, 0, FPS_W, FPS_H, 1};   // bottom right
        dkCmdBufBlitImage(g_cmd, &fv, &fs, &fbv, &fd, 0, 0);
    }
    if (g_menu_open) {
        DkImageView pv;
        dkImageViewDefaults(&pv, &g_panel_img);
        const u32* px = port_menu_panel();
        if (px) {   // rows bottom to top, like the screens (the output is shown upside down)
            u32* dst = (u32*)dkMemBlockGetCpuAddr(g_panel_mem);
            for (u32 y = 0; y < PANEL_H; y++) memcpy(dst + y * PANEL_W, px + (PANEL_H - 1 - y) * PANEL_W, PANEL_W * 4);
            DkCopyBuf src = {dkMemBlockGetGpuAddr(g_panel_mem), 0, 0};
            DkImageRect r = {0, 0, 0, PANEL_W, PANEL_H, 1};
            dkCmdBufCopyBufferToImage(g_cmd, &src, &pv, &r, 0);
        }
        DkImageRect ps = {0, 0, 0, PANEL_W, PANEL_H, 1}, pd = {(FB_W - PANEL_W) / 2, (FB_H - PANEL_H) / 2, 0, PANEL_W, PANEL_H, 1};
        dkCmdBufBlitImage(g_cmd, &pv, &ps, &fbv, &pd, 0, 0);
    }
    // Submit and present, then wait for this list alone, outside the queue lock: waiting for the
    // whole queue under the lock made the game thread stop until the GPU was idle (test 6c).
    DkFence done;
    mutexLock(&g_qlock);
    dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(g_cmd));
    dkQueueSignalFence(g_queue, &done, false);
    dkQueuePresentImage(g_queue, g_swap, slot);
    dkQueueFlush(g_queue);
    mutexUnlock(&g_qlock);
    dkFenceWait(&done, -1);
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
    stretch_reset();
    for (;;) {
        AudioOutBuffer* done = 0;
        u32 n = 0;
        audoutWaitPlayFinish(&done, &n, UINT64_MAX);
        if (!done) continue;
        // stretched to the game's speed (no crackle in slowdowns), then resampled to 48 kHz
        stretch_fill((s16*)done->buffer, AUD_SAMPLES, 48000.0, msr_audio_ring, RING, msr_audio_write);
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
// The fragment configurations the pack was made from (fs.bin), for the start-up self-test.
extern const u8 msr_fsrec_data[] __attribute__((weak));
extern const u8 msr_fsrec_end[] __attribute__((weak));

int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h);
static u32 g_shot_buf[400 * 240];
static u8 g_shot_rgb[400 * 240 * 3];
static void convert_screen(int screen, u32* out, int flip);
// Both screens as the game drew them, straight from its framebuffers.
static void save_shots(const char* tag) {
    for (int sc = 0; sc < 2; sc++) {
        u32 W = sc ? 320 : 400;
        if (g_gpu_render) {   // what present() last put up (rows bottom to top there)
            const u32* src = g_upload + (sc ? 400 * 240 : 0);
            for (u32 y = 0; y < 240; y++) memcpy(g_shot_buf + y * W, src + (239 - y) * W, W * 4);
        } else convert_screen(sc, g_shot_buf, 0);
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
    u32 b = port_menu_input(k);   // the port menu's button mapping (0 while the menu is open)
    // ZL + ZR + minus held for a second: capture the next frame for the reference renderer
    {
        static unsigned long long held_since;
        static int fired;
        u64 combo = HidNpadButton_ZL | HidNpadButton_ZR | HidNpadButton_Minus;
        if ((k & combo) == combo) {
            unsigned long long t = msr_now_ns();
            if (!held_since) held_since = t;
            if (!fired && t - held_since >= 1000000000ull) {   // a second by the clock (test 7 counted loops)
                fired = 1;
                static int nth;
                char path[300];
                snprintf(path, sizeof path, "%s/capture%d.bin", g_run_dir, ++nth);
                void msr_capture_request(const char* path);
                msr_capture_request(path);
                msr_receipt("input: frame capture requested (%s)", path);
            }
        } else { held_since = 0; fired = 0; }
    }
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
    msr_circle_x = g_menu_open ? 0 : (s16)(s.x * 156 / 32767);
    msr_circle_y = g_menu_open ? 0 : (s16)(s.y * 156 / 32767);
    // touch screen (handheld): wherever the layout puts the bottom screen
    HidTouchScreenState ts = {0};
    msr_touch_down = 0;
    Rect tr, br;
    port_menu_layout(&tr, &br);
    if (!g_menu_open && br.w && hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
        int x = ((int)ts.touches[0].x - br.x) * 320 / br.w, y = ((int)ts.touches[0].y - br.y) * 240 / br.h;
        if ((int)ts.touches[0].x >= br.x && (int)ts.touches[0].y >= br.y && x < 320 && y < 240) {
            msr_touch_down = 1; msr_touch_x = (u16)x; msr_touch_y = (u16)y;
        }
    }
    // Next beam (port menu, ZR): a tap on the next slot of the beam column, as a finger would. An
    // empty slot does nothing, so if the lit slot has not changed a moment later the one after it is
    // tried (each slot once).
    static int bt_frames, bt_from, bt_target, bt_tries;
    if (port_menu_beam_request() && !g_menu_open && !bt_frames) {
        bt_from = selected_beam_slot();
        if (bt_from >= 0) { bt_target = (bt_from + 1) % 3; bt_tries = 0; bt_frames = 1; }
    }
    if (bt_frames) {
        if (bt_frames <= 4) { msr_touch_down = 1; msr_touch_x = 285; msr_touch_y = (u16)(k_beam_y[bt_target] + 35); }
        if (++bt_frames > 16) {   // about a quarter second: did the lit slot move?
            int now_sel = selected_beam_slot();
            if (now_sel != bt_from || ++bt_tries >= 2) {
                msr_receipt("input: next beam, slot %d to %d", bt_from, now_sel);
                bt_frames = 0;
            } else { bt_target = (bt_target + 1) % 3; bt_frames = 1; }
        }
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
    msr_receipt("MSR-nx Switch test build 17 (%s)", cpu_render ? "CPU reference renderer" : "deko3d PICA renderer");
    snprintf(msr_save_dir, sizeof msr_save_dir, DIR "/save");
    sched_realtime = 1;
    { extern size_t sched_thread_stack; sched_thread_stack = 4u << 20; }
    msr_time_limit_ns = ~0ull;
    msr_svc_limit = ~0u;

    port_menu_init(DIR "/settings.ini");
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
        void dkr_compiler_init(const char* cache_dir);
        dkr_compiler_init(DIR "/shadercache");
        void dkr_selftest(const u8* rec, size_t size, const char* png_path);
        char st_png[300];
        snprintf(st_png, sizeof st_png, "%s/selftest.png", g_run_dir);
        dkr_selftest(msr_fsrec_data, msr_fsrec_data ? (size_t)(msr_fsrec_end - msr_fsrec_data) : 0, st_png);
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
    int reports = 0;
    unsigned long long shot_at = g_start_ns + 5000000000ull;
    void dkr_force_general(int vs, int fs);
    if (g_gpu_render) dkr_force_general(0, 0);   // the shader pack, with the general shaders only for misses
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
            msr_receipt("speed at %.0f s: %.1f top-screen pictures per second; clocks CPU %u MHz, GPU %u MHz, memory %u MHz",
                        (now - g_start_ns) / 1e9, (sw - last_swaps) / secs, ccpu / 1000000, cgpu / 1000000,
                        cmem / 1000000);
            msr_perf_report(secs);
            port_menu_clocks_refresh();
            if (g_gpu_render) {
                void dkr_vs_stats(unsigned* hits, unsigned* misses);
                unsigned h, m;
                dkr_vs_stats(&h, &m);
                msr_receipt("  vertex shaders: %u draws translated, %u on the interpreter", h, m);
                void dkr_fs_stats(unsigned* hits, unsigned* misses, unsigned* waits);
                unsigned wt;
                dkr_fs_stats(&h, &m, &wt);
                msr_receipt("  fragment shaders: %u draws specialized, %u on the general one, %u waited for a compile", h, m, wt);
                void dkr_compile_stats(unsigned* compiled, unsigned* avg_ms, unsigned* max_ms, unsigned* pending);
                unsigned cc, ca, cm, cp;
                dkr_compile_stats(&cc, &ca, &cm, &cp);
                if (cc || cp) msr_receipt("  compiled on the Switch so far: %u (%u ms each on average, longest %u ms), %u in progress", cc, ca, cm, cp);
                void dkr_gpu_times(unsigned* draw_ms, unsigned* vram_ms, unsigned* other_ms);
                unsigned gd, gv, go;
                dkr_gpu_times(&gd, &gv, &go);
                msr_receipt("  GPU time: draws %u ms, VRAM work %u ms, elsewhere or idle %u ms", gd, gv, go);
                extern unsigned eye_runs[2], eye_total[2];
                static unsigned er0, er1, et0, et1;
                msr_receipt("  eyes: left %u draws run of %u, right %u run of %u (the frame plan skips the rest)", eye_runs[0] - er0, eye_total[0] - et0,
                            eye_runs[1] - er1, eye_total[1] - et1);
                er0 = eye_runs[0]; er1 = eye_runs[1]; et0 = eye_total[0]; et1 = eye_total[1];
                void dkr_barrier_stats(unsigned* asked, unsigned* done);
                unsigned ba, bd;
                dkr_barrier_stats(&ba, &bd);
                msr_receipt("  GPU barriers: %u recorded of %u asked (GPU sync %s)", bd, ba,
                            g_set.sync == 0 ? "Safe" : g_set.sync == 1 ? "Fewer waits" : "Lean");
                unsigned dkr_ez_stats(void);
                msr_receipt("  early depth: %u draws", dkr_ez_stats());
                void dkr_vram_stats(unsigned* out);
                unsigned vo[9];
                dkr_vram_stats(vo);
                msr_receipt("  VRAM on the GPU: %u fills, %u copies (%u from images), %u transfers (%u from images), %u stores, %u loads, %u texture decodes",
                            vo[0], vo[1] + vo[8], vo[8], vo[2] + vo[7], vo[7], vo[3], vo[4], vo[5]);
                static unsigned last_skip, last_total, last_realign;
                extern unsigned fp_realigned;
                msr_receipt("  frame plan: %u of %u GPU operations skipped (right eye and other unseen work), %u realigned",
                            fp_skipped_ops - last_skip, fp_total_ops - last_total, fp_realigned - last_realign);
                last_skip = fp_skipped_ops; last_total = fp_total_ops; last_realign = fp_realigned;
            }
            last_swaps = sw;
            next_report = now + 5000000000ull;
            // pictures of both screens every 30 seconds, for comparing against the reference renderer
            if (++reports % 6 == 0) shot_at = now;
        }
        {   // the top screen mostly white (the white-out): capture that frame, up to twice a run. Off
            // since test 16: the white-out is fixed, the game's own white screens tripped it, and
            // saving the 80 MB capture stalls the game
            static unsigned long long next_check;
            static int auto_caps = 2;
            if (g_gpu_render && auto_caps < 2 && now >= next_check) {
                next_check = now + 1000000000ull;
                unsigned white = 0, total = 0;
                for (u32 y = 8; y < 240; y += 16)
                    for (u32 x = 8; x < 400; x += 16) {
                        u32 c = g_upload[y * 400 + x];
                        if ((c & 0xFF) > 235 && ((c >> 8) & 0xFF) > 235 && ((c >> 16) & 0xFF) > 235) white++;
                        total++;
                    }
                if (white * 10 >= total * 8) {   // 80% of the samples
                    char path[300];
                    snprintf(path, sizeof path, "%s/autocapture%d.bin", g_run_dir, ++auto_caps);
                    void msr_capture_request(const char* path);
                    msr_capture_request(path);
                    msr_receipt("white-out detected (%u of %u samples white): capturing the next frame (%s)", white, total, path);
                    shot_at = now;   // and pictures of both screens
                    next_check = now + 20000000000ull;
                }
            }
        }
        if (g_shot_request) { g_shot_request = 0; shot_at = now + 300000000ull; }   // the pause view: once it has settled
        if (shot_at && now >= shot_at) {
            char tag[48];
            snprintf(tag, sizeof tag, "t%03d", (int)((now - g_start_ns) / 1000000000ull));
            save_shots(tag);
            shot_at = 0;
        }
        if ((padGetButtons(&g_pad) & (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_R)) ==
            (HidNpadButton_Plus | HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_R))
            break;   // L + R + plus + minus: quit
        svcSleepThread(16666667);
    }
    port_menu_clocks_restore();
    msr_receipt("quit");
    msr_log_close();
    appletUnlockExit();
    exit(0);
}
