// MSR-nx: the PICA200 renderer on the Switch GPU (deko3d).
//
// The game's command lists are read by pica_cmd.c as everywhere; each draw comes here instead of
// the CPU reference renderer. Vertices are fetched on the CPU into a buffer, the game's vertex
// program runs on the GPU in an interpreter shader (pica_vsh.glsl), and the fragment pipeline
// (textures, lighting, combiners, alpha test, PICA depth) runs in pica_fsh.glsl. Fixed-function
// state (blending, depth, stencil, culling, viewport, scissor) maps to deko3d state.
//
// Render targets live on the GPU as images that mirror the guest memory the game points them at;
// a surface the GPU drew is copied back (and tiled the PICA way) before the CPU side reads that
// memory (display transfers, texture copies, DMA, textures), and guest memory the CPU side wrote
// is uploaded again before the GPU draws into it.
#include <deko3d.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include "msr_log.h"
#include "pica.h"
#include "dk_shaders.h"   // generated at build time from the .glsl files (uam)

#define DYN_SIZE (32u << 20)
#define IMG_SIZE (128u << 20)
#define CMD_SIZE (8u << 20)
#define SURF_REGION (64u << 20)
#define VS_UBO_SIZE 0x800u
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
static DkShader g_vsh, g_fsh;
static u8* g_dyn_cpu;
static DkGpuAddr g_dyn_gpu;
static u32 g_dyn_pos, g_img_pos, g_tex_pos = SURF_REGION;
static unsigned g_draws_since_flush;

// ---- memory -------------------------------------------------------------------------------------

static void flush(int wait);
static const DkShader* vs_select(void);
static const DkShader* fs_select(void);

static u32 dyn_alloc(u32 size, u32 align) {
    u32 p = (g_dyn_pos + align - 1) & ~(align - 1);
    if (p + size > DYN_SIZE) {   // full: let the GPU finish everything and start over
        flush(1);
        p = 0;
    }
    g_dyn_pos = p + size;
    return p;
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

static void surf_upload(Surf* s) {
    if (s->depth) { s->cpu_dirty = 0; return; }   // depth only ever comes from clears
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
    s->cpu_dirty = 0;
    MSR_PERF_END(PERF_SURF_UPLOAD);
}

static void surf_readback(Surf* s) {
    if (s->depth || !s->gpu_dirty) { s->gpu_dirty = 0; return; }
    MSR_PERF_BEGIN(PERF_READBACK);
    u32 bytes = s->w * s->h * 4;
    if (bytes > RB_SIZE) { s->gpu_dirty = 0; return; }
    DkImageView v;
    dkImageViewDefaults(&v, &s->img);
    DkCopyBuf cb = {g_rb_gpu, 0, 0};
    DkImageRect r = {0, 0, 0, s->w, s->h, 1};
    dkCmdBufCopyImageToBuffer(g_cmd, &v, &r, &cb, 0);
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

static Surf* surf_get(u32 addr, u32 w, u32 h, u32 fmt, int depth) {
    Surf* free_s = 0;
    for (int k = 0; k < MAX_SURF; k++) {
        Surf* s = &g_surf[k];
        if (s->used && s->addr == addr && s->w == w && s->h == h && s->depth == depth) { s->fmt = fmt; return s; }
        if (!s->used && !free_s) free_s = s;
    }
    if (!free_s) return 0;
    Surf* s = free_s;
    DkImageLayoutMaker lm;
    DkImageLayout lay;
    dkImageLayoutMakerDefaults(&lm, g_dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    lm.format = depth ? DkImageFormat_Z24S8 : DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = w;
    lm.dimensions[1] = h;
    dkImageLayoutInitialize(&lay, &lm);
    u32 off = img_alloc(&lay);
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

static void sync_range(u32 a, u32 n) {
    for (int k = 0; k < MAX_SURF; k++)
        if (overlaps(&g_surf[k], a, n) && g_surf[k].gpu_dirty) surf_readback(&g_surf[k]);
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
    sync_range(v, size);
    Tex* hit = 0, *free_t = 0;
    for (int k = 0; k < MAX_TEX; k++) {
        Tex* t = &g_tex[k];
        if (t->used && t->addr == v && t->w == w && t->h == h && t->fmt == fmt) { hit = t; break; }
        if (!t->used && !free_t) free_t = t;
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
}

// ---- submission ----------------------------------------------------------------------------------

static unsigned g_program_uploaded = ~0u;
static DkGpuAddr g_program_addr;
static unsigned g_flushes;

static void flush(int wait) {
    mutexLock(g_qlock);
    dkQueueSubmitCommands(g_q, dkCmdBufFinishList(g_cmd));
    dkQueueFlush(g_q);
    if (wait) { MSR_PERF_BEGIN(PERF_GPU_WAIT); dkQueueWaitIdle(g_q); MSR_PERF_END(PERF_GPU_WAIT); }
    mutexUnlock(g_qlock);
    if (wait) {
        dkCmdBufClear(g_cmd);
        g_dyn_pos = 0;
        g_program_uploaded = ~0u;   // it lived in the dynamic area
    }
    g_draws_since_flush = 0;
    g_flushes++;
    g_bound_color = g_bound_depth = 0;   // bindings do not carry over a new command list
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
    u64 perm = (u64)r[0x2BB] | ((u64)r[0x2BC] << 32);
    u32 nattr = (r[0x202] >> 28) + 1, stride = 0;
    for (u32 a = 0; a < nattr && a < 12; a++) { u32 reg = (perm >> (4 * a)) & 0xF; if (reg + 1 > stride) stride = reg + 1; }
    if (!stride) stride = 1;
    u32 need = count * stride * 16 + n * 2 + (4096 + 128) * 4 + 1024 + 4 * 256;
    if (((g_dyn_pos + 255) & ~255u) + need > DYN_SIZE) flush(1);
    u32 voff = dyn_alloc(count * stride * 16, 256);
    float* vdst = (float*)(g_dyn_cpu + voff);
    MSR_PERF_BEGIN(PERF_VERTEX);
    for (u32 i = 0; i < count; i++) {
        Vec4 in[16];
        for (u32 k = 0; k < stride; k++) in[k] = (Vec4){0, 0, 0, 1};
        pica_load_attributes(lo + i, in);
        memcpy(vdst + i * stride * 4, in, stride * 16);
    }
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
            cs->clear_pending = 0;
        }
        if (ds && ds->clear_pending) {
            u32 v = ds->clear_value;
            dkCmdBufClearDepthStencil(g_cmd, true, (v & 0xFFFFFF) / 16777215.0f, 0xFF, (u8)(v >> 24));
            ds->clear_pending = 0;
        }
    }
    DkBufExtents sb[2] = {{g_program_addr, (4096 + 128) * 4}, {g_dyn_gpu + voff, count * stride * 16}};
    dkCmdBufBindStorageBuffers(g_cmd, DkStage_Vertex, 0, sb, 2);
    struct {
        Vec4 f[96];
        u32 ints[4][4];
        u32 cfg[4];
        u32 outmap[7][4];
        u32 base[4];
    } vs;
    memcpy(vs.f, g_pica.vs.f, sizeof vs.f);
    for (int k = 0; k < 4; k++) {
        u32 iu = r[0x2B1 + k];
        vs.ints[k][0] = iu & 0xFF; vs.ints[k][1] = (iu >> 8) & 0xFF; vs.ints[k][2] = (iu >> 16) & 0xFF; vs.ints[k][3] = 0;
    }
    vs.cfg[0] = r[0x2B0] & 0xFFFF; vs.cfg[1] = r[0x2BA] & 0xFFFF; vs.cfg[2] = r[0x2BD]; vs.cfg[3] = r[0x4F] & 7;
    for (int k = 0; k < 7; k++) { vs.outmap[k][0] = r[0x50 + k]; vs.outmap[k][1] = vs.outmap[k][2] = vs.outmap[k][3] = 0; }
    vs.base[0] = 0; vs.base[1] = stride; vs.base[2] = vs.base[3] = 0;
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
    DkViewport vp = {(float)vx, (float)vy, 2 * vw, 2 * vh, 0.0f, 1.0f};
    dkCmdBufSetViewports(g_cmd, 0, &vp, 1);
    DkScissor sc = {0, 0, W, H};
    if (r[0x65] & 3) {
        u32 x0 = r[0x66] & 0x3FF, y0 = (r[0x66] >> 16) & 0x3FF, x1 = r[0x67] & 0x3FF, y1 = (r[0x67] >> 16) & 0x3FF;
        sc = (DkScissor){x0, y0, x1 >= x0 ? x1 - x0 + 1 : 0, y1 >= y0 ? y1 - y0 + 1 : 0};
    }
    dkCmdBufSetScissors(g_cmd, 0, &sc, 1);
    DkShader const* sh[2] = {vs_select(), fs_select()};
    dkCmdBufBindShaders(g_cmd, DkStageFlag_GraphicsMask, sh, 2);
    u32 mode = (r[0x25E] >> 8) & 3;
    DkPrimitive prim = mode == 1 ? DkPrimitive_TriangleStrip : mode == 2 ? DkPrimitive_TriangleFan : DkPrimitive_Triangles;
    if (indexed) {
        dkCmdBufBindIdxBuffer(g_cmd, DkIdxFormat_Uint16, g_dyn_gpu + ioff);
        dkCmdBufDrawIndexed(g_cmd, prim, n, 1, 0, 0, 0);
    } else dkCmdBufDraw(g_cmd, prim, n, 1, 0, 0);
    if (r[0x113] && ((dm >> 8) & 0xF)) cs->gpu_dirty = 1;
    if (++g_draws_since_flush >= 256) flush(0);
    return 1;
}

// ---- hooks from the GSP side ------------------------------------------------------------------------

static void dk_sync(u32 a, u32 n) { if (n) sync_range(a, n); }

static void dk_invalidate(u32 a, u32 n) {
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

static const DkShader* vs_select(void) {
    static u64 last_key;
    static const DkShader* last = 0;
    static unsigned seen_version;
    u64 key = pica_vs_key();
    if (seen_version != g_force_version) { seen_version = g_force_version; last = 0; }
    if (last && key == last_key) { if (last == &g_vsh) g_vs_misses++; else g_vs_hits++; return last; }
    last_key = key;
    const DkShader* sh = g_force_vs_interp ? 0 : pack_find(key);
    if (sh) { g_vs_hits++; return last = sh; }
    if (g_force_vs_interp) { g_vs_misses++; return last = &g_vsh; }
    g_vs_misses++;
    if (first_miss(key)) {
        msr_receipt("gpu: vertex shader %016llX is not in the pack; it runs on the interpreter (saved to vs_new.bin)",
                    (unsigned long long)key);
        pica_vs_dump(key, g_new_path);
    }
    return last = &g_vsh;
}

static unsigned g_fs_hits, g_fs_misses;
static const DkShader* fs_select(void) {
    static u64 last_key;
    static const DkShader* last = 0;
    static unsigned seen_version;
    u64 key = pica_fs_key();
    if (seen_version != g_force_version) { seen_version = g_force_version; last = 0; }
    if (last && key == last_key) { if (last == &g_fsh) g_fs_misses++; else g_fs_hits++; return last; }
    last_key = key;
    const DkShader* sh = g_force_fs_general ? 0 : pack_find(key);
    if (sh) { g_fs_hits++; return last = sh; }
    if (g_force_fs_general) { g_fs_misses++; return last = &g_fsh; }
    g_fs_misses++;
    if (first_miss(key)) {
        msr_receipt("gpu: fragment shader %016llX is not in the pack; it runs on the general one (saved to fs_new.bin)",
                    (unsigned long long)key);
        pica_fs_dump(key, g_fs_new_path);
    }
    return last = &g_fsh;
}

void dkr_fs_stats(unsigned* hits, unsigned* misses) { *hits = g_fs_hits; *misses = g_fs_misses; g_fs_hits = g_fs_misses = 0; }

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
    u32 code_size = ((pica_vsh_dksh_size + pica_fsh_dksh_size + 2 * DK_SHADER_CODE_ALIGNMENT + DK_SHADER_CODE_UNUSABLE_SIZE) + DK_MEMBLOCK_ALIGNMENT - 1) & ~(DK_MEMBLOCK_ALIGNMENT - 1);
    dkMemBlockMakerDefaults(&mm, dev, code_size);
    mm.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    g_code = dkMemBlockCreate(&mm);
    u32 co = 0;
    shader_load(&g_vsh, pica_vsh_dksh, pica_vsh_dksh_size, &co);
    shader_load(&g_fsh, pica_fsh_dksh, pica_fsh_dksh_size, &co);

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

    dkMemBlockMakerDefaults(&mm, dev, 0x1000);
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
    g_cmd = dkCmdBufCreate(&cm);
    dkCmdBufAddMemory(g_cmd, g_cmdmem, 0, CMD_SIZE);

    // the lighting tables: 256 x 24, value and difference
    DkImageLayoutMaker lm;
    DkImageLayout lay;
    dkImageLayoutMakerDefaults(&lm, dev);
    lm.format = DkImageFormat_RG32_Float;
    lm.dimensions[0] = 256;
    lm.dimensions[1] = 24;
    dkImageLayoutInitialize(&lay, &lm);
    dkImageInitialize(&g_lut_img, &lay, g_img, img_alloc(&lay));

    pica_draw_hook = dk_draw;
    pica_sync_hook = dk_sync;
    pica_invalidate_hook = dk_invalidate;
    pica_fill_hook = dk_fill;
    msr_receipt("gpu: deko3d PICA renderer ready (shaders %u + %u bytes)", (unsigned)pica_vsh_dksh_size, (unsigned)pica_fsh_dksh_size);
}

// Called before the platform shows the LCD framebuffers: everything drawn so far reaches memory.
void dkr_present_sync(void) {
    if (g_draws_since_flush) flush(0);
}
