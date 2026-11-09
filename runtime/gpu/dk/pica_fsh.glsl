// MSR-nx: PICA200 fragment pipeline on the Switch GPU: textures, fragment lighting, the six
// texture combiner stages, alpha test and the PICA depth value. Reads the game's own register
// words (0x80..0x1DF) from a uniform block; same rules as sw_raster.c and sw_light.c.
#version 460

layout (location = 0) in vec4 v_color;
layout (location = 1) in vec4 v_uv01;
layout (location = 2) in vec4 v_uv2w;
layout (location = 3) in vec4 v_quat;
layout (location = 4) in vec4 v_view;

layout (location = 0) out vec4 out_color;

layout (binding = 0) uniform sampler2D tex0;
layout (binding = 1) uniform sampler2D tex1;
layout (binding = 2) uniform sampler2D tex2;
layout (binding = 3) uniform sampler2D luts;   // 256 x 24, value in r, difference in g

layout (std140, binding = 1) uniform FragmentState {
    uvec4 regs[88];   // PICA registers 0x80..0x1DF
    vec4 depth;       // x scale, y offset, z 1 for a w-buffer
};

// The register's word: the uvec4 by index, then the component by plain choices. No array in this
// shader is indexed by a run-time value: the compiler puts such arrays in local memory, and the
// general shader (the only one that had local memory, 2048 bytes per warp) is where the screen
// went white on the Switch, test builds 3 to 8.
uint dreg(uint id) {
    uint i = id - 0x80u;
    uvec4 v = regs[i >> 2];
    uint c = i & 3u;
    return c == 0u ? v.x : c == 1u ? v.y : c == 2u ? v.z : v.w;
}
// The shader pack's fragment shaders replace the next two lines with constants for the registers
// that choose what the shader does (tools/shaders/pica2glsl.py, fragment_shader()); everything else
// still comes from the uniform block. Keep reg() for those registers and dreg() for values.
uint reg(uint id) { return dreg(id); }   // MSR-STRUCTURAL
bool wbuffer() { return depth.z != 0.0; }   // MSR-WBUFFER
vec4 rgba8(uint v) { return vec4(float(v & 0xFFu), float((v >> 8) & 0xFFu), float((v >> 16) & 0xFFu), float(v >> 24)) / 255.0; }

vec4 prim, fprim, fsec, texc0, texc1, texc2, texc3, prevc, bufc;
vec4 texsel(uint i) { return i == 0u ? texc0 : i == 1u ? texc1 : i == 2u ? texc2 : texc3; }

vec4 tev_src(uint s, vec4 cst) {
    if (s == 0u) return prim;
    if (s == 1u) return fprim;
    if (s == 2u) return fsec;
    if (s == 3u) return texc0;
    if (s == 4u) return texc1;
    if (s == 5u) return texc2;
    if (s == 6u) return texc3;
    if (s == 13u) return bufc;
    if (s == 14u) return cst;
    return prevc;
}

vec3 rgb_op(uint op, vec4 c) {
    if (op == 0u) return c.rgb;
    if (op == 1u) return 1.0 - c.rgb;
    if (op == 2u) return vec3(c.a);
    if (op == 3u) return vec3(1.0 - c.a);
    if (op == 4u) return vec3(c.r);
    if (op == 5u) return vec3(1.0 - c.r);
    if (op == 8u) return vec3(c.g);
    if (op == 9u) return vec3(1.0 - c.g);
    if (op == 12u) return vec3(c.b);
    if (op == 13u) return vec3(1.0 - c.b);
    return c.rgb;
}

float a_op(uint op, vec4 c) {
    if (op == 0u) return c.a;
    if (op == 1u) return 1.0 - c.a;
    if (op == 2u) return c.r;
    if (op == 3u) return 1.0 - c.r;
    if (op == 4u) return c.g;
    if (op == 5u) return 1.0 - c.g;
    if (op == 6u) return c.b;
    return 1.0 - c.b;
}

vec3 comb3(uint m, vec3 a, vec3 b, vec3 c) {
    if (m == 0u) return a;
    if (m == 1u) return a * b;
    if (m == 2u) return a + b;
    if (m == 3u) return a + b - 0.5;
    if (m == 4u) return a * c + b * (1.0 - c);
    if (m == 5u) return a - b;
    if (m == 6u || m == 7u) return vec3(4.0 * dot(a - 0.5, b - 0.5));
    if (m == 8u) return a * b + c;
    if (m == 9u) return (a + b) * c;
    return a;
}

float comb1(uint m, float a, float b, float c) {
    if (m == 0u) return a;
    if (m == 1u) return a * b;
    if (m == 2u) return a + b;
    if (m == 3u) return a + b - 0.5;
    if (m == 4u) return a * c + b * (1.0 - c);
    if (m == 5u) return a - b;
    if (m == 8u) return a * b + c;
    if (m == 9u) return (a + b) * c;
    return a;
}

// ---- fragment lighting --------------------------------------------------------------------------
float lut(uint table, float x) { // x in entries (0..255.99)
    float e = clamp(floor(x), 0.0, 255.0);
    vec2 t = texelFetch(luts, ivec2(int(e), int(table)), 0).rg;
    return t.r + t.g * (x - e);
}
float pfloat(uint bits, uint man, uint ex) {
    uint width = man + ex + 1u;
    uint sgn = (bits >> (man + ex)) & 1u;
    if ((bits & ((1u << (width - 1u)) - 1u)) == 0u) return sgn != 0u ? -0.0 : 0.0;
    uint biased = (bits >> man) & ((1u << ex) - 1u);
    biased = biased == (1u << ex) - 1u ? 255u : biased + 128u - (1u << (ex - 1u));
    return uintBitsToFloat((sgn << 31) | ((bits & ((1u << man) - 1u)) << (23u - man)) | (biased << 23));
}
vec3 col10(uint v) { return vec3(float((v >> 20) & 0x3FFu), float((v >> 10) & 0x3FFu), float(v & 0x3FFu)) / 255.0; }
float fix13(uint v) { return float(int((v & 0x1FFFu) << 19) >> 19) / 2047.0; }
vec3 qrot(vec4 q, vec3 v) { vec3 t = cross(q.xyz, v); return v + 2.0 * cross(q.xyz, t + v * q.w); }
bool supported(uint config, uint table) {
    if (table == 0u) return config != 1u;
    if (table == 1u) return !(config == 0u || config == 1u || config == 5u);
    if (table == 8u) return !(config == 2u || config == 3u);
    if (table == 3u) return !(config == 0u || config == 2u || config == 4u);
    if (table == 6u) return config != 3u;
    return config == 4u || config == 5u || config == 8u;
}

void lighting() {
    uint c0 = reg(0x1C3u), c1 = reg(0x1C4u), config = (c0 >> 4) & 15u;
    uint absn = reg(0x1D0u), inputs = reg(0x1D1u), scales = reg(0x1D2u);
    vec4 q = length(v_quat) > 0.0 ? normalize(v_quat) : vec4(0.0, 0.0, 0.0, 1.0);
    vec4 shadow = vec4(1.0);
    bool sh_on = (c0 & 1u) != 0u;
    if (sh_on) { vec4 t = texsel((c0 >> 24) & 3u); shadow = ((c0 >> 18) & 1u) != 0u ? 1.0 - t : t; }
    vec3 sn = vec3(0.0, 0.0, 1.0), st = vec3(1.0, 0.0, 0.0);
    uint bump = (c0 >> 28) & 3u;
    if (bump == 1u) {
        sn = texsel((c0 >> 22) & 3u).rgb * 2.0 - 1.0;
        if (((c0 >> 30) & 1u) == 0u) sn.z = sqrt(max(1.0 - dot(sn.xy, sn.xy), 0.0));
    } else if (bump == 2u) st = texsel((c0 >> 22) & 3u).rgb * 2.0 - 1.0;
    vec3 n = qrot(q, sn), tg = config == 8u ? qrot(q, st) : st;
    vec3 view = v_view.xyz, nv = length(view) > 0.0 ? normalize(view) : vec3(0.0);
    vec4 diff = vec4(0.0, 0.0, 0.0, 1.0), spec = vec4(0.0, 0.0, 0.0, 1.0);
    uint count = (reg(0x1C2u) & 7u) + 1u;
    for (uint slot = 0u; slot < count; slot++) {
        uint num = (reg(0x1D9u) >> (4u * slot)) & 7u;
        uint b = 0x140u + 0x10u * num;
        vec3 s0 = col10(reg(b)), s1 = col10(reg(b + 1u)), dcol = col10(reg(b + 2u)), acol = col10(reg(b + 3u));
        vec3 pos = vec3(pfloat(reg(b + 4u) & 0xFFFFu, 10u, 5u), pfloat(reg(b + 4u) >> 16, 10u, 5u), pfloat(reg(b + 5u) & 0xFFFFu, 10u, 5u));
        vec3 spot = vec3(fix13(reg(b + 6u)), fix13(reg(b + 6u) >> 16), fix13(reg(b + 7u)));
        uint lf = reg(b + 9u);
        bool two = ((lf >> 1) & 1u) != 0u;
        vec3 lv = normalize((lf & 1u) != 0u ? pos : pos + view);
        vec3 half_v = nv + lv, hu = length(half_v) > 0.0 ? normalize(half_v) : vec3(0.0);
        float dist = 1.0;
        if (((c1 >> (24u + num)) & 1u) == 0u) {
            float bias = pfloat(reg(b + 10u) & 0xFFFFFu, 12u, 7u), scl = pfloat(reg(b + 11u) & 0xFFFFFu, 12u, 7u);
            float p = clamp(scl * length(-view - pos) + bias, 0.0, 1.0);
            dist = lut(16u + num, p * 256.0);
        }
        float val0 = 0.0, val1 = 0.0, val2 = 0.0, val3 = 0.0, val4 = 0.0, val5 = 0.0, val6 = 0.0;
        for (uint fld = 0u; fld < 7u; fld++) {
            uint in_ = (inputs >> (fld * 4u)) & 7u;
            bool absmode = ((absn >> (fld * 4u + 1u)) & 1u) == 0u;
            uint sc = (scales >> (fld * 4u)) & 7u;
            float scale = sc == 0u ? 1.0 : sc == 1u ? 2.0 : sc == 2u ? 4.0 : sc == 3u ? 8.0 : sc == 6u ? 0.25 : sc == 7u ? 0.5 : 0.0;
            float res = 0.0;
            if (in_ == 0u) res = dot(n, hu);
            else if (in_ == 1u) res = dot(nv, hu);
            else if (in_ == 2u) res = dot(n, nv);
            else if (in_ == 3u) res = dot(lv, n);
            else if (in_ == 4u) res = dot(lv, spot);
            else if (in_ == 5u && config == 8u) res = dot(hu - n * dot(n, hu), tg);
            uint table = fld == 2u ? 8u + num : fld;
            float x;
            if (absmode) { res = two ? abs(res) : max(res, 0.0); x = res * 256.0; }
            else { float e = clamp(floor(res * 128.0), -128.0, 127.0); float idx = e < 0.0 ? e + 256.0 : e; x = idx + (res * 128.0 - e); }
            float v = scale * lut(table, x);
            if (fld == 0u) val0 = v; else if (fld == 1u) val1 = v; else if (fld == 2u) val2 = v;
            else if (fld == 3u) val3 = v; else if (fld == 4u) val4 = v; else if (fld == 5u) val5 = v; else val6 = v;
        }
        float spotv = (((c1 >> (8u + num)) & 1u) == 0u && supported(config, 8u)) ? val2 : 1.0;
        float d0 = (((c1 >> 16) & 1u) == 0u && supported(config, 0u)) ? val0 : 1.0;
        float d1 = (((c1 >> 17) & 1u) == 0u && supported(config, 1u)) ? val1 : 1.0;
        float rr = (((c1 >> 20) & 1u) == 0u && supported(config, 6u)) ? val6 : 1.0;
        float rg = (((c1 >> 21) & 1u) == 0u && supported(config, 5u)) ? val5 : rr;
        float rb = (((c1 >> 22) & 1u) == 0u && supported(config, 4u)) ? val4 : rr;
        vec3 sp0 = s0 * d0, sp1 = d1 * vec3(rr, rg, rb) * s1;
        if (slot == count - 1u && ((c1 >> 19) & 1u) == 0u && supported(config, 3u)) {
            if (((c0 >> 2) & 1u) != 0u) diff.a = val3;
            if (((c0 >> 3) & 1u) != 0u) spec.a = val3;
        }
        float facing = dot(lv, n);
        facing = two ? abs(facing) : max(facing, 0.0);
        float hl = (((c0 >> 27) & 1u) != 0u && facing == 0.0) ? 0.0 : 1.0;
        if ((lf & 12u) != 0u) {
            float len2 = dot(half_v, half_v);
            float fct = len2 == 0.0 ? 0.0 : min(facing / len2, 1.0);
            if ((lf & 4u) != 0u) sp0 *= fct;
            if ((lf & 8u) != 0u) sp1 *= fct;
        }
        bool shadowed = ((c1 >> num) & 1u) == 0u;
        vec3 dsh = (sh_on && ((c0 >> 16) & 1u) != 0u && shadowed) ? shadow.rgb : vec3(1.0);
        vec3 ssh = (sh_on && ((c0 >> 17) & 1u) != 0u && shadowed) ? shadow.rgb : vec3(1.0);
        diff.rgb += (dcol * facing + acol) * dist * spotv * dsh;
        spec.rgb += (sp0 + sp1) * hl * dist * spotv * ssh;
    }
    if (sh_on && ((c0 >> 19) & 1u) != 0u) {
        if (((c0 >> 2) & 1u) != 0u) diff.a *= shadow.a;
        if (((c0 >> 3) & 1u) != 0u) spec.a *= shadow.a;
    }
    diff.rgb += col10(reg(0x1C0u));
    fprim = clamp(diff, 0.0, 1.0);
    fsec = clamp(spec, 0.0, 1.0);
}

bool alpha_ok(uint f, float a, float ref) {
    if (f == 0u) return false;
    if (f == 1u) return true;
    if (f == 2u) return a == ref;
    if (f == 3u) return a != ref;
    if (f == 4u) return a < ref;
    if (f == 5u) return a <= ref;
    if (f == 6u) return a > ref;
    return a >= ref;
}

void main() {
    prim = clamp(v_color, 0.0, 1.0);
    texc0 = texc1 = texc2 = texc3 = vec4(0.0, 0.0, 0.0, 1.0);
    uint tc = reg(0x80u);
    if ((tc & 1u) != 0u) {
        vec2 uv = v_uv01.xy;
        if (((reg(0x83u) >> 28) & 7u) == 3u && v_uv2w.z != 0.0) uv /= v_uv2w.z;
        texc0 = texture(tex0, uv);
    }
    if ((tc & 2u) != 0u) texc1 = texture(tex1, v_uv01.zw);
    if ((tc & 4u) != 0u) texc2 = texture(tex2, ((tc >> 13) & 1u) != 0u ? v_uv01.zw : v_uv2w.xy);
    fprim = fsec = vec4(0.0);
    if ((reg(0x8Fu) & 1u) != 0u && (reg(0x1C6u) & 1u) == 0u) lighting();

    prevc = prim;
    bufc = rgba8(reg(0xFDu));
    vec4 next_buf = bufc;
    uint e0 = reg(0xE0u);
    for (uint s = 0u; s < 6u; s++) {
        uint b = s < 4u ? 0xC0u + 8u * s : 0xF0u + 8u * (s - 4u);
        uint srcw = reg(b), opw = reg(b + 1u), cmb = reg(b + 2u), sc = reg(b + 4u);
        vec4 cst = rgba8(reg(b + 3u));
        vec3 ic0 = rgb_op(opw & 15u, tev_src(srcw & 15u, cst));
        vec3 ic1 = rgb_op((opw >> 4) & 15u, tev_src((srcw >> 4) & 15u, cst));
        vec3 ic2 = rgb_op((opw >> 8) & 15u, tev_src((srcw >> 8) & 15u, cst));
        float ia0 = a_op((opw >> 12) & 7u, tev_src((srcw >> 16) & 15u, cst));
        float ia1 = a_op((opw >> 16) & 7u, tev_src((srcw >> 20) & 15u, cst));
        float ia2 = a_op((opw >> 20) & 7u, tev_src((srcw >> 24) & 15u, cst));
        uint mr = cmb & 15u, ma = (cmb >> 16) & 15u;
        vec3 outc = comb3(mr, ic0, ic1, ic2);
        float outa = mr == 7u ? outc.r : comb1(ma, ia0, ia1, ia2);
        vec4 res = vec4(clamp(outc * float(1u << (sc & 3u)), 0.0, 1.0), clamp(outa * float(1u << ((sc >> 16) & 3u)), 0.0, 1.0));
        bufc = next_buf;
        if (s < 4u) {
            if (((e0 >> (8u + s)) & 1u) != 0u) next_buf.rgb = res.rgb;
            if (((e0 >> (12u + s)) & 1u) != 0u) next_buf.a = res.a;
        }
        prevc = res;
    }
    uint at = reg(0x104u);
    if ((at & 1u) != 0u) {
        float a8 = floor(prevc.a * 255.0 + 0.5);
        if (!alpha_ok((at >> 4) & 7u, a8, float((dreg(0x104u) >> 8) & 0xFFu))) discard;
    }
    out_color = prevc;
    // PICA depth: (z/w * scale + offset), times w for a w-buffer. gl_FragCoord.z is -z/w here.
    // (The shader pack's early-depth variants drop these lines: for a z-buffer the same value comes
    // from the viewport's depth range, and the GPU can then test depth before running the shader.)
    // MSR-DEPTH-BEGIN
    float d = -gl_FragCoord.z * depth.x + depth.y;
    if (wbuffer()) d *= 1.0 / gl_FragCoord.w;
    gl_FragDepth = clamp(d, 0.0, 1.0);
    // MSR-DEPTH-END
}
