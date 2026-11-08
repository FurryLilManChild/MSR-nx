// MSR-nx probe: checks the translated vertex shaders (tools/shaders/pica2glsl.py, C++ form) against
// the reference interpreter on the game's own vertices. Built only into the checking probe.
#include <math.h>
#include <string.h>
#include "msr_log.h"
#include "pica.h"

typedef struct { Vec4 pos, quat, col, uv01, uv2w, view; } ChkOut;
extern Vec4 msr_chk_f[96];
extern u32 ints[4][4];
extern u32 bools;
int msr_chk_run(u64 key, const Vec4* in, ChkOut* res);

static unsigned long long g_checked, g_bad, g_missing, g_draws;
static double g_worst;

static double diff(Vec4 a, Vec4 b) {
    double d = 0, e;
    float x[4] = {a.x, a.y, a.z, a.w}, y[4] = {b.x, b.y, b.z, b.w};
    for (int k = 0; k < 4; k++) {
        if (isnan(x[k]) && isnan(y[k])) continue;
        e = fabs((double)x[k] - y[k]) / (1.0 + fabs((double)x[k]));
        if (isnan(e)) e = 1e9;
        if (e > d) d = e;
    }
    return d;
}

static void check_draw(void) {
    const u32* r = g_pica.reg;
    u32 n = r[0x228];
    if (!n) return;
    g_draws++;
    u64 key = pica_vs_key();
    memcpy(msr_chk_f, g_pica.vs.f, sizeof msr_chk_f);
    for (int k = 0; k < 4; k++) {
        u32 iu = r[0x2B1 + k];
        ints[k][0] = iu & 0xFF; ints[k][1] = (iu >> 8) & 0xFF; ints[k][2] = (iu >> 16) & 0xFF; ints[k][3] = 0;
    }
    bools = r[0x2B0] & 0xFFFF;
    u32 first = r[0x22A];
    u32 ib = r[0x227];
    const u8* idx = pica_ptr((r[0x200] << 3) + (ib & 0x0FFFFFFF));
    int indexed = idx != 0, wide = (ib >> 31) & 1;
    for (u32 i = 0; i < n && i < 6; i++) {
        Vec4 in[16], out[16];
        for (int k = 0; k < 16; k++) in[k] = (Vec4){0, 0, 0, 1};
        u32 vi = first + i;
        if (indexed) { if (wide) { u16 t; memcpy(&t, idx + 2 * i, 2); vi = t; } else vi = idx[i]; }
        pica_load_attributes(vi, in);
        pica_shader_run(&g_pica.vs, r + 0x2B0, in, out);
        PicaVertex v;
        pica_map_outputs(out, &v);
        ChkOut c;
        if (!msr_chk_run(key, in, &c)) { g_missing++; return; }
        g_checked++;
        Vec4 uv01 = {v.uv0[0], v.uv0[1], v.uv1[0], v.uv1[1]}, uv2w = {v.uv2[0], v.uv2[1], v.uv0w, 0};
        Vec4 view = {v.view[0], v.view[1], v.view[2], 0};
        double d = diff(v.pos, c.pos);
        double e;
        if ((e = diff(v.quat, c.quat)) > d) d = e;
        if ((e = diff(v.color, c.col)) > d) d = e;
        if ((e = diff(uv01, c.uv01)) > d) d = e;
        if ((e = diff(uv2w, c.uv2w)) > d) d = e;
        if ((e = diff(view, c.view)) > d) d = e;
        if (d > g_worst) g_worst = d;
        if (d > 1e-5) {
            if (g_bad++ < 10)
                msr_receipt("vs check: key %016llX vertex %u differs by %g: pos %g %g %g %g vs %g %g %g %g",
                            (unsigned long long)key, vi, d, v.pos.x, v.pos.y, v.pos.z, v.pos.w,
                            c.pos.x, c.pos.y, c.pos.z, c.pos.w);
        }
    }
}

void vs_check_summary(void) {
    msr_receipt("vs check: %llu draws, %llu vertices compared, %llu differ, %llu draws with no translation, worst %g",
                g_draws, g_checked, g_bad, g_missing, g_worst);
}

void vs_check_install(void) {
    extern void (*pica_vs_check_hook)(void);
    pica_vs_check_hook = check_draw;
}
