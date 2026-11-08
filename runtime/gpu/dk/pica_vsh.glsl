// MSR-nx: PICA200 vertex shader interpreter, run on the Switch GPU (deko3d, compiled with uam).
// Each vertex runs the game's PICA vertex program from the program buffer; the outputs are mapped
// to attributes with the game's GPUREG_SH_OUTMAP registers. Same rules as pica_shader.c.
#version 460

layout (std430, binding = 0) readonly buffer Program { uint code[4096]; uint swz[128]; };
layout (std430, binding = 1) readonly buffer Vertices { vec4 vin[]; };   // base.y inputs per vertex

layout (std140, binding = 0) uniform VertexState {
    vec4 f[96];          // float uniforms c0..c95
    uvec4 ints[4];       // i0..i3: x count, y start, z step
    uvec4 cfg;           // x bools, y entry point, z output mask, w output total
    uvec4 outmap[7];     // GPUREG_SH_OUTMAP_O0..6 (x)
    uvec4 base;          // x first vertex slot in Vertices, y vec4s per vertex
};

layout (location = 0) out vec4 v_color;
layout (location = 1) out vec4 v_uv01;
layout (location = 2) out vec4 v_uv2w;
layout (location = 3) out vec4 v_quat;
layout (location = 4) out vec4 v_view;

vec4 r[16];
vec4 o[16];
int a0x, a0y, aL;
bool cx, cy;

vec4 vmul(vec4 a, vec4 b) {
    vec4 p = a * b;
    bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
    return mix(p, vec4(0.0), z);   // PICA: 0 * anything is 0
}
float smul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }

vec4 src(uint reg, uint rel, uint sel, bool neg) {
    vec4 v;
    if (reg < 16u) v = reg < base.y ? vin[(base.x + uint(gl_VertexID)) * base.y + reg] : vec4(0.0, 0.0, 0.0, 1.0);
    else if (reg < 32u) v = r[reg - 16u];
    else {
        int i = int(reg) - 32;
        if (rel == 1u) i += a0x; else if (rel == 2u) i += a0y; else if (rel == 3u) i += aL;
        i &= 127;
        v = i < 96 ? f[i] : vec4(1.0);
    }
    vec4 s = vec4(v[(sel >> 6) & 3u], v[(sel >> 4) & 3u], v[(sel >> 2) & 3u], v[sel & 3u]);
    return neg ? -s : s;
}

void dst(uint reg, uint mask, vec4 v) {
    if (reg < 16u) {
        if ((mask & 8u) != 0u) o[reg].x = v.x;
        if ((mask & 4u) != 0u) o[reg].y = v.y;
        if ((mask & 2u) != 0u) o[reg].z = v.z;
        if ((mask & 1u) != 0u) o[reg].w = v.w;
    } else if (reg < 32u) {
        uint t = reg - 16u;
        if ((mask & 8u) != 0u) r[t].x = v.x;
        if ((mask & 4u) != 0u) r[t].y = v.y;
        if ((mask & 2u) != 0u) r[t].z = v.z;
        if ((mask & 1u) != 0u) r[t].w = v.w;
    }
}

bool cmpop(float a, float b, uint op) {
    if (op == 0u) return a == b;
    if (op == 1u) return a != b;
    if (op == 2u) return a < b;
    if (op == 3u) return a <= b;
    if (op == 4u) return a > b;
    if (op == 5u) return a >= b;
    return true;
}

bool flowcond(uint w) {
    bool rx = cx == (((w >> 25) & 1u) != 0u), ry = cy == (((w >> 24) & 1u) != 0u);
    uint op = (w >> 22) & 3u;
    if (op == 0u) return rx || ry;
    if (op == 1u) return rx && ry;
    if (op == 2u) return rx;
    return ry;
}

bool boolu(uint w) { return ((cfg.x >> ((w >> 22) & 15u)) & 1u) != 0u; }

void main() {
    for (int k = 0; k < 16; k++) { o[k] = vec4(0.0, 0.0, 0.0, 1.0); r[k] = vec4(0.0); }
    a0x = 0; a0y = 0; aL = 0; cx = false; cy = false;
    uint sfinal[8], sret[8], srep[8], sinc[8], sloop[8];
    bool sisloop[8];
    int sp = 0;
    uint pc = cfg.y & 0xFFFFu;
    for (int steps = 0; steps < 8192; steps++) {
        while (sp > 0 && pc == sfinal[sp - 1]) {
            if (sisloop[sp - 1] && srep[sp - 1] > 0u) {
                srep[sp - 1]--;
                aL += int(int(sinc[sp - 1] << 24) >> 24);
                pc = sloop[sp - 1];
                break;
            }
            pc = sret[sp - 1];
            sp--;
        }
        if (pc >= 4096u) break;
        uint w = code[pc];
        uint op = w >> 26;
        if (op >= 0x30u) {   // MAD, MADI
            bool madi = op < 0x38u;
            uint desc = swz[w & 0x1Fu];
            uint d = (w >> 24) & 0x1Fu, rel = (w >> 22) & 3u, s1 = (w >> 17) & 0x1Fu;
            uint s2 = madi ? (w >> 12) & 0x1Fu : (w >> 10) & 0x7Fu;
            uint s3 = madi ? (w >> 5) & 0x7Fu : (w >> 5) & 0x1Fu;
            vec4 a = src(s1, 0u, (desc >> 5) & 0xFFu, ((desc >> 4) & 1u) != 0u);
            vec4 b = src(s2, madi ? 0u : rel, (desc >> 14) & 0xFFu, ((desc >> 13) & 1u) != 0u);
            vec4 c = src(s3, madi ? rel : 0u, (desc >> 23) & 0xFFu, ((desc >> 22) & 1u) != 0u);
            dst(d, desc & 0xFu, vmul(a, b) + c);
            pc++;
            continue;
        }
        uint desc = swz[w & 0x7Fu];
        uint mask = desc & 0xFu;
        uint d = (w >> 21) & 0x1Fu, rel = (w >> 19) & 3u;
        bool inv = op >= 0x18u && op <= 0x1Bu;
        uint s1 = inv ? (w >> 14) & 0x1Fu : (w >> 12) & 0x7Fu;
        uint s2 = inv ? (w >> 7) & 0x7Fu : (w >> 7) & 0x1Fu;
        uint D = (w >> 10) & 0xFFFu, N = w & 0xFFu;
        uint sel1 = (desc >> 5) & 0xFFu, sel2 = (desc >> 14) & 0xFFu;
        bool n1 = ((desc >> 4) & 1u) != 0u, n2 = ((desc >> 13) & 1u) != 0u;
        uint r1 = inv ? 0u : rel, r2 = inv ? rel : 0u;
        if (op == 0x00u) { dst(d, mask, src(s1, r1, sel1, n1) + src(s2, r2, sel2, n2)); }
        else if (op == 0x01u) { vec4 a = src(s1, r1, sel1, n1), b = src(s2, r2, sel2, n2); dst(d, mask, vec4(smul(a.x, b.x) + smul(a.y, b.y) + smul(a.z, b.z))); }
        else if (op == 0x02u) { vec4 p = vmul(src(s1, r1, sel1, n1), src(s2, r2, sel2, n2)); dst(d, mask, vec4(p.x + p.y + p.z + p.w)); }
        else if (op == 0x03u || op == 0x18u) { vec4 a = src(s1, r1, sel1, n1), b = src(s2, r2, sel2, n2); dst(d, mask, vec4(smul(a.x, b.x) + smul(a.y, b.y) + smul(a.z, b.z) + b.w)); }
        else if (op == 0x04u || op == 0x19u) { vec4 a = src(s1, r1, sel1, n1), b = src(s2, r2, sel2, n2); dst(d, mask, vec4(1.0, smul(a.y, b.y), a.z, b.w)); }
        else if (op == 0x05u) { dst(d, mask, vec4(exp2(src(s1, r1, sel1, n1).x))); }
        else if (op == 0x06u) { dst(d, mask, vec4(log2(src(s1, r1, sel1, n1).x))); }
        else if (op == 0x07u) {
            vec4 a = src(s1, r1, sel1, n1);
            cx = a.x > 0.0; cy = a.w > 0.0;
            dst(d, mask, vec4(max(a.x, 0.0), max(a.y, 0.0), clamp(a.z, -127.9961, 127.9961), max(a.w, 0.0)));
        }
        else if (op == 0x08u) { dst(d, mask, vmul(src(s1, r1, sel1, n1), src(s2, r2, sel2, n2))); }
        else if (op == 0x09u || op == 0x1Au) { dst(d, mask, vec4(greaterThanEqual(src(s1, r1, sel1, n1), src(s2, r2, sel2, n2)))); }
        else if (op == 0x0Au || op == 0x1Bu) { dst(d, mask, vec4(lessThan(src(s1, r1, sel1, n1), src(s2, r2, sel2, n2)))); }
        else if (op == 0x0Bu) { dst(d, mask, floor(src(s1, r1, sel1, n1))); }
        else if (op == 0x0Cu) { vec4 a = src(s1, r1, sel1, n1), b = src(s2, r2, sel2, n2); dst(d, mask, mix(b, a, greaterThan(a, b))); }
        else if (op == 0x0Du) { vec4 a = src(s1, r1, sel1, n1), b = src(s2, r2, sel2, n2); dst(d, mask, mix(b, a, lessThan(a, b))); }
        else if (op == 0x0Eu) { dst(d, mask, vec4(1.0 / src(s1, r1, sel1, n1).x)); }
        else if (op == 0x0Fu) { dst(d, mask, vec4(inversesqrt(src(s1, r1, sel1, n1).x))); }
        else if (op == 0x12u) { vec4 a = src(s1, r1, sel1, n1); if ((mask & 8u) != 0u) a0x = int(a.x); if ((mask & 4u) != 0u) a0y = int(a.y); }
        else if (op == 0x13u) { dst(d, mask, src(s1, r1, sel1, n1)); }
        else if (op == 0x20u || op == 0x23u) {   // BREAK, BREAKC
            if (op == 0x20u || flowcond(w)) {
                while (sp > 0 && !sisloop[sp - 1]) sp--;
                if (sp > 0) { pc = sret[sp - 1]; sp--; continue; }
            }
        }
        else if (op == 0x22u) break;   // END
        else if (op == 0x24u || op == 0x25u || op == 0x26u) {   // CALL, CALLC, CALLU
            bool take = op == 0x24u || (op == 0x25u && flowcond(w)) || (op == 0x26u && boolu(w));
            if (take && sp < 8) { sfinal[sp] = D + N; sret[sp] = pc + 1u; srep[sp] = 0u; sinc[sp] = 0u; sloop[sp] = 0u; sisloop[sp] = false; sp++; pc = D; continue; }
        }
        else if (op == 0x27u || op == 0x28u) {   // IFU, IFC
            bool t = op == 0x28u ? flowcond(w) : boolu(w);
            if (sp < 8) {
                sret[sp] = D + N; srep[sp] = 0u; sinc[sp] = 0u; sloop[sp] = 0u; sisloop[sp] = false;
                if (t) { sfinal[sp] = D; sp++; pc++; } else { sfinal[sp] = D + N; sp++; pc = D; }
                continue;
            }
        }
        else if (op == 0x29u) {   // LOOP
            uvec4 iu = ints[(w >> 22) & 3u];
            aL = int(iu.y);
            if (sp < 8) { sfinal[sp] = D + 1u; sret[sp] = D + 1u; srep[sp] = iu.x; sinc[sp] = iu.z; sloop[sp] = pc + 1u; sisloop[sp] = true; sp++; }
            pc++;
            continue;
        }
        else if (op == 0x2Cu) { if (flowcond(w)) { pc = D; continue; } }   // JMPC
        else if (op == 0x2Du) { if (boolu(w) != ((N & 1u) != 0u)) { pc = D; continue; } }   // JMPU
        else if (op == 0x2Eu || op == 0x2Fu) {   // CMP
            vec4 a = src((w >> 12) & 0x7Fu, rel, sel1, n1), b = src((w >> 7) & 0x1Fu, 0u, sel2, n2);
            cx = cmpop(a.x, b.x, (w >> 24) & 7u);
            cy = cmpop(a.y, b.y, (w >> 21) & 7u);
        }
        pc++;
    }

    // outputs to attributes
    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0), quat = vec4(0.0, 0.0, 0.0, 1.0), col = vec4(1.0);
    vec4 uv01 = vec4(0.0), uv2w = vec4(0.0), view = vec4(0.0);
    int oi = 0;
    for (uint k = 0u; k < 7u && k < (cfg.w & 7u); k++) {
        while (oi < 16 && ((cfg.z >> oi) & 1u) == 0u) oi++;
        if (oi >= 16) break;
        uint map = outmap[k].x;
        for (uint j = 0u; j < 4u; j++) {
            uint s = (map >> (8u * j)) & 0x1Fu;
            float v = o[oi][j];
            if (s <= 3u) pos[s] = v;
            else if (s <= 7u) quat[s - 4u] = v;
            else if (s <= 11u) col[s - 8u] = v;
            else if (s <= 15u) uv01[s - 12u] = v;
            else if (s == 16u) uv2w.z = v;
            else if (s >= 18u && s <= 20u) view[s - 18u] = v;
            else if (s == 22u) uv2w.x = v;
            else if (s == 23u) uv2w.y = v;
        }
        oi++;
    }
    v_color = col;
    v_uv01 = uv01;
    v_uv2w = uv2w;
    v_quat = quat;
    v_view = view;
    // PICA clip space keeps -w <= z <= 0; the Switch takes 0 <= z <= w.
    gl_Position = vec4(pos.x, pos.y, -pos.z, pos.w);
}
