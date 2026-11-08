#!/usr/bin/env python3
"""MSR-nx: translates the game's PICA200 vertex shader configurations into Switch shaders.

Input: a configuration dump written by the probe (MSR_VS_DUMP) or by the Switch build
(sdmc:/switch/msr/vs_new.bin). Each record is a 13-word header (magic "PVS1", key, entry point,
output mask, output total, GPUREG_SH_OUTMAP_O0..6), the 4096-word program and the 128 swizzle words.

Output, for each configuration:
  vs_<key>.glsl  a vertex shader for uam (deko3d), with the same uniforms, storage buffers and
                 outputs as the interpreter in runtime/gpu/dk/pica_vsh.glsl
and one file with every configuration as a C++ function (check.cpp), used by the probe to compare
the translation against the reference interpreter (runtime/gpu/pica_shader.c) on the game's own
vertices before anything goes to the Switch.

The program body is written once in a syntax both GLSL and C++ accept (constructors, members,
ternaries, if/for/break/return), so the code the probe checks is the code the Switch runs.

Control flow: CALL, IF, LOOP and forward JMPs that stay inside their block are rebuilt as structured
code (calls are inlined). Anything else (a backward jump, a jump out of a block) is reported and the
configuration is left to the interpreter.

The dump and everything generated from it come from the game; they stay out of the repository.
"""
import struct
import sys
import os

REC_WORDS = 13 + 4096 + 128


class Untranslatable(Exception):
    pass


def read_dump(path):
    data = open(path, 'rb').read()
    out = {}
    n = len(data) // (REC_WORDS * 4)
    for i in range(n):
        w = struct.unpack_from('<%dI' % REC_WORDS, data, i * REC_WORDS * 4)
        if w[0] != 0x31535650:
            raise SystemExit('bad record %d' % i)
        key = w[1] | (w[2] << 32)
        out[key] = {
            'key': key, 'entry': w[3], 'outmask': w[4], 'total': w[5],
            'outmap': list(w[6:13]), 'code': list(w[13:13 + 4096]), 'swz': list(w[13 + 4096:]),
        }
    return out


COMP = 'xyzw'


def swizzle(expr, sel):
    """Applies an 8-bit selector (two bits per component, x first) to a vec4 expression."""
    idx = [(sel >> (2 * (3 - k))) & 3 for k in range(4)]
    if idx == [0, 1, 2, 3]:
        return expr
    return 'vec4(%s)' % ', '.join('(%s).%s' % (expr, COMP[i]) for i in idx)


class Translator:
    def __init__(self, cfg):
        self.cfg = cfg
        self.code = cfg['code']
        self.swz = cfg['swz']
        self.lines = []
        self.used_inputs = set()
        self.depth = 0

    def out(self, s, ind):
        self.lines.append('    ' * ind + s)

    def src(self, reg, rel, sel, neg):
        if reg < 16:
            self.used_inputs.add(reg)
            e = 'v%d' % reg
        elif reg < 32:
            e = 'r%d' % (reg - 16)
        else:
            i = reg - 32
            if rel == 0:
                e = 'f[%d]' % i if i < 96 else 'vec4(1.0)'
            else:
                e = 'fr(%d + %s)' % (i, ['', 'a0x', 'a0y', 'aL'][rel])
        e = swizzle(e, sel)
        return '(-(%s))' % e if neg else e

    def dst(self, reg, mask, value, ind):
        if reg < 16:
            name = 'o%d' % reg
        elif reg < 32:
            name = 'r%d' % (reg - 16)
        else:
            return
        comps = [COMP[k] for k in range(4) if mask & (8 >> k)]
        if not comps:
            return
        if len(comps) == 4:
            self.out('%s = %s;' % (name, value), ind)
            return
        self.out('{ vec4 t = %s;%s }' % (value, ''.join(' %s.%s = t.%s;' % (name, c, c) for c in comps)), ind)

    @staticmethod
    def flowcond(w):
        refx, refy, op = (w >> 25) & 1, (w >> 24) & 1, (w >> 22) & 3
        rx = 'cx' if refx else '!cx'
        ry = 'cy' if refy else '!cy'
        return ['(%s || %s)' % (rx, ry), '(%s && %s)' % (rx, ry), rx, ry][op]

    @staticmethod
    def boolu(w):
        return '((bools >> %du) & 1u) != 0u' % ((w >> 22) & 15)

    def emit(self, start, end, ind, loops):
        """Emits instructions start <= pc < end at indent ind; loops = number of enclosing LOOPs."""
        self.depth += 1
        if self.depth > 24:
            raise Untranslatable('calls nest too deep')
        pc = start
        while pc < end:
            if pc >= 4096:
                raise Untranslatable('runs off the program')
            w = self.code[pc]
            op = w >> 26
            D, N = (w >> 10) & 0xFFF, w & 0xFF
            if op >= 0x30:   # MAD, MADI
                madi = op < 0x38
                desc = self.swz[w & 0x1F]
                d, rel, s1 = (w >> 24) & 0x1F, (w >> 22) & 3, (w >> 17) & 0x1F
                s2 = (w >> 12) & 0x1F if madi else (w >> 10) & 0x7F
                s3 = (w >> 5) & 0x7F if madi else (w >> 5) & 0x1F
                a = self.src(s1, 0, (desc >> 5) & 0xFF, (desc >> 4) & 1)
                b = self.src(s2, 0 if madi else rel, (desc >> 14) & 0xFF, (desc >> 13) & 1)
                c = self.src(s3, rel if madi else 0, (desc >> 23) & 0xFF, (desc >> 22) & 1)
                self.dst(d, desc & 0xF, 'pmul(%s, %s) + %s' % (a, b, c), ind)
                pc += 1
                continue
            desc = self.swz[w & 0x7F]
            mask = desc & 0xF
            d, rel = (w >> 21) & 0x1F, (w >> 19) & 3
            inv = 0x18 <= op <= 0x1B
            s1 = (w >> 14) & 0x1F if inv else (w >> 12) & 0x7F
            s2 = (w >> 7) & 0x7F if inv else (w >> 7) & 0x1F
            sel1, sel2 = (desc >> 5) & 0xFF, (desc >> 14) & 0xFF
            n1, n2 = (desc >> 4) & 1, (desc >> 13) & 1
            r1, r2 = (0, rel) if inv else (rel, 0)
            A = lambda: self.src(s1, r1, sel1, n1)
            B = lambda: self.src(s2, r2, sel2, n2)
            if op == 0x00:
                self.dst(d, mask, '%s + %s' % (A(), B()), ind)
            elif op == 0x01:
                self.out('{ vec4 p = pmul(%s, %s); float s = p.x + p.y + p.z;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op == 0x02:
                self.out('{ vec4 p = pmul(%s, %s); float s = p.x + p.y + p.z + p.w;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op in (0x03, 0x18):
                self.out('{ vec4 b = %s; vec4 p = pmul(%s, b); float s = p.x + p.y + p.z + b.w;' % (B(), A()), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op in (0x04, 0x19):
                self.out('{ vec4 a = %s; vec4 b = %s;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(1.0, smul(a.y, b.y), a.z, b.w)', ind + 1)
                self.out('}', ind)
            elif op == 0x05:
                self.out('{ float s = exp2((%s).x);' % A(), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op == 0x06:
                self.out('{ float s = log2((%s).x);' % A(), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op == 0x07:   # LITP
                self.out('{ vec4 a = %s; cx = a.x > 0.0; cy = a.w > 0.0;' % A(), ind)
                self.dst(d, mask, 'vec4(max(a.x, 0.0), max(a.y, 0.0), clamp(a.z, -127.9961, 127.9961), max(a.w, 0.0))', ind + 1)
                self.out('}', ind)
            elif op == 0x08:
                self.dst(d, mask, 'pmul(%s, %s)' % (A(), B()), ind)
            elif op in (0x09, 0x1A):
                self.out('{ vec4 a = %s; vec4 b = %s;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(a.x >= b.x ? 1.0 : 0.0, a.y >= b.y ? 1.0 : 0.0, a.z >= b.z ? 1.0 : 0.0, a.w >= b.w ? 1.0 : 0.0)', ind + 1)
                self.out('}', ind)
            elif op in (0x0A, 0x1B):
                self.out('{ vec4 a = %s; vec4 b = %s;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(a.x < b.x ? 1.0 : 0.0, a.y < b.y ? 1.0 : 0.0, a.z < b.z ? 1.0 : 0.0, a.w < b.w ? 1.0 : 0.0)', ind + 1)
                self.out('}', ind)
            elif op == 0x0B:
                self.out('{ vec4 a = %s;' % A(), ind)
                self.dst(d, mask, 'vec4(floor(a.x), floor(a.y), floor(a.z), floor(a.w))', ind + 1)
                self.out('}', ind)
            elif op == 0x0C:
                self.out('{ vec4 a = %s; vec4 b = %s;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z, a.w > b.w ? a.w : b.w)', ind + 1)
                self.out('}', ind)
            elif op == 0x0D:
                self.out('{ vec4 a = %s; vec4 b = %s;' % (A(), B()), ind)
                self.dst(d, mask, 'vec4(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z, a.w < b.w ? a.w : b.w)', ind + 1)
                self.out('}', ind)
            elif op == 0x0E:
                self.out('{ float s = 1.0 / (%s).x;' % A(), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op == 0x0F:
                self.out('{ float s = 1.0 / sqrt((%s).x);' % A(), ind)
                self.dst(d, mask, 'vec4(s, s, s, s)', ind + 1)
                self.out('}', ind)
            elif op == 0x12:   # MOVA
                parts = []
                if mask & 8:
                    parts.append('a0x = int(a.x);')
                if mask & 4:
                    parts.append('a0y = int(a.y);')
                if parts:
                    self.out('{ vec4 a = %s; %s }' % (A(), ' '.join(parts)), ind)
            elif op == 0x13:
                self.dst(d, mask, A(), ind)
            elif op in (0x20, 0x23):   # BREAK, BREAKC
                if not loops:
                    raise Untranslatable('BREAK outside a loop at %03X' % pc)
                self.out('break;' if op == 0x20 else 'if (%s) break;' % self.flowcond(w), ind)
            elif op == 0x21 or op in (0x2A, 0x2B):
                pass   # NOP, EMIT, SETEMIT
            elif op == 0x22:
                self.out('return;', ind)
                break   # the rest of this block cannot run
            elif op in (0x24, 0x25, 0x26):   # CALL, CALLC, CALLU
                if op == 0x24:
                    self.out('{', ind)
                else:
                    self.out('if (%s) {' % (self.flowcond(w) if op == 0x25 else self.boolu(w)), ind)
                self.emit(D, D + N, ind + 1, loops)
                self.out('}', ind)
            elif op in (0x27, 0x28):   # IFU, IFC
                if not (pc < D <= D + N <= end):
                    raise Untranslatable('IF at %03X leaves its block' % pc)
                self.out('if (%s) {' % (self.flowcond(w) if op == 0x28 else self.boolu(w)), ind)
                self.emit(pc + 1, D, ind + 1, loops)
                if N:
                    self.out('} else {', ind)
                    self.emit(D, D + N, ind + 1, loops)
                self.out('}', ind)
                pc = D + N
                continue
            elif op == 0x29:   # LOOP: the body runs i.x + 1 times; aL starts at i.y and steps by i.z between runs
                if not (pc < D < end):
                    raise Untranslatable('LOOP at %03X leaves its block' % pc)
                i = (w >> 22) & 3
                self.out('aL = int(ints[%d].y);' % i, ind)
                self.out('for (uint it%d = 0u; it%d <= ints[%d].x; it%d++) {' % (loops, loops, i, loops), ind)
                self.out('if (it%d != 0u) aL += sx8(ints[%d].z);' % (loops, i), ind + 1)
                self.emit(pc + 1, D + 1, ind + 1, loops + 1)
                self.out('}', ind)
                pc = D + 1
                continue
            elif op in (0x2C, 0x2D):   # JMPC, JMPU: forward jumps inside the block only
                if not (pc < D <= end):
                    raise Untranslatable('jump at %03X to %03X leaves its block' % (pc, D))
                cond = self.flowcond(w) if op == 0x2C else '(%s) != %s' % (self.boolu(w), 'true' if N & 1 else 'false')
                self.out('if (!(%s)) {' % cond, ind)
                self.emit(pc + 1, D, ind + 1, loops)
                self.out('}', ind)
                pc = D
                continue
            elif op in (0x2E, 0x2F):   # CMP
                ops = ['==', '!=', '<', '<=', '>', '>=']
                ox, oy = (w >> 24) & 7, (w >> 21) & 7
                a = self.src((w >> 12) & 0x7F, rel, sel1, n1)
                b = self.src((w >> 7) & 0x1F, 0, sel2, n2)
                self.out('{ vec4 a = %s; vec4 b = %s; cx = %s; cy = %s; }' % (
                    a, b, 'a.x %s b.x' % ops[ox] if ox < 6 else 'true', 'a.y %s b.y' % ops[oy] if oy < 6 else 'true'), ind)
            else:
                self.out('// opcode %02X at %03X: not handled (the interpreter skips it too)' % (op, pc), ind)
            pc += 1
        self.depth -= 1

    def body(self):
        # the program runs from the entry point until END (or the end of the program memory)
        self.emit(self.cfg['entry'], 4096, 1, 0)
        return self.lines

    def outputs(self):
        """Output registers to vertex attributes (GPUREG_SH_OUTMAP), resolved now."""
        lines = []
        oi = 0
        for k in range(min(7, self.cfg['total'] & 7)):
            while oi < 16 and not (self.cfg['outmask'] >> oi) & 1:
                oi += 1
            if oi >= 16:
                break
            m = self.cfg['outmap'][k]
            for j in range(4):
                s = (m >> (8 * j)) & 0x1F
                src = 'o%d.%s' % (oi, COMP[j])
                if s <= 3:
                    lines.append('pos.%s = %s;' % (COMP[s], src))
                elif s <= 7:
                    lines.append('quat.%s = %s;' % (COMP[s - 4], src))
                elif s <= 11:
                    lines.append('col.%s = %s;' % (COMP[s - 8], src))
                elif s <= 15:
                    lines.append('uv01.%s = %s;' % (COMP[s - 12], src))
                elif s == 16:
                    lines.append('uv2w.z = %s;' % src)
                elif 18 <= s <= 20:
                    lines.append('view.%s = %s;' % (COMP[s - 18], src))
                elif s == 22:
                    lines.append('uv2w.x = %s;' % src)
                elif s == 23:
                    lines.append('uv2w.y = %s;' % src)
            oi += 1
        return lines


GLSL_HEAD = '''// Generated by tools/shaders/pica2glsl.py from one of the game's vertex shaders: not hand-edited,
// not committed. Uniforms and buffers match runtime/gpu/dk/pica_vsh.glsl.
#version 460
layout (std430, binding = 1) readonly buffer Vertices { vec4 vin[]; };
layout (std140, binding = 0) uniform VertexState {
    vec4 f[96];
    uvec4 ints[4];
    uvec4 cfg;
    uvec4 outmap[7];
    uvec4 base;
};
layout (location = 0) out vec4 v_color;
layout (location = 1) out vec4 v_uv01;
layout (location = 2) out vec4 v_uv2w;
layout (location = 3) out vec4 v_quat;
layout (location = 4) out vec4 v_view;
#define bools cfg.x
vec4 pmul(vec4 a, vec4 b) {
    vec4 p = a * b;
    bvec4 z = bvec4(uvec4(equal(a, vec4(0.0))) | uvec4(equal(b, vec4(0.0))));
    return mix(p, vec4(0.0), z);
}
float smul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 fr(int i) { i &= 127; return i < 96 ? f[i] : vec4(1.0); }
int sx8(uint u) { return int(u) - (u >= 128u ? 256 : 0); }
vec4 inp(uint reg) { return reg < base.y ? vin[(base.x + uint(gl_VertexID)) * base.y + reg] : vec4(0.0, 0.0, 0.0, 1.0); }
'''


def regs_decl(used):
    lines = ['vec4 r%d = vec4(0.0);' % k for k in range(16)]
    lines += ['vec4 o%d = vec4(0.0, 0.0, 0.0, 1.0);' % k for k in range(16)]
    lines += ['int a0x = 0;', 'int a0y = 0;', 'int aL = 0;', 'bool cx = false;', 'bool cy = false;']
    return lines


def glsl(cfg):
    t = Translator(cfg)
    body = t.body()
    out = [GLSL_HEAD]
    out += regs_decl(t.used_inputs)
    out += ['vec4 v%d;' % k for k in sorted(t.used_inputs)]
    out.append('void run() {')
    out += body
    out.append('}')
    out.append('void main() {')
    out += ['    v%d = inp(%du);' % (k, k) for k in sorted(t.used_inputs)]
    out.append('    run();')
    out.append('    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0), quat = vec4(0.0, 0.0, 0.0, 1.0), col = vec4(1.0);')
    out.append('    vec4 uv01 = vec4(0.0), uv2w = vec4(0.0), view = vec4(0.0);')
    out += ['    ' + l for l in t.outputs()]
    out.append('    v_color = col; v_uv01 = uv01; v_uv2w = uv2w; v_quat = quat; v_view = view;')
    out.append('    gl_Position = vec4(pos.x, pos.y, -pos.z, pos.w);')
    out.append('}')
    return '\n'.join(out) + '\n'


CPP_HEAD = '''// Generated by tools/shaders/pica2glsl.py: the translated vertex shaders as C++, for the probe's
// check against the reference interpreter. Not hand-edited, not committed.
#include <cmath>
#include <cstdint>
#include <cstring>
typedef uint32_t uint;
struct vec4 {
    float x, y, z, w;
    vec4() : x(0), y(0), z(0), w(0) {}
    explicit vec4(double s) : x((float)s), y((float)s), z((float)s), w((float)s) {}
    vec4(double a, double b, double c, double d) : x((float)a), y((float)b), z((float)c), w((float)d) {}
};
static inline vec4 operator+(vec4 a, vec4 b) { return vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); }
static inline vec4 operator-(vec4 a) { return vec4(-a.x, -a.y, -a.z, -a.w); }
struct uvec4 { uint x, y, z, w; };
// the reference interpreter's multiply: 0 * inf is 0
static inline float fm(float a, float b) { float r = a * b; if (r != r && a == a && b == b) return 0.0f; return r; }
static inline vec4 pmul(vec4 a, vec4 b) { return vec4(fm(a.x, b.x), fm(a.y, b.y), fm(a.z, b.z), fm(a.w, b.w)); }
static inline float smul(float a, float b) { return fm(a, b); }
static inline float max(float a, double b) { return a > (float)b ? a : (float)b; }
static inline float clamp(float a, double lo, double hi) { return a < (float)lo ? (float)lo : a > (float)hi ? (float)hi : a; }
static inline int sx8(uint u) { return (int)u - (u >= 128u ? 256 : 0); }
// state shared with the probe
extern "C" { vec4 msr_chk_f[96]; uvec4 ints[4]; uint bools; }
#define f msr_chk_f
static inline vec4 fr(int i) { i &= 127; return i < 96 ? f[i] : vec4(1.0); }
struct Out { vec4 pos, quat, col, uv01, uv2w, view; };
// float math like the interpreter's
#define exp2 exp2f
#define log2 log2f
#define floor floorf
#define sqrt sqrtf
'''


def cpp(cfg, name):
    t = Translator(cfg)
    body = t.body()
    out = ['namespace %s {' % name]
    out += regs_decl(t.used_inputs)
    out += ['vec4 v%d;' % k for k in range(16)]
    out.append('static void run() {')
    out += body
    out.append('}')
    out.append('static void go(const vec4* in, Out* res) {')
    out += ['    r%d = vec4(0.0);' % k for k in range(16)]
    out += ['    o%d = vec4(0.0, 0.0, 0.0, 1.0);' % k for k in range(16)]
    out.append('    a0x = 0; a0y = 0; aL = 0; cx = false; cy = false;')
    out += ['    v%d = in[%d];' % (k, k) for k in range(16)]
    out.append('    run();')
    out.append('    vec4 pos = vec4(0.0, 0.0, 0.0, 1.0), quat = vec4(0.0, 0.0, 0.0, 1.0), col = vec4(1.0);')
    out.append('    vec4 uv01 = vec4(0.0), uv2w = vec4(0.0), view = vec4(0.0);')
    out += ['    ' + l for l in t.outputs()]
    out.append('    res->pos = pos; res->quat = quat; res->col = col; res->uv01 = uv01; res->uv2w = uv2w; res->view = view;')
    out.append('}')
    out.append('}')
    return '\n'.join(out) + '\n'


def main():
    if len(sys.argv) < 3:
        raise SystemExit('usage: pica2glsl.py <dump> <out dir>')
    cfgs = read_dump(sys.argv[1])
    od = sys.argv[2]
    os.makedirs(od, exist_ok=True)
    ok, bad = [], []
    cpp_parts = [CPP_HEAD]
    for key, cfg in sorted(cfgs.items()):
        try:
            src = glsl(cfg)
            cpp_parts.append(cpp(cfg, 's_%016x' % key))
        except Untranslatable as e:
            bad.append((key, str(e)))
            continue
        open(os.path.join(od, 'vs_%016x.glsl' % key), 'w').write(src)
        ok.append(key)
    table = ['extern "C" int msr_chk_run(uint64_t key, const vec4* in, Out* res) {', '    switch (key) {']
    table += ['    case 0x%016xull: s_%016x::go(in, res); return 1;' % (k, k) for k in ok]
    table += ['    }', '    return 0;', '}']
    cpp_parts.append('\n'.join(table) + '\n')
    open(os.path.join(od, 'check.cpp'), 'w').write('\n'.join(cpp_parts))
    open(os.path.join(od, 'keys.txt'), 'w').write(''.join('%016x\n' % k for k in ok))
    print('%d configurations: %d translated, %d left to the interpreter' % (len(cfgs), len(ok), len(bad)))
    for k, why in bad:
        print('  %016x: %s' % (k, why))


if __name__ == '__main__':
    main()
