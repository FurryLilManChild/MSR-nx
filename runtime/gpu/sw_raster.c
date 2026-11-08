// MSR-nx: software reference renderer for the PICA200. Used by the PC and cloud probes only, to see
// frames and to check the deko3d renderer against; it is never part of a Switch build.
//
// Behaviour follows 3dbrew "GPU" pages, with fearkov/zakuro (MIT) zakuro-gpu raster.rs, tev.rs,
// texture.rs and blend.rs as a second reference. The code here is ours.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msr_log.h"
#include "pica.h"

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---- tiling ---------------------------------------------------------------------------------------

static u32 morton(u32 x, u32 y) {
    // x in bits 0, 2, 4; y in bits 1, 3, 5
    return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}

static u32 tiled_offset(u32 x, u32 y, u32 width) {
    return ((y & ~7u) * width + (x & ~7u) * 8 + morton(x & 7, y & 7));
}

// ---- colors ---------------------------------------------------------------------------------------

typedef struct { float r, g, b, a; } Color;

static const u32 k_fb_bpp[8] = {4, 3, 2, 2, 2, 4, 4, 4};

static Color decode_color(const u8* p, u32 fmt) {
    Color c = {0, 0, 0, 1};
    u16 h;
    switch (fmt) {
    case 0: c.a = p[0] / 255.f; c.b = p[1] / 255.f; c.g = p[2] / 255.f; c.r = p[3] / 255.f; break;
    case 1: c.b = p[0] / 255.f; c.g = p[1] / 255.f; c.r = p[2] / 255.f; break;
    case 2: memcpy(&h, p, 2); c.r = ((h >> 11) & 31) / 31.f; c.g = ((h >> 6) & 31) / 31.f; c.b = ((h >> 1) & 31) / 31.f; c.a = h & 1; break;
    case 3: memcpy(&h, p, 2); c.r = ((h >> 11) & 31) / 31.f; c.g = ((h >> 5) & 63) / 63.f; c.b = (h & 31) / 31.f; break;
    case 4: memcpy(&h, p, 2); c.r = ((h >> 12) & 15) / 15.f; c.g = ((h >> 8) & 15) / 15.f; c.b = ((h >> 4) & 15) / 15.f; c.a = (h & 15) / 15.f; break;
    }
    return c;
}

static void encode_color(u8* p, u32 fmt, Color c) {
    u32 r = (u32)(clampf(c.r, 0, 1) * 255 + 0.5f), g = (u32)(clampf(c.g, 0, 1) * 255 + 0.5f);
    u32 b = (u32)(clampf(c.b, 0, 1) * 255 + 0.5f), a = (u32)(clampf(c.a, 0, 1) * 255 + 0.5f);
    u16 h;
    switch (fmt) {
    case 0: p[0] = (u8)a; p[1] = (u8)b; p[2] = (u8)g; p[3] = (u8)r; break;
    case 1: p[0] = (u8)b; p[1] = (u8)g; p[2] = (u8)r; break;
    case 2: h = (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)); memcpy(p, &h, 2); break;
    case 3: h = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); memcpy(p, &h, 2); break;
    case 4: h = (u16)(((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)); memcpy(p, &h, 2); break;
    }
}

// ---- textures -------------------------------------------------------------------------------------

static const int k_etc_mod[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

static Color etc1_texel(const u8* blk, u32 x, u32 y) {
    u64 w;
    memcpy(&w, blk, 8);
    u32 hi = (u32)(w >> 32), lo = (u32)w;
    int flip = hi & 1, diff = (hi >> 1) & 1;
    int sub = flip ? (y >= 2) : (x >= 2);
    int r, g, b;
    if (diff) {
        int r0 = (hi >> 27) & 31, g0 = (hi >> 19) & 31, b0 = (hi >> 11) & 31;
        int dr = (int)((hi >> 24) & 7) << 29 >> 29, dg = (int)((hi >> 16) & 7) << 29 >> 29, db = (int)((hi >> 8) & 7) << 29 >> 29;
        if (sub) { r0 += dr; g0 += dg; b0 += db; }
        r = (r0 << 3) | (r0 >> 2); g = (g0 << 3) | (g0 >> 2); b = (b0 << 3) | (b0 >> 2);
    } else {
        int s = sub ? 0 : 4;
        r = (hi >> (24 + s)) & 15; g = (hi >> (16 + s)) & 15; b = (hi >> (8 + s)) & 15;
        r |= r << 4; g |= g << 4; b |= b << 4;
    }
    int table = sub ? (hi >> 2) & 7 : (hi >> 5) & 7;
    int i = x * 4 + y;
    int msb = (lo >> (16 + i)) & 1, lsb = (lo >> i) & 1;
    int m = k_etc_mod[table][lsb];
    if (msb) m = -m;
    r = r + m < 0 ? 0 : r + m > 255 ? 255 : r + m;
    g = g + m < 0 ? 0 : g + m > 255 ? 255 : g + m;
    b = b + m < 0 ? 0 : b + m > 255 ? 255 : b + m;
    return (Color){r / 255.f, g / 255.f, b / 255.f, 1};
}

static Color fetch_texel(const u8* base, u32 fmt, u32 w, u32 x, u32 y) {
    if (fmt == 12 || fmt == 13) {   // ETC1, ETC1A4: 8x8 tiles of four 4x4 blocks
        u32 bs = fmt == 13 ? 16 : 8;
        u32 tile = (y / 8) * (w / 8) + x / 8;
        u32 sub = ((y & 4) >> 1) | ((x & 4) >> 2);
        const u8* blk = base + (tile * 4 + sub) * bs;
        Color c = etc1_texel(blk + (fmt == 13 ? 8 : 0), x & 3, y & 3);
        if (fmt == 13) {
            u64 a;
            memcpy(&a, blk, 8);
            c.a = ((a >> (4 * ((x & 3) * 4 + (y & 3)))) & 15) / 15.f;
        }
        return c;
    }
    u32 t = tiled_offset(x, y, w);
    Color c = {0, 0, 0, 1};
    const u8* p;
    switch (fmt) {
    case 0: case 1: case 2: case 3: case 4: {
        static const u32 bpp[5] = {4, 3, 2, 2, 2};
        return decode_color(base + t * bpp[fmt], fmt);
    }
    case 5: p = base + t * 2; c.r = c.g = c.b = p[1] / 255.f; c.a = p[0] / 255.f; break;   // IA8
    case 6: p = base + t * 2; c.r = p[1] / 255.f; c.g = p[0] / 255.f; c.b = 0; break;      // RG8
    case 7: c.r = c.g = c.b = base[t] / 255.f; break;                                        // I8
    case 8: c.r = c.g = c.b = 0; c.a = base[t] / 255.f; break;                               // A8
    case 9: { u8 v = base[t]; c.r = c.g = c.b = (v >> 4) / 15.f; c.a = (v & 15) / 15.f; break; } // IA4
    case 10: { u8 v = base[t / 2]; v = t & 1 ? v >> 4 : v & 15; c.r = c.g = c.b = v / 15.f; break; } // I4
    case 11: { u8 v = base[t / 2]; v = t & 1 ? v >> 4 : v & 15; c.r = c.g = c.b = 0; c.a = v / 15.f; break; } // A4
    }
    return c;
}

static int wrap(int i, int n, u32 mode, int* border) {
    switch (mode) {
    case 0: return i < 0 ? 0 : i >= n ? n - 1 : i;              // clamp to edge
    case 1: if (i < 0 || i >= n) { *border = 1; return 0; } return i;   // clamp to border
    case 3: { int p = ((i % (2 * n)) + 2 * n) % (2 * n); return p < n ? p : 2 * n - 1 - p; }  // mirror
    default: return ((i % n) + n) % n;                            // repeat
    }
}

static Color sample(int unit, float s, float t) {
    static const u32 k_regs[3][6] = {
        {0x81, 0x82, 0x83, 0x85, 0x8E}, {0x91, 0x92, 0x93, 0x95, 0x96}, {0x99, 0x9A, 0x9B, 0x9D, 0x9E}};
    const u32* r = g_pica.reg;
    u32 dim = r[k_regs[unit][1]], par = r[k_regs[unit][2]];
    u32 h = dim & 0x7FF, w = (dim >> 16) & 0x7FF, fmt = r[k_regs[unit][4]] & 0xF;
    const u8* base = pica_ptr(r[k_regs[unit][3]] << 3);
    if (!base || !w || !h) return (Color){0, 0, 0, 1};
    int border = 0;
    int x = (int)floorf(s * w), y = (int)floorf(t * h);
    x = wrap(x, (int)w, (par >> 12) & 7, &border);
    y = wrap(y, (int)h, (par >> 8) & 7, &border);
    if (border) {
        u32 b = r[k_regs[unit][0]];
        return (Color){(b & 0xFF) / 255.f, ((b >> 8) & 0xFF) / 255.f, ((b >> 16) & 0xFF) / 255.f, (b >> 24) / 255.f};
    }
    // texture rows are stored top first; t = 0 is the bottom row
    return fetch_texel(base, fmt, w, (u32)x, h - 1 - (u32)y);
}

// ---- texture combiners ----------------------------------------------------------------------------

static Color g_frag_prim, g_frag_sec;

static Color tev_source(u32 s, const Color* prim, const Color* tex, const Color* prev, const Color* buf, Color cst) {
    switch (s) {
    case 0: return *prim;
    case 1: return g_frag_prim;
    case 2: return g_frag_sec;
    case 3: return tex[0];
    case 4: return tex[1];
    case 5: return tex[2];
    case 6: return tex[3];
    case 0xD: return *buf;
    case 0xE: return cst;
    default: return *prev;
    }
}

static void tev_rgb_op(u32 op, Color c, float* o) {
    switch (op) {
    case 0: o[0] = c.r; o[1] = c.g; o[2] = c.b; break;
    case 1: o[0] = 1 - c.r; o[1] = 1 - c.g; o[2] = 1 - c.b; break;
    case 2: o[0] = o[1] = o[2] = c.a; break;
    case 3: o[0] = o[1] = o[2] = 1 - c.a; break;
    case 4: o[0] = o[1] = o[2] = c.r; break;
    case 5: o[0] = o[1] = o[2] = 1 - c.r; break;
    case 8: o[0] = o[1] = o[2] = c.g; break;
    case 9: o[0] = o[1] = o[2] = 1 - c.g; break;
    case 12: o[0] = o[1] = o[2] = c.b; break;
    case 13: o[0] = o[1] = o[2] = 1 - c.b; break;
    default: o[0] = c.r; o[1] = c.g; o[2] = c.b; break;
    }
}

static float tev_a_op(u32 op, Color c) {
    switch (op) {
    case 0: return c.a;
    case 1: return 1 - c.a;
    case 2: return c.r;
    case 3: return 1 - c.r;
    case 4: return c.g;
    case 5: return 1 - c.g;
    case 6: return c.b;
    case 7: return 1 - c.b;
    }
    return c.a;
}

static float combine(u32 mode, float a, float b, float c) {
    switch (mode) {
    case 0: return a;
    case 1: return a * b;
    case 2: return a + b;
    case 3: return a + b - 0.5f;
    case 4: return a * c + b * (1 - c);
    case 5: return a - b;
    case 8: return a * b + c;
    case 9: return (a + b) * c;
    }
    return a;
}

static const u32 k_tev_base[6] = {0xC0, 0xC8, 0xD0, 0xD8, 0xF0, 0xF8};

static Color run_tev(Color prim, const Color* tex) {
    const u32* r = g_pica.reg;
    Color prev = prim, buf;
    u32 bc = r[0xFD];
    buf = (Color){(bc & 0xFF) / 255.f, ((bc >> 8) & 0xFF) / 255.f, ((bc >> 16) & 0xFF) / 255.f, (bc >> 24) / 255.f};
    Color next_buf = buf;
    for (int s = 0; s < 6; s++) {
        const u32* t = r + k_tev_base[s];
        u32 src = t[0], op = t[1], cmb = t[2], cc = t[3], sc = t[4];
        Color cst = {(cc & 0xFF) / 255.f, ((cc >> 8) & 0xFF) / 255.f, ((cc >> 16) & 0xFF) / 255.f, (cc >> 24) / 255.f};
        float in[3][3], ia[3];
        for (int k = 0; k < 3; k++) {
            Color cs = tev_source((src >> (4 * k)) & 0xF, &prim, tex, &prev, &buf, cst);
            Color as = tev_source((src >> (16 + 4 * k)) & 0xF, &prim, tex, &prev, &buf, cst);
            tev_rgb_op((op >> (4 * k)) & 0xF, cs, in[k]);
            ia[k] = tev_a_op((op >> (12 + 4 * k)) & 7, as);
        }
        u32 mr = cmb & 0xF, ma = (cmb >> 16) & 0xF;
        float out[3], oa;
        if (mr == 6 || mr == 7) {
            float d = 4 * ((in[0][0] - 0.5f) * (in[1][0] - 0.5f) + (in[0][1] - 0.5f) * (in[1][1] - 0.5f) + (in[0][2] - 0.5f) * (in[1][2] - 0.5f));
            out[0] = out[1] = out[2] = d;
        } else for (int k = 0; k < 3; k++) out[k] = combine(mr, in[0][k], in[1][k], in[2][k]);
        if (mr == 7) oa = out[0];
        else oa = combine(ma, ia[0], ia[1], ia[2]);
        float rs = (float)(1u << (sc & 3)), as = (float)(1u << ((sc >> 16) & 3));
        Color res = {clampf(out[0] * rs, 0, 1), clampf(out[1] * rs, 0, 1), clampf(out[2] * rs, 0, 1), clampf(oa * as, 0, 1)};
        // the combiner buffer takes the previous stage's result for stages 1..4 when asked
        buf = next_buf;
        if (s < 4) {
            u32 e0 = r[0xE0];
            if ((e0 >> (8 + s)) & 1) { next_buf.r = res.r; next_buf.g = res.g; next_buf.b = res.b; }
            if ((e0 >> (12 + s)) & 1) next_buf.a = res.a;
        }
        prev = res;
    }
    return prev;
}

// ---- output merger --------------------------------------------------------------------------------

static float blend_factor(u32 f, int alpha, Color s, Color d, Color k, int ch) {
    float sc[4] = {s.r, s.g, s.b, s.a}, dc[4] = {d.r, d.g, d.b, d.a}, kc[4] = {k.r, k.g, k.b, k.a};
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return sc[ch];
    case 3: return 1 - sc[ch];
    case 4: return dc[ch];
    case 5: return 1 - dc[ch];
    case 6: return s.a;
    case 7: return 1 - s.a;
    case 8: return d.a;
    case 9: return 1 - d.a;
    case 10: return kc[ch];
    case 11: return 1 - kc[ch];
    case 12: return k.a;
    case 13: return 1 - k.a;
    case 14: return alpha ? 1 : (s.a < 1 - d.a ? s.a : 1 - d.a);
    }
    return 1;
}

static float blend_eq(u32 eq, float s, float d) {
    switch (eq) {
    case 0: return s + d;
    case 1: return s - d;
    case 2: return d - s;
    case 3: return s < d ? s : d;
    case 4: return s > d ? s : d;
    }
    return s + d;
}

static int compare(u32 f, u32 a, u32 b) {
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return a == b;
    case 3: return a != b;
    case 4: return a < b;
    case 5: return a <= b;
    case 6: return a > b;
    case 7: return a >= b;
    }
    return 1;
}

static u8 stencil_op(u32 op, u8 v, u8 ref) {
    switch (op) {
    case 0: return v;
    case 1: return 0;
    case 2: return ref;
    case 3: return v == 255 ? 255 : v + 1;
    case 4: return v == 0 ? 0 : v - 1;
    case 5: return (u8)~v;
    case 6: return v + 1;
    case 7: return v - 1;
    }
    return v;
}

typedef struct {
    u8* color; u8* depth;
    u32 w, h, cfmt, dfmt, cbpp;
} Target;

static unsigned g_st_frag, g_st_alpha, g_st_depth, g_st_written;
static void fragment(Target* T, int x, int y, float depth, Color c) {
    g_st_frag++;
    const u32* r = g_pica.reg;
    // alpha test
    u32 at = r[0x104];
    u32 a8 = (u32)(clampf(c.a, 0, 1) * 255 + 0.5f);
    if ((at & 1) && !compare((at >> 4) & 7, a8, (at >> 8) & 0xFF)) { g_st_alpha++; return; }
    u32 my = T->h - 1 - (u32)y;   // memory rows run top to bottom
    u32 off = tiled_offset((u32)x, my, T->w);
    // depth and stencil
    u32 dm = r[0x107];
    u8* dp = 0;
    u32 dbytes = T->dfmt == 0 ? 2 : T->dfmt == 3 ? 4 : 3;
    u32 dmax = T->dfmt == 0 ? 0xFFFF : 0xFFFFFF;
    u32 z = (u32)(clampf(depth, 0, 1) * dmax);
    int depth_ok = 1;
    if (T->depth) {
        dp = T->depth + off * dbytes;
        u32 old = dp[0] | (dp[1] << 8) | (dbytes >= 3 ? dp[2] << 16 : 0);
        u32 st = r[0x105];
        u8 sv = dbytes == 4 ? dp[3] : 0;
        if ((st & 1) && T->dfmt == 3) {
            u8 ref = (st >> 16) & 0xFF, im = (st >> 24) & 0xFF, wm = (st >> 8) & 0xFF;
            u32 so = r[0x106];
            if (!compare((st >> 4) & 7, ref & im, sv & im)) {
                dp[3] = (u8)((sv & ~wm) | (stencil_op(so & 7, sv, ref) & wm));
                return;
            }
            int zp = !(dm & 1) || compare((dm >> 4) & 7, z, old);
            u8 nv = stencil_op(zp ? (so >> 8) & 7 : (so >> 4) & 7, sv, ref);
            dp[3] = (u8)((sv & ~wm) | (nv & wm));
            if (!zp) return;
        } else if ((dm & 1) && !compare((dm >> 4) & 7, z, old)) depth_ok = 0;
        if (!depth_ok) { g_st_depth++; return; }
        if ((dm >> 12) & 1 && r[0x115]) {
            dp[0] = (u8)z; dp[1] = (u8)(z >> 8);
            if (dbytes >= 3) dp[2] = (u8)(z >> 16);
        }
    }
    if (!T->color || !r[0x113]) return;
    g_st_written++;
    u8* cp = T->color + off * T->cbpp;
    Color d = decode_color(cp, T->cfmt);
    Color o = c;
    if ((r[0x100] >> 8) & 1) {   // blending
        u32 bf = r[0x101], kc = r[0x103];
        Color k = {(kc & 0xFF) / 255.f, ((kc >> 8) & 0xFF) / 255.f, ((kc >> 16) & 0xFF) / 255.f, (kc >> 24) / 255.f};
        float sc[4] = {c.r, c.g, c.b, c.a}, dc[4] = {d.r, d.g, d.b, d.a}, oc[4];
        for (int ch = 0; ch < 4; ch++) {
            int al = ch == 3;
            u32 sfac = al ? (bf >> 24) & 0xF : (bf >> 16) & 0xF, dfac = al ? (bf >> 28) & 0xF : (bf >> 20) & 0xF;
            u32 eq = al ? (bf >> 8) & 7 : bf & 7;
            float s = sc[ch] * blend_factor(sfac, al, c, d, k, ch), dd = dc[ch] * blend_factor(dfac, al, c, d, k, ch);
            oc[ch] = (eq == 3 || eq == 4) ? blend_eq(eq, sc[ch], dc[ch]) : blend_eq(eq, s, dd);
        }
        o = (Color){oc[0], oc[1], oc[2], oc[3]};
    }
    u32 wm = (dm >> 8) & 0xF;
    if (wm != 0xF) {
        if (!(wm & 1)) o.r = d.r;
        if (!(wm & 2)) o.g = d.g;
        if (!(wm & 4)) o.b = d.b;
        if (!(wm & 8)) o.a = d.a;
    }
    encode_color(cp, T->cfmt, o);
}

// ---- triangles ------------------------------------------------------------------------------------

#define NV 9
static int clip_poly(PicaVertex* in, int n, PicaVertex* out, float a, float b, float c, float d) {
    // keeps a*x + b*y + c*z + d*w >= 0
    int m = 0;
    for (int i = 0; i < n; i++) {
        PicaVertex* p = &in[i];
        PicaVertex* q = &in[(i + 1) % n];
        float dp = a * p->pos.x + b * p->pos.y + c * p->pos.z + d * p->pos.w;
        float dq = a * q->pos.x + b * q->pos.y + c * q->pos.z + d * q->pos.w;
        if (dp >= 0 && m < 2 * NV) out[m++] = *p;
        if ((dp >= 0) != (dq >= 0) && m < 2 * NV) {
            float t = dp / (dp - dq);
            float* fp = (float*)p, *fq = (float*)q, *fo = (float*)&out[m];
            for (unsigned k = 0; k < sizeof(PicaVertex) / 4; k++) fo[k] = fp[k] + (fq[k] - fp[k]) * t;
            m++;
        }
    }
    return m;
}

typedef struct { float x, y, z, iw; PicaVertex v; } Screen;

static void raster_tri(Target* T, const Screen* a, const Screen* b, const Screen* c) {
    const u32* r = g_pica.reg;
    float area = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (area == 0) return;
    u32 cull = r[0x40] & 3;
    if (cull == 1 && area > 0) return;   // keep clockwise
    if (cull == 2 && area < 0) return;   // keep counter-clockwise
    int x0 = (int)floorf(fminf(a->x, fminf(b->x, c->x))), x1 = (int)ceilf(fmaxf(a->x, fmaxf(b->x, c->x)));
    int y0 = (int)floorf(fminf(a->y, fminf(b->y, c->y))), y1 = (int)ceilf(fmaxf(a->y, fmaxf(b->y, c->y)));
    int sx0 = 0, sy0 = 0, sx1 = (int)T->w - 1, sy1 = (int)T->h - 1;
    if (r[0x65] & 3) {   // scissor (keep inside)
        sx0 = r[0x66] & 0x3FF; sy0 = (r[0x66] >> 16) & 0x3FF; sx1 = r[0x67] & 0x3FF; sy1 = (r[0x67] >> 16) & 0x3FF;
    }
    if (x0 < sx0) x0 = sx0;
    if (y0 < sy0) y0 = sy0;
    if (x1 > sx1) x1 = sx1;
    if (y1 > sy1) y1 = sy1;
    float inv = 1.0f / area;
    int wbuf = !(r[0x6D] & 1);
    u32 tex_on = r[0x80] & 7;
    int pica_lighting_on(void);
    void pica_lighting(const float* q, const float* view, const float (*tex)[4], float* prim, float* sec);
    int light_on = pica_lighting_on();
    int uv2_from_uv1 = (r[0x80] >> 13) & 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float w0 = ((b->x - px) * (c->y - py) - (c->x - px) * (b->y - py)) * inv;
            float w1 = ((c->x - px) * (a->y - py) - (a->x - px) * (c->y - py)) * inv;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * a->z + w1 * b->z + w2 * c->z;
            float iw = w0 * a->iw + w1 * b->iw + w2 * c->iw;
            if (wbuf && iw != 0) z /= iw;
            float p0 = w0 * a->iw / iw, p1 = w1 * b->iw / iw, p2 = w2 * c->iw / iw;
            #define I(f) (p0 * a->v.f + p1 * b->v.f + p2 * c->v.f)
            Color prim = {clampf(I(color.x), 0, 1), clampf(I(color.y), 0, 1), clampf(I(color.z), 0, 1), clampf(I(color.w), 0, 1)};
            Color tex[4] = {{0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}, {0, 0, 0, 1}};
            if (tex_on & 1) {
                float s = I(uv0[0]), t = I(uv0[1]);
                if (((r[0x83] >> 28) & 7) == 3) { float q = I(uv0w); if (q) { s /= q; t /= q; } }
                tex[0] = sample(0, s, t);
            }
            if (tex_on & 2) tex[1] = sample(1, I(uv1[0]), I(uv1[1]));
            if (tex_on & 4) tex[2] = uv2_from_uv1 ? sample(2, I(uv1[0]), I(uv1[1])) : sample(2, I(uv2[0]), I(uv2[1]));
            if (light_on) {
                float qv[4] = {I(quat.x), I(quat.y), I(quat.z), I(quat.w)}, vv[3] = {I(view[0]), I(view[1]), I(view[2])};
                float tx[4][4], fp[4], fs[4];
                for (int k = 0; k < 4; k++) { tx[k][0] = tex[k].r; tx[k][1] = tex[k].g; tx[k][2] = tex[k].b; tx[k][3] = tex[k].a; }
                pica_lighting(qv, vv, (const float(*)[4])tx, fp, fs);
                g_frag_prim = (Color){fp[0], fp[1], fp[2], fp[3]};
                g_frag_sec = (Color){fs[0], fs[1], fs[2], fs[3]};
            } else g_frag_prim = g_frag_sec = (Color){0, 0, 0, 0};
            #undef I
            Color out = run_tev(prim, tex);
            fragment(T, x, y, z, out);
        }
    }
}

unsigned msr_sw_triangles;

// Pixel work is only done for the frames that get saved: the two frames before each saved one
// are drawn (the game's double buffering), the rest only run their commands. 0 = draw all.
int msr_draw_this_frame = 1;
unsigned msr_skipped_triangles;

void pica_draw_triangles(const PicaVertex* v, unsigned count) {
    if (!msr_draw_this_frame) { msr_skipped_triangles += count / 3; return; }
    static unsigned logged;
    unsigned f0 = g_st_frag, a0 = g_st_alpha, d0 = g_st_depth, w0 = g_st_written;
    void draw_body(const PicaVertex* v, unsigned count);
    draw_body(v, count);
    extern u64 sched_now_ns(void);
    if (getenv("MSR_DRAWLOG") && logged < 4000 && sched_now_ns() / 1000000000ull >= (u64)atoi(getenv("MSR_DRAWLOG"))) {
        logged++;
        const u32* r = g_pica.reg;
        msr_log("raster: target %08X %ux%u fmt %u, %u tris, frags %u alpha-fail %u depth-fail %u written %u, tex %X fmts %X/%X/%X, light %u",
                r[0x11D] << 3, r[0x11E] & 0x7FF, ((r[0x11E] >> 12) & 0x3FF) + 1, (r[0x117] >> 16) & 7, count / 3,
                g_st_frag - f0, g_st_alpha - a0, g_st_depth - d0, g_st_written - w0, r[0x80] & 7, r[0x8E] & 15, r[0x96] & 15, r[0x9E] & 15,
                r[0x8F] & 1);
        msr_log("raster:   depth scale %g offset %g map %u depthreg %08X stencil %08X v0 z/w %g w %g", pica_f24(r[0x4D]), pica_f24(r[0x4E]),
                r[0x6D] & 1, r[0x107], r[0x105], v[0].pos.z / v[0].pos.w, v[0].pos.w);
        if (count / 3 <= 2) {
            msr_log("raster:   tev %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X | %08X %08X %08X %08X buf %08X %08X",
                r[0xC0], r[0xC1], r[0xC2], r[0xC3], r[0xC8], r[0xC9], r[0xCA], r[0xCB], r[0xD0], r[0xD1], r[0xD2], r[0xD3],
                r[0xD8], r[0xD9], r[0xDA], r[0xDB], r[0xF0], r[0xF1], r[0xF2], r[0xF3], r[0xF8], r[0xF9], r[0xFA], r[0xFB], r[0xE0], r[0xFD]);
            msr_log("raster:   blend op %08X func %08X color %08X alpha test %08X depth/mask %08X tex0 dim %08X par %08X addr %08X; tex1 dim %08X addr %08X; v0 color %g %g %g %g uv %g %g",
                r[0x100], r[0x101], r[0x103], r[0x104], r[0x107], r[0x82], r[0x83], r[0x85] << 3, r[0x92], r[0x95] << 3,
                v[0].color.x, v[0].color.y, v[0].color.z, v[0].color.w, v[0].uv0[0], v[0].uv0[1]);
        }
        if ((r[0x80] & 7) == 3 && count / 3 <= 2) {
            Color a = sample(0, 0.5f, 0.6f), b = sample(1, 0.5f, 0.6f);
            msr_log("raster:   sample tex0 %g %g %g %g tex1 %g %g %g %g", a.r, a.g, a.b, a.a, b.r, b.g, b.b, b.a);
            u8* t0 = pica_ptr(r[0x85] << 3);
            unsigned nz = 0, nz2 = 0;
            for (u32 k = 0; k < 256 * 512 * 4; k++) nz += t0[k] != 0;
            u8* rt = g_mem + 0x1F0EA600u;
            for (u32 k = 0; k < 240 * 400 * 4; k++) nz2 += rt[k] != 0;
            msr_log("raster:   tex0 nonzero bytes %u, 1F0EA600 nonzero bytes %u", nz, nz2);
        }
        if (g_st_frag == f0) msr_log("raster:   v0 %g %g %g %g  v1 %g %g %g %g cull %u", v[0].pos.x, v[0].pos.y, v[0].pos.z, v[0].pos.w,
                                     v[1].pos.x, v[1].pos.y, v[1].pos.z, v[1].pos.w, r[0x40] & 3);
    }
}

void draw_body(const PicaVertex* v, unsigned count) {
    const u32* r = g_pica.reg;
    Target T;
    T.w = r[0x11E] & 0x7FF;
    T.h = ((r[0x11E] >> 12) & 0x3FF) + 1;
    T.cfmt = (r[0x117] >> 16) & 7;
    T.cbpp = k_fb_bpp[T.cfmt];
    T.dfmt = r[0x116] & 3;
    T.color = pica_ptr(r[0x11D] << 3);
    T.depth = r[0x11C] ? pica_ptr(r[0x11C] << 3) : 0;
    if (!T.w || !T.h || (!T.color && !T.depth)) return;
    float vw = pica_f24(r[0x41]), vh = pica_f24(r[0x43]);
    s32 vx = (s32)(r[0x68] << 22) >> 22, vy = (s32)((r[0x68] >> 16) << 22) >> 22;
    float zs = pica_f24(r[0x4D]), zo = pica_f24(r[0x4E]);
    int zbuf = r[0x6D] & 1;
    for (unsigned i = 0; i + 2 < count; i += 3) {
        PicaVertex A[2 * NV], B[2 * NV];
        A[0] = v[i]; A[1] = v[i + 1]; A[2] = v[i + 2];
        int n = 3;
        static const float planes[7][4] = {
            {0, 0, 0, 1}, {0, 0, -1, 0}, {0, 0, 1, 1}, {1, 0, 0, 1}, {-1, 0, 0, 1}, {0, 1, 0, 1}, {0, -1, 0, 1}};
        for (int p = 0; p < 7 && n >= 3; p++) {
            if (p == 0) {   // w > a small epsilon
                n = clip_poly(A, n, B, 0, 0, 0, 1);
                for (int k = 0; k < n; k++) if (B[k].pos.w < 1e-5f) B[k].pos.w = 1e-5f;
            } else n = clip_poly(A, n, B, planes[p][0], planes[p][1], planes[p][2], planes[p][3]);
            memcpy(A, B, sizeof(PicaVertex) * n);
        }
        if (n < 3) continue;
        Screen S[2 * NV];
        for (int k = 0; k < n; k++) {
            float iw = 1.0f / A[k].pos.w;
            S[k].x = (A[k].pos.x * iw + 1) * vw + vx;
            S[k].y = (A[k].pos.y * iw + 1) * vh + vy;
            S[k].z = A[k].pos.z * iw * zs + zo;   // a w-buffer multiplies by w per pixel
            S[k].iw = iw;
            S[k].v = A[k];
        }
        for (int k = 1; k + 1 < n; k++) raster_tri(&T, &S[0], &S[k], &S[k + 1]);
        msr_sw_triangles++;
    }
}

// ---- display transfer, texture copy, frame output ----------------------------------------------

void pica_display_transfer(u32 in_v, u32 out_v, u32 in_dim, u32 out_dim, u32 flags) {
    u32 iw = in_dim & 0xFFFF, ih = in_dim >> 16;
    int flip = flags & 1, in_linear = (flags >> 1) & 1, same = (flags >> 5) & 1;
    int out_tiled = in_linear != same;
    u32 fi = (flags >> 8) & 7, fo = (flags >> 12) & 7, down = (flags >> 24) & 3;
    u32 sx = down ? 2 : 1, sy = down == 2 ? 2 : 1;
    u32 ow = (out_dim & 0xFFFF) / sx, oh = (out_dim >> 16) / sy;
    u32 cw = ow < iw / sx ? ow : iw / sx, ch = oh < ih / sy ? oh : ih / sy;
    if (!in_v || !out_v || fi > 4 || fo > 4) return;
    u8* in = g_mem + in_v;
    u8* out = g_mem + out_v;
    u32 bi = k_fb_bpp[fi], bo = k_fb_bpp[fo];
    for (u32 y = 0; y < ch; y++)
        for (u32 x = 0; x < cw; x++) {
            Color acc = {0, 0, 0, 0};
            for (u32 j = 0; j < sy; j++)
                for (u32 i = 0; i < sx; i++) {
                    u32 xx = x * sx + i, yy = y * sy + j;
                    u32 o = in_linear ? yy * iw + xx : tiled_offset(xx, yy, iw);
                    Color c = decode_color(in + o * bi, fi);
                    acc.r += c.r; acc.g += c.g; acc.b += c.b; acc.a += c.a;
                }
            float n = (float)(sx * sy);
            acc.r /= n; acc.g /= n; acc.b /= n; acc.a /= n;
            u32 dy = flip ? ch - 1 - y : y;
            u32 o = out_tiled ? tiled_offset(x, dy, ow) : dy * ow + x;
            encode_color(out + o * bo, fo, acc);
        }
}

int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h);
extern char msr_frames_dir[512];
// Debug: saves a 240x400 RGBA8 tiled render target as seen on the top screen.
void pica_debug_dump_target(u32 v, const char* tag) {
    static unsigned n;
    if (!getenv("MSR_DUMPRT") || n >= 40) return;
    u8* rgb = (u8*)malloc(400 * 240 * 3);
    for (u32 x = 0; x < 400; x++)
        for (u32 y = 0; y < 240; y++) {
            u32 o = tiled_offset(239 - y, 399 - x, 240);
            Color c = decode_color(g_mem + v + o * 4, 0);
            u8* d = rgb + (y * 400 + x) * 3;
            d[0] = (u8)(clampf(c.r, 0, 1) * 255); d[1] = (u8)(clampf(c.g, 0, 1) * 255); d[2] = (u8)(clampf(c.b, 0, 1) * 255);
        }
    char path[700];
    snprintf(path, sizeof path, "%s/rt_%02u_%s.png", msr_frames_dir, n++, tag);
    msr_write_png(path, rgb, 400, 240);
    free(rgb);
}

void pica_texture_copy(u32 in_v, u32 out_v, u32 size, u32 in_gap, u32 out_gap, u32 flags) {
    (void)flags;
    if (msr_draw_this_frame && in_v == 0x1F08CA00u) pica_debug_dump_target(in_v, "copy");
    u32 iw = (in_gap & 0xFFFF) * 16, ig = (in_gap >> 16) * 16;
    u32 ow = (out_gap & 0xFFFF) * 16, og = (out_gap >> 16) * 16;
    if (!in_v || !out_v) return;
    if (!iw) iw = size;
    if (!ow) ow = size;
    u32 ip = 0, op = 0, done = 0, ic = 0, oc = 0;
    size &= ~15u;
    while (done < size) {
        u32 n = iw - ic < ow - oc ? iw - ic : ow - oc;
        if (n > size - done) n = size - done;
        memmove(g_mem + out_v + op, g_mem + in_v + ip, n);
        ip += n; op += n; ic += n; oc += n; done += n;
        if (ic == iw) { ic = 0; ip += ig; }
        if (oc == ow) { oc = 0; op += og; }
    }
}

// ---- PNG output ------------------------------------------------------------------------------

static u32 crc_table[256];
static u32 crc32_update(u32 c, const u8* p, size_t n) {
    if (!crc_table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_table[i] = v;
        }
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put32(FILE* f, u32 v) { u8 b[4] = {(u8)(v >> 24), (u8)(v >> 16), (u8)(v >> 8), (u8)v}; fwrite(b, 1, 4, f); }

static void chunk(FILE* f, const char* type, const u8* data, u32 n) {
    put32(f, n);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    u32 c = crc32_update(0xFFFFFFFFu, (const u8*)type, 4);
    c = crc32_update(c, data, n);
    put32(f, c ^ 0xFFFFFFFFu);
}

// Writes an RGB image as a PNG with stored (uncompressed) deflate blocks.
int msr_write_png(const char* path, const u8* rgb, u32 w, u32 h) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    u8 ihdr[13] = {(u8)(w >> 24), (u8)(w >> 16), (u8)(w >> 8), (u8)w, (u8)(h >> 24), (u8)(h >> 16), (u8)(h >> 8), (u8)h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    size_t raw_n = (size_t)h * (w * 3 + 1);
    u8* raw = (u8*)malloc(raw_n);
    for (u32 y = 0; y < h; y++) {
        raw[y * (w * 3 + 1)] = 0;
        memcpy(raw + y * (w * 3 + 1) + 1, rgb + (size_t)y * w * 3, w * 3);
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t zn = 2 + raw_n + blocks * 5 + 4;
    u8* z = (u8*)malloc(zn);
    size_t p = 0;
    z[p++] = 0x78; z[p++] = 0x01;
    u32 s1 = 1, s2 = 0;
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        z[p++] = off + n >= raw_n ? 1 : 0;
        z[p++] = (u8)n; z[p++] = (u8)(n >> 8); z[p++] = (u8)~n; z[p++] = (u8)(~n >> 8);
        memcpy(z + p, raw + off, n);
        p += n;
    }
    for (size_t i = 0; i < raw_n; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    u32 ad = (s2 << 16) | s1;
    z[p++] = (u8)(ad >> 24); z[p++] = (u8)(ad >> 16); z[p++] = (u8)(ad >> 8); z[p++] = (u8)ad;
    chunk(f, "IDAT", z, (u32)p);
    chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}

char msr_frames_dir[512];
unsigned msr_frame_every = 30;

// An LCD framebuffer is 240 pixels per column, columns left to right, each column bottom to top.
void pica_frame_shown(int screen, u32 left_v, u32 stride, u32 format) {
    static unsigned shown[2];
    unsigned n = shown[screen]++;
    {   // MSR_FRAMES_AFTER=seconds: no pictures (and no pixel work) before then
        extern u64 sched_now_ns(void);
        const char* fa = getenv("MSR_FRAMES_AFTER");
        if (fa && sched_now_ns() / 1000000000ull < (u64)atoi(fa)) { msr_draw_this_frame = 0; return; }
    }
    if (screen == 0 && msr_frame_every > 3) {
        unsigned left = msr_frame_every - (n + 1) % msr_frame_every;   // frames until the next save
        msr_draw_this_frame = left <= 3 || (n + 1) % msr_frame_every == 0;
    }
    if (!msr_frames_dir[0] || n % msr_frame_every || !left_v) return;
    u32 fmt = format & 7, bpp = k_fb_bpp[fmt];
    u32 W = screen == 0 ? 400 : 320, H = 240;
    if (!stride) stride = 240 * bpp;
    u8* rgb = (u8*)malloc(W * H * 3);
    for (u32 x = 0; x < W; x++)
        for (u32 y = 0; y < H; y++) {
            Color c = decode_color(g_mem + left_v + x * stride + (H - 1 - y) * bpp, fmt);
            u8* o = rgb + (y * W + x) * 3;
            o[0] = (u8)(clampf(c.r, 0, 1) * 255); o[1] = (u8)(clampf(c.g, 0, 1) * 255); o[2] = (u8)(clampf(c.b, 0, 1) * 255);
        }
    char path[700];
    snprintf(path, sizeof path, "%s/%s_%05u.png", msr_frames_dir, screen ? "bottom" : "top", n);
    msr_write_png(path, rgb, W, H);
    free(rgb);
    u8* q = g_mem + left_v + 720 * 200 + 300;
    msr_log("frame: wrote %s (%08X fmt %u: %02X %02X %02X %02X %02X %02X %02X %02X)", path, left_v, fmt,
            q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7]);
}
