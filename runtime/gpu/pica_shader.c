// MSR-nx: PICA200 shader interpreter (vertex shader; the geometry shader comes when a draw needs it).
//
// Encodings from 3dbrew "GPU/Shader Instruction Set". PICA float rules kept here: 0 times anything,
// infinity included, is 0.
#include <math.h>
#include <string.h>
#include "msr_log.h"
#include "pica.h"

static float fmul(float a, float b) {
    float r = a * b;
    if (r != r && a == a && b == b) return 0.0f;   // 0 * inf
    return r;
}

typedef struct { u32 final, ret, repeat, inc, loop; int is_loop; } Frame;

typedef struct {
    const ShaderUnit* u;
    const u32* rb;            // the unit's register block (0x2B0 or 0x280)
    const Vec4* in;
    Vec4* out;
    Vec4 r[16];
    s32 a0x, a0y, aL;
    int cond[2];
} Ctx;

static Vec4 src(Ctx* x, u32 reg, int rel, u32 sel, int neg) {
    Vec4 v;
    if (reg < 0x10) v = x->in[reg];
    else if (reg < 0x20) v = x->r[reg - 0x10];
    else {
        s32 i = (s32)reg - 0x20;
        if (rel == 1) i += x->a0x;
        else if (rel == 2) i += x->a0y;
        else if (rel == 3) i += x->aL;
        i &= 0x7F;
        v = i < 96 ? x->u->f[i] : (Vec4){1, 1, 1, 1};
    }
    float c[4] = {v.x, v.y, v.z, v.w}, o[4];
    for (int k = 0; k < 4; k++) o[k] = c[(sel >> (2 * (3 - k))) & 3];
    if (neg) for (int k = 0; k < 4; k++) o[k] = -o[k];
    return (Vec4){o[0], o[1], o[2], o[3]};
}

int pica_shader_trace;
static void dst(Ctx* x, u32 reg, u32 mask, Vec4 v) {
    if (pica_shader_trace) msr_log("  -> %c%u mask %X = %g %g %g %g", reg < 0x10 ? 'o' : 'r', reg & 15, mask, v.x, v.y, v.z, v.w);
    Vec4* d = reg < 0x10 ? &x->out[reg] : reg < 0x20 ? &x->r[reg - 0x10] : 0;
    if (!d) return;
    if (mask & 8) d->x = v.x;
    if (mask & 4) d->y = v.y;
    if (mask & 2) d->z = v.z;
    if (mask & 1) d->w = v.w;
}

static Vec4 splat(float f) { return (Vec4){f, f, f, f}; }

static int cmp(float a, float b, u32 op) {
    switch (op) {
    case 0: return a == b;
    case 1: return a != b;
    case 2: return a < b;
    case 3: return a <= b;
    case 4: return a > b;
    case 5: return a >= b;
    }
    return 1;
}

static int flow_cond(Ctx* x, u32 w) {
    int refx = (w >> 25) & 1, refy = (w >> 24) & 1;
    int rx = x->cond[0] == refx, ry = x->cond[1] == refy;
    switch ((w >> 22) & 3) {
    case 0: return rx || ry;
    case 1: return rx && ry;
    case 2: return rx;
    default: return ry;
    }
}

void pica_shader_run(ShaderUnit* u, const u32* rb, const Vec4* in, Vec4* out) {
    Ctx x;
    memset(&x, 0, sizeof x);
    x.u = u; x.rb = rb; x.in = in; x.out = out;
    for (int k = 0; k < 16; k++) out[k] = (Vec4){0, 0, 0, 1};
    Frame st[16];
    int sp = 0;
    u32 bools = rb[0] & 0xFFFF;
    u32 pc = rb[0xA] & 0xFFFF;
    for (unsigned steps = 0; steps < 200000; steps++) {
        while (sp > 0 && pc == st[sp - 1].final) {
            Frame* f = &st[sp - 1];
            if (f->is_loop && f->repeat) { f->repeat--; x.aL += (s32)(s8)f->inc; pc = f->loop; break; }
            pc = f->ret;
            sp--;
        }
        if (pc >= 4096) return;
        u32 w = u->program[pc];
        u32 op = w >> 26;
        if (pica_shader_trace) msr_log("  %03X: %08X op %02X desc %08X a0 %d %d aL %d", pc, w, op, u->swizzle[op >= 0x30 ? w & 0x1F : w & 0x7F], x.a0x, x.a0y, x.aL);
        if (op >= 0x30) {   // MAD / MADI
            int madi = op < 0x38;
            u32 desc = u->swizzle[w & 0x1F];
            u32 d = (w >> 24) & 0x1F, rel = (w >> 22) & 3, s1 = (w >> 17) & 0x1F;
            u32 s2 = madi ? (w >> 12) & 0x1F : (w >> 10) & 0x7F;
            u32 s3 = madi ? (w >> 5) & 0x7F : (w >> 5) & 0x1F;
            Vec4 a = src(&x, s1, 0, (desc >> 5) & 0xFF, (desc >> 4) & 1);
            Vec4 b = src(&x, s2, madi ? 0 : rel, (desc >> 14) & 0xFF, (desc >> 13) & 1);
            Vec4 c = src(&x, s3, madi ? rel : 0, (desc >> 23) & 0xFF, (desc >> 22) & 1);
            Vec4 r = {fmul(a.x, b.x) + c.x, fmul(a.y, b.y) + c.y, fmul(a.z, b.z) + c.z, fmul(a.w, b.w) + c.w};
            dst(&x, d, desc & 0xF, r);
            pc++;
            continue;
        }
        u32 desc = u->swizzle[w & 0x7F];
        u32 mask = desc & 0xF;
        u32 d = (w >> 21) & 0x1F, rel = (w >> 19) & 3;
        int inv = op >= 0x18 && op <= 0x1B;
        u32 s1 = inv ? (w >> 14) & 0x1F : (w >> 12) & 0x7F;
        u32 s2 = inv ? (w >> 7) & 0x7F : (w >> 7) & 0x1F;
        #define A src(&x, s1, inv ? 0 : rel, (desc >> 5) & 0xFF, (desc >> 4) & 1)
        #define B src(&x, s2, inv ? rel : 0, (desc >> 14) & 0xFF, (desc >> 13) & 1)
        u32 D = (w >> 10) & 0xFFF, N = w & 0xFF;
        switch (op) {
        case 0x00: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}); break; }
        case 0x01: { Vec4 a = A, b = B; dst(&x, d, mask, splat(fmul(a.x, b.x) + fmul(a.y, b.y) + fmul(a.z, b.z))); break; }
        case 0x02: { Vec4 a = A, b = B; dst(&x, d, mask, splat(fmul(a.x, b.x) + fmul(a.y, b.y) + fmul(a.z, b.z) + fmul(a.w, b.w))); break; }
        case 0x03: case 0x18: { Vec4 a = A, b = B; dst(&x, d, mask, splat(fmul(a.x, b.x) + fmul(a.y, b.y) + fmul(a.z, b.z) + b.w)); break; }
        case 0x04: case 0x19: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){1.0f, fmul(a.y, b.y), a.z, b.w}); break; }
        case 0x05: { Vec4 a = A; dst(&x, d, mask, splat(exp2f(a.x))); break; }
        case 0x06: { Vec4 a = A; dst(&x, d, mask, splat(log2f(a.x))); break; }
        case 0x07: {  // LITP
            Vec4 a = A;
            Vec4 r = {a.x > 0 ? a.x : 0, a.y > 0 ? a.y : 0, a.z < -127.9961f ? -127.9961f : a.z > 127.9961f ? 127.9961f : a.z, a.w > 0 ? a.w : 0};
            x.cond[0] = a.x > 0; x.cond[1] = a.w > 0;
            dst(&x, d, mask, r);
            break;
        }
        case 0x08: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){fmul(a.x, b.x), fmul(a.y, b.y), fmul(a.z, b.z), fmul(a.w, b.w)}); break; }
        case 0x09: case 0x1A: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){a.x >= b.x, a.y >= b.y, a.z >= b.z, a.w >= b.w}); break; }
        case 0x0A: case 0x1B: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){a.x < b.x, a.y < b.y, a.z < b.z, a.w < b.w}); break; }
        case 0x0B: { Vec4 a = A; dst(&x, d, mask, (Vec4){floorf(a.x), floorf(a.y), floorf(a.z), floorf(a.w)}); break; }
        case 0x0C: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w}); break; }
        case 0x0D: { Vec4 a = A, b = B; dst(&x, d, mask, (Vec4){a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w}); break; }
        case 0x0E: { Vec4 a = A; dst(&x, d, mask, splat(1.0f / a.x)); break; }
        case 0x0F: { Vec4 a = A; dst(&x, d, mask, splat(1.0f / sqrtf(a.x))); break; }
        case 0x12: { Vec4 a = A; if (mask & 8) x.a0x = (s32)a.x; if (mask & 4) x.a0y = (s32)a.y; break; }
        case 0x13: dst(&x, d, mask, A); break;
        case 0x20:  // BREAK
        case 0x23:  // BREAKC
            if (op == 0x20 || flow_cond(&x, w)) {
                while (sp > 0 && !st[sp - 1].is_loop) sp--;
                if (sp > 0) { pc = st[sp - 1].ret; sp--; continue; }
            }
            break;
        case 0x21: break;
        case 0x22: return;   // END
        case 0x24: case 0x25: case 0x26: {  // CALL / CALLC / CALLU
            int take = op == 0x24 || (op == 0x25 && flow_cond(&x, w)) || (op == 0x26 && ((bools >> ((w >> 22) & 0xF)) & 1));
            if (take && sp < 16) { st[sp++] = (Frame){D + N, pc + 1, 0, 0, 0, 0}; pc = D; continue; }
            break;
        }
        case 0x27: case 0x28: {  // IFU / IFC
            int t = op == 0x28 ? flow_cond(&x, w) : (bools >> ((w >> 22) & 0xF)) & 1;
            if (sp >= 16) break;
            if (t) { st[sp++] = (Frame){D, D + N, 0, 0, 0, 0}; pc++; }
            else { st[sp++] = (Frame){D + N, D + N, 0, 0, 0, 0}; pc = D; }
            continue;
        }
        case 0x29: {  // LOOP
            u32 iu = rb[1 + ((w >> 22) & 3)];
            x.aL = (s32)((iu >> 8) & 0xFF);
            if (sp < 16) { st[sp++] = (Frame){D + 1, D + 1, iu & 0xFF, (iu >> 16) & 0xFF, pc + 1, 1}; }
            pc++;
            continue;
        }
        case 0x2A: case 0x2B: break;   // EMIT / SETEMIT: geometry shader only
        case 0x2C: if (flow_cond(&x, w)) { pc = D; continue; } break;   // JMPC
        case 0x2D: if (((bools >> ((w >> 22) & 0xF)) & 1) != (N & 1)) { pc = D; continue; } break;  // JMPU
        case 0x2E: case 0x2F: {  // CMP
            Vec4 a = src(&x, (w >> 12) & 0x7F, rel, (desc >> 5) & 0xFF, (desc >> 4) & 1);
            Vec4 b = src(&x, (w >> 7) & 0x1F, 0, (desc >> 14) & 0xFF, (desc >> 13) & 1);
            x.cond[0] = cmp(a.x, b.x, (w >> 24) & 7);
            x.cond[1] = cmp(a.y, b.y, (w >> 21) & 7);
            break;
        }
        default: {
            static int warned;
            if (warned++ < 8) msr_log("pica: shader opcode %02X at %03X not handled", op, pc);
            break;
        }
        }
        #undef A
        #undef B
        pc++;
    }
}

// GPUREG_SH_OUTMAP_O0..6: for each enabled output register, four semantic bytes.
void pica_map_outputs(const Vec4* out, PicaVertex* v) {
    const u32* r = g_pica.reg;
    memset(v, 0, sizeof *v);
    v->pos.w = 1;
    v->quat.w = 1;
    v->color = (Vec4){1, 1, 1, 1};
    u32 mask = r[0x2BD];
    u32 total = r[0x4F] & 7;
    int o = 0;
    for (u32 k = 0; k < 7 && k < total; k++) {
        while (o < 16 && !(mask & (1u << o))) o++;
        if (o >= 16) break;
        float c[4] = {out[o].x, out[o].y, out[o].z, out[o].w};
        u32 map = r[0x50 + k];
        for (int j = 0; j < 4; j++) {
            u32 s = (map >> (8 * j)) & 0x1F;
            float f = c[j];
            switch (s) {
            case 0x00: v->pos.x = f; break;
            case 0x01: v->pos.y = f; break;
            case 0x02: v->pos.z = f; break;
            case 0x03: v->pos.w = f; break;
            case 0x04: v->quat.x = f; break;
            case 0x05: v->quat.y = f; break;
            case 0x06: v->quat.z = f; break;
            case 0x07: v->quat.w = f; break;
            case 0x08: v->color.x = f; break;
            case 0x09: v->color.y = f; break;
            case 0x0A: v->color.z = f; break;
            case 0x0B: v->color.w = f; break;
            case 0x0C: v->uv0[0] = f; break;
            case 0x0D: v->uv0[1] = f; break;
            case 0x0E: v->uv1[0] = f; break;
            case 0x0F: v->uv1[1] = f; break;
            case 0x10: v->uv0w = f; break;
            case 0x12: v->view[0] = f; break;
            case 0x13: v->view[1] = f; break;
            case 0x14: v->view[2] = f; break;
            case 0x16: v->uv2[0] = f; break;
            case 0x17: v->uv2[1] = f; break;
            }
        }
        o++;
    }
}
