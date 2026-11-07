"""Generates C for every function of the game.

Each guest function becomes `void F_xxxxxxxx(Cpu* c)`: branches inside it are gotos, direct calls
are real C calls (followed by a check that the callee returned where the guest expects; if not,
control unwinds to the runtime, which looks up c->pc), returns are real C returns, jump tables are
switches. Output is split into units of about 5 MB for the compiler.

Usage: python3 generate.py <extract dir> <functions.csv> <out dir> [extra_entries.txt]
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode  # noqa: E402
import emit  # noqa: E402
import regs  # noqa: E402

UNIT_BYTES = 5 * 1024 * 1024


def fname(addr):
    return 'F_%08X' % addr


class FnHooks:
    def __init__(self, prog, fn, stats):
        self.prog = prog
        self.fn = fn
        self.stats = stats

    def leave(self, e, target_expr):
        e.line(regs.sync_out())
        e.line('c->pc = %s; return;' % target_expr)

    def tail(self, e, target):
        e.line(regs.sync_out())
        e.line('%s(c); return;' % fname(target & ~1))
        self.stats['tailcalls'] += 1

    def branch(self, e, target):
        if target in self.fn.insns and (target != self.fn.addr or True):
            e.line('goto L_%08X;' % target)
        elif target in self.prog.entries:
            self.tail(e, target)
        else:
            self.unknown(e, 'branch to undecoded %08X' % target)

    def call(self, e, target, ret):
        a = target & ~1
        e.line(regs.sync_out())
        if a in self.prog.entries:
            e.line('%s(c);' % fname(a))
        else:
            e.line('msr_call_indirect(c, %s);' % emit.h(target))
            self.stats['calls_to_unknown'] += 1
        e.line('if (__builtin_expect(c->pc != %s, 0)) { msr_unwind(c, %s, %s); return; }'
               % (emit.h(ret), emit.h(e.ins.addr), emit.h(ret)))
        e.line(regs.sync_in())

    def call_indirect(self, e, t, ret):
        e.line(regs.sync_out())
        e.line('msr_call_indirect(c, %s);' % t)
        e.line('if (__builtin_expect(c->pc != %s, 0)) { msr_unwind(c, %s, %s); return; }'
               % (emit.h(ret), emit.h(e.ins.addr), emit.h(ret)))
        e.line(regs.sync_in())
        self.stats['icalls'] += 1

    def bx(self, e, t, is_lr):
        if is_lr:
            self.leave(e, t)
        else:
            e.line(regs.sync_out())
            e.line('msr_jump_indirect(c, %s); return;' % t)
            self.stats['ijumps'] += 1

    def write_pc(self, e, t, is_load):
        addr = e.ins.addr
        if addr in self.fn.tables:
            targets = sorted({x for x, _ in self.fn.tables[addr]})
            e.line('switch (%s) {' % t)
            for x in targets:
                if x in self.fn.insns:
                    e.line('  case %s: goto L_%08X;' % (emit.h(x), x))
            e.line('  default: break;')
            e.line('}')
            self.unknown(e, 'jump table %08X out of range' % addr)
            self.stats['tables'] += 1
            return
        if not is_load and not e.ins.thumb:
            t = '(%s & ~3u)' % t
        if is_load:
            self.leave(e, t)   # pop {pc} / ldr pc, [sp], #4: a return
        else:
            # mov pc, lr is a return; anything else goes through the runtime.
            if e.ins.id == emit.A.ARM_INS_MOV and e.ins.operands[1].type == emit.A.ARM_OP_REG and \
                    e.ins.operands[1].reg == emit.A.ARM_REG_LR:
                self.leave(e, t)
            else:
                e.line(regs.sync_out())
                e.line('msr_jump_indirect(c, %s); return;' % t)
                self.stats['ijumps'] += 1

    def svc(self, e, imm):
        nxt = (e.ins.addr + e.ins.size) | (1 if e.ins.thumb else 0)
        e.line(regs.sync_out())
        e.line('c->pc = %s; msr_svc(c, 0x%X);' % (emit.h(nxt), imm))
        e.line(regs.sync_in())
        self.stats['svc'] += 1

    def unknown(self, e, what):
        e.line(regs.sync_out())
        e.line('msr_unknown(c, %s, "%s"); return;' % (emit.h(e.ins.addr), what))


def gen_function(prog, fn, stats, problems):
    out = ['void %s(Cpu* __restrict c) {' % fname(fn.addr)]
    out += ['  ' + l for l in regs.declare_in()]
    hooks = FnHooks(prog, fn, stats)
    addrs = sorted(fn.insns)
    if addrs and addrs[0] != fn.addr:
        # Code reached by a branch lies below the entry: start at the entry, not the lowest address.
        out.append('  goto L_%08X;' % fn.addr)
        stats['entry_not_first'] += 1
    for k, a in enumerate(addrs):
        ins = fn.insns[a]
        out.append('L_%08X: // %s %s' % (a, ins.mnem, ins.op_str))
        try:
            body = emit.emit(ins, hooks)
        except emit.Unsupported as ex:
            problems[str(ex)] += 1
            e = emit.Emit(ins, hooks)
            hooks.unknown(e, 'untranslated %s' % ins.mnem)
            body = ['{'] + ['  ' + l for l in e.out] + ['}']
        out += ['  ' + l for l in body]
        nxt = a + ins.size
        following = addrs[k + 1] if k + 1 < len(addrs) else None
        if following != nxt and can_fall_through(ins):
            if nxt in fn.insns:
                out.append('  goto L_%08X;' % nxt)
            elif nxt in prog.entries:
                e = emit.Emit(ins, hooks)
                hooks.tail(e, nxt)
                out += ['  ' + l for l in e.out]
            else:
                e = emit.Emit(ins, hooks)
                hooks.unknown(e, 'fell off the end')
                out += ['  ' + l for l in e.out]
                stats['fall_off'] += 1
    out.append('}')
    # Labels nobody jumps to are fine; silence the warning in the build flags.
    return out


def can_fall_through(ins):
    """False for instructions that always leave (unconditional branch, return, table)."""
    from capstone import arm as A
    if ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
        return True
    if ins.id in (A.ARM_INS_B, A.ARM_INS_BX):
        return False
    if decode.writes_pc(ins):
        return False
    if ins.id == A.ARM_INS_UDF:
        return False
    return True


def main():
    extract, functions_csv, out_dir = sys.argv[1:4]
    extra = sys.argv[4] if len(sys.argv) > 4 else None
    prog = decode.load(extract, functions_csv, extra).run()
    os.makedirs(out_dir, exist_ok=True)
    stats = collections.Counter()
    problems = collections.Counter()
    header = ['#include "msr_cpu.h"', '#define DECL(a) void a(Cpu* __restrict c);']
    decls = ['void %s(Cpu* __restrict c);' % fname(a) for a in sorted(prog.functions)]
    open(os.path.join(out_dir, 'functions.h'), 'w').write('#pragma once\n#include "msr_cpu.h"\n' + '\n'.join(decls) + '\n')
    unit, size, n_units = [], 0, 0

    def flush():
        nonlocal unit, size, n_units
        if not unit:
            return
        path = os.path.join(out_dir, 'unit_%03d.c' % n_units)
        open(path, 'w').write('#include "functions.h"\n' + '\n'.join(unit) + '\n')
        n_units += 1
        unit, size = [], 0

    for a in sorted(prog.functions):
        lines = gen_function(prog, prog.functions[a], stats, problems)
        text = '\n'.join(lines)
        unit.append(text)
        size += len(text)
        if size >= UNIT_BYTES:
            flush()
    flush()
    # Function table for the runtime: sorted addresses (bit 0 = Thumb) and pointers.
    tab = ['#include "functions.h"', 'const unsigned msr_fn_count = %d;' % len(prog.functions),
           'const u32 msr_fn_addr[] = {']
    tab += ['  0x%08Xu,' % (a | (1 if prog.functions[a].thumb else 0)) for a in sorted(prog.functions)]
    tab += ['};', 'void (*const msr_fn_ptr[])(Cpu* __restrict) = {']
    tab += ['  %s,' % fname(a) for a in sorted(prog.functions)]
    tab += ['};']
    open(os.path.join(out_dir, 'table.c'), 'w').write('\n'.join(tab) + '\n')
    print('functions %d, units %d' % (len(prog.functions), n_units))
    print('stats', dict(stats))
    print('untranslated', sum(problems.values()))
    for k, v in problems.most_common(30):
        print('  ', v, k)


if __name__ == '__main__':
    main()
