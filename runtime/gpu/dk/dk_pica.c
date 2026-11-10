// MSR-nx: the PICA200 renderer on the Switch GPU (deko3d).
//
// The game's command lists are read by pica_cmd.c as everywhere; each draw comes here instead of
// the CPU reference renderer. Vertices are fetched on the CPU into a buffer, the game's vertex
// program runs on the GPU in an interpreter shader (pica_vsh.glsl), and the fragment pipeline
// (textures, lighting, combiners, alpha test, PICA depth) runs in pica_fsh.glsl. Fixed-function
// state (blending, depth, stencil, culling, viewport, scissor) maps to deko3d state.
//
// The 3DS's VRAM (6 MiB at 0x1F000000) lives in Switch GPU memory, and the GX operations on it
// (fills, display transfers, texture copies) run there as compute shaders (vram_ops.inc), so the
// game's pictures never leave the GPU: render targets are images stored to and loaded from that
// copy of VRAM, textures in VRAM are decoded from it, and the screens are converted from it. The
// CPU's copy of VRAM (g_mem) is brought up to date only when the CPU side reads it. Render targets
// and textures outside VRAM keep the older CPU path (readback to and upload from guest memory).
#include <deko3d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <switch.h>
#include "msr_log.h"
#include "pica.h"
#include "vsgen.h"
#include "dk_shaders.h"   // generated at build time from the .glsl files (uam)

#define DYN_SIZE (32u << 20)
#define IMG_SIZE (128u << 20)
#define CMD_SIZE (16u << 20)
#define CMD_CHUNK (256u << 10)
#define CMD_CHUNKS (CMD_SIZE / CMD_CHUNK)
#define DYN_SEGS 4u
#define DYN_SEG (DYN_SIZE / DYN_SEGS)
#define SURF_REGION (64u << 20)
#define VS_UBO_SIZE 0xA00u
#define FS_UBO_SIZE 0x600u

static DkDevice g_dev;
static DkQueue g_q;
static Mutex* g_qlock;
static DkMemBlock g_code, g_dyn, g_img, g_cmdmem, g_ubo;
// Readbacks land in CPU-cached memory: reading the uncached dynamic area cost about 2 ms per
// 240x400 picture on test 4. The CPU's cache lines for it are invalidated before each read.
#define RB_SIZE (2u << 20)
static DkMemBlock g_rb;
static u8* g_rb_cpu;
static DkGpuAddr g_rb_gpu;
static DkCmdBuf g_cmd;
static DkShader g_vsh, g_fsh, g_test_vsh;
static u8* g_dyn_cpu;
static DkGpuAddr g_dyn_gpu;
static u32 g_dyn_pos, g_img_pos, g_tex_pos = SURF_REGION;
static unsigned g_draws_since_flush;

// ---- memory -------------------------------------------------------------------------------------

static void flush(int wait);
static const DkShader* vs_select(void);
static const DkShader* fs_select(int* ez);

// The dynamic area (vertex data, uploads, descriptors) is used as four segments in turn. When one
// fills up, what was recorded so far is submitted with a fence, and the next segment is used once
// the GPU has finished with it (normally long ago). Test 6b crashed when nothing ever freed memory.
static u32 g_dyn_seg;
static void dyn_next_segment(void);
static u32 dyn_alloc(u32 size, u32 align) {
    u32 p = (g_dyn_pos + align - 1) & ~(align - 1);
    if (p + size > (g_dyn_seg + 1) * DYN_SEG) {
        dyn_next_segment();
        p = (g_dyn_pos + align - 1) & ~(align - 1);
    }
    g_dyn_pos = p + size;
    return p;
}
// Room for need bytes in the current segment (a draw keeps all its data together).
static void dyn_reserve(u32 need) {
    if (((g_dyn_pos + 255) & ~255u) + need > (g_dyn_seg + 1) * DYN_SEG) dyn_next_segment();
}

// ---- surfaces (render targets) --------------------------------------------------------------------

typedef struct {
    u32 addr, w, h, bytes;   // guest address, PICA width x height, size in guest memory
    u32 fmt;                 // PICA color format (or depth format)
    int depth, used;
    int gpu_dirty;           // the GPU drew it; guest memory is older
    int cpu_dirty;           // guest memory changed; the image is older
    int clear_pending;
    u32 clear_value;
    DkImage img;
} Surf;

#define MAX_SURF 32
static Surf g_surf[MAX_SURF];
static Surf* g_bound_color;
static Surf* g_bound_depth;

static const u32 k_bpp[8] = {4, 3, 2, 2, 2, 4, 4, 4};

static u32 img_alloc_in(u32* pos, u32 limit, DkImageLayout* lay) {
    u32 a = dkImageLayoutGetAlignment(lay), size = (u32)dkImageLayoutGetSize(lay);
    u32 p = (*pos + a - 1) & ~(a - 1);
    if (p + size > limit) return ~0u;
    *pos = p + size;
    return p;
}
static u32 img_alloc(DkImageLayout* lay) { return img_alloc_in(&g_img_pos, SURF_REGION, lay); }

static u32 morton(u32 x, u32 y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
static u32 tiled(u32 x, u32 y, u32 w) { return (y & ~7u) * w + (x & ~7u) * 8 + morton(x & 7, y & 7); }

// RGBA8 render targets, the common case: a guest pixel is the byte-reversed image pixel. These walk
// the picture 8x8 tile by tile instead of converting pixel by pixel through the format switch.
static u8 g_morton[64];
static void rgba8_detile(const u8* src, u8* dst, u32 w, u32 h) {   // guest tiled -> image rows, row y = tiled row h-1-y
    if (!g_morton[1]) for (u32 i = 0; i < 64; i++) g_morton[i] = (u8)morton(i & 7, i >> 3);
    const u32* s = (const u32*)src;
    u32* d = (u32*)dst;
    for (u32 ty = 0; ty < h; ty += 8)
        for (u32 tx = 0; tx < w; tx += 8) {
            const u32* tile = s + ty * w + tx * 8;
            for (u32 i = 0; i < 64; i++) {
                u32 x = tx + (i & 7), y = ty + (i >> 3);
                d[(h - 1 - y) * w + x] = __builtin_bswap32(tile[g_morton[i]]);
            }
        }
}
static void rgba8_tile(const u8* src, u8* dst, u32 w, u32 h) {     // image rows -> guest tiled
    if (!g_morton[1]) for (u32 i = 0; i < 64; i++) g_morton[i] = (u8)morton(i & 7, i >> 3);
    const u32* s = (const u32*)src;
    u32* d = (u32*)dst;
    for (u32 ty = 0; ty < h; ty += 8)
        for (u32 tx = 0; tx < w; tx += 8) {
            u32* tile = d + ty * w + tx * 8;
            for (u32 i = 0; i < 64; i++) {
                u32 x = tx + (i & 7), y = ty + (i >> 3);
                tile[g_morton[i]] = __builtin_bswap32(s[(h - 1 - y) * w + x]);
            }
        }
}

// Guest (PICA tiled, any color format) -> RGBA8 rows, row 0 = bottom.
static void decode_color_px(const u8* p, u32 fmt, u8* o) {
    u16 h;
    switch (fmt) {
    case 0: o[0] = p[3]; o[1] = p[2]; o[2] = p[1]; o[3] = p[0]; break;
    case 1: o[0] = p[2]; o[1] = p[1]; o[2] = p[0]; o[3] = 255; break;
    case 2: memcpy(&h, p, 2); o[0] = ((h >> 11) & 31) * 255 / 31; o[1] = ((h >> 6) & 31) * 255 / 31; o[2] = ((h >> 1) & 31) * 255 / 31; o[3] = (h & 1) * 255; break;
    case 3: memcpy(&h, p, 2); o[0] = ((h >> 11) & 31) * 255 / 31; o[1] = ((h >> 5) & 63) * 255 / 63; o[2] = (h & 31) * 255 / 31; o[3] = 255; break;
    default: memcpy(&h, p, 2); o[0] = (h >> 12) * 17; o[1] = ((h >> 8) & 15) * 17; o[2] = ((h >> 4) & 15) * 17; o[3] = (h & 15) * 17; break;
    }
}
static void encode_color_px(u8* p, u32 fmt, const u8* c) {
    u16 h;
    switch (fmt) {
    case 0: p[0] = c[3]; p[1] = c[2]; p[2] = c[1]; p[3] = c[0]; break;
    case 1: p[0] = c[2]; p[1] = c[1]; p[2] = c[0]; break;
    case 2: h = (u16)(((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (c[3] >> 7)); memcpy(p, &h, 2); break;
    case 3: h = (u16)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3)); memcpy(p, &h, 2); break;
    default: h = (u16)(((c[0] >> 4) << 12) | ((c[1] >> 4) << 8) | ((c[2] >> 4) << 4) | (c[3] >> 4)); memcpy(p, &h, 2); break;
    }
}

// ---- VRAM on the GPU ---------------------------------------------------------------------------------

#define VRAM_BASE 0x1F000000u
#define VRAM_SIZE (6u << 20)
#define VRAM_PAGES (VRAM_SIZE >> 12)
enum { K_FILL, K_COPY, K_TRANSFER, K_STORE, K_LOAD, K_TEXDECODE, K_SCREEN, K_TRANSFER_IMG, K_COPY_IMG, K_COUNT };
static DkMemBlock g_vram;
static DkGpuAddr g_vram_gpu;
static DkShader g_k[K_COUNT];
static u32 g_vram_ver[VRAM_PAGES];   // bumped whenever the GPU's VRAM page changes (textures recheck)
static u8 g_cpu_stale[VRAM_PAGES];   // the GPU changed the page after the CPU's copy was taken
static u32 g_vram_tick;
static unsigned g_vram_ops[K_COUNT];

static int in_vram(u32 a, u32 n) { return a >= VRAM_BASE && n <= VRAM_SIZE && a - VRAM_BASE <= VRAM_SIZE - n; }

static void vram_touch(u32 a, u32 n) {   // the GPU wrote [a, a + n) of VRAM
    if (!n) return;
    g_vram_tick++;
    for (u32 p = (a - VRAM_BASE) >> 12; p <= (a - VRAM_BASE + n - 1) >> 12 && p < VRAM_PAGES; p++) {
        g_vram_ver[p] = g_vram_tick;
        g_cpu_stale[p] = 1;
    }
}

static u32 vram_version(u32 a, u32 n) {
    u32 v = 0;
    for (u32 p = (a - VRAM_BASE) >> 12; p <= (a - VRAM_BASE + n - 1) >> 12 && p < VRAM_PAGES; p++)
        if (g_vram_ver[p] > v) v = g_vram_ver[p];
    return v;
}

// A barrier between steps that hand data to each other through memory or images.
// GPU sync (port menu): 0 Safe, a full barrier with every cache invalidated at each call; 1 Fewer
// waits, no barrier when nothing was recorded since the last one (back-to-back kernels asked for
// two); 2 Lean, as 1 without invalidating the L2 cache. g_clean: nothing recorded since a barrier.
static int g_sync_mode, g_clean;
static unsigned g_barriers_asked, g_barriers_done;
#define GPU_WORK() (g_clean = 0)
static void gpu_barrier(void) {
    g_barriers_asked++;
    if (g_sync_mode >= 1 && g_clean) return;
    g_barriers_done++;
    u32 inv = DkInvalidateFlags_Image | DkInvalidateFlags_Shader | (g_sync_mode >= 2 ? 0 : DkInvalidateFlags_L2Cache);
    dkCmdBufBarrier(g_cmd, DkBarrier_Full, inv);
    g_clean = 1;
}
void dkr_set_sync_mode(int m) {
    static const char* names[3] = {"Safe", "Fewer waits", "Lean"};
    if (m < 0 || m > 2) m = 0;
    g_sync_mode = m;
    g_clean = 0;   // the next barrier is always recorded
    msr_receipt("gpu: GPU sync set to %s", names[m]);
}
void dkr_barrier_stats(unsigned* asked, unsigned* done) {
    *asked = g_barriers_asked; *done = g_barriers_done;
    g_barriers_asked = g_barriers_done = 0;
}

// ---- GPU timers -------------------------------------------------------------------------------
// The GPU writes its clock into a ring at every change between kinds of work in the game's
// command lists (draws, VRAM work: kernels and copies) and at the start of each list. The time from
// one mark to the next goes to the kind of work that ended at it; time up to a list's first mark
// is the GPU doing something else (the presenter's lists) or nothing. Read back when the GPU has
// written the marks, reported every 5 seconds.
enum { TS_NONE, TS_DRAW, TS_VRAM, TS_START, TS_KINDS };
#define TS_RING 4096
static DkMemBlock g_ts_mem;
static u8 g_ts_kind[TS_RING];
static u32 g_ts_w, g_ts_r;
static int g_ts_seg;
static u64 g_ts_ns[TS_KINDS], g_ts_last;
static void ts_mark(int kind) {
    if (!g_ts_mem || kind == g_ts_seg) return;
    if (g_ts_w - g_ts_r >= TS_RING) return;   // the reader is behind: skip marks rather than overwrite
    u32 i = g_ts_w % TS_RING;
    u64* e = (u64*)((u8*)dkMemBlockGetCpuAddr(g_ts_mem) + i * 16);
    e[0] = e[1] = 0;
    g_ts_kind[i] = (u8)g_ts_seg;   // the segment that ends here
    dkCmdBufReportCounter(g_cmd, DkCounter_Timestamp, dkMemBlockGetGpuAddr(g_ts_mem) + i * 16);
    g_ts_w++;
    g_ts_seg = kind;
}
static void ts_collect(void) {
    if (!g_ts_mem) return;
    const u8* base = (const u8*)dkMemBlockGetCpuAddr(g_ts_mem);
    while (g_ts_r != g_ts_w) {
        u32 i = g_ts_r % TS_RING;
        u64 t = *(volatile const u64*)(base + i * 16 + 8);
        if (!t) break;   // not written yet
        t = dkTimestampToNs(t);   // GPU ticks (as deko3d's own timestamps); the three parts adding up to the
                                  // report period (5000 ms) in the log is the check of this
        if (g_ts_last && t > g_ts_last && t - g_ts_last < 1000000000ull) g_ts_ns[g_ts_kind[i]] += t - g_ts_last;
        g_ts_last = t;
        g_ts_r++;
    }
}
// GPU milliseconds (per 5 seconds, or whatever the caller's period) in draws, VRAM work, and
// elsewhere (the presenter's lists, idle), then zeroed.
void dkr_gpu_times(unsigned* draw_ms, unsigned* vram_ms, unsigned* other_ms) {
    ts_collect();
    *draw_ms = (unsigned)(g_ts_ns[TS_DRAW] / 1000000);
    *vram_ms = (unsigned)(g_ts_ns[TS_VRAM] / 1000000);
    *other_ms = (unsigned)((g_ts_ns[TS_START] + g_ts_ns[TS_NONE]) / 1000000);
    memset(g_ts_ns, 0, sizeof g_ts_ns);
}

// Runs one of the vram_ops.inc kernels: q = four uvec4 parameters, n = invocations.
static void kernel(int k, const u32 q[16], u32 n, const DkImage* src, const DkImage* dst) {
    if (!n) return;
    g_vram_ops[k]++;
    ts_mark(TS_VRAM);
    gpu_barrier();   // what earlier draws and kernels wrote is visible
    DkGpuAddr ubo = dkMemBlockGetGpuAddr(g_ubo) + 0x1000;   // after the draw uniforms
    dkCmdBufPushConstants(g_cmd, ubo, 64, 0, 64, q);
    DkBufExtents ub = {ubo, 64};
    dkCmdBufBindUniformBuffers(g_cmd, DkStage_Compute, 0, &ub, 1);
    DkBufExtents sb[2] = {{g_vram_gpu, VRAM_SIZE}, {g_vram_gpu, VRAM_SIZE}};
    dkCmdBufBindStorageBuffers(g_cmd, DkStage_Compute, 0, sb, 2);
    if (src || dst) {
        u32 doff = dyn_alloc(sizeof(DkImageDescriptor) + sizeof(DkSamplerDescriptor), 64);
        DkImageDescriptor* id = (DkImageDescriptor*)(g_dyn_cpu + doff);
        DkSamplerDescriptor* sd = (DkSamplerDescriptor*)(g_dyn_cpu + doff + sizeof(DkImageDescriptor));
        DkImageView iv;
        dkImageViewDefaults(&iv, src ? src : dst);
        dkImageDescriptorInitialize(id, &iv, false, false);
        DkSampler smp;
        dkSamplerDefaults(&smp);
        dkSamplerDescriptorInitialize(sd, &smp);
        dkCmdBufBindImageDescriptorSet(g_cmd, g_dyn_gpu + doff, 1);
        dkCmdBufBindSamplerDescriptorSet(g_cmd, g_dyn_gpu + doff + sizeof(DkImageDescriptor), 1);
        if (src) {
            DkResHandle th = dkMakeTextureHandle(0, 0);
            dkCmdBufBindTextures(g_cmd, DkStage_Compute, 0, &th, 1);
        } else {
            DkResHandle ih = dkMakeImageHandle(0);
            dkCmdBufBindImages(g_cmd, DkStage_Compute, 0, &ih, 1);
        }
    }
    DkShader const* sh = &g_k[k];
    dkCmdBufBindShaders(g_cmd, DkStageFlag_Compute, &sh, 1);
    dkCmdBufDispatchCompute(g_cmd, (n + 63) / 64, 1, 1);
    GPU_WORK();
    gpu_barrier();
}

// The CPU wrote guest VRAM [a, a + n): send it to the GPU's copy, in order with queued work.
static void vram_upload(u32 a, u32 n) {
    ts_mark(TS_VRAM);
    while (n) {
        u32 chunk = n > (4u << 20) ? (4u << 20) : n;
        u32 off = dyn_alloc((chunk + 3) & ~3u, 256);
        memcpy(g_dyn_cpu + off, g_mem + a, chunk);
        dkCmdBufCopyBuffer(g_cmd, g_dyn_gpu + off, g_vram_gpu + (a - VRAM_BASE), chunk);
        GPU_WORK();
        a += chunk;
        n -= chunk;
    }
    gpu_barrier();
}

static void surf_store(Surf* s);
static void surf_load(Surf* s);

static void surf_upload(Surf* s) {
    if (s->depth) { s->cpu_dirty = 0; return; }   // depth only ever comes from clears
    if (in_vram(s->addr, s->bytes)) { surf_load(s); return; }
    MSR_PERF_BEGIN(PERF_SURF_UPLOAD);
    u32 off = dyn_alloc(s->w * s->h * 4, 256);
    u8* dst = g_dyn_cpu + off;
    const u8* src = g_mem + s->addr;
    u32 bpp = k_bpp[s->fmt];
    if (s->fmt == 0 && !(s->w & 7) && !(s->h & 7)) rgba8_detile(src, dst, s->w, s->h);
    else
    for (u32 y = 0; y < s->h; y++)
        for (u32 x = 0; x < s->w; x++)
            decode_color_px(src + tiled(x, s->h - 1 - y, s->w) * bpp, s->fmt, dst + (y * s->w + x) * 4);
    DkImageView v;
    dkImageViewDefaults(&v, &s->img);
    DkCopyBuf cb = {g_dyn_gpu + off, 0, 0};
    DkImageRect r = {0, 0, 0, s->w, s->h, 1};
    dkCmdBufCopyBufferToImage(g_cmd, &cb, &v, &r, 0);
    GPU_WORK();
    s->cpu_dirty = 0;
    MSR_PERF_END(PERF_SURF_UPLOAD);
}

static void surf_readback(Surf* s) {
    if (s->depth || !s->gpu_dirty) { s->gpu_dirty = 0; return; }
    if (in_vram(s->addr, s->bytes)) { surf_store(s); return; }
    MSR_PERF_BEGIN(PERF_READBACK);
    u32 bytes = s->w * s->h * 4;
    if (bytes > RB_SIZE) { s->gpu_dirty = 0; return; }
    DkImageView v;
    dkImageViewDefaults(&v, &s->img);
    DkCopyBuf cb = {g_rb_gpu, 0, 0};
    DkImageRect r = {0, 0, 0, s->w, s->h, 1};
    dkCmdBufCopyImageToBuffer(g_cmd, &v, &r, &cb, 0);
    GPU_WORK();
    flush(1);
    armDCacheFlush(g_rb_cpu, bytes);   // drop any stale lines before reading what the GPU wrote
    const u8* src = g_rb_cpu;
    u8* dst = g_mem + s->addr;
    u32 bpp = k_bpp[s->fmt];
    if (s->fmt == 0 && !(s->w & 7) && !(s->h & 7)) rgba8_tile(src, dst, s->w, s->h);
    else
    for (u32 y = 0; y < s->h; y++)
        for (u32 x = 0; x < s->w; x++)
            encode_color_px(dst + tiled(x, s->h - 1 - y, s->w) * bpp, s->fmt, src + (y * s->w + x) * 4);
    s->gpu_dirty = 0;
    MSR_PERF_END(PERF_READBACK);
}

// Out of surface slots or surface memory: every render target goes back to memory (VRAM or guest)
// and the set starts over; they are loaded again from there when next drawn to. Depth buffers start
// cleared (this game clears them every frame).
static u32 g_img_floor;
static void surf_reset_all(void) {
    for (int k = 0; k < MAX_SURF; k++)
        if (g_surf[k].used && g_surf[k].gpu_dirty) surf_readback(&g_surf[k]);
    flush(1);
    for (int k = 0; k < MAX_SURF; k++) g_surf[k].used = 0;
    g_img_pos = g_img_floor;
    g_bound_color = g_bound_depth = 0;
    msr_receipt("gpu: render target set started over (out of slots or memory)");
}

static Surf* surf_get(u32 addr, u32 w, u32 h, u32 fmt, int depth) {
    Surf* free_s = 0;
    for (int k = 0; k < MAX_SURF; k++) {
        Surf* s = &g_surf[k];
        if (s->used && s->addr == addr && s->w == w && s->h == h && s->depth == depth) { s->fmt = fmt; return s; }
        if (!s->used && !free_s) free_s = s;
    }
    if (!free_s) { surf_reset_all(); free_s = &g_surf[0]; }
    Surf* s = free_s;
    DkImageLayoutMaker lm;
    DkImageLayout lay;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine | (depth ? 0 : DkImageFlags_UsageLoadStore);
    lm.format = depth ? DkImageFormat_Z24S8 : DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    dkImageLayoutInitialize(&lay, &lm);
    u32 off = img_alloc(&lay);
    if (off == ~0u) { surf_reset_all(); s = &g_surf[0]; off = img_alloc(&lay); }
    if (off == ~0u) return 0;
    memset(s, 0, sizeof *s);
    dkImageInitialize(&s->img, &lay, g_img, off);
    s->used = 1;
    s->addr = addr; s->w = w; s->h = h; s->fmt = fmt; s->depth = depth;
    s->bytes = depth ? w * h * (fmt == 0 ? 2 : fmt == 3 ? 4 : 3) : w * h * k_bpp[fmt];
    s->cpu_dirty = 1;
    if (depth) { s->clear_pending = 1; s->clear_value = 0x00FFFFFFu; }
    msr_log("gpu: new %s surface %08X %ux%u format %u", depth ? "depth" : "color", addr, w, h, fmt);
    return s;
}

static int overlaps(Surf* s, u32 a, u32 n) { return s->used && a < s->addr + s->bytes && s->addr < a + n; }

// A render target in VRAM: image -> the GPU's VRAM, and back.
static void surf_store(Surf* s) {
    u32 q[16] = {s->addr - VRAM_BASE, s->w, s->h, s->fmt};
    kernel(K_STORE, q, s->w * s->h / 4, &s->img, 0);
    vram_touch(s->addr, s->bytes);
    s->gpu_dirty = 0;
}
static void surf_load(Surf* s) {
    u32 q[16] = {s->addr - VRAM_BASE, s->w, s->h, s->fmt};
    g_bound_color = g_bound_depth = 0;   // the image is written outside the render pass
    kernel(K_LOAD, q, s->w * s->h, 0, &s->img);
    s->cpu_dirty = 0;
}

// Everything the GPU drew in [a, a + n) reaches the GPU's VRAM (or guest memory, outside VRAM).
static void store_range(u32 a, u32 n) {
    for (int k = 0; k < MAX_SURF; k++)
        if (overlaps(&g_surf[k], a, n) && g_surf[k].gpu_dirty) surf_readback(&g_surf[k]);
}

// About to change [a, a + n) of VRAM on the GPU: render targets there keep what lies outside the
// write (stored first) and will load the result before they are drawn to again.
static void vram_will_write(u32 a, u32 n) {
    store_range(a, n);
    for (int k = 0; k < MAX_SURF; k++) {
        Surf* s = &g_surf[k];
        if (!overlaps(s, a, n) || s->depth) continue;
        s->cpu_dirty = 1;
        s->clear_pending = 0;
    }
}

// The CPU side is about to read [a, a + n): bring guest memory up to date.
static void sync_range(u32 a, u32 n) {
    store_range(a, n);
    if (!n || !in_vram(a, n)) return;
    u32 p0 = (a - VRAM_BASE) >> 12, p1 = (a - VRAM_BASE + n - 1) >> 12;
    int stale = 0;
    for (u32 p = p0; p <= p1; p++) stale |= g_cpu_stale[p];
    if (!stale) return;
    flush(1);
    const u8* src = (const u8*)dkMemBlockGetCpuAddr(g_vram);
    for (u32 p = p0; p <= p1; p++)
        if (g_cpu_stale[p]) {
            memcpy(g_mem + VRAM_BASE + (p << 12), src + (p << 12), 4096);
            g_cpu_stale[p] = 0;
        }
}

// ---- textures --------------------------------------------------------------------------------------

typedef struct {
    u32 addr, w, h, fmt, used;
    u64 hash;
    unsigned checked_frame;
    DkImage img;
} Tex;

#define MAX_TEX 512
static Tex g_tex[MAX_TEX];
static unsigned g_frame;
static DkImage g_lut_img;
static unsigned g_lut_uploaded = ~0u;

static u64 hash_mem(const u8* p, u32 n) {
    u64 h = 0xCBF29CE484222325ull;
    u32 step = n > 0x40000 ? 64 : 8;   // big textures: a sparse check (writes from our own hooks are caught separately)
    for (u32 i = 0; i + 8 <= n; i += step) {
        u64 v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
    }
    return h;
}

static const u32 k_tex_bits[14] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8};

static const int k_etc_mod[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
static void etc1_block(const u8* blk, u8 out[16][4]) {
    u64 w;
    memcpy(&w, blk, 8);
    u32 hi = (u32)(w >> 32), lo = (u32)w;
    int flip = hi & 1, diff = (hi >> 1) & 1;
    for (int x = 0; x < 4; x++)
        for (int y = 0; y < 4; y++) {
            int sub = flip ? (y >= 2) : (x >= 2);
            int r, g, b;
            if (diff) {
                int r0 = (hi >> 27) & 31, g0 = (hi >> 19) & 31, b0 = (hi >> 11) & 31;
                if (sub) { r0 += (int)((hi >> 24) & 7) << 29 >> 29; g0 += (int)((hi >> 16) & 7) << 29 >> 29; b0 += (int)((hi >> 8) & 7) << 29 >> 29; }
                r = (r0 << 3) | (r0 >> 2); g = (g0 << 3) | (g0 >> 2); b = (b0 << 3) | (b0 >> 2);
            } else {
                int s = sub ? 0 : 4;
                r = (hi >> (24 + s)) & 15; g = (hi >> (16 + s)) & 15; b = (hi >> (8 + s)) & 15;
                r |= r << 4; g |= g << 4; b |= b << 4;
            }
            int table = sub ? (hi >> 2) & 7 : (hi >> 5) & 7;
            int i = x * 4 + y;
            int m = k_etc_mod[table][(lo >> i) & 1];
            if ((lo >> (16 + i)) & 1) m = -m;
            u8* o = out[y * 4 + x];
            o[0] = (u8)(r + m < 0 ? 0 : r + m > 255 ? 255 : r + m);
            o[1] = (u8)(g + m < 0 ? 0 : g + m > 255 ? 255 : g + m);
            o[2] = (u8)(b + m < 0 ? 0 : b + m > 255 ? 255 : b + m);
            o[3] = 255;
        }
}

// Decodes a PICA texture into RGBA8 rows, row 0 = t 0 (the bottom row in memory order).
static void decode_texture(const u8* src, u32 fmt, u32 w, u32 h, u8* dst) {
    if (fmt == 12 || fmt == 13) {
        u32 bs = fmt == 13 ? 16 : 8;
        for (u32 ty = 0; ty < h; ty += 8)
            for (u32 tx = 0; tx < w; tx += 8)
                for (u32 sub = 0; sub < 4; sub++) {
                    const u8* blk = src + (((ty / 8) * (w / 8) + tx / 8) * 4 + sub) * bs;
                    u8 px[16][4];
                    etc1_block(blk + (fmt == 13 ? 8 : 0), px);
                    u64 a = 0;
                    if (fmt == 13) memcpy(&a, blk, 8);
                    u32 bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
                    for (u32 y = 0; y < 4; y++)
                        for (u32 x = 0; x < 4; x++) {
                            u8* o = dst + ((h - 1 - (by + y)) * w + bx + x) * 4;
                            memcpy(o, px[y * 4 + x], 4);
                            if (fmt == 13) o[3] = (u8)(((a >> (4 * (x * 4 + y))) & 15) * 17);
                        }
                }
        return;
    }
    u32 bits = k_tex_bits[fmt];
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u32 t = tiled(x, y, w);
            u8* o = dst + ((h - 1 - y) * w + x) * 4;
            const u8* p = src + (t * bits) / 8;
            switch (fmt) {
            case 0: case 1: case 2: case 3: case 4: decode_color_px(p, fmt, o); break;
            case 5: o[0] = o[1] = o[2] = p[1]; o[3] = p[0]; break;
            case 6: o[0] = p[1]; o[1] = p[0]; o[2] = 0; o[3] = 255; break;
            case 7: o[0] = o[1] = o[2] = p[0]; o[3] = 255; break;
            case 8: o[0] = o[1] = o[2] = 0; o[3] = p[0]; break;
            case 9: o[0] = o[1] = o[2] = (p[0] >> 4) * 17; o[3] = (p[0] & 15) * 17; break;
            case 10: { u8 v = t & 1 ? p[0] >> 4 : p[0] & 15; o[0] = o[1] = o[2] = v * 17; o[3] = 255; break; }
            case 11: { u8 v = t & 1 ? p[0] >> 4 : p[0] & 15; o[0] = o[1] = o[2] = 0; o[3] = v * 17; break; }
            }
        }
}

static void tex_create(Tex* t, u32 w, u32 h) {
    DkImageLayoutMaker lm;
    DkImageLayout lay;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageLoadStore;   // VRAM textures are decoded into it by a compute shader
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    dkImageLayoutInitialize(&lay, &lm);
    u32 off = img_alloc_in(&g_tex_pos, IMG_SIZE, &lay);
    if (off == ~0u) {   // out of texture memory: drop every texture and start again
        flush(1);
        for (int k = 0; k < MAX_TEX; k++) g_tex[k].used = 0;
        g_tex_pos = SURF_REGION;
        off = img_alloc_in(&g_tex_pos, IMG_SIZE, &lay);
    }
    dkImageInitialize(&t->img, &lay, g_img, off);
}

static DkImage* tex_get(u32 paddr, u32 fmt, u32 w, u32 h) {
    u32 v = pica_vaddr(paddr);
    if (!v || !w || !h || fmt > 13) return 0;
    u32 size = w * h * k_tex_bits[fmt] / 8;
    int vr = in_vram(v, size);
    if (vr) store_range(v, size);
    else sync_range(v, size);
    Tex* hit = 0, *free_t = 0;
    for (int k = 0; k < MAX_TEX; k++) {
        Tex* t = &g_tex[k];
        if (t->used && t->addr == v && t->w == w && t->h == h && t->fmt == fmt) { hit = t; break; }
        if (!t->used && !free_t) free_t = t;
    }
    if (vr) {   // decoded on the GPU from its VRAM whenever those pages changed
        u32 ver = vram_version(v, size);
        if (hit && hit->hash == ver && hit->checked_frame != ~0u) return &hit->img;
        if (!hit) {
            if (!free_t) {
                flush(1);
                for (int k = 0; k < MAX_TEX; k++) g_tex[k].used = 0;
                g_tex_pos = SURF_REGION;
                free_t = &g_tex[0];
            }
            hit = free_t;
            memset(hit, 0, sizeof *hit);
            hit->used = 1; hit->addr = v; hit->w = w; hit->h = h; hit->fmt = fmt;
            tex_create(hit, w, h);
        }
        hit->hash = ver;
        hit->checked_frame = g_frame;
        u32 q[16] = {v - VRAM_BASE, w, h, fmt};
        kernel(K_TEXDECODE, q, w * h, 0, &hit->img);
        return &hit->img;
    }
    if (hit && hit->checked_frame == g_frame) return &hit->img;
    u64 hs = hash_mem(g_mem + v, size);
    if (hit && hit->hash == hs) { hit->checked_frame = g_frame; return &hit->img; }
    if (!hit) {
        if (!free_t) {   // table full: start the texture cache over
            flush(1);
            for (int k = 0; k < MAX_TEX; k++) g_tex[k].used = 0;
            g_tex_pos = SURF_REGION;
            free_t = &g_tex[0];
        }
        hit = free_t;
        memset(hit, 0, sizeof *hit);
        hit->used = 1; hit->addr = v; hit->w = w; hit->h = h; hit->fmt = fmt;
        tex_create(hit, w, h);
    }
    hit->hash = hs;
    hit->checked_frame = g_frame;
    MSR_PERF_BEGIN(PERF_TEX_UPLOAD);
    u32 off = dyn_alloc(w * h * 4, 256);
    decode_texture(g_mem + v, fmt, w, h, g_dyn_cpu + off);
    MSR_PERF_END(PERF_TEX_UPLOAD);
    DkImageView iv;
    dkImageViewDefaults(&iv, &hit->img);
    DkCopyBuf cb = {g_dyn_gpu + off, 0, 0};
    DkImageRect r = {0, 0, 0, w, h, 1};
    dkCmdBufCopyBufferToImage(g_cmd, &cb, &iv, &r, 0);
    GPU_WORK();
    return &hit->img;
}

static void lut_update(void) {
    if (g_lut_uploaded == pica_lut_version) return;
    g_lut_uploaded = pica_lut_version;
    u32 off = dyn_alloc(256 * 24 * 8, 256);
    memcpy(g_dyn_cpu + off, pica_lut, 256 * 24 * 8);
    DkImageView iv;
    dkImageViewDefaults(&iv, &g_lut_img);
    DkCopyBuf cb = {g_dyn_gpu + off, 0, 0};
    DkImageRect r = {0, 0, 0, 256, 24, 1};
    dkCmdBufCopyBufferToImage(g_cmd, &cb, &iv, &r, 0);
    GPU_WORK();
}

// ---- submission ----------------------------------------------------------------------------------

static unsigned g_program_uploaded = ~0u;
static DkGpuAddr g_program_addr;
static unsigned g_flushes;

// Command memory is handed to the command buffer in 256 KB chunks around a ring. Each submitted
// list's chunks carry the fence signalled after it, and a chunk is reused only after that fence.
static DkFence g_chunk_fence[CMD_CHUNKS];
static u8 g_chunk_pending[CMD_CHUNKS];
static u32 g_chunk_next, g_list_chunks[CMD_CHUNKS], g_list_n;
static DkFence g_seg_fence[DYN_SEGS];
static u8 g_seg_pending[DYN_SEGS];
static DkFence g_last_fence;

static void cmd_add_chunk(void) {
    u32 c = g_chunk_next;
    g_chunk_next = (c + 1) % CMD_CHUNKS;
    if (g_chunk_pending[c]) { dkFenceWait(&g_chunk_fence[c], -1); g_chunk_pending[c] = 0; }
    dkCmdBufAddMemory(g_cmd, g_cmdmem, c * CMD_CHUNK, CMD_CHUNK);
    if (g_list_n < CMD_CHUNKS) g_list_chunks[g_list_n++] = c;
}
static void cmd_out_of_memory(void* user, DkCmdBuf cmd, size_t need) { (void)user; (void)cmd; (void)need; cmd_add_chunk(); }

static void flush(int wait) {
    ts_mark(TS_NONE);   // the end of the list's last segment
    mutexLock(g_qlock);
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cmd));
    dkQueueSignalFence(g_q, &g_last_fence, false);
    dkQueueFlush(g_q);
    if (wait) { MSR_PERF_BEGIN(PERF_GPU_WAIT); dkQueueWaitIdle(g_q); MSR_PERF_END(PERF_GPU_WAIT); }
    mutexUnlock(g_qlock);
    for (u32 k = 0; k < g_list_n; k++) {
        g_chunk_fence[g_list_chunks[k]] = g_last_fence;
        g_chunk_pending[g_list_chunks[k]] = !wait;
    }
    g_list_n = 0;
    dkCmdBufClear(g_cmd);
    cmd_add_chunk();
    if (wait) for (u32 k = 0; k < DYN_SEGS; k++) g_seg_pending[k] = 0;   // the GPU is idle
    g_draws_since_flush = 0;
    g_flushes++;
    g_bound_color = g_bound_depth = 0;   // bindings do not carry over a new command list
    g_ts_seg = TS_START;
    ts_mark(TS_NONE);   // the GPU reaching this list: the time since the last one was not the game's
    ts_collect();
}

static void dyn_next_segment(void) {
    flush(0);   // everything using this segment is submitted; its fence marks when it is free
    g_seg_fence[g_dyn_seg] = g_last_fence;
    g_seg_pending[g_dyn_seg] = 1;
    g_dyn_seg = (g_dyn_seg + 1) % DYN_SEGS;
    if (g_seg_pending[g_dyn_seg]) { dkFenceWait(&g_seg_fence[g_dyn_seg], -1); g_seg_pending[g_dyn_seg] = 0; }
    g_dyn_pos = g_dyn_seg * DYN_SEG;
    g_program_uploaded = ~0u;   // the program copy lived in the segment left behind
}

// ---- state mapping ---------------------------------------------------------------------------------

static DkCompareOp cmp_op(u32 f) {
    static const DkCompareOp m[8] = {DkCompareOp_Never, DkCompareOp_Always, DkCompareOp_Equal, DkCompareOp_NotEqual,
                                     DkCompareOp_Less, DkCompareOp_Lequal, DkCompareOp_Greater, DkCompareOp_Gequal};
    return m[f & 7];
}
static DkBlendFactor blend_factor(u32 f) {
    static const DkBlendFactor m[16] = {
        DkBlendFactor_Zero, DkBlendFactor_One, DkBlendFactor_SrcColor, DkBlendFactor_InvSrcColor, DkBlendFactor_DstColor,
        DkBlendFactor_InvDstColor, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha, DkBlendFactor_DstAlpha,
        DkBlendFactor_InvDstAlpha, DkBlendFactor_ConstColor, DkBlendFactor_InvConstColor, DkBlendFactor_ConstAlpha,
        DkBlendFactor_InvConstAlpha, DkBlendFactor_SrcAlphaSaturate, DkBlendFactor_One};
    return m[f & 15];
}
static DkBlendOp blend_op(u32 e) {
    static const DkBlendOp m[8] = {DkBlendOp_Add, DkBlendOp_Sub, DkBlendOp_RevSub, DkBlendOp_Min, DkBlendOp_Max,
                                   DkBlendOp_Add, DkBlendOp_Add, DkBlendOp_Add};
    return m[e & 7];
}
static DkStencilOp stencil_op(u32 o) {
    static const DkStencilOp m[8] = {DkStencilOp_Keep, DkStencilOp_Zero, DkStencilOp_Replace, DkStencilOp_Incr,
                                     DkStencilOp_Decr, DkStencilOp_Invert, DkStencilOp_IncrWrap, DkStencilOp_DecrWrap};
    return m[o & 7];
}
static DkLogicOp logic_op(u32 o) {
    static const DkLogicOp m[16] = {DkLogicOp_Clear, DkLogicOp_And, DkLogicOp_AndReverse, DkLogicOp_Copy, DkLogicOp_Set,
                                    DkLogicOp_CopyInverted, DkLogicOp_NoOp, DkLogicOp_Invert, DkLogicOp_Nand, DkLogicOp_Or,
                                    DkLogicOp_Nor, DkLogicOp_Xor, DkLogicOp_Equivalent, DkLogicOp_AndInverted,
                                    DkLogicOp_OrReverse, DkLogicOp_OrInverted};
    return m[o & 15];
}
static DkWrapMode wrap(u32 m) {
    switch (m & 7) {
    case 0: return DkWrapMode_ClampToEdge;
    case 1: return DkWrapMode_ClampToBorder;
    case 3: return DkWrapMode_MirroredRepeat;
    default: return DkWrapMode_Repeat;
    }
}

// ---- draws ---------------------------------------------------------------------------------------------

static const u32 k_texregs[3][5] = {{0x81, 0x82, 0x83, 0x85, 0x8E}, {0x91, 0x92, 0x93, 0x95, 0x96}, {0x99, 0x9A, 0x9B, 0x9D, 0x9E}};

static int dk_draw(int indexed) {
    u32* r = g_pica.reg;
    if ((r[0x229] & 3) == 2) return 1;   // geometry shader draws: not yet (none seen in this game)
    u32 n = r[0x228];
    if (!n) return 1;
    u32 W = r[0x11E] & 0x7FF, H = ((r[0x11E] >> 12) & 0x3FF) + 1;
    u32 cva = pica_vaddr(r[0x11D] << 3), dva = r[0x11C] ? pica_vaddr(r[0x11C] << 3) : 0;
    if (!cva || !W || !H) return 1;

    // ---- phase A: everything that may flush (surface and texture uploads, readbacks)
    Surf* cs = 0, *ds = 0;
    DkImage* tex[3] = {0, 0, 0};
    for (int attempt = 0; attempt < 2; attempt++) {
        unsigned f0 = g_flushes;
        cs = surf_get(cva, W, H, (r[0x117] >> 16) & 7, 0);
        ds = dva ? surf_get(dva, W, H, r[0x116] & 3, 1) : 0;
        if (!cs) return 1;
        if (cs->cpu_dirty) surf_upload(cs);
        if (ds && ds->cpu_dirty) surf_upload(ds);
        for (int u = 0; u < 3; u++) {
            tex[u] = 0;
            if ((r[0x80] >> u) & 1) {
                u32 dim = r[k_texregs[u][1]];
                tex[u] = tex_get(r[k_texregs[u][3]] << 3, r[k_texregs[u][4]] & 0xF, (dim >> 16) & 0x7FF, dim & 0x7FF);
            }
        }
        lut_update();
        if (g_flushes == f0) break;   // nothing moved under us
    }

    // ---- phase B: the draw's own data, in one piece of the dynamic area
    u32 ib = r[0x227], base = r[0x200] << 3;
    u32 first = r[0x22A], lo = first, hi = first + n - 1;
    const u8* idx = 0;
    int wide = (ib >> 31) & 1;
    if (indexed) {
        idx = pica_ptr(base + (ib & 0x0FFFFFFF));
        if (!idx) return 1;
        lo = 0xFFFFFFFF; hi = 0;
        for (u32 i = 0; i < n; i++) {
            u32 v;
            if (wide) { u16 s16; memcpy(&s16, idx + 2 * i, 2); v = s16; } else v = idx[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    u32 count = hi - lo + 1;
    if (count > 65536) return 1;
    // the game's vertex bytes go to the GPU as they are; the vertex shader decodes them
    static PicaAttrSrc vsrc[16];
    static PicaLoaderCopy vcp[12];
    u32 ncp, vbytes;
    pica_vertex_layout(lo, count, vsrc, vcp, &ncp, &vbytes);
    if (!vbytes) vbytes = 16;
    u32 need = vbytes + n * 2 + (4096 + 128) * 4 + 1024 + 4 * 256;
    dyn_reserve(need);
    u32 voff = dyn_alloc(vbytes, 256);
    MSR_PERF_BEGIN(PERF_VERTEX);
    for (u32 k = 0; k < ncp; k++) memcpy(g_dyn_cpu + voff + vcp[k].dst, g_mem + vcp[k].src, vcp[k].bytes);
    MSR_PERF_END(PERF_VERTEX);
    u32 ioff = 0;
    if (indexed) {
        ioff = dyn_alloc(n * 2, 4);
        u16* id16 = (u16*)(g_dyn_cpu + ioff);
        for (u32 i = 0; i < n; i++) {
            u32 v;
            if (wide) { u16 s16; memcpy(&s16, idx + 2 * i, 2); v = s16; } else v = idx[i];
            id16[i] = (u16)(v - lo);
        }
    }
    if (g_program_uploaded != pica_program_version) {
        g_program_uploaded = pica_program_version;
        u32 po = dyn_alloc((4096 + 128) * 4, 256);
        memcpy(g_dyn_cpu + po, g_pica.vs.program, 4096 * 4);
        memcpy(g_dyn_cpu + po + 4096 * 4, g_pica.vs.swizzle, 128 * 4);
        g_program_addr = g_dyn_gpu + po;
    }
    u32 doff = dyn_alloc(4 * sizeof(DkImageDescriptor) + 4 * sizeof(DkSamplerDescriptor), 64);
    DkImageDescriptor* idesc = (DkImageDescriptor*)(g_dyn_cpu + doff);
    DkSamplerDescriptor* sdesc = (DkSamplerDescriptor*)(g_dyn_cpu + doff + 4 * sizeof(DkImageDescriptor));
    for (int u = 0; u < 4; u++) {
        DkImage* im = u < 3 ? tex[u] : &g_lut_img;
        DkSampler smp;
        dkSamplerDefaults(&smp);
        if (u < 3 && im) {
            u32 par = r[k_texregs[u][2]];
            smp.magFilter = (par >> 1) & 1 ? DkFilter_Linear : DkFilter_Nearest;
            smp.minFilter = (par >> 2) & 1 ? DkFilter_Linear : DkFilter_Nearest;
            smp.wrapMode[0] = wrap(par >> 12);
            smp.wrapMode[1] = wrap(par >> 8);
            u32 bc = r[k_texregs[u][0]];
            for (int c = 0; c < 4; c++) smp.borderColor[c].value_f = ((bc >> (8 * c)) & 0xFF) / 255.f;
        }
        if (!im) im = &g_lut_img;   // anything valid; the shader does not read a disabled unit
        DkImageView iv;
        dkImageViewDefaults(&iv, im);
        dkImageDescriptorInitialize(&idesc[u], &iv, false, false);
        dkSamplerDescriptorInitialize(&sdesc[u], &smp);
    }

    // ---- phase C: commands
    ts_mark(TS_DRAW);
    if (g_bound_color != cs || g_bound_depth != ds) {
        DkImageView cv, dv;
        dkImageViewDefaults(&cv, &cs->img);
        if (ds) dkImageViewDefaults(&dv, &ds->img);
        dkCmdBufBindRenderTarget(g_cmd, &cv, ds ? &dv : NULL);
        g_bound_color = cs;
        g_bound_depth = ds;
    }
    if (cs->clear_pending || (ds && ds->clear_pending)) {
        DkScissor full = {0, 0, W, H};
        dkCmdBufSetScissors(g_cmd, 0, &full, 1);
        DkColorWriteState all;
        dkColorWriteStateDefaults(&all);
        dkCmdBufBindColorWriteState(g_cmd, &all);
        if (cs->clear_pending) {
            u32 v = cs->clear_value;
            dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, (v >> 24) / 255.f, ((v >> 16) & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, (v & 0xFF) / 255.f);
            GPU_WORK();
            cs->clear_pending = 0;
        }
        if (ds && ds->clear_pending) {
            u32 v = ds->clear_value;
            dkCmdBufClearDepthStencil(g_cmd, true, (v & 0xFFFFFF) / 16777215.0f, 0xFF, (u8)(v >> 24));
            GPU_WORK();
            ds->clear_pending = 0;
        }
    }
    DkBufExtents sb[2] = {{g_program_addr, (4096 + 128) * 4}, {g_dyn_gpu + voff, (vbytes + 3) & ~3u}};
    dkCmdBufBindStorageBuffers(g_cmd, DkStage_Vertex, 0, sb, 2);
    struct {
        Vec4 f[96];
        u32 ints[4][4];
        u32 cfg[4];
        u32 outmap[7][4];
        u32 base[4];
        u32 attr[16][4];
        Vec4 fixd[16];
    } vs;
    memcpy(vs.f, g_pica.vs.f, sizeof vs.f);
    for (int k = 0; k < 4; k++) {
        u32 iu = r[0x2B1 + k];
        vs.ints[k][0] = iu & 0xFF; vs.ints[k][1] = (iu >> 8) & 0xFF; vs.ints[k][2] = (iu >> 16) & 0xFF; vs.ints[k][3] = 0;
    }
    vs.cfg[0] = r[0x2B0] & 0xFFFF; vs.cfg[1] = r[0x2BA] & 0xFFFF; vs.cfg[2] = r[0x2BD]; vs.cfg[3] = r[0x4F] & 7;
    for (int k = 0; k < 7; k++) { vs.outmap[k][0] = r[0x50 + k]; vs.outmap[k][1] = vs.outmap[k][2] = vs.outmap[k][3] = 0; }
    vs.base[0] = vs.base[1] = vs.base[2] = vs.base[3] = 0;
    for (int k = 0; k < 16; k++) {
        vs.attr[k][0] = vsrc[k].mode;
        vs.attr[k][1] = vsrc[k].offset;
        vs.attr[k][2] = vsrc[k].stride;
        vs.attr[k][3] = vsrc[k].type | (vsrc[k].comps << 4);
        vs.fixd[k] = vsrc[k].fixed;
    }
    DkGpuAddr ubo = dkMemBlockGetGpuAddr(g_ubo);
    dkCmdBufPushConstants(g_cmd, ubo, VS_UBO_SIZE, 0, sizeof vs, &vs);
    struct { u32 regs[352]; float depth[4]; } fs;
    memcpy(fs.regs, r + 0x80, sizeof fs.regs);
    fs.depth[0] = pica_f24(r[0x4D]); fs.depth[1] = pica_f24(r[0x4E]); fs.depth[2] = (r[0x6D] & 1) ? 0.f : 1.f; fs.depth[3] = 0;
    dkCmdBufPushConstants(g_cmd, ubo + VS_UBO_SIZE, FS_UBO_SIZE, 0, sizeof fs, &fs);
    DkBufExtents ub0 = {ubo, VS_UBO_SIZE}, ub1 = {ubo + VS_UBO_SIZE, FS_UBO_SIZE};
    dkCmdBufBindUniformBuffers(g_cmd, DkStage_Vertex, 0, &ub0, 1);
    dkCmdBufBindUniformBuffers(g_cmd, DkStage_Fragment, 1, &ub1, 1);
    dkCmdBufBindImageDescriptorSet(g_cmd, g_dyn_gpu + doff, 4);
    dkCmdBufBindSamplerDescriptorSet(g_cmd, g_dyn_gpu + doff + 4 * sizeof(DkImageDescriptor), 4);
    DkResHandle th[4] = {dkMakeTextureHandle(0, 0), dkMakeTextureHandle(1, 1), dkMakeTextureHandle(2, 2), dkMakeTextureHandle(3, 3)};
    dkCmdBufBindTextures(g_cmd, DkStage_Fragment, 0, th, 4);

    DkRasterizerState rs;
    dkRasterizerStateDefaults(&rs);
    rs.frontFace = DkFrontFace_CCW;
    u32 cull = r[0x40] & 3;
    rs.cullMode = cull == 1 ? DkFace_Front : cull == 2 ? DkFace_Back : DkFace_None;
    dkCmdBufBindRasterizerState(g_cmd, &rs);
    DkColorState cst;
    dkColorStateDefaults(&cst);
    int blend = (r[0x100] >> 8) & 1;
    cst.blendEnableMask = blend ? 1 : 0;
    cst.logicOp = blend ? DkLogicOp_Copy : logic_op(r[0x102]);
    dkCmdBufBindColorState(g_cmd, &cst);
    DkBlendState bs;
    dkBlendStateDefaults(&bs);
    u32 bf = r[0x101];
    bs.colorBlendOp = blend_op(bf);
    bs.alphaBlendOp = blend_op(bf >> 8);
    bs.srcColorBlendFactor = blend_factor(bf >> 16);
    bs.dstColorBlendFactor = blend_factor(bf >> 20);
    bs.srcAlphaBlendFactor = blend_factor(bf >> 24);
    bs.dstAlphaBlendFactor = blend_factor(bf >> 28);
    dkCmdBufBindBlendStates(g_cmd, 0, &bs, 1);
    u32 kc = r[0x103];
    dkCmdBufSetBlendConst(g_cmd, (kc & 0xFF) / 255.f, ((kc >> 8) & 0xFF) / 255.f, ((kc >> 16) & 0xFF) / 255.f, (kc >> 24) / 255.f);
    u32 dm = r[0x107];
    DkColorWriteState cw;
    dkColorWriteStateDefaults(&cw);
    dkColorWriteStateSetMask(&cw, 0, r[0x113] ? ((dm >> 8) & 0xF) : 0);
    dkCmdBufBindColorWriteState(g_cmd, &cw);
    DkDepthStencilState dss;
    dkDepthStencilStateDefaults(&dss);
    dss.depthTestEnable = ds != 0;
    dss.depthWriteEnable = ds && ((dm >> 12) & 1) && r[0x115];
    dss.depthCompareOp = (dm & 1) ? cmp_op(dm >> 4) : DkCompareOp_Always;
    u32 st = r[0x105], so = r[0x106];
    dss.stencilTestEnable = ds && (st & 1) && ds->fmt == 3;
    dss.stencilFrontCompareOp = dss.stencilBackCompareOp = cmp_op(st >> 4);
    dss.stencilFrontFailOp = dss.stencilBackFailOp = stencil_op(so);
    dss.stencilFrontDepthFailOp = dss.stencilBackDepthFailOp = stencil_op(so >> 4);
    dss.stencilFrontPassOp = dss.stencilBackPassOp = stencil_op(so >> 8);
    dkCmdBufBindDepthStencilState(g_cmd, &dss);
    if (dss.stencilTestEnable)
        dkCmdBufSetStencil(g_cmd, DkFace_FrontAndBack, r[0x115] ? (u8)(st >> 8) : 0, (u8)(st >> 16), (u8)(st >> 24));
    float vw = pica_f24(r[0x41]), vh = pica_f24(r[0x43]);
    s32 vx = (s32)(r[0x68] << 22) >> 22, vy = (s32)((r[0x68] >> 16) << 22) >> 22;
    int ez = 0;
    DkShader const* sh[2] = {vs_select(), fs_select(&ez)};
    if (!sh[1]) return 1;   // its fragment shader is still compiling: this draw sits out
    DkViewport vp = {(float)vx, (float)vy, 2 * vw, 2 * vh, 0.0f, 1.0f};
    if (ez) {   // the PICA depth mapping in the viewport: depth = offset + (-z/w) * (-scale)
        float scale = pica_f24(r[0x4D]), offset = pica_f24(r[0x4E]);
        vp.near = offset;
        vp.far = offset - scale;
    }
    dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
    DkScissor sc = {0, 0, W, H};
    if (r[0x65] & 3) {
        u32 x0 = r[0x66] & 0x3FF, y0 = (r[0x66] >> 16) & 0x3FF, x1 = r[0x67] & 0x3FF, y1 = (r[0x67] >> 16) & 0x3FF;
        sc = (DkScissor){x0, y0, x1 >= x0 ? x1 - x0 + 1 : 0, y1 >= y0 ? y1 - y0 + 1 : 0};
    }
    dkCmdBufSetScissors(g_cmd, 0, &sc, 1);
    dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, sh, 2);
    u32 mode = (r[0x25E] >> 8) & 3;
    DkPrimitive prim = mode == 1 ? DkPrimitive_TriangleStrip : mode == 2 ? DkPrimitive_TriangleFan : DkPrimitive_Triangles;
    if (indexed) {
        dkCmdBufBindIdxBuffer(g_cmd, DkIdxFormat_Uint16, g_dyn_gpu + ioff);
        dkCmdBufDrawIndexed(g_cmd, prim, n, 1, 0, 0, 0);
        GPU_WORK();
    } else dkCmdBufDraw(g_cmd, prim, n, 1, 0, 0);
    GPU_WORK();
    if (r[0x113] && ((dm >> 8) & 0xF)) cs->gpu_dirty = 1;
    if (++g_draws_since_flush >= 256) flush(0);
    return 1;
}

// ---- hooks from the GSP side ------------------------------------------------------------------------

static void dk_sync(u32 a, u32 n) { if (n) sync_range(a, n); }

static void dk_invalidate(u32 a, u32 n) {
    if (n && in_vram(a, n)) {   // the CPU wrote VRAM: the GPU's copy takes it
        vram_will_write(a, n);
        vram_upload(a, n);
        vram_touch(a, n);
        return;
    }
    for (int k = 0; k < MAX_SURF; k++)
        if (overlaps(&g_surf[k], a, n)) {
            if (g_surf[k].gpu_dirty) surf_readback(&g_surf[k]);   // keep what the GPU drew outside the write
            g_surf[k].cpu_dirty = 1;
        }
    for (int k = 0; k < MAX_TEX; k++) {
        Tex* t = &g_tex[k];
        if (t->used && a < t->addr + t->w * t->h * k_tex_bits[t->fmt] / 8 && t->addr < a + n) t->checked_frame = ~0u;
    }
}

static void dk_fill(u32 start, u32 end, u32 value, u32 width) {
    // a fill that covers a whole surface becomes a GPU clear; anything else is re-uploaded later
    for (int k = 0; k < MAX_SURF; k++) {
        Surf* s = &g_surf[k];
        if (!s->used || !overlaps(s, start, end - start)) continue;
        if (start <= s->addr && end >= s->addr + s->bytes) {
            s->clear_pending = 1;
            if (s->depth) s->clear_value = value;
            else if (width & 2) s->clear_value = value;   // 32-bit fill of an RGBA8 target: R G B A from the top byte
            else s->clear_value = (value << 8) | 0xFF;      // 24-bit fill: RGB
            s->gpu_dirty = 0;
            s->cpu_dirty = 0;
        } else {
            if (s->gpu_dirty) surf_readback(s);
            s->cpu_dirty = 1;
        }
    }
}

// A transfer or copy that reads only a render target the GPU drew since it was last stored (and
// nothing else not yet stored) can read its image directly: no store to VRAM first. The render
// target stays unstored until something else needs its VRAM. Port menu: Copies, From images (1)
// or Through VRAM (0).
static int g_img_path = 1;
void dkr_set_img_path(int on) { g_img_path = on; }
static Surf* img_source(u32 a, u32 n) {
    if (!g_img_path) return 0;
    Surf* src = 0;
    for (int k = 0; k < MAX_SURF; k++) {
        Surf* s = &g_surf[k];
        if (!overlaps(s, a, n) || s->depth || !s->gpu_dirty) continue;   // depth is never stored; VRAM is current for the rest
        if (src) return 0;
        src = s;
    }
    if (!src || src->clear_pending || a < src->addr || a + n > src->addr + src->bytes || (src->w & 7) || (src->h & 7)) return 0;
    return src;
}

// GX commands on VRAM, done on the GPU (pica_gx_hook): see hle_hid_gsp.c for the command words.
static const u32 k_fb_bytes[8] = {4, 3, 2, 2, 2, 4, 4, 4};
static int dk_gx(int op, const u32* d) {
    if (op == 2) {   // fill: start, end, value, width
        u32 start = d[0], end = d[1], value = d[2], width = d[3];
        if (!in_vram(start, end - start) || (start & 3) || (end & 3)) return 0;
        for (int k = 0; k < MAX_SURF; k++) {
            Surf* s = &g_surf[k];
            if (!s->used || !overlaps(s, start, end - start)) continue;
            if (!(start <= s->addr && end >= s->addr + s->bytes) && s->gpu_dirty) surf_readback(s);   // keep the rest
            if (start <= s->addr && end >= s->addr + s->bytes) {   // the whole surface: a GPU clear
                s->clear_pending = 1;
                if (s->depth) s->clear_value = value;
                else if (width & 2) s->clear_value = value;
                else s->clear_value = (value << 8) | 0xFF;
                s->gpu_dirty = 0;
                s->cpu_dirty = 0;
            } else if (!s->depth) {
                s->cpu_dirty = 1;
                s->clear_pending = 0;
            }
        }
        u32 q[16] = {start - VRAM_BASE, end - VRAM_BASE, value, width};
        kernel(K_FILL, q, (end - start + 3) / 4 + 1, 0, 0);
        vram_touch(start, end - start);
        return 1;
    }
    if (op == 3) {   // display transfer: input, output, input size, output size, flags
        u32 fi = (d[4] >> 8) & 7, fo = (d[4] >> 12) & 7, down = (d[4] >> 24) & 3;
        u32 ow = (d[3] & 0xFFFF) >> (down ? 1 : 0), oh = (d[3] >> 16) >> (down == 2 ? 1 : 0);
        u32 inb = (d[2] & 0xFFFF) * (d[2] >> 16) * k_fb_bytes[fi], outb = ow * oh * k_fb_bytes[fo];
        if (!inb || !outb || !in_vram(d[0], inb) || !in_vram(d[1], outb) || ((d[0] | d[1]) & 3) || (ow * oh) & 3) return 0;
        MSR_PERF_BEGIN(PERF_TRANSFER);
        u32 iw = d[2] & 0xFFFF, ih = d[2] >> 16;
        Surf* is = (d[4] & 2) ? 0 : img_source(d[0], inb);   // tiled input only
        if (is && is->addr == d[0] && is->w == iw && is->h == ih && is->fmt == fi) {
            vram_will_write(d[1], outb);
            u32 q[16] = {d[0] - VRAM_BASE, d[1] - VRAM_BASE, d[2], d[3], d[4]};
            kernel(K_TRANSFER_IMG, q, ow * oh / 4, &is->img, 0);
            vram_touch(d[1], outb);
            MSR_PERF_END(PERF_TRANSFER);
            return 1;
        }
        store_range(d[0], inb);
        vram_will_write(d[1], outb);
        u32 q[16] = {d[0] - VRAM_BASE, d[1] - VRAM_BASE, d[2], d[3], d[4]};
        kernel(K_TRANSFER, q, ow * oh / 4, 0, 0);
        vram_touch(d[1], outb);
        MSR_PERF_END(PERF_TRANSFER);
        return 1;
    }
    if (op == 4) {   // texture copy: input, output, size, input gap, output gap, flags
        u32 iw = (d[3] & 0xFFFF) * 16, ig = (d[3] >> 16) * 16, ow = (d[4] & 0xFFFF) * 16, og = (d[4] >> 16) * 16;
        u32 size = d[2] & ~15u;
        u32 in_span = iw ? size + size / iw * ig : size, out_span = ow ? size + size / ow * og : size;
        if (!size || !in_vram(d[0], in_span) || !in_vram(d[1], out_span) || ((d[0] | d[1]) & 3)) return 0;
        if (d[0] < d[1] + out_span && d[1] < d[0] + in_span) return 0;   // overlapping copies stay on the CPU
        MSR_PERF_BEGIN(PERF_TEXCOPY);
        Surf* is = img_source(d[0], in_span);
        if (is) {
            vram_will_write(d[1], out_span);
            u32 q[16] = {d[0] - VRAM_BASE, d[1] - VRAM_BASE, size, 0, iw, ig, ow, og, is->addr - VRAM_BASE, is->w, is->h, is->fmt};
            kernel(K_COPY_IMG, q, size / 4, &is->img, 0);
            vram_touch(d[1], out_span);
            MSR_PERF_END(PERF_TEXCOPY);
            return 1;
        }
        store_range(d[0], in_span);
        vram_will_write(d[1], out_span);
        u32 q[16] = {d[0] - VRAM_BASE, d[1] - VRAM_BASE, size, 0, iw, ig, ow, og};
        kernel(K_COPY, q, size / 4, 0, 0);
        vram_touch(d[1], out_span);
        MSR_PERF_END(PERF_TEXCOPY);
        return 1;
    }
    if (op == 0) {   // DMA: source, destination, size
        u32 src = d[0], dst = d[1], n = d[2];
        if (!n || !src || !dst || !in_vram(dst, n)) return 0;   // into guest memory: the CPU path (it syncs)
        if (in_vram(src, n)) {
            if (src < dst + n && dst < src + n) return 0;
            store_range(src, n);
            vram_will_write(dst, n);
            ts_mark(TS_VRAM);
            dkCmdBufCopyBuffer(g_cmd, g_vram_gpu + (src - VRAM_BASE), g_vram_gpu + (dst - VRAM_BASE), n);
            GPU_WORK();
            gpu_barrier();
            vram_touch(dst, n);
            return 1;
        }
        store_range(src, n);   // a render target in guest memory reaches it first
        memmove(g_mem + dst, g_mem + src, n);   // guest memory -> VRAM: the CPU's copy too
        vram_will_write(dst, n);
        vram_upload(dst, n);
        vram_touch(dst, n);
        return 1;
    }
    return 0;
}

// A picture was latched for display: everything queued so far goes to the GPU, so the screen output
// (on the main thread, the same queue) sees it.
static void dk_latch(int screen) { (void)screen; flush(0); }

// The screens, converted on the GPU from its VRAM into out (top 400x240 then bottom 320x240, RGBA8,
// rows bottom to top as the swapchain wants). Recorded into the caller's command buffer; returns
// 0 for a framebuffer outside VRAM (the caller converts that one on the CPU).
int dkr_record_screen(DkCmdBuf cmd, int screen, u32 addr, u32 stride, u32 format, DkGpuAddr out, u32 out_word) {
    u32 W = screen ? 320 : 400, bpp = (format & 7) == 0 ? 4 : (format & 7) == 1 ? 3 : 2;
    u32 bytes = W * (stride ? stride : 240 * bpp);
    if (!in_vram(addr, bytes)) return 0;
    u32 q[16] = {addr - VRAM_BASE, stride, format, W, out_word, 1};
    DkGpuAddr ubo = dkMemBlockGetGpuAddr(g_ubo) + 0x1100;   // the main thread's own slot
    dkCmdBufPushConstants(cmd, ubo, 64, 0, 64, q);
    DkBufExtents ub = {ubo, 64};
    dkCmdBufBindUniformBuffers(cmd, DkStage_Compute, 0, &ub, 1);
    DkBufExtents sb[2] = {{g_vram_gpu, VRAM_SIZE}, {out, (400 + 320) * 240 * 4}};
    dkCmdBufBindStorageBuffers(cmd, DkStage_Compute, 0, sb, 2);
    DkShader const* sh = &g_k[K_SCREEN];
    dkCmdBufBindShaders(cmd, DkStageFlag_Compute, &sh, 1);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader);   // the game's VRAM writes first
    dkCmdBufDispatchCompute(cmd, (W * 240 + 63) / 64, 1, 1);
    dkCmdBufBarrier(cmd, DkBarrier_Full, DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
    return 1;
}

void dkr_vram_stats(unsigned* out) { for (int k = 0; k < K_COUNT; k++) { out[k] = g_vram_ops[k]; g_vram_ops[k] = 0; } }

void dkr_frame_end(void) { g_frame++; }

// ---- setup ------------------------------------------------------------------------------------------------

// ---- the shader pack ----------------------------------------------------------------------------
// shaders.pack holds the game's vertex shaders translated to Switch shaders ahead of time
// (tools/shaders/pica2glsl.py and mkpack.py). Layout: "MSRVSPK1", u32 count, u32 0, then count
// entries {u64 key, u32 offset, u32 size} sorted by key, then the .dksh files at those offsets.
// A configuration missing from the pack runs on the interpreter (pica_vsh.glsl) and is appended
// to vs_new.bin so the next pack can include it.
typedef struct { u64 key; u32 offset, size; } PackEntry;
static PackEntry* g_pack;
static DkShader* g_pack_sh;
static u8* g_pack_ready;
static u32 g_pack_n;
static DkMemBlock g_pack_code;
static char g_new_path[256], g_fs_new_path[256];
static unsigned g_vs_hits, g_vs_misses;
static const DkShader* rt_find(u64 key);
static int compile_request(u64 key, int vert, int ez);
static void fs_compile_poll(void);
static volatile unsigned g_jobs_live;
static unsigned g_rt_version;
static int g_force_vs_interp, g_force_fs_general;
// Comparison switches for tests: run every draw on the interpreter vertex shader and/or the general
// fragment shader, as test builds 1 and 2 did.
static volatile unsigned g_force_version;
void dkr_force_general(int vs, int fs) { g_force_vs_interp = vs; g_force_fs_general = fs; g_force_version++; }

// The pack, from memory (built into msr.nro). new_path and fs_new_path collect missing configurations.
void dkr_load_pack(const u8* data, size_t size, const char* new_path, const char* fs_new_path) {
    snprintf(g_new_path, sizeof g_new_path, "%s", new_path);
    snprintf(g_fs_new_path, sizeof g_fs_new_path, "%s", fs_new_path);
    if (!data || size < 16 || memcmp(data, "MSRVSPK1", 8)) {
        msr_receipt("gpu: no shader pack in this build; every draw uses the general shaders");
        return;
    }
    u32 n;
    memcpy(&n, data + 8, 4);
    if (16 + (size_t)n * sizeof(PackEntry) > size) { msr_receipt("gpu: shader pack is cut short"); return; }
    g_pack = (PackEntry*)malloc(n * sizeof *g_pack);
    memcpy(g_pack, data + 16, n * sizeof *g_pack);
    u32 code_size = (u32)((size + DK_SHADER_CODE_UNUSABLE_SIZE + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1));
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, g_dev, code_size);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    g_pack_code = dkMemBlockCreate(&mm);
    memcpy(dkMemBlockGetCpuAddr(g_pack_code), data, size);
    g_pack_sh = (DkShader*)calloc(n, sizeof *g_pack_sh);
    g_pack_ready = (u8*)calloc(n, 1);
    g_pack_n = n;
    msr_receipt("gpu: shader pack with %u shaders (%u bytes)", n, (unsigned)size);
}

// Finds a key in the pack and prepares its shader the first time; null when it is not there.
static const DkShader* pack_find(u64 key) {
    u32 lo = 0, hi = g_pack_n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_pack[mid].key < key) lo = mid + 1; else hi = mid;
    }
    if (lo >= g_pack_n || g_pack[lo].key != key) return 0;
    if (!g_pack_ready[lo]) {
        DkShaderMaker sm;
        dkShaderMakerDefaults(&sm, g_pack_code, g_pack[lo].offset);
        dkShaderInitialize(&g_pack_sh[lo], &sm);
        g_pack_ready[lo] = 1;
    }
    return &g_pack_sh[lo];
}

// A configuration missing from the pack: a receipt line and a dump for the next pack, once each.
static int first_miss(u64 key) {
    static u64 told[512];
    static unsigned ntold;
    for (unsigned k = 0; k < ntold; k++) if (told[k] == key) return 0;
    if (ntold < 512) told[ntold++] = key;
    return 1;
}

static unsigned g_rt_version_seen_vs;
static const DkShader* vs_select(void) {
    static u64 last_key;
    static const DkShader* last = 0;
    static unsigned seen_version;
    u64 key = pica_vs_key();
    if (g_jobs_live) fs_compile_poll();
    if (seen_version != g_force_version) { seen_version = g_force_version; last = 0; }
    if (g_rt_version_seen_vs != g_rt_version) { g_rt_version_seen_vs = g_rt_version; last = 0; }
    if (last && key == last_key) { if (last == &g_vsh) g_vs_misses++; else g_vs_hits++; return last; }
    last_key = key;
    const DkShader* sh = g_force_vs_interp ? 0 : pack_find(key);
    if (!sh && !g_force_vs_interp) sh = rt_find(key);
    if (sh) { g_vs_hits++; return last = sh; }
    if (g_force_vs_interp) { g_vs_misses++; return last = &g_vsh; }
    g_vs_misses++;
    if (first_miss(key)) {
        msr_receipt("gpu: vertex shader %016llX is not in the pack; compiling it on the Switch, the interpreter meanwhile (saved to vs_new.bin)",
                    (unsigned long long)key);
        pica_vs_dump(key, g_new_path);
    }
    if (compile_request(key, 1, 0)) { last = 0; return &g_vsh; }   // asks again next draw until it is ready
    return last = &g_vsh;
}

static unsigned g_fs_hits, g_fs_misses;
// The fragment shader for this draw. *ez is set when it is the early-depth variant (no depth write;
// the caller gives the viewport the PICA depth mapping instead): only for a z-buffer whose mapping
// stays inside 0..1. Null while the configuration is being compiled on the Switch (the draw waits
// out those frames rather than run on the general shader).
#define EZ_BIT (1ull << 63)
static unsigned g_ez_draws, g_fs_waits;
static const DkShader* fs_select(int* ez) {
    static u64 last_key;
    static int last_ez;
    static const DkShader* last = 0;
    static unsigned seen_version;
    const u32* r = g_pica.reg;
    float scale = pica_f24(r[0x4D]), offset = pica_f24(r[0x4E]);
    float nearz = offset, farz = offset - scale;
    int want_ez = (r[0x6D] & 1) && nearz >= 0 && nearz <= 1 && farz >= 0 && farz <= 1;
    u64 base = pica_fs_key();
    u64 key = base ^ (want_ez ? EZ_BIT : 0);
    if (g_jobs_live) fs_compile_poll();
    if (seen_version != g_force_version) { seen_version = g_force_version; last = 0; }
    if (last && key == last_key) {
        if (last == &g_fsh) g_fs_misses++; else g_fs_hits++;
        *ez = last_ez;
        if (last_ez) g_ez_draws++;
        return last;
    }
    *ez = 0;
    if (g_force_fs_general) { g_fs_misses++; last_key = key; last_ez = 0; return last = &g_fsh; }
    const DkShader* sh;
    int got_ez = want_ez;
    if (!(sh = pack_find(key)) && !(sh = rt_find(key))) {
        got_ez = 0;
        if (want_ez && !(sh = pack_find(base))) sh = rt_find(base);
    }
    if (sh) {
        g_fs_hits++;
        last_key = key; last_ez = got_ez; *ez = got_ez;
        if (got_ez) g_ez_draws++;
        return last = sh;
    }
    // missing: compile it here (the exact variant wanted); the general shader only if that failed
    if (first_miss(base)) {
        msr_receipt("gpu: fragment shader %016llX is not in the pack; compiling it on the Switch (saved to fs_new.bin)",
                    (unsigned long long)base);
        pica_fs_dump(base, g_fs_new_path);
    }
    if (compile_request(key, 0, want_ez)) { g_fs_waits++; last = 0; return 0; }
    g_fs_misses++;
    last_key = key; last_ez = 0;
    return last = &g_fsh;
}

void dkr_fs_stats(unsigned* hits, unsigned* misses, unsigned* waits) { *hits = g_fs_hits; *misses = g_fs_misses; *waits = g_fs_waits; g_fs_hits = g_fs_misses = g_fs_waits = 0; }
unsigned dkr_ez_stats(void) { unsigned n = g_ez_draws; g_ez_draws = 0; return n; }

// ---- shaders compiled on the Switch -------------------------------------------------------------
// A configuration missing from the pack is turned into GLSL here exactly as the pack's tools do
// (fragment: pica_fsh.glsl with the configuration's registers as constants, as fsgen.py; vertex:
// vsgen.c, the C version of pica2glsl.py) and compiled by uam, the same compiler the pack is built
// with, linked in from tools/switch/uam. One worker thread compiles (uam's front end is shared
// state, so two at once are not safe); the result goes to sdmc:/switch/msr/shadercache, which is
// loaded at start, so each configuration is compiled once per SD card. A fragment shader's draws
// wait for it; a vertex shader's draws run on the interpreter meanwhile.
int msr_uam_compile_frag(const char* glsl, const char* out_path);   // tools/switch/uam/uam_iface.cpp
int msr_uam_compile_vert(const char* glsl, const char* out_path);
#define RT_CODE_SIZE (16u << 20)
#define RT_MAX 2048
#define MAX_JOBS 64
typedef struct { u64 key; DkShader sh; } RtShader;
static RtShader g_rt[RT_MAX];
static u32 g_rt_n, g_rt_pos;
static DkMemBlock g_rt_code;
static char g_cache_dir[256];
enum { JOB_FREE, JOB_QUEUED, JOB_RUNNING, JOB_DONE, JOB_FAILED };
typedef struct { int state, vert; u64 key; char* glsl; u8* dksh; u32 size; u32 ms; } Job;
static Job g_jobs[MAX_JOBS];
static Mutex g_jmutex;
static CondVar g_jcond;
static Thread g_cthread;
static int g_compiler_on;
static u64 g_failed[256];
static unsigned g_nfailed, g_compiled, g_compile_ms, g_compile_max_ms, g_cache_loaded;

static const DkShader* rt_find(u64 key) {
    for (u32 k = 0; k < g_rt_n; k++) if (g_rt[k].key == key) return &g_rt[k].sh;
    return 0;
}

static int rt_install(u64 key, const u8* dksh, u32 size) {
    if (size < 64 || memcmp(dksh, "DKSH", 4) || g_rt_n >= RT_MAX) return 0;
    u32 pos = (g_rt_pos + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (pos + size + DK_SHADER_CODE_UNUSABLE_SIZE > RT_CODE_SIZE) return 0;
    memcpy((u8*)dkMemBlockGetCpuAddr(g_rt_code) + pos, dksh, size);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, g_rt_code, pos);
    dkShaderInitialize(&g_rt[g_rt_n].sh, &sm);
    g_rt[g_rt_n].key = key;
    g_rt_n++;
    g_rt_pos = pos + size;
    g_rt_version++;
    return 1;
}

// pica_fsh.glsl for the current configuration: fsgen.py's specialize(), line for line.
static char* fs_source_from(const u32* regs, const u32* vals, unsigned n, u32 wbuf, int ez) {
    size_t src_len = strlen(pica_fsh_glsl);
    char* out = (char*)malloc(src_len + 64 * 64 + 256);
    if (!out) return 0;
    char* o = out;
    o += sprintf(o, "// Specialized on the Switch from pica_fsh.glsl (dk_pica.c, fs_source).\n");
    const char* p = pica_fsh_glsl;
    int skipping = 0;
    while (*p) {
        const char* e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        size_t cl = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, p, cl);
        line[cl] = 0;
        p += len + (e ? 1 : 0);
        if (ez && strstr(line, "MSR-DEPTH-BEGIN")) { skipping = 1; continue; }
        if (skipping) { if (strstr(line, "MSR-DEPTH-END")) skipping = 0; continue; }
        if (strstr(line, "MSR-STRUCTURAL")) {
            o += sprintf(o, "uint reg(uint id) {\n");
            for (unsigned k = 0; k < n; k++) o += sprintf(o, "    if (id == 0x%Xu) return 0x%Xu;\n", (unsigned)regs[k], (unsigned)vals[k]);
            o += sprintf(o, "    return dreg(id);\n}\n");
        } else if (strstr(line, "MSR-WBUFFER")) {
            o += sprintf(o, "bool wbuffer() { return %s; }\n", wbuf ? "true" : "false");
        } else {
            memcpy(o, p - len - (e ? 1 : 0), len);
            o += len;
            *o++ = '\n';
        }
    }
    *o = 0;
    return out;
}

static char* fs_source(int ez) {
    u32 regs[64], vals[64], wbuf;
    unsigned n = pica_fs_pairs(regs, vals, &wbuf);
    return fs_source_from(regs, vals, n, wbuf, ez);
}

static void compile_thread(void* arg) {
    (void)arg;
    for (;;) {
        mutexLock(&g_jmutex);
        Job* j = 0;
        for (;;) {
            for (int k = 0; k < MAX_JOBS && !j; k++) if (g_jobs[k].state == JOB_QUEUED) j = &g_jobs[k];
            if (j) break;
            condvarWait(&g_jcond, &g_jmutex);
        }
        j->state = JOB_RUNNING;
        mutexUnlock(&g_jmutex);
        u64 t0 = armGetSystemTick();
        char tmp[300], final[300];
        const char* pre = j->vert ? "vs" : "fs";
        snprintf(tmp, sizeof tmp, "%s/%s_%016llX.tmp", g_cache_dir, pre, (unsigned long long)j->key);
        snprintf(final, sizeof final, "%s/%s_%016llX.dksh", g_cache_dir, pre, (unsigned long long)j->key);
        int ok = j->vert ? msr_uam_compile_vert(j->glsl, tmp) : msr_uam_compile_frag(j->glsl, tmp);
        u8* data = 0;
        long size = 0;
        if (ok) {
            remove(final);
            rename(tmp, final);
            FILE* f = fopen(final, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                size = ftell(f);
                fseek(f, 0, SEEK_SET);
                data = size > 0 ? (u8*)malloc(size) : 0;
                if (data && fread(data, 1, size, f) != (size_t)size) { free(data); data = 0; }
                fclose(f);
            }
        }
        u32 ms = (u32)(armTicksToNs(armGetSystemTick() - t0) / 1000000);
        mutexLock(&g_jmutex);
        free(j->glsl);
        j->glsl = 0;
        j->dksh = data;
        j->size = (u32)size;
        j->ms = ms;
        j->state = data ? JOB_DONE : JOB_FAILED;
        mutexUnlock(&g_jmutex);
    }
}

// Finished compiles become usable shaders (on the drawing thread, which owns the code memory).
static void fs_compile_poll(void) {
    mutexLock(&g_jmutex);
    for (int k = 0; k < MAX_JOBS; k++) {
        Job* j = &g_jobs[k];
        if (j->state == JOB_DONE) {
            if (!rt_install(j->key, j->dksh, j->size)) {
                if (g_nfailed < 256) g_failed[g_nfailed++] = j->key;
                msr_receipt("gpu: compiled %s shader %016llX did not fit", j->vert ? "vertex" : "fragment", (unsigned long long)j->key);
            } else {
                g_compiled++;
                g_compile_ms += j->ms;
                if (j->ms > g_compile_max_ms) g_compile_max_ms = j->ms;
                msr_receipt("gpu: %s shader %016llX compiled on the Switch in %u ms", j->vert ? "vertex" : "fragment", (unsigned long long)j->key, j->ms);
            }
            free(j->dksh);
            j->dksh = 0;
            j->state = JOB_FREE;
            g_jobs_live--;
        } else if (j->state == JOB_FAILED) {
            if (g_nfailed < 256) g_failed[g_nfailed++] = j->key;
            msr_receipt("gpu: %s shader %016llX failed to compile on the Switch; it uses the %s", j->vert ? "vertex" : "fragment",
                        (unsigned long long)j->key, j->vert ? "interpreter" : "general one");
            j->state = JOB_FREE;
            g_jobs_live--;
        }
    }
    mutexUnlock(&g_jmutex);
}

// 1: being compiled, 0: no compiler, or it failed before (general shader or interpreter).
static int compile_request(u64 key, int vert, int ez) {
    if (!g_compiler_on) return 0;
    for (unsigned k = 0; k < g_nfailed; k++) if (g_failed[k] == key) return 0;
    mutexLock(&g_jmutex);
    Job* free_j = 0;
    for (int k = 0; k < MAX_JOBS; k++) {
        Job* j = &g_jobs[k];
        if (j->state != JOB_FREE && j->key == key) { mutexUnlock(&g_jmutex); return 1; }
        if (j->state == JOB_FREE && !free_j) free_j = j;
    }
    if (!free_j) { mutexUnlock(&g_jmutex); return 1; }   // queue full: wait a frame and ask again
    char* src;
    if (vert) {
        const u32* r = g_pica.reg;
        VsConfig c;
        c.code = g_pica.vs.program;
        c.swz = g_pica.vs.swizzle;
        c.entry = r[0x2BA] & 0xFFFF;
        c.outmask = r[0x2BD];
        c.total = r[0x4F] & 7;
        for (int k = 0; k < 7; k++) c.outmap[k] = r[0x50 + k];
        const char* why = 0;
        src = vsgen_glsl(&c, &why);
        if (!src) {
            if (g_nfailed < 256) g_failed[g_nfailed++] = key;
            msr_receipt("gpu: vertex shader %016llX cannot be translated (%s); it stays on the interpreter", (unsigned long long)key, why ? why : "?");
        }
    } else src = fs_source(ez);
    if (!src) { mutexUnlock(&g_jmutex); return 0; }
    free_j->vert = vert;
    free_j->key = key;
    free_j->glsl = src;
    free_j->state = JOB_QUEUED;
    g_jobs_live++;
    condvarWakeOne(&g_jcond);
    mutexUnlock(&g_jmutex);
    return 1;
}

// Start-up: the compiled shaders already on the SD card, and the compiler thread.
void dkr_compiler_init(const char* cache_dir) {
    snprintf(g_cache_dir, sizeof g_cache_dir, "%s", cache_dir);
    mkdir(cache_dir, 0777);
    DkMemBlockMaker mm;
    dkMemBlockMakerDefaults(&mm, g_dev, RT_CODE_SIZE);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    g_rt_code = dkMemBlockCreate(&mm);
    DIR* d = opendir(cache_dir);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != 0) {
            const char* nm = e->d_name;
            if ((strncmp(nm, "fs_", 3) && strncmp(nm, "vs_", 3)) || strlen(nm) != 3 + 16 + 5 || strcmp(nm + 19, ".dksh")) continue;
            u64 key = strtoull(nm + 3, 0, 16);
            if (pack_find(key) || rt_find(key)) continue;
            char path[300];
            snprintf(path, sizeof path, "%s/%s", cache_dir, nm);
            FILE* f = fopen(path, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            u8* data = size > 0 && size < (1 << 20) ? (u8*)malloc(size) : 0;
            if (data && fread(data, 1, size, f) == (size_t)size && rt_install(key, data, (u32)size)) g_cache_loaded++;
            free(data);
            fclose(f);
        }
        closedir(d);
    }
    mutexInit(&g_jmutex);
    condvarInit(&g_jcond);
    Result rc = threadCreate(&g_cthread, compile_thread, 0, 0, 4u << 20, 0x2C, 2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_cthread);
    g_compiler_on = R_SUCCEEDED(rc);
    msr_receipt("gpu: %u compiled shaders loaded from %s; compiling on the Switch %s",
                g_cache_loaded, cache_dir, g_compiler_on ? "is on" : "could not start");
}

void dkr_compile_stats(unsigned* compiled, unsigned* avg_ms, unsigned* max_ms, unsigned* pending) {
    *compiled = g_compiled;
    *avg_ms = g_compiled ? g_compile_ms / g_compiled : 0;
    *max_ms = g_compile_max_ms;
    *pending = g_jobs_live;
}

void dkr_vs_stats(unsigned* hits, unsigned* misses) { *hits = g_vs_hits; *misses = g_vs_misses; g_vs_hits = g_vs_misses = 0; }

static void shader_load(DkShader* sh, const u8* code, u32 size, u32* code_off) {
    u8* cpu = (u8*)dkMemBlockGetCpuAddr(g_code);
    memcpy(cpu + *code_off, code, size);
    DkShaderMaker sm;
    dkShaderMakerDefaults(&sm, g_code, *code_off);
    dkShaderInitialize(sh, &sm);
    *code_off += (size + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
}

void dkr_init(DkDevice dev, DkQueue q, Mutex* qlock) {
    g_dev = dev;
    g_q = q;
    g_qlock = qlock;
    DkMemBlockMaker mm;
    static const u8* const kcode[K_COUNT] = {vram_fill_dksh, vram_copy_dksh, vram_transfer_dksh, vram_store_dksh,
                                             vram_load_dksh, vram_texdecode_dksh, vram_screen_dksh, vram_transfer_img_dksh, vram_copy_img_dksh};
    static const u32 ksize[K_COUNT] = {vram_fill_dksh_size, vram_copy_dksh_size, vram_transfer_dksh_size, vram_store_dksh_size,
                                       vram_load_dksh_size, vram_texdecode_dksh_size, vram_screen_dksh_size, vram_transfer_img_dksh_size,
                                       vram_copy_img_dksh_size};
    u32 all = pica_vsh_dksh_size + pica_fsh_dksh_size + selftest_vsh_dksh_size;
    for (int k = 0; k < K_COUNT; k++) all += ksize[k];
    u32 code_size = ((all + (3 + K_COUNT) * DK_SHADER_CODE_ALIGNMENT + DK_SHADER_CODE_UNUSABLE_SIZE) + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dkMemBlockMakerDefaults(&mm, dev, code_size);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    g_code = dkMemBlockCreate(&mm);
    u32 co = 0;
    shader_load(&g_vsh, pica_vsh_dksh, pica_vsh_dksh_size, &co);
    shader_load(&g_fsh, pica_fsh_dksh, pica_fsh_dksh_size, &co);
    shader_load(&g_test_vsh, selftest_vsh_dksh, selftest_vsh_dksh_size, &co);
    for (int k = 0; k < K_COUNT; k++) shader_load(&g_k[k], kcode[k], ksize[k], &co);

    // the GPU's copy of VRAM, empty like the guest's at boot. (dkr_init runs before the guest
    // address space is mapped: guest memory must not be touched here.)
    dkMemBlockMakerDefaults(&mm, dev, VRAM_SIZE);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_vram = dkMemBlockCreate(&mm);
    g_vram_gpu = dkMemBlockGetGpuAddr(g_vram);
    memset(dkMemBlockGetCpuAddr(g_vram), 0, VRAM_SIZE);
    dkMemBlockMakerDefaults(&mm, dev, TS_RING * 16);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_ts_mem = dkMemBlockCreate(&mm);

    dkMemBlockMakerDefaults(&mm, dev, DYN_SIZE);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_dyn = dkMemBlockCreate(&mm);
    dkMemBlockMakerDefaults(&mm, dev, RB_SIZE);
    mm.flags = DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached;
    g_rb = dkMemBlockCreate(&mm);
    g_rb_cpu = (u8*)dkMemBlockGetCpuAddr(g_rb);
    g_rb_gpu = dkMemBlockGetGpuAddr(g_rb);
    g_dyn_cpu = (u8*)dkMemBlockGetCpuAddr(g_dyn);
    g_dyn_gpu = dkMemBlockGetGpuAddr(g_dyn);

    dkMemBlockMakerDefaults(&mm, dev, 0x2000);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_ubo = dkMemBlockCreate(&mm);

    dkMemBlockMakerDefaults(&mm, dev, IMG_SIZE);
    mm.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    g_img = dkMemBlockCreate(&mm);

    dkMemBlockMakerDefaults(&mm, dev, CMD_SIZE);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    g_cmdmem = dkMemBlockCreate(&mm);
    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, dev);
    cm.cbAddMem = cmd_out_of_memory;
    g_cmd = dkCmdBufCreate(&cm);
    cmd_add_chunk();

    // the lighting tables: 256 x 24, value and difference
    DkImageLayoutMaker lm;
    DkImageLayout lay;
    dkImageLayoutMakerDefaults(&lm, dev);
    lm.format = DkImageFormat_RG32_Float;
    lm.dimensions[0] = 256;
    lm.dimensions[1] = 24;
    dkImageLayoutInitialize(&lay, &lm);
    dkImageInitialize(&g_lut_img, &lay, g_img, img_alloc(&lay));
    g_img_floor = g_img_pos;   // render targets go after the lighting tables

    pica_draw_hook = dk_draw;
    pica_sync_hook = dk_sync;
    pica_invalidate_hook = dk_invalidate;
    pica_fill_hook = dk_fill;
    pica_gx_hook = dk_gx;
    pica_latch_hook = dk_latch;
    msr_receipt("gpu: deko3d PICA renderer ready (shaders %u + %u bytes)", (unsigned)pica_vsh_dksh_size, (unsigned)pica_fsh_dksh_size);
}

// ---- self-test: the general fragment shader against the pack's, on this GPU ----------------------
// Each fragment configuration the pack was made from (records as in fs_new.bin, built into msr.nro)
// is drawn over a 64x64 target with the same inputs: with the pack's shader for it and with the
// general shader; the first few are also specialized and compiled here on the Switch (as for a
// configuration missing from the pack) and drawn with that. Both should give the pack's picture.
int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h);
static u32 st_rand(u32* x) { *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5; return *x; }

void dkr_selftest(const u8* rec, size_t size, const char* png_path) {
    if (!rec || size < 20 || !g_pack_n) return;
    enum { S = 64, NI = 3 };
    DkImageLayoutMaker lm;
    DkImageLayout lay, tlay;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = S;
    lm.dimensions[1] = S;
    dkImageLayoutInitialize(&lay, &lm);
    DkImage rt[NI], texi;
    for (int k = 0; k < NI; k++) dkImageInitialize(&rt[k], &lay, g_img, img_alloc(&lay));
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = S;
    lm.dimensions[1] = S;
    dkImageLayoutInitialize(&tlay, &lm);
    dkImageInitialize(&texi, &tlay, g_img, img_alloc(&tlay));
    {   // a texture with every channel varying, and lighting tables with a slope
        u32 off = dyn_alloc(S * S * 4, 256);
        u8* t = g_dyn_cpu + off;
        for (u32 y = 0; y < S; y++)
            for (u32 x = 0; x < S; x++) {
                u8* p = t + (y * S + x) * 4;
                p[0] = (u8)(x * 4); p[1] = (u8)(y * 4); p[2] = (u8)((x ^ y) * 4); p[3] = (u8)(255 - x * 2);
            }
        DkImageView v;
        dkImageViewDefaults(&v, &texi);
        DkCopyBuf cb = {g_dyn_gpu + off, 0, 0};
        DkImageRect r = {0, 0, 0, S, S, 1};
        dkCmdBufCopyBufferToImage(g_cmd, &cb, &v, &r, 0);
        GPU_WORK();
        u32 lo = dyn_alloc(256 * 24 * 8, 256);
        float* l = (float*)(g_dyn_cpu + lo);
        for (u32 tb = 0; tb < 24; tb++)
            for (u32 e = 0; e < 256; e++) { l[(tb * 256 + e) * 2] = e / 255.f * (0.5f + tb / 48.f); l[(tb * 256 + e) * 2 + 1] = 1.f / 255.f; }
        dkImageViewDefaults(&v, &g_lut_img);
        DkCopyBuf lb = {g_dyn_gpu + lo, 0, 0};
        DkImageRect lr = {0, 0, 0, 256, 24, 1};
        dkCmdBufCopyBufferToImage(g_cmd, &lb, &v, &lr, 0);
        g_lut_uploaded = ~0u;   // the game's tables go in at its first draw
    }
    unsigned tested = 0, diff_new = 0, diff_old = 0, worst_n = 0, lines = 0, n_comp = 0;
    u64 worst_key = 0;
    static u8 worst[S * NI * S * 3];
    size_t pos = 0;
    while (pos + 16 <= size) {
        u32 h[4];
        memcpy(h, rec + pos, 16);
        if (h[0] != 0x31534650u || pos + 16 + h[3] * 8 + 4 > size) break;
        u64 key = h[1] | ((u64)h[2] << 32);
        const u8* pairs = rec + pos + 16;
        u32 wbuf;
        memcpy(&wbuf, pairs + h[3] * 8, 4);
        pos += 16 + h[3] * 8 + 4;
        const DkShader* packed = pack_find(key);
        if (!packed) continue;
        struct { u32 regs[352]; float depth[4]; } fs;
        u32 seed = (u32)key | 1u;
        for (int k = 0; k < 352; k++) fs.regs[k] = st_rand(&seed);
        for (u32 k = 0; k < h[3]; k++) {
            u32 rv[2];
            memcpy(rv, pairs + 8 * k, 8);
            if (rv[0] >= 0x80 && rv[0] < 0x80 + 352) fs.regs[rv[0] - 0x80] = rv[1];
        }
        fs.depth[0] = -1.f; fs.depth[1] = 0.f; fs.depth[2] = wbuf ? 1.f : 0.f; fs.depth[3] = 0.f;
        dyn_reserve(64 * 1024);
        DkGpuAddr ubo = dkMemBlockGetGpuAddr(g_ubo);
        dkCmdBufPushConstants(g_cmd, ubo + VS_UBO_SIZE, FS_UBO_SIZE, 0, sizeof fs, &fs);
        DkBufExtents ub1 = {ubo + VS_UBO_SIZE, FS_UBO_SIZE};
        dkCmdBufBindUniformBuffers(g_cmd, DkStage_Fragment, 1, &ub1, 1);
        u32 doff = dyn_alloc(4 * sizeof(DkImageDescriptor) + 4 * sizeof(DkSamplerDescriptor), 64);
        DkImageDescriptor* idesc = (DkImageDescriptor*)(g_dyn_cpu + doff);
        DkSamplerDescriptor* sdesc = (DkSamplerDescriptor*)(g_dyn_cpu + doff + 4 * sizeof(DkImageDescriptor));
        for (int u = 0; u < 4; u++) {
            DkImageView iv;
            dkImageViewDefaults(&iv, u < 3 ? &texi : &g_lut_img);
            dkImageDescriptorInitialize(&idesc[u], &iv, false, false);
            DkSampler smp;
            dkSamplerDefaults(&smp);
            dkSamplerDescriptorInitialize(&sdesc[u], &smp);
        }
        dkCmdBufBindImageDescriptorSet(g_cmd, g_dyn_gpu + doff, 4);
        dkCmdBufBindSamplerDescriptorSet(g_cmd, g_dyn_gpu + doff + 4 * sizeof(DkImageDescriptor), 4);
        DkResHandle th[4] = {dkMakeTextureHandle(0, 0), dkMakeTextureHandle(1, 1), dkMakeTextureHandle(2, 2), dkMakeTextureHandle(3, 3)};
        dkCmdBufBindTextures(g_cmd, DkStage_Fragment, 0, th, 4);
        DkRasterizerState rs;
        dkRasterizerStateDefaults(&rs);
        rs.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(g_cmd, &rs);
        DkColorState cst;
        dkColorStateDefaults(&cst);
        dkCmdBufBindColorState(g_cmd, &cst);
        DkColorWriteState cw;
        dkColorWriteStateDefaults(&cw);
        dkCmdBufBindColorWriteState(g_cmd, &cw);
        DkDepthStencilState dss;
        dkDepthStencilStateDefaults(&dss);
        dss.depthTestEnable = false;
        dss.depthWriteEnable = false;
        dkCmdBufBindDepthStencilState(g_cmd, &dss);
        DkViewport vp = {0, 0, S, S, 0.0f, 1.0f};
        dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
        DkScissor sc = {0, 0, S, S};
        dkCmdBufSetScissors(g_cmd, 0, &sc, 1);
        const DkShader* compiled = 0;
        u32 cms = 0;
        if (g_compiler_on && n_comp < 4) {
            u32 regs[64], vals[64];
            unsigned np = 0;
            for (u32 k = 0; k < h[3] && np < 64; k++) { u32 rv[2]; memcpy(rv, pairs + 8 * k, 8); regs[np] = rv[0]; vals[np] = rv[1]; np++; }
            char* src = fs_source_from(regs, vals, np, wbuf, 0);
            char path[300];
            snprintf(path, sizeof path, "%s/selftest.dksh", g_cache_dir);
            u64 t0 = armGetSystemTick();
            int ok = src && msr_uam_compile_frag(src, path);
            cms = (u32)(armTicksToNs(armGetSystemTick() - t0) / 1000000);
            free(src);
            FILE* f = ok ? fopen(path, "rb") : 0;
            if (f) {
                static u8 buf[1 << 20];
                size_t sz = fread(buf, 1, sizeof buf, f);
                fclose(f);
                if (rt_install(~0ull - n_comp, buf, (u32)sz)) compiled = &g_rt[g_rt_n - 1].sh;
            }
            remove(path);
            n_comp++;
            if (!compiled) msr_receipt("fs self-test: %016llX could not be compiled on the Switch", (unsigned long long)key);
        }
        const DkShader* fsh[NI] = {packed, &g_fsh, compiled ? compiled : &g_fsh};
        for (int k = 0; k < NI; k++) {
            DkImageView cv;
            dkImageViewDefaults(&cv, &rt[k]);
            dkCmdBufBindRenderTarget(g_cmd, &cv, NULL);
            dkCmdBufClearColorFloat(g_cmd, 0, DkColorMask_RGBA, 1.f, 0.f, 1.f, 0.5f);   // magenta: discarded
            DkShader const* sh[2] = {&g_test_vsh, fsh[k]};
            dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, sh, 2);
            dkCmdBufDraw(g_cmd, DkPrimitive_Triangles, 3, 1, 0, 0);
            gpu_barrier();
            DkCopyBuf cb = {g_rb_gpu + k * S * S * 4, 0, 0};
            DkImageRect r = {0, 0, 0, S, S, 1};
            dkCmdBufCopyImageToBuffer(g_cmd, &cv, &r, &cb, 0);
        }
        flush(1);
        armDCacheFlush(g_rb_cpu, NI * S * S * 4);
        const u8* px = g_rb_cpu;
        unsigned nd[NI] = {0}, md[NI] = {0};
        for (int k = 1; k < NI; k++)
            for (u32 i = 0; i < S * S * 4; i += 4) {
                unsigned m = 0;
                for (int c = 0; c < 4; c++) {
                    int d = (int)px[k * S * S * 4 + i + c] - (int)px[i + c];
                    if (d < 0) d = -d;
                    if ((unsigned)d > m) m = (unsigned)d;
                }
                if (m > 2) nd[k]++;
                if (m > md[k]) md[k] = m;
            }
        tested++;
        if (nd[1]) diff_new++;
        if (compiled) {
            if (nd[2]) diff_old++;
            msr_receipt("fs self-test: %016llX compiled on the Switch in %u ms: differs from the pack's at %u of %u pixels (most %u)",
                        (unsigned long long)key, cms, nd[2], S * S, md[2]);
        }
        unsigned score = compiled ? nd[2] * 2 + 1 : 0;
        if (score > worst_n || tested == 1) {   // the first one is pictured even when all match
            worst_n = score;
            worst_key = key;
            for (int k = 0; k < NI; k++)
                for (u32 y = 0; y < S; y++)
                    for (u32 x = 0; x < S; x++)
                        for (int c = 0; c < 3; c++) worst[(y * S * NI + k * S + x) * 3 + c] = px[k * S * S * 4 + (y * S + x) * 4 + c];
        }
    }
    msr_receipt("fs self-test: %u configurations drawn with the pack's shader and the general one: the general one differs in %u; %u compiled on the Switch, %u of them differ",
                tested, diff_new, n_comp, diff_old);
    if (tested && png_path) {
        msr_write_png(png_path, worst, S * NI, S);
        msr_receipt("fs self-test: %016llX pictured in %s (pack's, general, compiled on the Switch)", (unsigned long long)worst_key, png_path);
    }
    g_bound_color = g_bound_depth = 0;
    g_img_pos = g_img_floor;   // the test's images are free again
}

// Called before the platform shows the LCD framebuffers: everything drawn so far reaches memory.
void dkr_present_sync(void) {
    if (g_draws_since_flush) flush(0);
}
