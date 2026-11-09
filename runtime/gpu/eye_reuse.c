// MSR-nx: right eye reuse. The game draws every 3D frame twice, once per eye of the 3DS's
// stereoscopic screen, and with the 3D effect off (always, on the Switch) the two eyes come out
// identical: test 8's capture, replayed, gave byte-identical eye images. The draws alternate, left
// then right, object by object.
//
// Every VRAM range the GPU writes in a frame gets a token, a hash of how it was made: a fill's
// value; a copy's or transfer's settings and its source's token; a render target's token before a
// draw combined with everything the draw depends on (registers except the target addresses, vertex
// shader uniforms and program, lookup tables, and its textures' tokens in place of their
// addresses). Two ranges with the same token hold the same pixels. A right eye draw whose target
// tokens and draw hash equal those of the left eye draw just before it would produce the same
// result, so it is skipped and the right eye's tokens become the left eye's. The right eye's image
// is then behind what the game thinks it holds; before anything reads it (a copy, a transfer, a
// texture, a draw that is not skipped), the left eye's image is copied over it while the left eye
// still holds that token. If it no longer does, reuse turns itself off for a while and says so.
//
// The eyes are the render targets the game transfers to the top screen's left and right
// framebuffers. Off unless eye_reuse_set(1). tools: the probe's MSR_EYE=1 runs it on the reference
// renderer, where the shown pictures must stay byte-identical.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pica.h"
#include "msr_log.h"
#include "eye_reuse.h"

#define MAX_TOK 256
typedef struct { u32 lo, hi; u64 tok; } Tok;
static Tok g_tok[MAX_TOK];
static unsigned g_ntok;
static u64 g_fresh = 0x9E3779B97F4A7C15ull;
static int g_on, g_off_frames;
unsigned eye_skipped, eye_drawn_right, eye_materialized, eye_failures;
unsigned eye_runs[2], eye_total[2];   // draws on the left (right) eye's target: run, all

// the eyes: color and depth targets (guest virtual addresses) and their byte sizes, from the frame before
static u32 g_lc, g_rc, g_ld, g_rd, g_csz, g_dsz;
static u32 g_next_lc, g_next_rc, g_next_ld, g_next_rd, g_next_csz, g_next_dsz;
static u32 g_fb_left[4], g_fb_right[4], g_nleft, g_nright;
// The left eye's images hold the pictures for both eyes. phys_c, phys_d: the tokens they really
// hold (the left eye's own token, or the right eye's after a right eye draw went there first: the
// game draws an object into the left eye then the right, the next one right then left). r_ok_c,
// r_ok_d: the right eye's own images hold its tokens (not behind).
static u64 g_phys_c, g_phys_d;
static int g_r_ok_c = 1, g_r_ok_d = 1, g_r_garbage, g_r_garbage_d;   // garbage: the right eye's own color (depth) image holds nothing anyone knows
static u32 g_l_reg_c, g_l_reg_d, g_saved_c, g_saved_d;
static int g_redirected;
// MSR_EYEDEBUG=N: the first N draws reuse could not handle, and why
static int g_dbg = -1;
static unsigned g_frame, g_dbg_from;   // MSR_EYEFROM=frame: the debug output from that frame on
static u32 g_dbg_regs[2][PICA_REGS];
static Vec4 g_dbg_f[2][96];
static u64 g_dbg_tex[2][3], g_dbg_vh[2];

// One multiply per value: the draw hash takes about 1300 values a draw, a few hundred draws a frame.
static u64 mix(u64 h, u64 v) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}
static u64 fresh(void) { return mix(0x6A09E667F3BCC908ull, ++g_fresh); }   // a counter: mix(x, x + 1) repeats
// Tokens of dead output (the frame plan skipped the draw: nothing shown depends on it). A read of
// one (the right eye's transfer to a framebuffer that is not shown) needs no real content.
static u64 g_dead[64];
static unsigned g_ndead;
static u64 dead(void) { u64 t = fresh(); g_dead[g_ndead++ & 63] = t; return t; }
static int is_dead(u64 t) { for (unsigned k = 0; k < 64; k++) if (g_dead[k] == t) return 1; return 0; }

void eye_reuse_set(int on) {
    g_on = on;
    g_r_ok_c = g_r_ok_d = 1;
    g_r_garbage = g_r_garbage_d = 1;   // whatever skipped draws left behind before
    msr_receipt("eye: right eye %s", on ? "reuses the left eye's pictures" : "drawn");
}
int eye_reuse_on(void) { return g_on; }

// The token of [lo, hi): that of a range written from lo (a read of a different size gets a token
// of its own, the same for both eyes' equal ranges), else 0. A texture read is often larger than
// what was written into it (a 240x400 copy into a 256x512 texture): the part past the write is
// taken to be unused padding. The probe's byte-identical check is what holds this up.
static u64 tok_get(u32 lo, u32 hi) {
    for (unsigned k = 0; k < g_ntok; k++)
        if (g_tok[k].lo == lo) return hi == g_tok[k].hi ? g_tok[k].tok : mix(g_tok[k].tok, hi - lo);
    return 0;
}
// A source's token, or (no token: last frame's or the CPU's data) its address and size.
static u64 src_tok(u32 lo, u32 hi) {
    u64 t = tok_get(lo, hi);
    return t ? t : mix(mix(0x51ED270B27u, lo), hi);
}
static void tok_set(u32 lo, u32 hi, u64 t) {
    if (lo >= hi) return;
    unsigned w = 0;
    for (unsigned k = 0; k < g_ntok; k++)
        if (g_tok[k].hi <= lo || g_tok[k].lo >= hi) g_tok[w++] = g_tok[k];
    g_ntok = w;
    if (g_ntok < MAX_TOK) g_tok[g_ntok++] = (Tok){lo, hi, t};
}
static int overlaps(u32 lo, u32 hi, u32 a, u32 n) { return n && lo < a + n && a < hi; }
static int covers(u32 lo, u32 hi, u32 a, u32 n) { return lo <= a && a + n <= hi; }

static int trf(void) { static int f = -2; if (f == -2) { const char* e = getenv("MSR_EYETRF"); f = e ? atoi(e) : -1; } return f >= 0 && (int)g_frame == f; }
static void (*g_copy)(u32 src, u32 dst, u32 size);
void eye_reuse_copier(void (*copy)(u32 src, u32 dst, u32 size)) { g_copy = copy; }
static void fail(const char* why) {
    eye_failures++;
    if (eye_failures <= 20 || (g_dbg > 0 && g_frame >= g_dbg_from))
        msr_receipt("eye: reuse stopped for a second (%s) frame %u: L %08X R %08X Ld %08X Rd %08X sizes %X/%X garbage %d/%d r_ok %d/%d", why, g_frame,
                    g_lc, g_rc, g_ld, g_rd, g_csz, g_dsz, g_r_garbage, g_r_garbage_d, g_r_ok_c, g_r_ok_d);
    g_off_frames = 30;
}
static int active(void) {
    static int preset;
    if (!preset) {   // MSR_EYE_PRESET=L,R (probe, replays): the eyes' color targets without a frame before
        preset = 1;
        const char* e = getenv("MSR_EYE_PRESET");
        if (e) {   // L,R[,Ld,Rd]
            u32 v[4] = {0};
            char* p = (char*)e;
            for (int k = 0; k < 4 && *p; k++) { v[k] = (u32)strtoul(p, &p, 16); if (*p == ',') p++; }
            g_lc = v[0]; g_rc = v[1]; g_ld = v[2]; g_rd = v[3];
            g_csz = g_dsz = 240 * 400 * 4;   // the top screen's 32-bit targets
        }
    }
    return g_on && !g_off_frames && g_lc && g_rc && g_lc != g_rc && g_csz && g_l_reg_c;
}
static u64 l_tok(int depth) { return depth ? src_tok(g_ld, g_ld + g_dsz) : src_tok(g_lc, g_lc + g_csz); }
static u64 r_tok(int depth) { return depth ? src_tok(g_rd, g_rd + g_dsz) : src_tok(g_rc, g_rc + g_csz); }

// The right eye's own image made to hold its token: a copy of the left eye's, which must hold it.
static void r_make_real(int depth) {
    if (depth ? g_r_ok_d : g_r_ok_c) return;
    if (depth && !g_rd) return;
    u64 want = r_tok(depth), have = depth ? g_phys_d : g_phys_c;
    if (trf()) msr_receipt("eyetrf: make real %s want %016llX have %016llX", depth ? "depth" : "color", (unsigned long long)want, (unsigned long long)have);
    if (is_dead(want)) { if (depth) g_r_ok_d = 1; else g_r_ok_c = 1; return; }   // garbage either way (garbage flags stay)
    if (want != have || !g_copy) { fail(depth ? "right eye depth read, left eye moved on" : "right eye read, left eye moved on"); return; }
    u32 l = depth ? g_ld : g_lc, r = depth ? g_rd : g_rc, n = depth ? g_dsz : g_csz;
    g_copy(l, r, n);   // the whole image: whatever was there before (garbage) is gone
    eye_materialized++;
    if (depth) { g_r_ok_d = 1; g_r_garbage_d = 0; } else { g_r_ok_c = 1; g_r_garbage = 0; }
}
static void l_check(int depth) {   // the left eye's images must hold the left eye's token when read
    if (is_dead(l_tok(depth))) return;
    if (depth ? (g_ld && g_phys_d != l_tok(1)) : (g_phys_c != l_tok(0))) fail("left eye read while it held the right eye's picture");
}

// Before something reads [lo, hi) (a GX operation, a texture), and before a GX operation writes
// part of an eye.
void eye_before_read(u32 lo, u32 hi) {
    if (!active()) return;
    if (trf()) msr_receipt("eyetrf: read %08X-%08X", lo, hi);
    if (overlaps(lo, hi, g_rc, g_csz)) r_make_real(0);
    if (g_rd && overlaps(lo, hi, g_rd, g_dsz)) r_make_real(1);
    if (overlaps(lo, hi, g_lc, g_csz)) l_check(0);
    if (g_ld && overlaps(lo, hi, g_ld, g_dsz)) l_check(1);
}
// After a GX operation wrote [lo, hi) with content token t (0: unknown, a fresh one).
void eye_written(u32 lo, u32 hi, u64 t) {
    if (!t) t = fresh();
    if (trf()) msr_receipt("eyetrf: written %08X-%08X %016llX", lo, hi, (unsigned long long)t);
    if (active()) {
        // a write over part of an eye keeps the rest: the rest has to be real first
        if (overlaps(lo, hi, g_rc, g_csz) && !covers(lo, hi, g_rc, g_csz)) r_make_real(0);
        if (g_rd && overlaps(lo, hi, g_rd, g_dsz) && !covers(lo, hi, g_rd, g_dsz)) r_make_real(1);
        if (overlaps(lo, hi, g_lc, g_csz) && !covers(lo, hi, g_lc, g_csz)) l_check(0);
        if (g_ld && overlaps(lo, hi, g_ld, g_dsz) && !covers(lo, hi, g_ld, g_dsz)) l_check(1);
    }
    tok_set(lo, hi, t);
    // what was written is physically there
    if (overlaps(lo, hi, g_rc, g_csz)) { g_r_ok_c = 1; if (covers(lo, hi, g_rc, g_csz)) g_r_garbage = 0; }
    if (g_rd && overlaps(lo, hi, g_rd, g_dsz)) { g_r_ok_d = 1; if (covers(lo, hi, g_rd, g_dsz)) g_r_garbage_d = 0; }
    if (overlaps(lo, hi, g_lc, g_csz)) g_phys_c = l_tok(0);
    if (g_ld && overlaps(lo, hi, g_ld, g_dsz)) g_phys_d = l_tok(1);
}
u64 eye_token_of(u32 lo, u32 hi) { return src_tok(lo, hi); }

// A display transfer: the top screen's framebuffers tell which targets are the eyes.
void eye_note_transfer(u32 src, u32 dst, u32 src_bytes) {
    for (u32 k = 0; k < g_nleft; k++) if (g_fb_left[k] == dst) { g_next_lc = src; g_next_csz = src_bytes; }
    for (u32 k = 0; k < g_nright; k++) if (g_fb_right[k] == dst) g_next_rc = src;
}
void eye_note_framebuffers(u32 left_va, u32 right_va) {
    int k;
    if (left_va) { for (k = 0; k < (int)g_nleft && g_fb_left[k] != left_va; k++); if (k == (int)g_nleft && g_nleft < 4) g_fb_left[g_nleft++] = left_va; }
    if (right_va) { for (k = 0; k < (int)g_nright && g_fb_right[k] != right_va; k++); if (k == (int)g_nright && g_nright < 4) g_fb_right[g_nright++] = right_va; }
}

int eye_draw_kind;   // set by pica_cmd.c before each draw: 0 vertex arrays, 1 indexed, 2 immediate vertices

static u64 bytes_hash(u64 h, const u8* p, u32 n) {
    u32 k = 0;
    for (; k + 8 <= n; k += 8) { u64 v; memcpy(&v, p + k, 8); h = (h ^ v) * 0x100000001B3ull; h ^= h >> 29; }
    for (; k < n; k++) h = (h ^ p[k]) * 0x100000001B3ull;
    return h;
}

// The draw's vertices: for a small draw (the full-screen passes, the same quad written twice for
// the two eyes) their bytes; for a larger one where they are (the eyes' draws of an object share
// its buffers).
static u64 vertex_hash(u64 h) {
    const u32* r = g_pica.reg;
    u32 n = r[0x228], first = r[0x22A];
    if (eye_draw_kind == 2) return mix(h, fresh());   // immediate vertices: never matched
    if (n > 64) {
        for (u32 i = 0x200; i <= 0x227; i++) h = mix(h, r[i]);
        return h;
    }
    for (u32 i = 0x200; i <= 0x227; i++) {   // the layout, without where the buffers are
        if (i == 0x200 || i == 0x227) continue;
        if (i >= 0x203 && (i - 0x203) % 3 == 0 && i < 0x203 + 36) continue;
        h = mix(h, r[i]);
    }
    u32 lo = first, hi = first + (n ? n - 1 : 0);
    if (eye_draw_kind == 1) {
        u32 ib = r[0x227];
        const u8* idx = pica_ptr((r[0x200] << 3) + (ib & 0x0FFFFFFF));
        if (!idx) return mix(h, fresh());
        int wide = (ib >> 31) & 1;
        h = bytes_hash(h, idx, n * (wide ? 2 : 1));
        lo = 0xFFFFFFFF; hi = 0;
        for (u32 i = 0; i < n; i++) {
            u32 v;
            if (wide) { u16 s16; memcpy(&s16, idx + 2 * i, 2); v = s16; } else v = idx[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        if (lo > hi) return h;
    }
    PicaAttrSrc in[16];
    PicaLoaderCopy cp[16];
    u32 ncp = 0, total = 0;
    pica_vertex_layout(lo, hi - lo + 1, in, cp, &ncp, &total);
    for (u32 k = 0; k < ncp; k++) {
        h = bytes_hash(mix(h, cp[k].bytes), g_mem + cp[k].src, cp[k].bytes);   // src: a guest virtual address
    }
    for (u32 k = 0; k < 16; k++) { h = mix(h, in[k].mode); h = mix(h, in[k].stride); h = mix(h, in[k].type | in[k].comps << 8); }
    return h;
}

static u64 draw_hash(const u32* tex, unsigned ntex) {
    const u32* r = g_pica.reg;
    u64 h = 0xCBF29CE484222325ull;
    h = vertex_hash(h);
    for (u32 i = 0; i < PICA_REGS; i++) {
        if (i == 0x11C || i == 0x11D || i == 0x85 || i == 0x95 || i == 0x9D) continue;   // targets, textures
        if (i >= 0x200 && i <= 0x227) continue;   // vertex buffers (above)
        // index and data ports (what they fill is hashed below), command list jumps
        if (i == 0x1C5 || (i >= 0x1C8 && i <= 0x1CF)) continue;   // lighting tables (their version)
        if (i >= 0x232 && i <= 0x235) continue;                    // fixed attributes (their values)
        if (i >= 0x238 && i <= 0x23D) continue;
        if (i >= 0x290 && i <= 0x2AD) continue;                    // geometry shader uploads
        if (i >= 0x2C0 && i <= 0x2C8) continue;                    // vertex shader uniforms (their values)
        if (i >= 0x2CB && i <= 0x2D5) continue;                    // vertex shader program (its version)
        h = mix(h, r[i]);
    }
    const u32* f = (const u32*)g_pica.vs.f;
    for (u32 i = 0; i < 96 * 4; i++) h = mix(h, f[i]);
    const u32* fa = (const u32*)g_pica.fixed_attr;
    for (u32 i = 0; i < 16 * 4; i++) h = mix(h, fa[i]);
    // the program and the lighting tables by content: a command list may load the same ones again
    static unsigned pv = ~0u, lv = ~0u;
    static u64 ph, lh;
    if (pv != pica_program_version) {
        pv = pica_program_version;
        ph = bytes_hash(bytes_hash(0x50, (const u8*)g_pica.vs.program, sizeof g_pica.vs.program), (const u8*)g_pica.vs.swizzle, sizeof g_pica.vs.swizzle);
    }
    if (lv != pica_lut_version) { lv = pica_lut_version; lh = bytes_hash(0x17, (const u8*)pica_lut, sizeof pica_lut); }
    h = mix(h, ph);
    h = mix(h, lh);
    for (unsigned k = 0; k < ntex; k++) h = mix(h, src_tok(tex[2 * k], tex[2 * k + 1]));
    return h;
}

// A draw. run: the frame plan lets it run. c, d: color and depth target addresses; cw, dw: it
// writes them. Returns 0 when it is skipped; a right eye draw may be sent to the left eye's images
// (eye_draw_done() puts the target registers back).
int eye_draw(int run, u32 c, u32 d, u32 csz, u32 dsz, int cw, int dw, int dr, const u32* tex, unsigned ntex) {
    (void)dr;
    u32* r = g_pica.reg;
    // a depth buffer that is neither read nor written does not matter (the right eye's passes after
    // the glow name the left eye's depth buffer, unused)
    if (d && !dw && !(r[0x114] & 3)) d = 0;
    // the eyes' depth buffers: those of this frame's full-size draws on them (the bottom screen is
    // drawn into the left eye's target later, smaller, with a depth buffer of its own size)
    if (c && c == g_lc && d && csz == g_csz) { g_next_ld = d; g_next_dsz = dsz; }
    if (c && c == g_rc && d && csz == g_csz) g_next_rd = d;
    if (c == g_lc) { g_l_reg_c = r[0x11D]; g_l_reg_d = r[0x11C]; if (!g_csz) g_csz = csz; if (!g_dsz && csz == g_csz) g_dsz = dsz; }
    int is_l = c == g_lc, is_r = c == g_rc;
    if (is_l || is_r) { eye_total[is_r]++; eye_runs[is_r] += run != 0; }
    if (trf()) msr_receipt("eyetrf: draw c %08X d %08X csz %X run %d cw %d dw %d is %d%d r_ok %d%d garb %d%d tokR %016llX", c, d, csz, run, cw, dw, is_l, is_r, g_r_ok_c, g_r_ok_d, g_r_garbage, g_r_garbage_d, (unsigned long long)r_tok(0));
    if (!active() || !(is_l || is_r) || (d && d != (is_l ? g_ld : g_rd))) {
        if (active()) for (unsigned k = 0; k < ntex; k++) eye_before_read(tex[2 * k], tex[2 * k + 1]);
        if (run) {
            if (cw) eye_written(c, c + csz, fresh());
            if (dw && d) eye_written(d, d + dsz, fresh());
        } else {
            if (cw) tok_set(c, c + csz, dead());
            if (dw && d) tok_set(d, d + dsz, dead());
        }
        return run;
    }
    csz = g_csz;   // an eye's tokens are for its whole target, whatever size a draw uses (the bottom
    dsz = g_dsz;   // screen is drawn into the left eye's target too, at 240x320)
    if (!run) {   // dead output: nothing will read it
        if (cw) tok_set(c, c + csz, dead());
        if (dw && d) tok_set(d, d + dsz, dead());
        if (is_r) { if (cw) { g_r_ok_c = 0; g_r_garbage = 1; } if (dw && d) { g_r_ok_d = 0; g_r_garbage_d = 1; } }
        else { if (cw) g_phys_c = fresh(); if (dw) g_phys_d = fresh(); }   // nobody can rely on its content
        return 0;
    }
    for (unsigned k = 0; k < ntex; k++) eye_before_read(tex[2 * k], tex[2 * k + 1]);
    u64 h = draw_hash(tex, ntex);
    if (g_dbg != 0) {
        memcpy(g_dbg_regs[is_r], r, sizeof g_dbg_regs[0]); memcpy(g_dbg_f[is_r], g_pica.vs.f, sizeof g_dbg_f[0]);
        for (unsigned k = 0; k < 3; k++) g_dbg_tex[is_r][k] = k < ntex ? src_tok(tex[2 * k], tex[2 * k + 1]) : 0;
        g_dbg_vh[is_r] = vertex_hash(0);
    }
    u64 pre_c = is_l ? l_tok(0) : r_tok(0), pre_d = d ? (is_l ? l_tok(1) : r_tok(1)) : 0;
    // what the result depends on: the color target's content only when the draw reads it (blending,
    // or not a full-screen pass), the depth target's when depth is read
    int cm = dr, dm = d && ((r[0x114] & 3) || (dw && dr));
    u64 post_c = cw ? mix(cm ? pre_c : 0, h) : pre_c, post_d = (dw && d) ? mix(dm ? pre_d : 0, h ^ 0xD) : pre_d;
    { static int tr = -1; if (tr < 0) { const char* e = getenv("MSR_EYETRACE"); tr = e ? atoi(e) : 0; }
      if (tr > 0 || trf()) { tr--; msr_receipt("eyetrace: %c c %08X d %08X w%d%d m%d%d pre %016llX %016llX phys %016llX %016llX hash %016llX", is_l ? 'L' : 'R', c, d, cw, dw, cm, dm,
                    (unsigned long long)pre_c, (unsigned long long)pre_d, (unsigned long long)g_phys_c, (unsigned long long)g_phys_d, (unsigned long long)h); } }
    int color_ok_post = cw ? g_phys_c == post_c : (!cm || g_phys_c == pre_c);
    int depth_ok_post = (dw && d) ? g_phys_d == post_d : (!dm || g_phys_d == pre_d);
    int color_ok_pre = !cm || g_phys_c == pre_c, depth_ok_pre = !dm || g_phys_d == pre_d;
    if (color_ok_post && depth_ok_post) {
        // the other eye already drew this into the shared images: nothing to do
        if (cw) tok_set(c, c + csz, post_c);
        if (dw && d) tok_set(d, d + dsz, post_d);
        if (is_r) { if (cw) g_r_ok_c = 0; if (dw && d) g_r_ok_d = 0; }
        eye_skipped++;
        return 0;
    }
    if (color_ok_pre && depth_ok_pre) {
        // the shared images hold what this draw needs of this eye's picture: draw there
        if (cw) tok_set(c, c + csz, post_c);
        if (dw && d) tok_set(d, d + dsz, post_d);
        if (cw) g_phys_c = post_c;
        if (dw && d) g_phys_d = post_d;
        if (is_r) {
            if (cw) g_r_ok_c = 0;
            if (dw && d) g_r_ok_d = 0;
            g_saved_c = r[0x11D]; g_saved_d = r[0x11C];
            r[0x11D] = g_l_reg_c;
            if (d) r[0x11C] = g_l_reg_d;
            g_redirected = 1;
            eye_drawn_right++;
        }
        return 1;
    }
    // the eyes differ for real
    if (g_dbg < 0) { const char* e = getenv("MSR_EYEDEBUG"); g_dbg = e ? atoi(e) : 0; e = getenv("MSR_EYEFROM"); g_dbg_from = e ? atoi(e) : 0; }
    if (g_dbg > 0 && g_frame >= g_dbg_from) {
        g_dbg--;
        char regs[400] = "";
        int n = 0, nf = 0;
        for (u32 i = 0; i < PICA_REGS && n < 300; i++)
            if (g_dbg_regs[0][i] != g_dbg_regs[1][i]) n += snprintf(regs + n, sizeof regs - n, " %03X:%X/%X", i, g_dbg_regs[0][i], g_dbg_regs[1][i]);
        for (int i = 0; i < 96; i++)
            if (memcmp(&g_dbg_f[0][i], &g_dbg_f[1][i], sizeof(Vec4))) {
                nf++;
                if (nf <= 4 && n < 300) n += snprintf(regs + n, sizeof regs - n, " c%d:%g,%g,%g,%g/%g,%g,%g,%g", i, g_dbg_f[0][i].x, g_dbg_f[0][i].y, g_dbg_f[0][i].z, g_dbg_f[0][i].w,
                                           g_dbg_f[1][i].x, g_dbg_f[1][i].y, g_dbg_f[1][i].z, g_dbg_f[1][i].w);
            }
        msr_receipt("eye: textures L %llX %llX %llX R %llX %llX %llX, vertices L %llX R %llX",
                    (unsigned long long)g_dbg_tex[0][0], (unsigned long long)g_dbg_tex[0][1], (unsigned long long)g_dbg_tex[0][2],
                    (unsigned long long)g_dbg_tex[1][0], (unsigned long long)g_dbg_tex[1][1], (unsigned long long)g_dbg_tex[1][2],
                    (unsigned long long)g_dbg_vh[0], (unsigned long long)g_dbg_vh[1]);
        msr_receipt("eye: %c draw fits neither: color %s/%s, depth %s/%s; last L vs R: %d uniforms, regs%s", is_l ? 'L' : 'R', color_ok_pre ? "pre" : "-",
                    color_ok_post ? "post" : "-", depth_ok_pre ? "pre" : "-", depth_ok_post ? "post" : "-", nf, regs);
    }
    if (is_r && g_r_ok_c && !g_r_garbage && (!d || (g_r_ok_d && !g_r_garbage_d))) {
        // the right eye's own images are current: draw it there, as without reuse
        if (cw) tok_set(c, c + csz, post_c);
        if (dw && d) tok_set(d, d + dsz, post_d);
        return 1;
    }
    fail(is_l ? "left eye drawn differently from the right" : "right eye drawn differently from the left");
    if (cw) tok_set(c, c + csz, post_c);
    if (dw && d) tok_set(d, d + dsz, post_d);
    if (is_l) { if (cw) g_phys_c = post_c; if (dw && d) g_phys_d = post_d; }
    return 1;
}
void eye_draw_done(void) {
    if (!g_redirected) return;
    g_pica.reg[0x11D] = g_saved_c;
    g_pica.reg[0x11C] = g_saved_d;
    g_redirected = 0;
}

// The frame is over: the eyes found in it are next frame's; tokens start over.
void eye_frame_end(void) {
    if (trf()) msr_receipt("eyetrf: frame end");
    if (!g_r_ok_c) g_r_garbage = 1;   // the right eye's images are behind: not for reading
    if (!g_r_ok_d) g_r_garbage_d = 1;
    g_r_ok_c = g_r_ok_d = 1;
    int same = g_next_lc == g_lc && g_next_rc == g_rc;
    g_lc = g_next_lc; g_rc = g_next_rc;
    if (same) { g_ld = g_next_ld; g_rd = g_next_rd; g_dsz = g_next_dsz; }
    else g_ld = g_rd = g_dsz = 0;
    g_csz = g_next_csz;
    g_next_lc = g_next_rc = g_next_ld = g_next_rd = 0;
    if (g_rd == g_ld) g_rd = g_ld = 0;
    g_ntok = 0;
    g_phys_c = l_tok(0);
    g_phys_d = g_ld ? l_tok(1) : 0;
    if (g_off_frames) g_off_frames--;
    g_frame++;
    if (g_dbg < 0) { const char* e = getenv("MSR_EYEDEBUG"); g_dbg = e ? atoi(e) : 0; e = getenv("MSR_EYEFROM"); g_dbg_from = e ? atoi(e) : 0; }
    { static int tr = -1; if (tr < 0) { const char* e = getenv("MSR_EYEFRAMES"); tr = e ? atoi(e) : 0; }
      if (tr > 0 && g_frame >= g_dbg_from) { tr--; msr_receipt("eye: frame %u eyes L %08X R %08X Ld %08X Rd %08X sizes %X/%X, %u skipped so far, %u failures",
                      g_frame, g_lc, g_rc, g_ld, g_rd, g_csz, g_dsz, eye_skipped, eye_failures); } }
}
