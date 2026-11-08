// MSR-nx: PICA200 command processor. Reads a command list into the register file, feeds the shader
// units' upload ports, and on a draw fetches vertices, runs the vertex shader and hands triangles
// to the renderer.
//
// Command format (3dbrew "GPU/Commands"): a parameter word, then a header word: register id in
// bits 0-15, byte mask in 16-19, extra parameter count in 20-27, bit 31 "consecutive" (the extra
// words go to the following registers instead of the same one). Commands are padded to 8 bytes.
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "msr_log.h"
#include "pica.h"
#include <stdio.h>
#include <time.h>

Pica g_pica;

float pica_f24(u32 v) {
    v &= 0xFFFFFF;
    u32 sign = v >> 23, exp = (v >> 16) & 0x7F, man = v & 0xFFFF;
    u32 bits;
    if (exp == 0 && man == 0) bits = sign << 31;
    else if (exp == 0x7F) bits = (sign << 31) | 0x7F800000u | (man << 7);
    else bits = (sign << 31) | ((exp + 64) << 23) | (man << 7);   // bias 63 -> 127
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static float f32_of(u32 v) { float f; memcpy(&f, &v, 4); return f; }

// Four float24 values packed into three words, most significant (w) first.
static Vec4 unpack_f24x4(const u32* w) {
    Vec4 r;
    r.x = pica_f24(w[2] & 0xFFFFFF);
    r.y = pica_f24(((w[1] & 0xFFFF) << 8) | (w[2] >> 24));
    r.z = pica_f24(((w[0] & 0xFF) << 16) | (w[1] >> 16));
    r.w = pica_f24(w[0] >> 8);
    return r;
}

static void uniform_word(ShaderUnit* u, u32 word) {
    u->f_stage[u->f_pos++] = word;
    if (u->f_pos < (u->f_wide ? 4u : 3u)) return;
    u->f_pos = 0;
    Vec4 v;
    if (u->f_wide) { v.x = f32_of(u->f_stage[3]); v.y = f32_of(u->f_stage[2]); v.z = f32_of(u->f_stage[1]); v.w = f32_of(u->f_stage[0]); }
    else v = unpack_f24x4(u->f_stage);
    if ((v.x != v.x || v.y != v.y || v.z != v.z || v.w != v.w)) {
        static int n;
        if (n++ < 20) msr_log("pica: uniform c%u is NaN from %s words %08X %08X %08X %08X", u->f_index,
                              u->f_wide ? "f32" : "f24", u->f_stage[0], u->f_stage[1], u->f_stage[2], u->f_stage[3]);
    }
    if (u->f_index < 96) u->f[u->f_index] = v;
    u->f_index++;
}

// ---- vertex fetch and draws ---------------------------------------------------------------------

static const u32 k_attr_size[4] = {1, 1, 2, 4};

int (*pica_draw_hook)(int indexed);
void (*pica_sync_hook)(u32 vaddr, u32 size);
void (*pica_invalidate_hook)(u32 vaddr, u32 size);
void (*pica_fill_hook)(u32 start, u32 end, u32 value, u32 width);
unsigned pica_program_version;

void pica_load_attributes(u32 index, Vec4* in) {
    const u32* r = g_pica.reg;
    u32 base = r[0x200] << 3;
    u64 fmt = (u64)r[0x201] | ((u64)(r[0x202] & 0xFFFF) << 32);
    u32 fixed_mask = (r[0x202] >> 16) & 0xFFF;
    u32 count = (r[0x202] >> 28) + 1;
    Vec4 attr[12];
    for (u32 a = 0; a < 12; a++) attr[a] = (Vec4){0, 0, 0, 1};
    for (u32 a = 0; a < count && a < 12; a++)
        if (fixed_mask & (1u << a)) attr[a] = g_pica.fixed_attr[a];
    for (int l = 0; l < 12; l++) {
        u32 off = r[0x203 + 3 * l], c1 = r[0x204 + 3 * l], c2 = r[0x205 + 3 * l];
        u32 n = c2 >> 28, stride = (c2 >> 16) & 0xFF;
        if (!n) continue;
        u8* p = pica_ptr(base + off + index * stride);
        if (!p) continue;
        u32 pos = 0;
        for (u32 k = 0; k < n; k++) {
            u32 id = k < 8 ? (c1 >> (4 * k)) & 0xF : (c2 >> (4 * (k - 8))) & 0xF;
            if (id >= 12) { pos += 4 * (id - 11); continue; }   // padding of 4, 8, 12 or 16 bytes
            u32 f = (u32)(fmt >> (4 * id)) & 0xF, type = f & 3, comps = (f >> 2) + 1;
            u32 sz = k_attr_size[type];
            pos = (pos + sz - 1) & ~(sz - 1);   // components align to their size
            float v[4] = {0, 0, 0, 1};
            for (u32 c = 0; c < comps; c++) {
                u8* q = p + pos + c * sz;
                switch (type) {
                case 0: v[c] = (float)(s8)q[0]; break;
                case 1: v[c] = (float)q[0]; break;
                case 2: { s16 s; memcpy(&s, q, 2); v[c] = (float)s; break; }
                case 3: { float fl; memcpy(&fl, q, 4); v[c] = fl; break; }
                }
            }
            pos += comps * sz;
            if (!(fixed_mask & (1u << id))) attr[id] = (Vec4){v[0], v[1], v[2], v[3]};
        }
    }
    // attribute i goes to the shader input register the permutation names
    u64 perm = (u64)r[0x2BB] | ((u64)r[0x2BC] << 32);
    for (u32 a = 0; a < count && a < 12; a++) in[(perm >> (4 * a)) & 0xF] = attr[a];
}

#define MAX_DRAW 65536
static PicaVertex g_verts[MAX_DRAW];
static PicaVertex g_tris[MAX_DRAW * 3];

// Vertex cache: indexed draws reuse shaded vertices.
#define CACHE 256
static u32 g_cache_tag[CACHE];
static PicaVertex g_cache_v[CACHE];

static void shade(u32 index, PicaVertex* out) {
    Vec4 in[16], o[16];
    memset(in, 0, sizeof in);
    memset(o, 0, sizeof o);
    pica_load_attributes(index, in);
    pica_shader_run(&g_pica.vs, g_pica.reg + 0x2B0, in, o);
    pica_map_outputs(o, out);
}

static void assemble_and_draw(const PicaVertex* v, unsigned n) {
    u32 mode = (g_pica.reg[0x25E] >> 8) & 3;
    unsigned t = 0;
    if (mode == 0 || mode == 3) {          // triangle list (3: geometry shader primitives)
        for (unsigned i = 0; i + 2 < n && t + 3 <= MAX_DRAW * 3; i += 3) {
            g_tris[t++] = v[i]; g_tris[t++] = v[i + 1]; g_tris[t++] = v[i + 2];
        }
    } else if (mode == 1) {                // strip
        for (unsigned i = 0; i + 2 < n && t + 3 <= MAX_DRAW * 3; i++) {
            if (i & 1) { g_tris[t++] = v[i + 1]; g_tris[t++] = v[i]; g_tris[t++] = v[i + 2]; }
            else { g_tris[t++] = v[i]; g_tris[t++] = v[i + 1]; g_tris[t++] = v[i + 2]; }
        }
    } else {                               // fan
        for (unsigned i = 1; i + 1 < n && t + 3 <= MAX_DRAW * 3; i++) {
            g_tris[t++] = v[0]; g_tris[t++] = v[i]; g_tris[t++] = v[i + 1];
        }
    }
    if (t) pica_draw_triangles(g_tris, t);
}

// MSR_VTIME: measures the CPU cost of the GPU renderer's vertex fetch (every vertex a draw uses,
// decoded once), so speed work can be planned from numbers.
static int pica_vtime = -1;
static double g_vtime_ns;
static unsigned long long g_vtime_verts;
static void vtime_draw(int indexed) {
    const u32* r = g_pica.reg;
    u32 n = r[0x228], first = r[0x22A], lo = first, hi = first + n - 1;
    if (!n) return;
    if (indexed) {
        u32 ib = r[0x227];
        const u8* idx = pica_ptr((r[0x200] << 3) + (ib & 0x0FFFFFFF));
        if (!idx) return;
        int wide = (ib >> 31) & 1;
        lo = ~0u; hi = 0;
        for (u32 i = 0; i < n; i++) {
            u32 v; if (wide) { u16 t; memcpy(&t, idx + 2 * i, 2); v = t; } else v = idx[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    static volatile float sink;
    for (u32 i = lo; i <= hi; i++) { Vec4 in[16]; pica_load_attributes(i, in); sink += in[0].x; }
    clock_gettime(CLOCK_MONOTONIC, &b);
    g_vtime_ns += (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
    g_vtime_verts += hi - lo + 1;
}

// ---- vertex shader keys --------------------------------------------------------------------------
// A vertex shader configuration is the game's PICA program (only the instructions reachable from the
// entry point, with the swizzle patterns they use) plus the output setup. Its 64-bit key names the
// translated Switch shader in the shader pack. New configurations can be written to a file
// (pica_vs_dump_path) for the translator, tools/shaders/pica2glsl.py.
static u64 fnv(u64 h, u32 v) { for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 0x100000001B3ull; } return h; }

static u64 program_hash(const ShaderUnit* u, u32 entry) {
    static u8 seen[4096];
    static u16 work[4096];
    memset(seen, 0, sizeof seen);
    int n = 0;
    work[n++] = (u16)entry;
    u64 h = 0xCBF29CE484222325ull;
    while (n) {
        u32 pc = work[--n];
        while (pc < 4096 && !seen[pc]) {
            seen[pc] = 1;
            u32 w = u->program[pc], op = w >> 26;
            u32 D = (w >> 10) & 0xFFF, N = w & 0xFF;
            if (op == 0x22) break;   // END
            if ((op >= 0x24 && op <= 0x29) || op == 0x2C || op == 0x2D) {
                if (n < 4090) work[n++] = (u16)D;
                if ((op >= 0x24 && op <= 0x28) && n < 4090 && D + N < 4096) work[n++] = (u16)(D + N);
                if (op == 0x29 && n < 4090 && D + 1 < 4096) work[n++] = (u16)(D + 1);
            }
            pc++;
        }
    }
    for (u32 pc = 0; pc < 4096; pc++) {
        if (!seen[pc]) continue;
        u32 w = u->program[pc], op = w >> 26;
        h = fnv(h, pc);
        h = fnv(h, w);
        if (op >= 0x30) h = fnv(h, u->swizzle[w & 0x1F]);
        else if (op < 0x20 || op == 0x2E || op == 0x2F) h = fnv(h, u->swizzle[w & 0x7F]);
    }
    return h;
}

const char* pica_vs_dump_path;
void (*pica_vs_check_hook)(void);   // the probe's translation check
static unsigned g_key_version = ~0u;
static u32 g_key_entry;
static u64 g_key_prog;

void pica_vs_dump(u64 h, const char* path) {
    const u32* r = g_pica.reg;
    FILE* f = fopen(path, "ab");
    if (!f) return;
    u32 hdr[13] = {0x31535650u /* "PVS1" */, (u32)h, (u32)(h >> 32), r[0x2BA] & 0xFFFF, r[0x2BD], r[0x4F] & 7};
    for (int k = 0; k < 7; k++) hdr[6 + k] = r[0x50 + k];
    fwrite(hdr, 4, 13, f);
    fwrite(g_pica.vs.program, 4, 4096, f);
    fwrite(g_pica.vs.swizzle, 4, 128, f);
    fclose(f);
}

u64 pica_vs_key(void) {
    const u32* r = g_pica.reg;
    u32 entry = r[0x2BA] & 0xFFFF;
    if (g_key_version != pica_program_version || g_key_entry != entry) {
        g_key_prog = program_hash(&g_pica.vs, entry);
        g_key_version = pica_program_version;
        g_key_entry = entry;
    }
    u64 h = fnv(g_key_prog, entry);
    h = fnv(h, r[0x2BD]);
    h = fnv(h, r[0x4F] & 7);
    for (int k = 0; k < 7; k++) h = fnv(h, r[0x50 + k]);
    if (pica_vs_dump_path) {
        #define SEEN_MAX 4096
        static u64 seen_keys[SEEN_MAX];
        static unsigned nseen;
        for (unsigned k = 0; k < nseen; k++) if (seen_keys[k] == h) return h;
        if (nseen < SEEN_MAX) seen_keys[nseen++] = h;
        pica_vs_dump(h, pica_vs_dump_path);
    }
    return h;
}

// Render-target statistics for planning the GPU renderer's speed work: which surfaces are drawn to,
// and how often a draw samples a texture that an earlier draw rendered (render to texture).
#define RT_MAX 64
static u32 g_rt_addr[RT_MAX], g_rt_wh[RT_MAX], g_rt_draws[RT_MAX], g_rt_sampled[RT_MAX], g_rt_n;
unsigned pica_stat_rtt_draws, pica_stat_tex_binds, pica_stat_rt_switches;
static u32 g_last_rt;
static int rt_find(u32 a) { for (u32 k = 0; k < g_rt_n; k++) if (g_rt_addr[k] == a) return (int)k; return -1; }
static void rt_stats(void) {
    const u32* r = g_pica.reg;
    u32 a = r[0x11D] << 3;
    u32 wh = r[0x11E];
    int k = rt_find(a);
    if (k < 0 && g_rt_n < RT_MAX) { k = (int)g_rt_n++; g_rt_addr[k] = a; g_rt_wh[k] = wh; }
    if (k >= 0) g_rt_draws[k]++;
    if (a != g_last_rt) { pica_stat_rt_switches++; g_last_rt = a; }
    static const u32 addr_reg[3] = {0x85, 0x95, 0x9D};
    int any = 0;
    for (int u = 0; u < 3; u++) {
        if (!((r[0x80] >> u) & 1)) continue;
        pica_stat_tex_binds++;
        int t = rt_find(r[addr_reg[u]] << 3);
        if (t >= 0) { g_rt_sampled[t]++; any = 1; }
    }
    if (any) pica_stat_rtt_draws++;
}
void pica_rt_summary(void) {
    if (pica_vtime > 0)
        msr_receipt("vertex fetch: %llu vertices decoded in %.1f ms of CPU time", g_vtime_verts, g_vtime_ns / 1e6);
    extern unsigned gx_stat[4];
    msr_receipt("gx stats: %u DMA, %u fills, %u display transfers, %u texture copies", gx_stat[0], gx_stat[1],
                gx_stat[2], gx_stat[3]);
    msr_receipt("gpu stats: %u texture binds, %u draws sample a render target, %u render target switches",
                pica_stat_tex_binds, pica_stat_rtt_draws, pica_stat_rt_switches);
    for (u32 k = 0; k < g_rt_n; k++)
        msr_receipt("  target %08X %ux%u: %u draws, sampled %u times", g_rt_addr[k], g_rt_wh[k] & 0x7FF,
                    ((g_rt_wh[k] >> 12) & 0x3FF) + 1, g_rt_draws[k], g_rt_sampled[k]);
}

static void draw(int indexed) {
    const u32* r = g_pica.reg;
    u32 n = r[0x228], first = r[0x22A];
    if (n > MAX_DRAW) n = MAX_DRAW;
    g_pica.draws++;
    rt_stats();
    if (pica_vs_dump_path) pica_vs_key();
    if (pica_vs_check_hook) pica_vs_check_hook();
    if (pica_vtime < 0) pica_vtime = getenv("MSR_VTIME") ? 1 : 0;
    if (pica_vtime) vtime_draw(indexed);
    if (pica_draw_hook && pica_draw_hook(indexed)) { g_pica.vertices += n; if (indexed) g_pica.draws_indexed++; return; }
    if (indexed) g_pica.draws_indexed++;
    g_pica.vertices += n;
    if ((r[0x229] & 3) == 2) g_pica.gs_draws++;
    { extern int msr_draw_this_frame; if (!msr_draw_this_frame) return; }   // frame not saved: skip the vertex work too
    u32 ib = r[0x227];
    u8* idx = indexed ? pica_ptr((r[0x200] << 3) + (ib & 0x0FFFFFFF)) : 0;
    int wide = (ib >> 31) & 1;
    memset(g_cache_tag, 0xFF, sizeof g_cache_tag);
    for (u32 i = 0; i < n; i++) {
        u32 index;
        if (indexed) {
            if (!idx) return;
            if (wide) { u16 s; memcpy(&s, idx + 2 * i, 2); index = s; }
            else index = idx[i];
        } else index = first + i;
        u32 slot = index % CACHE;
        if (indexed && g_cache_tag[slot] == index) { g_verts[i] = g_cache_v[slot]; continue; }
        { extern int pica_shader_trace; pica_shader_trace = g_pica.draws == 1 && i == 0; }
        shade(index, &g_verts[i]);
        { extern int pica_shader_trace; if (pica_shader_trace) {
            for (int k = 0; k < 96; k++) msr_log("  c%d = %g %g %g %g", k, g_pica.vs.f[k].x, g_pica.vs.f[k].y, g_pica.vs.f[k].z, g_pica.vs.f[k].w);
            msr_log("  ints %08X %08X %08X %08X bools %X entry %X", g_pica.reg[0x2B1], g_pica.reg[0x2B2], g_pica.reg[0x2B3], g_pica.reg[0x2B4], g_pica.reg[0x2B0], g_pica.reg[0x2BA]);
            pica_shader_trace = 0; } }
        if (indexed) { g_cache_tag[slot] = index; g_cache_v[slot] = g_verts[i]; }
    }
    if (g_pica.draws <= 40) {
        Vec4 in[16];
        memset(in, 0, sizeof in);
        pica_load_attributes(indexed ? 0 : first, in);
        msr_log("draw %u: %u vertices, prim %X, in0 %g %g %g %g in1 %g %g %g %g -> pos %g %g %g %g color %g %g %g %g uv %g %g",
                g_pica.draws, n, r[0x25E], in[0].x, in[0].y, in[0].z, in[0].w, in[1].x, in[1].y, in[1].z, in[1].w,
                g_verts[0].pos.x, g_verts[0].pos.y, g_verts[0].pos.z, g_verts[0].pos.w,
                g_verts[0].color.x, g_verts[0].color.y, g_verts[0].color.z, g_verts[0].color.w, g_verts[0].uv0[0], g_verts[0].uv0[1]);
    }
    assemble_and_draw(g_verts, n);
}

// Immediate mode: attributes written through GPUREG_FIXEDATTRIB_DATA with index 0xF.
static unsigned g_imm_n;
static void immediate_vertex(void) {
    Vec4 in[16], o[16];
    memset(in, 0, sizeof in);
    memset(o, 0, sizeof o);
    u32 count = (g_pica.reg[0x242] & 0xF) + 1;
    for (u32 a = 0; a < count; a++) in[a] = g_pica.immediate[a];
    pica_shader_run(&g_pica.vs, g_pica.reg + 0x2B0, in, o);
    if (g_imm_n < MAX_DRAW) pica_map_outputs(o, &g_verts[g_imm_n++]);
    g_pica.immediate_vertices++;
}

static void flush_immediate(void) {
    if (!g_imm_n) return;
    assemble_and_draw(g_verts, g_imm_n);
    g_imm_n = 0;
}

// ---- register writes ----------------------------------------------------------------------------

static int g_jump;
static u32 g_jump_addr, g_jump_size;

static void write_reg(u32 id, u32 value, u32 mask) {
    static const u32 k_bytes[16] = {
        0, 0xFF, 0xFF00, 0xFFFF, 0xFF0000, 0xFF00FF, 0xFFFF00, 0xFFFFFF, 0xFF000000u, 0xFF0000FFu,
        0xFF00FF00u, 0xFF00FFFFu, 0xFFFF0000u, 0xFFFF00FFu, 0xFFFFFF00u, 0xFFFFFFFFu};
    if (id >= PICA_REGS) return;
    u32 bits = k_bytes[mask & 0xF];
    u32* r = g_pica.reg;
    if (id != 0x233 && id != 0x234 && id != 0x235) flush_immediate();
    r[id] = (r[id] & ~bits) | (value & bits);
    value = r[id];
    ShaderUnit* u = id >= 0x2B0 ? &g_pica.vs : &g_pica.gs;
    u32 off = id >= 0x2B0 ? id - 0x2B0 : id - 0x280;
    int shader_block = (id >= 0x280 && id < 0x2AE) || (id >= 0x2B0 && id < 0x2DE);
    int mirror = id >= 0x2B0 && !(r[0x244] & 1);   // the geometry unit takes the vertex unit's uploads
    if (shader_block) {
        switch (off) {
        case 0x10: u->f_index = value & 0x7F; u->f_wide = value >> 31; u->f_pos = 0; return;
        case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17: case 0x18:
            uniform_word(u, value);
            return;
        case 0x1B: u->program_pos = value & 0xFFF; if (mirror) g_pica.gs.program_pos = u->program_pos; return;
        case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22: case 0x23:
            pica_program_version++;
            if (u->program_pos < 4096) u->program[u->program_pos++] = value;
            if (mirror && g_pica.gs.program_pos < 4096) g_pica.gs.program[g_pica.gs.program_pos++] = value;
            return;
        case 0x25: u->swizzle_pos = value & 0x7F; if (mirror) g_pica.gs.swizzle_pos = u->swizzle_pos; return;
        case 0x26: case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
            pica_program_version++;
            if (u->swizzle_pos < 128) u->swizzle[u->swizzle_pos++] = value;
            if (mirror && g_pica.gs.swizzle_pos < 128) g_pica.gs.swizzle[g_pica.gs.swizzle_pos++] = value;
            return;
        }
        return;
    }
    if (id >= 0x1C8 && id <= 0x1CF) { void pica_light_lut_write(u32 v); pica_light_lut_write(value); return; }
    switch (id) {
    case 0x22E: draw(0); return;   // DRAWARRAYS
    case 0x22F: draw(1); return;   // DRAWELEMENTS
    case 0x232: g_pica.fixed_index = value & 0xF; g_pica.fixed_pos = 0; return;
    case 0x233: case 0x234: case 0x235:
        g_pica.fixed_stage[g_pica.fixed_pos++] = value;
        if (g_pica.fixed_pos == 3) {
            g_pica.fixed_pos = 0;
            Vec4 v = unpack_f24x4(g_pica.fixed_stage);
            if (g_pica.fixed_index == 0xF) {
                static u32 imm_attr;
                g_pica.immediate[imm_attr++] = v;
                if (imm_attr > (g_pica.reg[0x242] & 0xF)) { imm_attr = 0; immediate_vertex(); }
            } else {
                g_pica.fixed_attr[g_pica.fixed_index] = v;
                g_pica.fixed_index++;
            }
        }
        return;
    case 0x23C: case 0x23D: {   // CMDBUF_JUMP0/1
        int k = id - 0x23C;
        g_jump = 1;
        g_jump_size = r[0x238 + k] << 3;
        g_jump_addr = r[0x23A + k] << 3;
        return;
    }
    }
}

void pica_summary(void) {
    extern unsigned msr_sw_triangles;
    msr_receipt("gpu: %u command lists, %u draws (%u indexed, %u with a geometry shader), %u vertices, "
                "%u immediate vertices, %u triangles rasterized", g_pica.lists, g_pica.draws, g_pica.draws_indexed,
                g_pica.gs_draws, g_pica.vertices, g_pica.immediate_vertices, msr_sw_triangles);
    pica_rt_summary();
    msr_perf_report(0);
    { extern void (*pica_vs_check_hook)(void); void vs_check_summary(void) __attribute__((weak));
      if (pica_vs_check_hook && vs_check_summary) vs_check_summary(); }
}

void pica_run_list(u32 vaddr, u32 size) {
    g_pica.lists++;
    const u8* base = g_mem + vaddr;
    u32 n = size / 4, i = 0, jumps = 0;
    while (i + 1 < n) {
        u32 data, hdr;
        memcpy(&data, base + 4 * i, 4);
        memcpy(&hdr, base + 4 * i + 4, 4);
        i += 2;
        u32 id = hdr & 0xFFFF, mask = (hdr >> 16) & 0xF, extra = (hdr >> 20) & 0xFF, cons = hdr >> 31;
        if (id >= PICA_REGS) break;
        write_reg(id, data, mask);
        for (u32 k = 0; k < extra && i < n; k++, i++) {
            u32 w;
            memcpy(&w, base + 4 * i, 4);
            write_reg(cons ? id + 1 + k : id, w, mask);
        }
        if (extra & 1) i++;
        if (g_jump) {
            g_jump = 0;
            if (++jumps > 4096) break;
            u8* p = pica_ptr(g_jump_addr);
            if (!p) break;
            base = p;
            n = g_jump_size / 4;
            i = 0;
        }
    }
    flush_immediate();
}
