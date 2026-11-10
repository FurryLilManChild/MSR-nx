// MSR-nx: runs the GPU VRAM kernels (runtime/gpu/dk/vram_ops.inc) on the CPU and compares them byte
// for byte with the reference implementations: the probe's display transfer, texture copy and fill,
// and the Switch renderer's CPU-side render target, texture and screen conversions (copied here as
// they were before the GPU versions replaced them).
//
// Build and run: g++ -O2 -I runtime/gpu/dk tools/tests/vram_ops_test.cpp -o vram_ops_test && ./vram_ops_test
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

// ---- the kernels, on the CPU -----------------------------------------------------------------------
#define VRAM_BYTES (6u << 20)
static std::vector<u32> g_vram(VRAM_BYTES / 4);
static std::vector<u32> g_img(1024 * 1024);
static u32 g_img_w = 1;
static std::vector<u32> g_out(400 * 240);
struct UV4 { u32 x, y, z, w; };
static UV4 q[4];
#define LDW(i) g_vram[(i)]
#define STW(i, v) (g_vram[(i)] = (v))
#define IMG_READ(x, y) g_img[(y) * g_img_w + (x)]
static std::vector<u32> g_dst(800 * 480);
static u32 g_dst_w = 0;   // 0: IMG_WRITE goes to g_img (k_load, k_texdecode); else to g_dst (k_hires)
#define IMG_WRITE(x, y, v) (g_dst_w ? (g_dst[(y) * g_dst_w + (x)] = (v)) : (g_img[(y) * g_img_w + (x)] = (v)))
#define OUTW(i, v) (g_out[(i)] = (v))
typedef uint32_t uint;
static inline uint min(uint a, uint b) { return a < b ? a : b; }
#include "vram_ops.inc"

template <class F> static void run(uint n, F f) { for (uint i = 0; i < n; i++) f(i); }

// ---- references -----------------------------------------------------------------------------------
static u8* M() { return (u8*)g_vram.data(); }

static void ref_fill(std::vector<u8>& m, u32 start, u32 end, u32 value, u32 width) {
    if (width == 0) for (u32 a = start; a + 2 <= end; a += 2) { m[a] = (u8)value; m[a + 1] = (u8)(value >> 8); }
    else if (width & 2) for (u32 a = start; a + 4 <= end; a += 4) memcpy(&m[a], &value, 4);
    else for (u32 a = start; a + 3 <= end; a += 3) { m[a] = (u8)value; m[a + 1] = (u8)(value >> 8); m[a + 2] = (u8)(value >> 16); }
}

static void ref_copy(std::vector<u8>& m, u32 in_v, u32 out_v, u32 size, u32 in_gap, u32 out_gap) {
    u32 iw = (in_gap & 0xFFFF) * 16, ig = (in_gap >> 16) * 16;
    u32 ow = (out_gap & 0xFFFF) * 16, og = (out_gap >> 16) * 16;
    if (!iw) iw = size;
    if (!ow) ow = size;
    u32 ip = 0, op = 0, done = 0, ic = 0, oc = 0;
    size &= ~15u;
    while (done < size) {
        u32 n = iw - ic < ow - oc ? iw - ic : ow - oc;
        if (n > size - done) n = size - done;
        memmove(&m[out_v + op], &m[in_v + ip], n);
        ip += n; op += n; ic += n; oc += n; done += n;
        if (ic == iw) { ic = 0; ip += ig; }
        if (oc == ow) { oc = 0; op += og; }
    }
}

// sw_raster.c pica_display_transfer
static u8 k_x5[32], k_x6[64], k_x4[16];
static const u32 k_fb_bpp[8] = {4, 3, 2, 2, 2, 4, 4, 4};
static u32 mort(u32 x, u32 y) { return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3); }
static u32 tiled_ref(u32 x, u32 y, u32 w) { return (y & ~7u) * w + (x & ~7u) * 8 + mort(x & 7, y & 7); }
static void px_read_ref(const u8* p, u32 fmt, u32 c[4]) {
    u16 h;
    switch (fmt) {
    case 0: c[0] = p[3]; c[1] = p[2]; c[2] = p[1]; c[3] = p[0]; break;
    case 1: c[0] = p[2]; c[1] = p[1]; c[2] = p[0]; c[3] = 255; break;
    case 2: memcpy(&h, p, 2); c[0] = k_x5[(h >> 11) & 31]; c[1] = k_x5[(h >> 6) & 31]; c[2] = k_x5[(h >> 1) & 31]; c[3] = (h & 1) ? 255 : 0; break;
    case 3: memcpy(&h, p, 2); c[0] = k_x5[(h >> 11) & 31]; c[1] = k_x6[(h >> 5) & 63]; c[2] = k_x5[h & 31]; c[3] = 255; break;
    default: memcpy(&h, p, 2); c[0] = k_x4[(h >> 12) & 15]; c[1] = k_x4[(h >> 8) & 15]; c[2] = k_x4[(h >> 4) & 15]; c[3] = k_x4[h & 15]; break;
    }
}
static void px_write_ref(u8* p, u32 fmt, const u32 c[4]) {
    u32 r = c[0], g = c[1], b = c[2], a = c[3];
    u16 h;
    switch (fmt) {
    case 0: p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)g; p[3] = (u8)r; break;
    case 1: p[0] = (u8)b; p[1] = (u8)g; p[2] = (u8)r; break;
    case 2: h = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)); memcpy(p, &h, 2); break;
    case 3: h = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); memcpy(p, &h, 2); break;
    default: h = (u16)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)); memcpy(p, &h, 2); break;
    }
}
static void ref_transfer(std::vector<u8>& m, u32 in_v, u32 out_v, u32 in_dim, u32 out_dim, u32 flags) {
    for (u32 v = 0; v < 32; v++) k_x5[v] = (u8)(v / 31.f * 255 + 0.5f);
    for (u32 v = 0; v < 64; v++) k_x6[v] = (u8)(v / 63.f * 255 + 0.5f);
    for (u32 v = 0; v < 16; v++) k_x4[v] = (u8)(v / 15.f * 255 + 0.5f);
    u32 iw = in_dim & 0xFFFF, ih = in_dim >> 16;
    int flip = flags & 1, in_linear = (flags >> 1) & 1, same = (flags >> 5) & 1;
    int out_tiled = in_linear != same;
    u32 fi = (flags >> 8) & 7, fo = (flags >> 12) & 7, down = (flags >> 24) & 3;
    u32 sx = down ? 2 : 1, sy = down == 2 ? 2 : 1;
    u32 ow = (out_dim & 0xFFFF) / sx, oh = (out_dim >> 16) / sy;
    u32 cw = ow < iw / sx ? ow : iw / sx, ch_ = oh < ih / sy ? oh : ih / sy;
    u32 bi = k_fb_bpp[fi], bo = k_fb_bpp[fo], n = sx * sy, half = n / 2;
    for (u32 y = 0; y < ch_; y++) {
        u32 dy = flip ? ch_ - 1 - y : y;
        for (u32 x = 0; x < cw; x++) {
            u32 acc[4] = {0, 0, 0, 0}, c[4];
            for (u32 j = 0; j < sy; j++)
                for (u32 i = 0; i < sx; i++) {
                    u32 xx = x * sx + i, yy = y * sy + j;
                    px_read_ref(&m[in_v + (in_linear ? yy * iw + xx : tiled_ref(xx, yy, iw)) * bi], fi, c);
                    acc[0] += c[0]; acc[1] += c[1]; acc[2] += c[2]; acc[3] += c[3];
                }
            if (n > 1) for (int k = 0; k < 4; k++) acc[k] = (acc[k] + half) / n;
            px_write_ref(&m[out_v + (out_tiled ? tiled_ref(x, dy, ow) : dy * ow + x) * bo], fo, acc);
        }
    }
}

// dk_pica.c decode_color_px / encode_color_px (with image pixels as r, g, b, a bytes)
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
            u32 t = tiled_ref(x, y, w);
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
// main_nx.c convert_screen
static void ref_screen(const u8* mem, u32 left, u32 stride_in, u32 format, u32 W, u32* out, int flip) {
    u32 H = 240, fmt = format & 7;
    u32 bpp = fmt == 0 ? 4 : fmt == 1 ? 3 : 2;
    u32 stride = stride_in ? stride_in : 240 * bpp;
    const u8* base = mem + left;
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
            out[(flip ? H - 1 - y : y) * W + x] = r | (g << 8) | (b << 16) | 0xFF000000u;
        }
    }
}

// ---- the checks -------------------------------------------------------------------------------------
static u32 g_seed = 12345;
static u32 rnd() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
static void randomize() { for (auto& w : g_vram) w = rnd() ^ (rnd() << 16); }
static int g_fail;
static void check(const char* what, const std::vector<u8>& ref) {
    if (memcmp(ref.data(), M(), VRAM_BYTES)) {
        size_t i = 0;
        while (ref[i] == M()[i]) i++;
        printf("FAIL %s: first difference at %zX (ref %02X, kernel %02X)\n", what, i, ref[i], M()[i]);
        g_fail++;
    } else printf("ok   %s\n", what);
}
static std::vector<u8> snapshot() { return std::vector<u8>(M(), M() + VRAM_BYTES); }

int main() {
    // fills: the game's (32-bit and 24-bit RT clears) and the 16-bit case
    u32 fills[][4] = {{0x0EA600, 0x148200, 0x000000FF, 2}, {0x45AE00, 0x4B8A00, 0x00FFFFFF, 2}, {0x45AE00, 0x4B8A00, 0x00FFFFFF, 1},
                      {0x100000, 0x100100, 0x1234ABCD, 0}, {0x200004, 0x200034, 0x00ABCDEF, 1}};
    for (auto& f : fills) {
        randomize();
        auto ref = snapshot();
        ref_fill(ref, f[0], f[1], f[2], f[3]);
        q[0] = {f[0], f[1], f[2], f[3]};
        run((f[1] - f[0] + 3) / 4 + 1, k_fill);
        char name[64]; snprintf(name, sizeof name, "fill %X..%X width %u", f[0], f[1], f[3]);
        check(name, ref);
    }
    // texture copies: the game's (240-wide tile rows into a 256-wide texture) and plain ones
    u32 copies[][5] = {{0x08CA00, 0x148200, 0x5DC00, 0x000001E0, 0x002001E0}, {0x0EA600, 0x4B8A00, 0x5DC00, 0x000001E0, 0x002001E0},
                       {0x100000, 0x300000, 0x8000, 0, 0}, {0x100000, 0x300000, 0x8000, 0x00100020, 0x00000040}};
    for (auto& c : copies) {
        randomize();
        auto ref = snapshot();
        ref_copy(ref, c[0], c[1], c[2], c[3], c[4]);
        q[0] = {c[0], c[1], c[2], 0};
        q[1] = {(c[3] & 0xFFFF) * 16, (c[3] >> 16) * 16, (c[4] & 0xFFFF) * 16, (c[4] >> 16) * 16};
        run(c[2] / 4, k_copy);
        char name[64]; snprintf(name, sizeof name, "copy %X -> %X", c[0], c[1]);
        check(name, ref);
    }
    // display transfers: the game's bloom chain and screen outputs, then other formats and modes
    u32 xf[][5] = {{0x4B8A00, 0x5B8A00, 0x01000200, 0x01000200, 0x02000020}, {0x5B8A00, 0x5D8A00, 0x00800100, 0x00800100, 0x02000020},
                   {0x5D8A00, 0x2F8200, 0x00400080, 0x00400080, 0x02000020}, {0x08CA00, 0x000000, 0x019000F0, 0x019000F0, 0x00001000},
                   {0x08CA00, 0x300000, 0x019000F0, 0x014000F0, 0x00001004}};
    for (auto& t : xf) {
        randomize();
        auto ref = snapshot();
        ref_transfer(ref, t[0], t[1], t[2], t[3], t[4]);
        q[0] = {t[0], t[1], t[2], t[3]};
        q[1] = {t[4], 0, 0, 0};
        run((t[3] & 0xFFFF) * (t[3] >> 16) / 4, k_transfer);
        char name[80]; snprintf(name, sizeof name, "transfer %X -> %X flags %08X", t[0], t[1], t[4]);
        check(name, ref);
    }
    for (u32 fi = 0; fi < 5; fi++)
        for (u32 fo = 0; fo < 5; fo++)
            for (u32 mode = 0; mode < 8; mode++) {
                u32 flags = (fi << 8) | (fo << 12) | (mode & 1) | ((mode & 2) ? 2 : 0) | ((mode & 4) ? 0x20 : 0) | ((fi + fo + mode) % 3 << 24);
                randomize();
                auto ref = snapshot();
                ref_transfer(ref, 0x100000, 0x300000, 0x00800080, 0x00800080, flags);
                q[0] = {0x100000, 0x300000, 0x00800080, 0x00800080};
                q[1] = {flags, 0, 0, 0};
                run(128 * 128 / 4, k_transfer);
                char name[80]; snprintf(name, sizeof name, "transfer formats %u -> %u flags %08X", fi, fo, flags);
                check(name, ref);
            }
    // render target store (image -> VRAM) for each color format
    for (u32 fmt = 0; fmt < 5; fmt++) {
        u32 w = 240, h = 400, addr = 0x08CA00;
        randomize();
        g_img_w = w;
        for (u32 i = 0; i < w * h; i++) g_img[i] = rnd() ^ (rnd() << 16);
        auto ref = snapshot();
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                u32 c = g_img[y * w + x];
                u8 px[4] = {(u8)c, (u8)(c >> 8), (u8)(c >> 16), (u8)(c >> 24)};
                encode_color_px(&ref[addr + tiled_ref(x, h - 1 - y, w) * k_fb_bpp[fmt]], fmt, px);
            }
        q[0] = {addr, w, h, fmt};
        run(w * h / 4, k_store);
        char name[64]; snprintf(name, sizeof name, "store format %u", fmt);
        check(name, ref);
    }
    // render target load (VRAM -> image)
    for (u32 fmt = 0; fmt < 5; fmt++) {
        u32 w = 240, h = 400, addr = 0x08CA00;
        randomize();
        g_img_w = w;
        std::vector<u32> ref(w * h);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                u8 o[4];
                decode_color_px(&M()[addr + tiled_ref(x, h - 1 - y, w) * k_fb_bpp[fmt]], fmt, o);
                ref[y * w + x] = o[0] | (o[1] << 8) | (o[2] << 16) | ((u32)o[3] << 24);
            }
        q[0] = {addr, w, h, fmt};
        run(w * h, k_load);
        printf("%s load format %u\n", memcmp(ref.data(), g_img.data(), w * h * 4) ? (g_fail++, "FAIL") : "ok  ", fmt);
    }
    // transfers and copies from a render target's image: the same VRAM (outside the render target)
    // as storing the image first and doing them from VRAM
    {
        struct T { u32 addr, w, h, fmt, out, odims, flags; };
        std::vector<T> ts = {{0x08CA00, 240, 400, 0, 0x000000, 0x019000F0, 0x00001000}, {0x08CA00, 240, 400, 0, 0x300000, 0x014000F0, 0x00001004},
                             {0x4B8A00, 512, 256, 0, 0x5B8A00, 0x01000200, 0x02000020}};
        for (u32 fi = 0; fi < 5; fi++)
            for (u32 fo = 0; fo < 5; fo++)
                for (u32 mode = 0; mode < 4; mode++)
                    ts.push_back({0x100000, 128, 128, fi, 0x300000, 0x00800080, (fi << 8) | (fo << 12) | (mode & 1) | ((mode & 2) ? 0x20 : 0) | ((fi + fo + mode) % 3 << 24)});
        for (auto& t : ts) {
            randomize();
            g_img_w = t.w;
            for (u32 i = 0; i < t.w * t.h; i++) g_img[i] = rnd() ^ (rnd() << 16);
            auto before = snapshot();
            q[0] = {t.addr, t.w, t.h, t.fmt};
            run(t.w * t.h / 4, k_store);
            q[0] = {t.addr, t.out, (t.h << 16) | t.w, t.odims};
            q[1] = {t.flags, 0, 0, 0};
            run((t.odims & 0xFFFF) * (t.odims >> 16) / 4, k_transfer);
            auto ref = snapshot();
            memcpy(M(), before.data(), VRAM_BYTES);
            run((t.odims & 0xFFFF) * (t.odims >> 16) / 4, k_transfer_img);
            u32 bytes = t.w * t.h * k_fb_bpp[t.fmt];
            memcpy(&ref[t.addr], &M()[t.addr], bytes);   // the render target's own range is not compared
            char name[96]; snprintf(name, sizeof name, "transfer from image %X fmt %u flags %08X", t.addr, t.fmt, t.flags);
            check(name, ref);
        }
        struct C { u32 addr, w, h, fmt, out, size, ig, og; };
        std::vector<C> cs = {{0x08CA00, 240, 400, 0, 0x148200, 0x5DC00, 0x000001E0, 0x002001E0}, {0x08CA00, 240, 400, 1, 0x148200, 0x46500, 0, 0}};
        for (u32 fmt = 0; fmt < 5; fmt++) cs.push_back({0x100000, 64, 64, fmt, 0x300000, (64 * 64 * k_fb_bpp[fmt] / 2) & ~255u, 0x00100010, 0x00080020});
        for (auto& c : cs) {
            randomize();
            g_img_w = c.w;
            for (u32 i = 0; i < c.w * c.h; i++) g_img[i] = rnd() ^ (rnd() << 16);
            auto before = snapshot();
            q[0] = {c.addr, c.w, c.h, c.fmt};
            run(c.w * c.h / 4, k_store);
            q[0] = {c.addr, c.out, c.size, 0};
            q[1] = {(c.ig & 0xFFFF) * 16, (c.ig >> 16) * 16, (c.og & 0xFFFF) * 16, (c.og >> 16) * 16};
            run(c.size / 4, k_copy);
            auto ref = snapshot();
            memcpy(M(), before.data(), VRAM_BYTES);
            q[2] = {c.addr, c.w, c.h, c.fmt};
            // only inputs inside the render target are sent this way: check the span fits
            u32 iw = q[1].x ? q[1].x : c.size, span = c.size + c.size / iw * q[1].y;
            if (span > c.w * c.h * k_fb_bpp[c.fmt]) { printf("skip copy %X fmt %u (span past the target)\n", c.addr, c.fmt); continue; }
            run(c.size / 4, k_copy_img);
            memcpy(&ref[c.addr], &M()[c.addr], c.w * c.h * k_fb_bpp[c.fmt]);
            char name[96]; snprintf(name, sizeof name, "copy from image %X fmt %u", c.addr, c.fmt);
            check(name, ref);
        }
    }
    // texture decode, every format
    for (u32 fmt = 0; fmt < 14; fmt++) {
        u32 w = 128, h = 64, addr = 0x200000;
        randomize();
        g_img_w = w;
        std::vector<u8> ref(w * h * 4);
        decode_texture(&M()[addr], fmt, w, h, ref.data());
        q[0] = {addr, w, h, fmt};
        run(w * h, k_texdecode);
        printf("%s texture format %u\n", memcmp(ref.data(), g_img.data(), w * h * 4) ? (g_fail++, "FAIL") : "ok  ", fmt);
    }
    // the resolution scaler: a target image s times the native size
    {
        // store from a 2x and a 1.5x image: as storing the native image the kernel sees (2 x 2
        // average, nearest)
        u32 w = 240, h = 400, addr = 0x08CA00;
        for (int sc = 0; sc < 2; sc++) {
            u32 iw = sc ? 360 : 480, ih = sc ? 600 : 800;
            randomize();
            g_img_w = iw;
            for (u32 i = 0; i < iw * ih; i++) g_img[i] = rnd() ^ (rnd() << 16);
            std::vector<u32> nat(w * h);
            for (u32 y = 0; y < h; y++)
                for (u32 x = 0; x < w; x++) {
                    if (!sc) {
                        u32 a = g_img[2 * y * iw + 2 * x], b = g_img[2 * y * iw + 2 * x + 1], c = g_img[(2 * y + 1) * iw + 2 * x], d = g_img[(2 * y + 1) * iw + 2 * x + 1], r = 0;
                        for (int k = 0; k < 4; k++) r |= ((((a >> 8 * k) & 255) + ((b >> 8 * k) & 255) + ((c >> 8 * k) & 255) + ((d >> 8 * k) & 255) + 2) >> 2) << 8 * k;
                        nat[y * w + x] = r;
                    } else nat[y * w + x] = g_img[((2 * y + 1) * ih / (2 * h)) * iw + (2 * x + 1) * iw / (2 * w)];
                }
            auto before = snapshot();
            q[0] = {addr, w, h, 0}; q[3] = {iw, ih, 0, 0};
            run(w * h / 4, k_store);
            auto got = snapshot();
            memcpy(M(), before.data(), VRAM_BYTES);
            std::vector<u32> keep(g_img.begin(), g_img.begin() + iw * ih);
            g_img_w = w;
            std::copy(nat.begin(), nat.end(), g_img.begin());
            q[3] = {0, 0, 0, 0};
            run(w * h / 4, k_store);
            check(sc ? "store from a 1.5x image" : "store from a 2x image", got);
            std::copy(keep.begin(), keep.end(), g_img.begin());
        }
        // load into a 2x image: each image pixel from the native pixel under it
        {
            u32 iw = 480, ih = 800;
            randomize();
            q[0] = {addr, w, h, 0}; q[3] = {iw, ih, 0, 0};
            g_img_w = iw;
            run(iw * ih, k_load);
            std::vector<u32> big(g_img.begin(), g_img.begin() + iw * ih);
            q[3] = {0, 0, 0, 0}; g_img_w = w;
            run(w * h, k_load);
            int bad = 0;
            for (u32 Y = 0; Y < ih; Y++) for (u32 X = 0; X < iw; X++) if (big[Y * iw + X] != g_img[(Y / 2) * w + X / 2]) bad++;
            printf("%s load into a 2x image\n", bad ? (g_fail++, "FAIL") : "ok  ");
        }
        // a texture copied out of the target, from a snapshot of its image: as store, the game's copy
        // (240-wide tile rows into a 256-wide texture) and the texture decode, for each format that
        // fits (1x; texels past the copied part are not compared)
        for (u32 fmt = 0; fmt < 2; fmt++) {
            u32 bpp = k_fb_bpp[fmt], size = 240 * 400 * bpp, row = 240 * 8 * bpp, gap = 16 * 8 * bpp;
            randomize();
            g_img_w = w;
            for (u32 i = 0; i < w * h; i++) g_img[i] = rnd() ^ (rnd() << 16);
            std::vector<u32> snap(g_img.begin(), g_img.begin() + w * h);
            q[0] = {addr, w, h, fmt}; q[3] = {0, 0, 0, 0};
            run(w * h / 4, k_store);
            q[0] = {addr, 0x148200, size, 0}; q[1] = {row, 0, row, gap};
            run(size / 4, k_copy);
            u32 tw = 256, th = 512;
            q[0] = {0x148200, tw, th, fmt}; q[3] = {0, 0, 0, 0};
            g_img_w = tw;
            run(tw * th, k_texdecode);
            std::vector<u32> ref(g_img.begin(), g_img.begin() + tw * th);
            std::copy(snap.begin(), snap.end(), g_img.begin());
            g_img_w = w;
            q[0] = {tw, th, fmt, 400}; q[1] = {tw, th, 0, 0}; q[3] = {w, h, 0, 0};
            g_dst_w = tw;
            run(tw * th, k_texup);
            g_dst_w = 0;
            int bad = 0;
            for (u32 y = 0; y < 400; y++) for (u32 x = 0; x < 240; x++) if (g_dst[(th - 1 - y) * tw + x] != ref[(th - 1 - y) * tw + x]) bad++;
            printf("%s texture from a target snapshot, format %u (%d differ)\n", bad ? (g_fail++, "FAIL") : "ok  ", fmt, bad);
            q[3] = {0, 0, 0, 0};
        }
        // the same at 2x: from a snapshot that is the native one with each pixel doubled, the 2x
        // texture is the 1x one with each texel doubled
        {
            u32 tw = 256, th = 512, fmt = 0;
            randomize();
            g_img_w = w;
            for (u32 i = 0; i < w * h; i++) g_img[i] = rnd() ^ (rnd() << 16);
            std::vector<u32> nat(g_img.begin(), g_img.begin() + w * h);
            q[0] = {tw, th, fmt, 400}; q[1] = {tw, th, 0, 0}; q[3] = {w, h, 0, 0};
            g_dst_w = tw;
            run(tw * th, k_texup);
            std::vector<u32> one(g_dst.begin(), g_dst.begin() + tw * th);
            g_img_w = 2 * w;
            for (u32 y = 0; y < 2 * h; y++) for (u32 x = 0; x < 2 * w; x++) g_img[y * 2 * w + x] = nat[(y / 2) * w + x / 2];
            std::vector<u32> big(512 * 1024);
            g_dst.assign(512 * 1024, 0);
            q[0] = {tw, th, fmt, 400}; q[1] = {2 * tw, 2 * th, 0, 0}; q[3] = {2 * w, 2 * h, 0, 0};
            g_dst_w = 2 * tw;
            run(4 * tw * th, k_texup);
            g_dst_w = 0;
            int bad = 0;
            for (u32 Y = 0; Y < 2 * th; Y++) for (u32 X = 0; X < 2 * tw; X++) if (g_dst[Y * 2 * tw + X] != one[(Y / 2) * tw + X / 2]) bad++;
            printf("%s texture from a 2x snapshot (%d differ)\n", bad ? (g_fail++, "FAIL") : "ok  ", bad);
            g_dst.assign(800 * 480, 0);
            q[3] = {0, 0, 0, 0};
        }
        // the top screen at full size from the image: as the transfer to the framebuffer and the
        // screen output (1x), for both transfer flips
        for (u32 flip = 0; flip < 2; flip++) {
            randomize();
            g_img_w = w;
            for (u32 i = 0; i < w * h; i++) g_img[i] = rnd() ^ (rnd() << 16);
            q[0] = {addr, w, h, 0}; q[3] = {0, 0, 0, 0};
            run(w * h / 4, k_store);
            q[0] = {addr, 0x046500, 0x019000F0, 0x019000F0}; q[1] = {0x00001000 | flip, 0, 0, 0};
            run(240 * 400 / 4, k_transfer);
            q[0] = {0x046500, 240 * 3, 1, 400}; q[1] = {0, 1, 0, 0};
            run(400 * 240, k_screen);
            q[0] = {flip, 0, 0, 0}; q[3] = {w, h, 0, 0};
            g_dst_w = 400;
            run(400 * 240, k_hires);
            g_dst_w = 0;
            int bad = 0;
            for (u32 i = 0; i < 400 * 240; i++) if (g_dst[i] != g_out[i]) bad++;
            printf("%s top screen from the image, flip %u (%d differ)\n", bad ? (g_fail++, "FAIL") : "ok  ", flip, bad);
            q[3] = {0, 0, 0, 0};
        }
    }
    // screen output
    for (u32 fmt = 0; fmt < 5; fmt++)
        for (int flip = 0; flip < 2; flip++) {
            u32 W = fmt & 1 ? 320 : 400, stride = fmt == 2 ? 0 : 240 * k_fb_bpp[fmt];
            randomize();
            std::vector<u32> ref(W * 240);
            ref_screen(M(), 0x046500, stride, fmt, W, ref.data(), flip);
            q[0] = {0x046500, stride, fmt, W};
            q[1] = {0, (u32)flip, 0, 0};
            run(W * 240, k_screen);
            printf("%s screen format %u flip %d\n", memcmp(ref.data(), g_out.data(), W * 240 * 4) ? (g_fail++, "FAIL") : "ok  ", fmt, flip);
        }
    printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail != 0;
}
