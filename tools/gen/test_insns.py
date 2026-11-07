"""Checks the instruction translator against Unicorn, one instruction at a time.

For a sample of every distinct instruction the game uses, builds a C function holding just that
instruction, runs it on random register, flag, VFP and memory states, runs the same raw bytes in
Unicorn (ARM1176, the closest model to the 3DS ARM11) on identical states, and compares every
register, flag, VFP register, FPSCR flag, written memory byte and the next pc.

Usage: python3 test_insns.py <extract dir> <functions.csv> [samples per instruction] [states]
"""
import collections
import ctypes
import os
import random
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode  # noqa: E402
import emit  # noqa: E402
import regs  # noqa: E402

import unicorn  # noqa: E402
from unicorn import arm_const as U  # noqa: E402
from capstone import arm as A  # noqa: E402

RUNTIME = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'runtime', 'cpu')
DATA_LO, DATA_HI = 0x08000000, 0x08010000   # where address-like register values point


class TestHooks:
    """Control transfers in a one-instruction test: record where control goes and stop."""

    def __init__(self):
        pass

    def exit(self, e, target_expr):
        e.line(regs.sync_out())
        e.line('c->pc = %s; return 1;' % target_expr)

    def write_pc(self, e, t, is_load):
        if e.ins.thumb or is_load:
            # ARMv6: loads to pc interwork (bit 0 selects Thumb); data processing in ARM mode does not.
            self.exit(e, t)
        else:
            self.exit(e, '(%s & ~3u)' % t)

    def branch(self, e, target):
        self.exit(e, emit.h(target | (1 if e.ins.thumb else 0)))

    def call(self, e, target, ret):
        self.exit(e, emit.h(target))

    def call_indirect(self, e, t, ret):
        self.exit(e, t)

    def bx(self, e, t, is_lr):
        self.exit(e, t)

    def svc(self, e, imm):
        raise emit.Unsupported('svc in test')

    def unknown(self, e, what):
        raise emit.Unsupported(what)


def build_tests(insns):
    lines = ['#include "msr_cpu.h"', 'u8* g_mem;',
             'void msr_svc(Cpu* c, u32 imm) {} void msr_call_indirect(Cpu* c, u32 t) {}',
             'void msr_jump_indirect(Cpu* c, u32 t) {} void msr_unknown(Cpu* c, u32 a, const char* w) {}',
             'void set_mem(u8* m) { g_mem = m; }']
    kept = []
    skipped = collections.Counter()
    for k, ins in enumerate(insns):
        try:
            body = emit.emit(ins, TestHooks())
        except emit.Unsupported as ex:
            skipped[str(ex).split(' ')[0] + ' ' + ins.mnem.split('.')[0]] += 1
            continue
        nxt = (ins.addr + ins.size) | (1 if ins.thumb else 0)
        lines.append('int T%d(Cpu* c) { // %08X %s %s' % (k, ins.addr, ins.mnem, ins.op_str))
        lines += ['  ' + l for l in regs.declare_in()]
        lines += ['  ' + l for l in body]
        lines.append('  ' + regs.sync_out())
        lines.append('  c->pc = %s; return 0;' % emit.h(nxt))
        lines.append('}')
        kept.append((k, ins))
    return '\n'.join(lines) + '\n', kept, skipped


def compile_so(src, workdir):
    c_path = os.path.join(workdir, 'tests.c')
    so_path = os.path.join(workdir, 'tests.so')
    open(c_path, 'w').write(src)
    subprocess.check_call(['gcc', '-O0', '-shared', '-fPIC', '-ffp-contract=off', '-fno-strict-aliasing',
                           '-w', '-I', RUNTIME, c_path, '-o', so_path])
    return ctypes.CDLL(so_path)


class CpuStruct(ctypes.Structure):
    _fields_ = [('r', ctypes.c_uint32 * 16), ('n', ctypes.c_uint32), ('z', ctypes.c_uint32),
                ('c', ctypes.c_uint32), ('v', ctypes.c_uint32), ('q', ctypes.c_uint32),
                ('ge', ctypes.c_uint32), ('pc', ctypes.c_uint32), ('fpscr', ctypes.c_uint32),
                ('tls', ctypes.c_uint32), ('excl_addr', ctypes.c_uint32),
                ('excl_valid', ctypes.c_uint32), ('pad', ctypes.c_uint32),
                ('s', ctypes.c_uint32 * 32)]


def rand_val(rng):
    k = rng.random()
    if k < 0.35:
        return rng.randrange(DATA_LO, DATA_HI) & ~3
    if k < 0.55:
        return rng.randrange(0, 64)
    if k < 0.65:
        return rng.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 31, 32, 33])
    return rng.getrandbits(32)


def rand_float(rng):
    k = rng.random()
    if k < 0.1:
        return rng.choice([0, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000])
    f = rng.uniform(-1e6, 1e6) if k < 0.8 else rng.uniform(-4, 4)
    return struct.unpack('<I', struct.pack('<f', f))[0]


def is_nan32(x):
    return (x & 0x7F800000) == 0x7F800000 and (x & 0x7FFFFF) != 0


def nan_only(d):
    """A VFP register difference where both sides are NaN: x86 and ARM pick different NaN
    signs and payloads; the Switch (AArch64) follows ARM's default NaN."""
    parts = d.split()
    if len(parts) == 3 and parts[0].startswith('s') and parts[1].startswith('c=') and parts[2].startswith('uc='):
        return is_nan32(int(parts[1][2:], 16)) and is_nan32(int(parts[2][3:], 16))
    return False


def run_case(lib, k, ins, state, mem_c, uc):
    rvals, flags, svals, membytes = state
    # --- Unicorn
    uc.mem_write(DATA_LO - 0x10000, membytes)
    n, z, cf, v, q, ge = flags
    cpsr = (n << 31) | (z << 30) | (cf << 29) | (v << 28) | (q << 27) | (ge << 16) | 0x10
    if ins.thumb:
        cpsr |= 0x20
    uc.reg_write(U.UC_ARM_REG_CPSR, cpsr)
    for i in range(13):
        uc.reg_write(U.UC_ARM_REG_R0 + i, rvals[i])
    uc.reg_write(U.UC_ARM_REG_SP, rvals[13])
    uc.reg_write(U.UC_ARM_REG_LR, rvals[14])
    for i in range(32):
        uc.reg_write(U.UC_ARM_REG_S0 + i, svals[i])
    uc.reg_write(U.UC_ARM_REG_FPSCR, 0)
    writes = []
    before = {}

    def on_write(u, access, address, size, value, data):
        writes.append((address, size))
        if address not in before and address + size <= (1 << 32):
            before[address] = bytes(u.mem_read(address, size))
    hk = uc.hook_add(unicorn.UC_HOOK_MEM_WRITE, on_write)
    try:
        uc.emu_start(ins.addr | (1 if ins.thumb else 0), ins.addr + 0x100, count=1)
        fault = None
    except unicorn.UcError as ex:
        fault = str(ex)
    uc.hook_del(hk)
    if fault:
        for a, sz in writes:
            uc.mem_write(a, before.get(a, bytes(sz)))
        return 'skip'
    u_r = [uc.reg_read(U.UC_ARM_REG_R0 + i) for i in range(13)] + \
        [uc.reg_read(U.UC_ARM_REG_SP), uc.reg_read(U.UC_ARM_REG_LR)]
    u_cpsr = uc.reg_read(U.UC_ARM_REG_CPSR)
    u_pc = uc.reg_read(U.UC_ARM_REG_PC) | (1 if u_cpsr & 0x20 else 0)
    u_s = [uc.reg_read(U.UC_ARM_REG_S0 + i) for i in range(32)]
    u_fpscr = uc.reg_read(U.UC_ARM_REG_FPSCR)
    try:
        u_mem = {a: uc.mem_read(a, s) for a, s in writes}
    except unicorn.UcError:
        for a, sz in writes:
            if a + sz <= (1 << 32):
                uc.mem_write(a, before.get(a, bytes(sz)))
        return 'skip'
    if any(a + s > (1 << 32) or (a < DATA_LO - 0x10000 and a >= decode.TEXT_BASE) for a, s in writes):
        pass
    # --- C
    base = DATA_LO - 0x10000
    ctypes.memmove(mem_c + base, membytes, len(membytes))
    cs = CpuStruct()
    for i in range(15):
        cs.r[i] = rvals[i]
    cs.n, cs.z, cs.c, cs.v, cs.q, cs.ge = flags
    for i in range(32):
        cs.s[i] = svals[i]
    cs.fpscr = 0
    getattr(lib, 'T%d' % k)(ctypes.byref(cs))
    diffs = []
    for i in range(15):
        if cs.r[i] != u_r[i]:
            diffs.append('r%d c=%08X uc=%08X' % (i, cs.r[i], u_r[i]))
    for name, bit, val in (('N', 31, cs.n), ('Z', 30, cs.z), ('C', 29, cs.c), ('V', 28, cs.v),
                           ('Q', 27, cs.q)):
        if ((u_cpsr >> bit) & 1) != (val & 1):
            diffs.append('%s c=%d uc=%d' % (name, val & 1, (u_cpsr >> bit) & 1))
    if ((u_cpsr >> 16) & 0xF) != cs.ge:
        diffs.append('GE c=%X uc=%X' % (cs.ge, (u_cpsr >> 16) & 0xF))
    if cs.pc != u_pc:
        diffs.append('pc c=%08X uc=%08X' % (cs.pc, u_pc))
    for i in range(32):
        if cs.s[i] != u_s[i]:
            diffs.append('s%d c=%08X uc=%08X' % (i, cs.s[i], u_s[i]))
    if (cs.fpscr >> 28) != (u_fpscr >> 28):
        diffs.append('fpscr c=%X uc=%X' % (cs.fpscr >> 28, u_fpscr >> 28))
    for a, data in u_mem.items():
        cbytes = ctypes.string_at(mem_c + a, len(data))
        if cbytes != bytes(data):
            diffs.append('mem %08X c=%s uc=%s' % (a, cbytes.hex(), bytes(data).hex()))
    # undo this case's writes on both sides so the next case starts from the same memory
    for a, sz in writes:
        orig = before.get(a, bytes(sz))
        uc.mem_write(a, orig)
        ctypes.memmove(mem_c + a, orig, sz)
    diffs = [d for d in diffs if not nan_only(d)]
    if diffs:
        diffs.append('regs ' + ' '.join('%08X' % v for v in rvals))
    return diffs


def main():
    extract, functions_csv = sys.argv[1], sys.argv[2]
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    states = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    rng = random.Random(1234)
    prog = decode.load(extract, functions_csv).run()
    by_kind = collections.defaultdict(list)
    for fn in prog.functions.values():
        for ins in fn.insns.values():
            key = (ins.thumb, ins.mnem.split('.')[0].rstrip('seqnhilotcvgmpla') or ins.mnem, ins.id)
            by_kind[(ins.thumb, ins.id, ins.update_flags, ins.cc == A.ARM_CC_AL)].append(ins)
    sample = []
    for key, lst in sorted(by_kind.items(), key=lambda kv: str(kv[0])):
        rng.shuffle(lst)
        seen = set()
        taken = 0
        for ins in lst:
            sig = (ins.op_str.count(','), '!' in ins.op_str, '], ' in ins.op_str, '-' in ins.op_str,
                   'pc' in ins.op_str, tuple(o.type for o in ins.operands))
            if sig in seen:
                continue
            seen.add(sig)
            sample.append(ins)
            taken += 1
            if taken >= per:
                break
        # plus a few more of the same shapes with other registers
        for ins in lst[:per]:
            if ins not in sample:
                sample.append(ins)
    code_bytes = prog.code
    del prog, by_kind
    import gc
    gc.collect()
    src, kept, skipped = build_tests(sample)
    work = tempfile.mkdtemp(prefix='msrtest')
    print('compiling', len(kept), 'test functions', flush=True)
    lib = compile_so(src, work)
    # 4 GiB window for the C side, like the real runtime (lazily backed by the OS).
    libc = ctypes.CDLL(None)
    libc.mmap.restype = ctypes.c_void_p
    libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
    mem_c = libc.mmap(None, (1 << 32) + 0x10000, 3, 0x22 | 0x4000, -1, 0)
    lib.set_mem(ctypes.c_void_p(mem_c))
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    try:
        uc.ctl_set_cpu_model(U.UC_CPU_ARM_1176)
    except Exception:
        pass
    uc.mem_map(0, 1 << 32)
    uc.mem_write(decode.TEXT_BASE, code_bytes)
    ctypes.memmove(mem_c + decode.TEXT_BASE, code_bytes, len(code_bytes))
    # Enable the VFP: CPACR full access to cp10/cp11, FPEXC.EN.
    # mrc p15,0,r0,c1,c0,2; orr r0,r0,#0xF00000; mcr p15,0,r0,c1,c0,2; mov r0,#0x40000000;
    # vmsr fpexc, r0 (run once in the privileged mode Unicorn starts in)
    setup = struct.pack('<5I', 0xEE110F50, 0xE380060F, 0xEE010F50, 0xE3A00101, 0xEEE80A10)
    uc.mem_write(0x1000, setup)
    uc.emu_start(0x1000, 0x1000 + len(setup))
    fails = collections.defaultdict(list)
    passed = skipped_cases = 0
    print('testing', len(kept), 'instructions', flush=True)
    for n_done, (k, ins) in enumerate(kept):
        if n_done % 50 == 0:
            print('  progress', n_done, 'of', len(kept), 'fails so far', len(fails), flush=True)
        for s in range(states):
            rvals = [rand_val(rng) for _ in range(15)]
            rvals[13] = rng.randrange(DATA_LO + 0x1000, DATA_HI - 0x1000) & ~7
            flags = tuple(rng.getrandbits(1) for _ in range(5)) + (rng.getrandbits(4),)
            svals = [rand_float(rng) for _ in range(32)]
            membytes = rng.randbytes(0x20000) if s == 0 else membytes
            res = run_case(lib, k, ins, (rvals, flags, svals, membytes), mem_c, uc)
            if res == 'skip':
                skipped_cases += 1
                continue
            if res:
                fails[(ins.thumb, ins.mnem, ins.op_str, hex(ins.addr))].append(res)
            else:
                passed += 1
    print('instructions tested %d, untranslated %d, cases passed %d, skipped (fault) %d, failing insns %d'
          % (len(kept), sum(skipped.values()), passed, skipped_cases, len(fails)))
    for key, cnt in skipped.most_common(40):
        print('  untranslated:', key, cnt)
    for key, lst in list(fails.items())[:60]:
        print('  FAIL', key, len(lst), lst[0][:3], lst[0][-1])


if __name__ == '__main__':
    main()
