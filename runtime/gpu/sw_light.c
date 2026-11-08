// MSR-nx: PICA200 fragment lighting for the software reference renderer.
//
// Registers from 3dbrew "GPU/Internal Registers" (0x8F, 0x140-0x1D9) and the shading rules of
// fearkov/zakuro (MIT) zakuro-gpu lighting.rs, read as reference. The code here is ours.
#include <math.h>
#include <string.h>
#include "pica.h"

// 24 lookup tables of 256 entries: value and difference to the next entry.
float pica_lut[24][256][2];
unsigned pica_lut_version;
#define g_lut pica_lut

void pica_light_lut_write(u32 value) {
    u32* r = g_pica.reg;
    u32 idx = r[0x1C5];
    u32 entry = idx & 0xFF, table = (idx >> 8) & 0x1F;
    if (table < 24) {
        float step = ((value >> 12) & 0x7FF) / 2047.0f;
        if (value & (1u << 23)) step = -step;
        pica_lut_version++;
        g_lut[table][entry][0] = (value & 0xFFF) / 4095.0f;
        g_lut[table][entry][1] = step;
    }
    r[0x1C5] = (idx & ~0xFFu) | ((entry + 1) & 0xFF);
}

static float lut(int table, u32 entry, float delta) { return g_lut[table][entry & 0xFF][0] + g_lut[table][entry & 0xFF][1] * delta; }

static float pfloat(u32 bits, int man, int exp) {
    int width = man + exp + 1;
    u32 sign = (bits >> (man + exp)) << 31;
    if (!(bits & ((1u << (width - 1)) - 1))) { float f; memcpy(&f, &sign, 4); return f; }
    u32 biased = (bits >> man) & ((1u << exp) - 1);
    biased = biased == (1u << exp) - 1 ? 255 : biased + 128 - (1u << (exp - 1));
    u32 b = sign | ((bits & ((1u << man) - 1)) << (23 - man)) | (biased << 23);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static void color3(u32 v, float* o) { o[0] = ((v >> 20) & 0x3FF) / 255.f; o[1] = ((v >> 10) & 0x3FF) / 255.f; o[2] = (v & 0x3FF) / 255.f; }
static float fixed13(u32 v) { return (float)((s32)((v & 0x1FFF) << 19) >> 19) / 2047.0f; }
static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void norm3(float* v) { float l = sqrtf(dot3(v, v)); if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void cross3(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void rotate(const float* q, const float* v, float* o) {
    float t[3], u[3], w[3];
    cross3(q, v, t);
    for (int i = 0; i < 3; i++) u[i] = t[i] + v[i] * q[3];
    cross3(q, u, w);
    for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * w[i];
}

static int supported(u32 config, int table) {
    switch (table) {
    case 0: return config != 1;
    case 1: return !(config == 0 || config == 1 || config == 5);
    case 8: return !(config == 2 || config == 3);
    case 3: return !(config == 0 || config == 2 || config == 4);
    case 6: return config != 3;
    default: return config == 4 || config == 5 || config == 8;
    }
}

int pica_lighting_on(void) { return (g_pica.reg[0x8F] & 1) && !(g_pica.reg[0x1C6] & 1); }

// quat: interpolated normal quaternion, view: interpolated view vector, tex: the four texture
// colors (rgba). Writes primary and secondary fragment colors (rgba).
void pica_lighting(const float* quat_in, const float* view, const float (*tex)[4], float* prim, float* sec) {
    const u32* r = g_pica.reg;
    u32 c0 = r[0x1C3], c1 = r[0x1C4], config = (c0 >> 4) & 0xF;
    u32 absn = r[0x1D0], inputs = r[0x1D1], scales = r[0x1D2];
    float q[4] = {quat_in[0], quat_in[1], quat_in[2], quat_in[3]};
    float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (ql > 0) for (int i = 0; i < 4; i++) q[i] /= ql; else { q[0] = q[1] = q[2] = 0; q[3] = 1; }
    float shadow[4] = {1, 1, 1, 1};
    int sh_on = c0 & 1;
    if (sh_on) {
        const float* t = tex[(c0 >> 24) & 3];
        for (int i = 0; i < 4; i++) shadow[i] = (c0 >> 18) & 1 ? 1 - t[i] : t[i];
    }
    float sn[3] = {0, 0, 1}, st[3] = {1, 0, 0};
    u32 bump = (c0 >> 28) & 3;
    if (bump == 1) {
        const float* t = tex[(c0 >> 22) & 3];
        for (int i = 0; i < 3; i++) sn[i] = t[i] * 2 - 1;
        if (!((c0 >> 30) & 1)) { float z = 1 - (sn[0] * sn[0] + sn[1] * sn[1]); sn[2] = z > 0 ? sqrtf(z) : 0; }
    } else if (bump == 2) {
        const float* t = tex[(c0 >> 22) & 3];
        for (int i = 0; i < 3; i++) st[i] = t[i] * 2 - 1;
    }
    float n[3], tg[3];
    rotate(q, sn, n);
    if (config == 8) rotate(q, st, tg); else memcpy(tg, st, sizeof tg);
    float nv[3] = {view[0], view[1], view[2]};
    norm3(nv);
    float diff[4] = {0, 0, 0, 1}, spec[4] = {0, 0, 0, 1};
    u32 count = (r[0x1C2] & 7) + 1;
    for (u32 slot = 0; slot < count; slot++) {
        u32 num = (r[0x1D9] >> (4 * slot)) & 7;
        const u32* b = r + 0x140 + 0x10 * num;
        float s0[3], s1[3], dcol[3], acol[3], pos[3], spot[3];
        color3(b[0], s0); color3(b[1], s1); color3(b[2], dcol); color3(b[3], acol);
        pos[0] = pfloat(b[4] & 0xFFFF, 10, 5); pos[1] = pfloat(b[4] >> 16, 10, 5); pos[2] = pfloat(b[5] & 0xFFFF, 10, 5);
        spot[0] = fixed13(b[6]); spot[1] = fixed13(b[6] >> 16); spot[2] = fixed13(b[7]);
        int directional = b[9] & 1, two = (b[9] >> 1) & 1, g0 = (b[9] >> 2) & 1, g1 = (b[9] >> 3) & 1;
        float lv[3];
        if (directional) { memcpy(lv, pos, sizeof lv); norm3(lv); }
        else { for (int i = 0; i < 3; i++) lv[i] = pos[i] + view[i]; norm3(lv); }
        float half[3], hu[3];
        for (int i = 0; i < 3; i++) half[i] = nv[i] + lv[i];
        memcpy(hu, half, sizeof hu);
        norm3(hu);
        float dist = 1;
        if (!((c1 >> (24 + num)) & 1)) {
            float bias = pfloat(b[10] & 0xFFFFF, 12, 7), scale = pfloat(b[11] & 0xFFFFF, 12, 7);
            float off[3] = {-view[0] - pos[0], -view[1] - pos[1], -view[2] - pos[2]};
            float p = scale * sqrtf(dot3(off, off)) + bias;
            p = p < 0 ? 0 : p > 1 ? 1 : p;
            float e = floorf(p * 256); if (e > 255) e = 255;
            dist = lut(16 + num, (u32)e, p * 256 - e);
        }
        #define VALUE(field, table) ({ \
            u32 in_ = (inputs >> ((field) * 4)) & 7; int abs_ = !((absn >> ((field) * 4 + 1)) & 1); \
            u32 sc_ = (scales >> ((field) * 4)) & 7; \
            float scale_ = sc_ == 0 ? 1 : sc_ == 1 ? 2 : sc_ == 2 ? 4 : sc_ == 3 ? 8 : sc_ == 6 ? 0.25f : sc_ == 7 ? 0.5f : 0; \
            float res_ = 0; \
            switch (in_) { case 0: res_ = dot3(n, hu); break; case 1: res_ = dot3(nv, hu); break; \
                case 2: res_ = dot3(n, nv); break; case 3: res_ = dot3(lv, n); break; case 4: res_ = dot3(lv, spot); break; \
                case 5: if (config == 8) { float al_ = dot3(n, hu), pr_[3] = {hu[0] - n[0] * al_, hu[1] - n[1] * al_, hu[2] - n[2] * al_}; res_ = dot3(pr_, tg); } break; } \
            u32 ent_; float del_; \
            if (abs_) { res_ = two ? fabsf(res_) : (res_ > 0 ? res_ : 0); float e_ = floorf(res_ * 256); if (e_ > 255) e_ = 255; ent_ = (u32)e_; del_ = res_ * 256 - e_; } \
            else { float e_ = floorf(res_ * 128); if (e_ < -128) e_ = -128; if (e_ > 127) e_ = 127; ent_ = (u32)(u8)(s8)(int)e_; del_ = res_ * 128 - e_; } \
            scale_ * lut((table), ent_, del_); })
        float spotv = (!((c1 >> (8 + num)) & 1) && supported(config, 8)) ? VALUE(2, 8 + num) : 1;
        float d0 = (!((c1 >> 16) & 1) && supported(config, 0)) ? VALUE(0, 0) : 1;
        float d1 = (!((c1 >> 17) & 1) && supported(config, 1)) ? VALUE(1, 1) : 1;
        float rr = (!((c1 >> 20) & 1) && supported(config, 6)) ? VALUE(6, 6) : 1;
        float rg = (!((c1 >> 21) & 1) && supported(config, 5)) ? VALUE(5, 5) : rr;
        float rb = (!((c1 >> 22) & 1) && supported(config, 4)) ? VALUE(4, 4) : rr;
        float refl[3] = {rr, rg, rb};
        float sp0[3], sp1[3];
        for (int i = 0; i < 3; i++) { sp0[i] = s0[i] * d0; sp1[i] = d1 * refl[i] * s1[i]; }
        if (slot == count - 1 && !((c1 >> 19) & 1) && supported(config, 3)) {
            float fr = VALUE(3, 3);
            if ((c0 >> 2) & 1) diff[3] = fr;
            if ((c0 >> 3) & 1) spec[3] = fr;
        }
        #undef VALUE
        float facing = dot3(lv, n);
        facing = two ? fabsf(facing) : (facing > 0 ? facing : 0);
        float hl = ((c0 >> 27) & 1) && facing == 0 ? 0 : 1;
        if (g0 || g1) {
            float len = dot3(half, half);
            float f = len == 0 ? 0 : facing / len;
            if (f > 1) f = 1;
            if (g0) for (int i = 0; i < 3; i++) sp0[i] *= f;
            if (g1) for (int i = 0; i < 3; i++) sp1[i] *= f;
        }
        int shadowed = !((c1 >> num) & 1);
        for (int i = 0; i < 3; i++) {
            float dsh = sh_on && ((c0 >> 16) & 1) && shadowed ? shadow[i] : 1;
            float ssh = sh_on && ((c0 >> 17) & 1) && shadowed ? shadow[i] : 1;
            diff[i] += (dcol[i] * facing + acol[i]) * dist * spotv * dsh;
            spec[i] += (sp0[i] + sp1[i]) * hl * dist * spotv * ssh;
        }
    }
    if (sh_on && ((c0 >> 19) & 1)) {
        if ((c0 >> 2) & 1) diff[3] *= shadow[3];
        if ((c0 >> 3) & 1) spec[3] *= shadow[3];
    }
    float amb[3];
    color3(r[0x1C0], amb);
    for (int i = 0; i < 3; i++) diff[i] += amb[i];
    for (int i = 0; i < 4; i++) {
        prim[i] = diff[i] < 0 ? 0 : diff[i] > 1 ? 1 : diff[i];
        sec[i] = spec[i] < 0 ? 0 : spec[i] > 1 ? 1 : spec[i];
    }
}
