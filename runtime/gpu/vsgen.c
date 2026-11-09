// MSR-nx: the vertex shader translator of tools/shaders/pica2glsl.py (its glsl() output), in C, so
// the Switch build can translate a configuration missing from the shader pack and compile it there.
// The output must stay byte for byte the same as the Python tool's; tools/tests/vsgen_test.sh checks
// that on every configuration in a dump.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vsgen.h"

typedef struct {
    char* s;
    size_t n, cap;
    int fail;
} Buf;

static void put(Buf* b, const char* fmt, ...) {
    if (b->fail) return;
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
        if (k < 0) { b->fail = 1; return; }
        if (b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        size_t nc = b->cap * 2 + (size_t)k + 1024;
        char* ns = (char*)realloc(b->s, nc);
        if (!ns) { b->fail = 1; return; }
        b->s = ns;
        b->cap = nc;
    }
}

typedef struct {
    const VsConfig* cfg;
    Buf body;
    unsigned used;   // input registers read (bit per register)
    int depth;
    const char* err;
} Tr;

static const char COMP[] = "xyzw";

static void indent(Buf* b, int ind) { for (int k = 0; k < ind; k++) put(b, "    "); }

// A source operand as an expression (pica2glsl.py src() and swizzle()); out must hold 256 bytes.
static void src(Tr* t, char* out, unsigned reg, unsigned rel, unsigned sel, unsigned neg) {
    char e[64];
    if (reg < 16) {
        t->used |= 1u << reg;
        sprintf(e, "v%u", reg);
    } else if (reg < 32) {
        sprintf(e, "r%u", reg - 16);
    } else {
        unsigned i = reg - 32;
        if (rel == 0) {
            if (i < 96) sprintf(e, "f[%u]", i); else strcpy(e, "vec4(1.0)");
        } else {
            static const char* rn[4] = {"", "a0x", "a0y", "aL"};
            sprintf(e, "fr(%u + %s)", i, rn[rel]);
        }
    }
    unsigned idx[4];
    for (int k = 0; k < 4; k++) idx[k] = (sel >> (2 * (3 - k))) & 3;
    char sw[256];
    if (idx[0] == 0 && idx[1] == 1 && idx[2] == 2 && idx[3] == 3) strcpy(sw, e);
    else snprintf(sw, sizeof sw, "vec4((%s).%c, (%s).%c, (%s).%c, (%s).%c)", e, COMP[idx[0]], e, COMP[idx[1]], e, COMP[idx[2]], e, COMP[idx[3]]);
    if (neg) snprintf(out, 256, "(-(%s))", sw); else strcpy(out, sw);
}

static void dst(Tr* t, unsigned reg, unsigned mask, const char* value, int ind) {
    char name[8];
    if (reg < 16) sprintf(name, "o%u", reg);
    else if (reg < 32) sprintf(name, "r%u", reg - 16);
    else return;
    int n = 0;
    for (int k = 0; k < 4; k++) if (mask & (8 >> k)) n++;
    if (!n) return;
    indent(&t->body, ind);
    if (n == 4) { put(&t->body, "%s = %s;\n", name, value); return; }
    put(&t->body, "{ vec4 t = %s;", value);
    for (int k = 0; k < 4; k++) if (mask & (8 >> k)) put(&t->body, " %s.%c = t.%c;", name, COMP[k], COMP[k]);
    put(&t->body, " }\n");
}

static void line(Tr* t, int ind, const char* fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    indent(&t->body, ind);
    put(&t->body, "%s\n", tmp);
}

static void flowcond(char* out, unsigned w) {
    unsigned refx = (w >> 25) & 1, refy = (w >> 24) & 1, op = (w >> 22) & 3;
    const char* rx = refx ? "cx" : "!cx";
    const char* ry = refy ? "cy" : "!cy";
    if (op == 0) sprintf(out, "(%s || %s)", rx, ry);
    else if (op == 1) sprintf(out, "(%s && %s)", rx, ry);
    else strcpy(out, op == 2 ? rx : ry);
}

static void boolu(char* out, unsigned w) { sprintf(out, "((bools >> %uu) & 1u) != 0u", (w >> 22) & 15); }

static int emit(Tr* t, unsigned start, unsigned end, int ind, int loops) {
    if (++t->depth > 24) { t->err = "calls nest too deep"; return 0; }
    const u32* code = t->cfg->code;
    const u32* swz = t->cfg->swz;
    unsigned pc = start;
    char a[256], b[256], c[256], v[1024], cond[96];
    while (pc < end) {
        if (pc >= 4096) { t->err = "runs off the program"; return 0; }
        unsigned w = code[pc], op = w >> 26;
        unsigned D = (w >> 10) & 0xFFF, N = w & 0xFF;
        if (op >= 0x30) {   // MAD, MADI
            int madi = op < 0x38;
            unsigned desc = swz[w & 0x1F];
            unsigned d = (w >> 24) & 0x1F, rel = (w >> 22) & 3, s1 = (w >> 17) & 0x1F;
            unsigned s2 = madi ? (w >> 12) & 0x1F : (w >> 10) & 0x7F;
            unsigned s3 = madi ? (w >> 5) & 0x7F : (w >> 5) & 0x1F;
            src(t, a, s1, 0, (desc >> 5) & 0xFF, (desc >> 4) & 1);
            src(t, b, s2, madi ? 0 : rel, (desc >> 14) & 0xFF, (desc >> 13) & 1);
            src(t, c, s3, madi ? rel : 0, (desc >> 23) & 0xFF, (desc >> 22) & 1);
            snprintf(v, sizeof v, "pmul(%s, %s) + %s", a, b, c);
            dst(t, d, desc & 0xF, v, ind);
            pc++;
            continue;
        }
        unsigned desc = swz[w & 0x7F], mask = desc & 0xF;
        unsigned d = (w >> 21) & 0x1F, rel = (w >> 19) & 3;
        int inv = op >= 0x18 && op <= 0x1B;
        unsigned s1 = inv ? (w >> 14) & 0x1F : (w >> 12) & 0x7F;
        unsigned s2 = inv ? (w >> 7) & 0x7F : (w >> 7) & 0x1F;
        unsigned sel1 = (desc >> 5) & 0xFF, sel2 = (desc >> 14) & 0xFF;
        unsigned n1 = (desc >> 4) & 1, n2 = (desc >> 13) & 1;
        unsigned r1 = inv ? 0 : rel, r2 = inv ? rel : 0;
#define A() src(t, a, s1, r1, sel1, n1)
#define B() src(t, b, s2, r2, sel2, n2)
        switch (op) {
        case 0x00: A(); B(); snprintf(v, sizeof v, "%s + %s", a, b); dst(t, d, mask, v, ind); break;
        case 0x01:
            A(); B(); line(t, ind, "{ vec4 p = pmul(%s, %s); float s = p.x + p.y + p.z;", a, b);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x02:
            A(); B(); line(t, ind, "{ vec4 p = pmul(%s, %s); float s = p.x + p.y + p.z + p.w;", a, b);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x03: case 0x18:
            B(); A(); line(t, ind, "{ vec4 b = %s; vec4 p = pmul(%s, b); float s = p.x + p.y + p.z + b.w;", b, a);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x04: case 0x19:
            A(); B(); line(t, ind, "{ vec4 a = %s; vec4 b = %s;", a, b);
            dst(t, d, mask, "vec4(1.0, smul(a.y, b.y), a.z, b.w)", ind + 1); line(t, ind, "}"); break;
        case 0x05:
            A(); line(t, ind, "{ float s = exp2((%s).x);", a);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x06:
            A(); line(t, ind, "{ float s = log2((%s).x);", a);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x07:
            A(); line(t, ind, "{ vec4 a = %s; cx = a.x > 0.0; cy = a.w > 0.0;", a);
            dst(t, d, mask, "vec4(max(a.x, 0.0), max(a.y, 0.0), clamp(a.z, -127.9961, 127.9961), max(a.w, 0.0))", ind + 1);
            line(t, ind, "}"); break;
        case 0x08: A(); B(); snprintf(v, sizeof v, "pmul(%s, %s)", a, b); dst(t, d, mask, v, ind); break;
        case 0x09: case 0x1A:
            A(); B(); line(t, ind, "{ vec4 a = %s; vec4 b = %s;", a, b);
            dst(t, d, mask, "vec4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0)", ind + 1);
            line(t, ind, "}"); break;
        case 0x0A: case 0x1B:
            A(); B(); line(t, ind, "{ vec4 a = %s; vec4 b = %s;", a, b);
            dst(t, d, mask, "vec4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0)", ind + 1);
            line(t, ind, "}"); break;
        case 0x0B:
            A(); line(t, ind, "{ vec4 a = %s;", a);
            dst(t, d, mask, "vec4(floor(a.x), floor(a.y), floor(a.z), floor(a.w))", ind + 1); line(t, ind, "}"); break;
        case 0x0C:
            A(); B(); line(t, ind, "{ vec4 a = %s; vec4 b = %s;", a, b);
            dst(t, d, mask, "vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w)", ind + 1);
            line(t, ind, "}"); break;
        case 0x0D:
            A(); B(); line(t, ind, "{ vec4 a = %s; vec4 b = %s;", a, b);
            dst(t, d, mask, "vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w)", ind + 1);
            line(t, ind, "}"); break;
        case 0x0E:
            A(); line(t, ind, "{ float s = 1.0 / (%s).x;", a);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x0F:
            A(); line(t, ind, "{ float s = 1.0 / sqrt((%s).x);", a);
            dst(t, d, mask, "vec4(s, s, s, s)", ind + 1); line(t, ind, "}"); break;
        case 0x12:   // MOVA
            if (mask & 0xC) {
                A();
                line(t, ind, "{ vec4 a = %s; %s%s%s }", a, (mask & 8) ? "a0x = int(a.x);" : "",
                     (mask & 8) && (mask & 4) ? " " : "", (mask & 4) ? "a0y = int(a.y);" : "");
            }
            break;
        case 0x13: A(); dst(t, d, mask, a, ind); break;
        case 0x20: case 0x23:   // BREAK, BREAKC
            if (!loops) { t->err = "BREAK outside a loop"; return 0; }
            if (op == 0x20) line(t, ind, "break;");
            else { flowcond(cond, w); line(t, ind, "if (%s) break;", cond); }
            break;
        case 0x21: case 0x2A: case 0x2B: break;   // NOP, EMIT, SETEMIT
        case 0x22: line(t, ind, "return;"); t->depth--; return 1;   // the rest of this block cannot run
        case 0x24: case 0x25: case 0x26:   // CALL, CALLC, CALLU
            if (op == 0x24) line(t, ind, "{");
            else {
                if (op == 0x25) flowcond(cond, w); else boolu(cond, w);
                line(t, ind, "if (%s) {", cond);
            }
            if (!emit(t, D, D + N, ind + 1, loops)) return 0;
            line(t, ind, "}");
            break;
        case 0x27: case 0x28:   // IFU, IFC
            if (!(pc < D && D <= D + N && D + N <= end)) { t->err = "IF leaves its block"; return 0; }
            if (op == 0x28) flowcond(cond, w); else boolu(cond, w);
            line(t, ind, "if (%s) {", cond);
            if (!emit(t, pc + 1, D, ind + 1, loops)) return 0;
            if (N) {
                line(t, ind, "} else {");
                if (!emit(t, D, D + N, ind + 1, loops)) return 0;
            }
            line(t, ind, "}");
            pc = D + N;
            continue;
        case 0x29: {   // LOOP
            if (!(pc < D && D < end)) { t->err = "LOOP leaves its block"; return 0; }
            unsigned i = (w >> 22) & 3;
            line(t, ind, "aL = int(ints[%u].y);", i);
            line(t, ind, "for (uint it%d = 0u; it%d <= ints[%u].x; it%d++) {", loops, loops, i, loops);
            line(t, ind + 1, "if (it%d != 0u) aL += sx8(ints[%u].z);", loops, i);
            if (!emit(t, pc + 1, D + 1, ind + 1, loops + 1)) return 0;
            line(t, ind, "}");
            pc = D + 1;
            continue;
        }
        case 0x2C: case 0x2D:   // JMPC, JMPU
            if (!(pc < D && D <= end)) { t->err = "jump leaves its block"; return 0; }
            if (op == 0x2C) flowcond(cond, w);
            else { char bu[64]; boolu(bu, w); sprintf(cond, "(%s) != %s", bu, (N & 1) ? "true" : "false"); }
            line(t, ind, "if (!(%s)) {", cond);
            if (!emit(t, pc + 1, D, ind + 1, loops)) return 0;
            line(t, ind, "}");
            pc = D;
            continue;
        case 0x2E: case 0x2F: {   // CMP
            static const char* ops[6] = {"==", "!=", "<", "<=", ">", ">="};
            unsigned ox = (w >> 24) & 7, oy = (w >> 21) & 7;
            src(t, a, (w >> 12) & 0x7F, rel, sel1, n1);
            src(t, b, (w >> 7) & 0x1F, 0, sel2, n2);
            char cxs[32], cys[32];
            if (ox < 6) sprintf(cxs, "a.x %s b.x", ops[ox]); else strcpy(cxs, "true");
            if (oy < 6) sprintf(cys, "a.y %s b.y", ops[oy]); else strcpy(cys, "true");
            line(t, ind, "{ vec4 a = %s; vec4 b = %s; cx = %s; cy = %s; }", a, b, cxs, cys);
            break;
        }
        default:
            line(t, ind, "// opcode %02X at %03X: not handled (the interpreter skips it too)", op, pc);
            break;
        }
#undef A
#undef B
        pc++;
    }
    t->depth--;
    return 1;
}

static const char k_head[] =
    "// Generated by tools/shaders/pica2glsl.py from one of the game's vertex shaders: not hand-edited,\n"
    "// not committed. Uniforms and buffers match runtime/gpu/dk/pica_vsh.glsl.\n"
    "#version 460\n"
    "layout (std430, binding = 1) readonly buffer Vertices { uint vraw[]; };\n"
    "layout (std140, binding = 0) uniform VertexState {\n"
    "    vec4 f[96];\n"
    "    uvec4 ints[4];\n"
    "    uvec4 cfg;\n"
    "    uvec4 outmap[7];\n"
    "    uvec4 base;\n"
    "    uvec4 attr[16];\n"
    "    vec4 fixd[16];\n"
    "};\n"
    "layout (location = 0) out vec4 v_color;\n"
    "layout (location = 1) out vec4 v_uv01;\n"
    "layout (location = 2) out vec4 v_uv2w;\n"
    "layout (location = 3) out vec4 v_quat;\n"
    "layout (location = 4) out vec4 v_view;\n"
    "#define bools cfg.x\n"
    "vec4 pmul(vec4 a, vec4 b) {\n"
    "    vec4 p = a * b;\n"
    "    bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));\n"
    "    return mix(p, vec4(0.0), z);\n"
    "}\n"
    "float smul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }\n"
    "vec4 fr(int i) { i &= 127; return i < 96 ? f[i] : vec4(1.0); }\n"
    "int sx8(uint u) { return int(u) - (u >= 128u ? 256 : 0); }\n"
    "// Vertex inputs, fetched and decoded here from the game's own vertex bytes (pica_cmd.c\n"
    "// pica_vertex_layout says where each input lives; pica_fetch_attr is the same code on the CPU).\n"
    "uint vbyte(uint a) { return (vraw[a >> 2u] >> ((a & 3u) * 8u)) & 0xFFu; }\n"
    "vec4 fetch_attr(uint reg) {\n"
    "    uvec4 t = attr[reg];\n"
    "    if (t.x == 1u) return fixd[reg];\n"
    "    vec4 v = vec4(0.0, 0.0, 0.0, 1.0);\n"
    "    if (t.x != 2u) return v;\n"
    "    uint type = t.w & 3u, comps = t.w >> 4u;\n"
    "    uint sz = type == 3u ? 4u : type == 2u ? 2u : 1u;\n"
    "    uint a = t.y + uint(gl_VertexID) * t.z;\n"
    "    for (uint c = 0u; c < comps; c++) {\n"
    "        uint p = a + c * sz;\n"
    "        float x;\n"
    "        if (type == 0u) x = float(int(vbyte(p) << 24u) >> 24);\n"
    "        else if (type == 1u) x = float(vbyte(p));\n"
    "        else if (type == 2u) x = float(int((vbyte(p) | (vbyte(p + 1u) << 8u)) << 16u) >> 16);\n"
    "        else x = uintBitsToFloat(vbyte(p) | (vbyte(p + 1u) << 8u) | (vbyte(p + 2u) << 16u) | (vbyte(p + 3u) << 24u));\n"
    "        if (c == 0u) v.x = x; else if (c == 1u) v.y = x; else if (c == 2u) v.z = x; else v.w = x;\n"
    "    }\n"
    "    return v;\n"
    "}\n"
    "vec4 inp(uint reg) { return fetch_attr(reg); }\n";

char* vsgen_glsl(const VsConfig* cfg, const char** why) {
    Tr t;
    memset(&t, 0, sizeof t);
    t.cfg = cfg;
    if (!emit(&t, cfg->entry, 4096, 1, 0) || t.body.fail) {
        if (why) *why = t.err ? t.err : "out of memory";
        free(t.body.s);
        return 0;
    }
    Buf o = {0};
    put(&o, "%s\n", k_head);   // Python: the head string (ending in a newline) joined with "\n"
    for (int k = 0; k < 16; k++) put(&o, "vec4 r%d = vec4(0.0);\n", k);
    for (int k = 0; k < 16; k++) put(&o, "vec4 o%d = vec4(0.0, 0.0, 0.0, 1.0);\n", k);
    put(&o, "int a0x = 0;\nint a0y = 0;\nint aL = 0;\nbool cx = false;\nbool cy = false;\n");
    for (int k = 0; k < 16; k++) if (t.used & (1u << k)) put(&o, "vec4 v%d;\n", k);
    put(&o, "void run() {\n");
    if (t.body.s) put(&o, "%s", t.body.s);
    put(&o, "}\nvoid main() {\n");
    for (int k = 0; k < 16; k++) if (t.used & (1u << k)) put(&o, "    v%d = inp(%du);\n", k, k);
    put(&o, "    run();\n");
    put(&o, "    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0), quat = vec4(0.0, 0.0, 0.0, 1.0), col = vec4(1.0);\n");
    put(&o, "    vec4 uv01 = vec4(0.0), uv2w = vec4(0.0), view = vec4(0.0);\n");
    // outputs (pica2glsl.py outputs())
    unsigned oi = 0, total = cfg->total & 7;
    for (unsigned k = 0; k < (total < 7 ? total : 7); k++) {
        while (oi < 16 && !((cfg->outmask >> oi) & 1)) oi++;
        if (oi >= 16) break;
        u32 m = cfg->outmap[k];
        for (int j = 0; j < 4; j++) {
            unsigned s = (m >> (8 * j)) & 0x1F;
            char sv[16];
            sprintf(sv, "o%u.%c", oi, COMP[j]);
            if (s <= 3) put(&o, "    pos.%c = %s;\n", COMP[s], sv);
            else if (s <= 7) put(&o, "    quat.%c = %s;\n", COMP[s - 4], sv);
            else if (s <= 11) put(&o, "    col.%c = %s;\n", COMP[s - 8], sv);
            else if (s <= 15) put(&o, "    uv01.%c = %s;\n", COMP[s - 12], sv);
            else if (s == 16) put(&o, "    uv2w.z = %s;\n", sv);
            else if (s >= 18 && s <= 20) put(&o, "    view.%c = %s;\n", COMP[s - 18], sv);
            else if (s == 22) put(&o, "    uv2w.x = %s;\n", sv);
            else if (s == 23) put(&o, "    uv2w.y = %s;\n", sv);
        }
        oi++;
    }
    put(&o, "    v_color = col; v_uv01 = uv01; v_uv2w = uv2w; v_quat = quat; v_view = view;\n");
    put(&o, "    gl_Position = vec4(pos.x, pos.y, -pos.z, pos.w);\n");
    put(&o, "}\n");
    free(t.body.s);
    if (o.fail) { free(o.s); if (why) *why = "out of memory"; return 0; }
    return o.s;
}
