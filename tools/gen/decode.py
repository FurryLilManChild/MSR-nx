"""Walks every function of code.bin from its entry and decodes its instructions.

Input: code.bin, layout.json (from extract) and a function list CSV (address, mode, found_by),
such as msr2ds's analysis output. Output: a Program with, per function, its instructions in
address order and the set of block leaders. This is MSR-nx's own decoder;
it reads the function list as data and does not run any upstream code.
"""
import csv
import json
import pathlib
import struct

import capstone
from capstone import arm as A

TEXT_BASE = 0x00100000


class Insn:
    __slots__ = ('addr', 'size', 'thumb', 'mnem', 'ops', 'id', 'cc', 'update_flags', 'writeback',
                 'operands', 'regs_read', 'regs_write', 'raw', 'op_str', 'post_index')

    def __init__(self, ci, thumb):
        self.addr = ci.address
        self.size = ci.size
        self.thumb = thumb
        self.mnem = ci.mnemonic
        self.op_str = ci.op_str
        self.id = ci.id
        self.cc = ci.cc
        self.update_flags = ci.update_flags
        self.writeback = ci.writeback
        self.operands = list(ci.operands)
        self.raw = bytes(ci.bytes)
        self.post_index = ci.post_index


class Function:
    def __init__(self, addr, thumb):
        self.addr = addr
        self.thumb = thumb
        self.insns = {}        # addr -> Insn
        self.leaders = {addr}  # block starts
        self.calls = set()     # (target, thumb)
        self.tailcalls = set()
        self.ijumps = []       # addresses of unresolved indirect jumps
        self.tables = {}       # addr of table jump -> list of targets


class Program:
    def __init__(self, code, layout, entries):
        self.code = code
        self.layout = layout
        self.text_end = TEXT_BASE + layout['text']['size']
        self.entries = entries  # addr -> thumb
        self.md_arm = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_ARM)
        self.md_thumb = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)
        for md in (self.md_arm, self.md_thumb):
            md.detail = True
        self.functions = {}

    def word(self, addr):
        return struct.unpack_from('<I', self.code, addr - TEXT_BASE)[0]

    def in_text(self, addr):
        return TEXT_BASE <= addr < self.text_end

    def decode_one(self, addr, thumb):
        md = self.md_thumb if thumb else self.md_arm
        off = addr - TEXT_BASE
        for ci in md.disasm(self.code[off:off + 4], addr, 1):
            return Insn(ci, thumb)
        return None

    def walk(self, fn):
        """Recursive descent inside one function. Stops at returns, at branches to other
        function entries (tail calls) and at unresolved indirect jumps."""
        work = [fn.addr]
        while work:
            pc = work.pop()
            while True:
                if pc in fn.insns or not self.in_text(pc):
                    break
                if pc != fn.addr and pc in self.entries and pc not in fn.leaders:
                    # Fell into another function's entry: treat as a tail call.
                    fn.tailcalls.add((pc, self.entries[pc]))
                    break
                ins = self.decode_one(pc, fn.thumb)
                if ins is None:
                    break
                fn.insns[pc] = ins
                nxt = pc + ins.size
                kind, targets = classify(ins, self)
                if kind == 'seq':
                    pc = nxt
                    continue
                if kind == 'call':
                    for t in targets:
                        fn.calls.add(t)
                    pc = nxt
                    continue
                if kind in ('jump', 'cjump'):
                    for t, thumb in targets:
                        if t in self.entries and t != fn.addr:
                            fn.tailcalls.add((t, self.entries[t]))
                        elif thumb == fn.thumb:
                            fn.leaders.add(t)
                            work.append(t)
                    if kind == 'cjump' or ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                        fn.leaders.add(nxt)
                        pc = nxt
                        continue
                    break
                if kind == 'table':
                    fn.tables[pc] = targets
                    for t, _ in targets:
                        fn.leaders.add(t)
                        work.append(t)
                    break
                if kind == 'return':
                    if ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                        fn.leaders.add(nxt)
                        pc = nxt
                        continue
                    break
                if kind == 'ijump':
                    fn.ijumps.append(pc)
                    if ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                        fn.leaders.add(nxt)
                        pc = nxt
                        continue
                    break
                if kind == 'stop':
                    break
                raise AssertionError(kind)

    def run(self):
        for addr, thumb in sorted(self.entries.items()):
            fn = Function(addr, thumb)
            self.walk(fn)
            self.functions[addr] = fn
        return self


def writes_pc(ins):
    for op in ins.operands:
        if op.type == A.ARM_OP_REG and op.reg == A.ARM_REG_PC and op.access & capstone.CS_AC_WRITE:
            return True
    if ins.mnem.startswith(('pop', 'ldm')) and 'pc' in ins.op_str:
        return True
    return False


def classify(ins, prog):
    """Returns (kind, targets). kind is seq, call, jump, cjump, table, return, ijump or stop."""
    m = ins.id
    ops = ins.operands
    if m in (A.ARM_INS_BL, A.ARM_INS_BLX):
        if ops and ops[0].type == A.ARM_OP_IMM:
            t = ops[0].imm & 0xFFFFFFFF
            thumb = ins.thumb if m == A.ARM_INS_BL else not ins.thumb
            return 'call', [(t, thumb)]
        return 'call', []  # indirect call: blx reg
    if m in (A.ARM_INS_B,):
        t = ops[0].imm & 0xFFFFFFFF
        cond = ins.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID)
        return ('cjump' if cond else 'jump'), [(t, ins.thumb)]
    if m in (A.ARM_INS_CBZ, A.ARM_INS_CBNZ):
        return 'cjump', [(ops[1].imm & 0xFFFFFFFF, True)]
    if m == A.ARM_INS_BX:
        if ops[0].reg == A.ARM_REG_LR:
            return 'return', []
        return 'ijump', []
    if m in (A.ARM_INS_TBB, A.ARM_INS_TBH):
        return 'table', table_targets_thumb(ins, prog)
    if m == A.ARM_INS_UDF or (m == A.ARM_INS_SVC and False):
        return 'stop', []
    if writes_pc(ins):
        if m in (A.ARM_INS_POP, A.ARM_INS_LDM) or (m == A.ARM_INS_LDR and ops[1].mem.base == A.ARM_REG_SP):
            return 'return', []
        if m == A.ARM_INS_MOV and ops[1].type == A.ARM_OP_REG and ops[1].reg == A.ARM_REG_LR:
            return 'return', []
        t = arm_table_targets(ins, prog)
        if t:
            return 'table', t
        return 'ijump', []
    return 'seq', []


def arm_table_targets(ins, prog):
    """ARM jump tables: `ldrls pc, [pc, rX, lsl #2]` preceded by `cmp rX, #n`, with the table
    right after; also `addls pc, pc, rX, lsl #2` followed by a run of branches."""
    ops = ins.operands
    if ins.id == A.ARM_INS_LDR and ops[1].type == A.ARM_OP_MEM and ops[1].mem.base == A.ARM_REG_PC:
        n = table_bound(ins, prog)
        if n is None:
            return None
        base = ins.addr + 8
        return [(prog.word(base + 4 * i) & 0xFFFFFFFE, False) for i in range(n + 1)] + \
            [(ins.addr + 4, False)]
    if ins.id == A.ARM_INS_ADD and len(ops) >= 3 and ops[1].type == A.ARM_OP_REG and \
            ops[1].reg == A.ARM_REG_PC and ops[2].type == A.ARM_OP_REG:
        n = table_bound(ins, prog)
        if n is None:
            return None
        base = ins.addr + 8
        return [(base + 4 * i, False) for i in range(n + 1)] + [(ins.addr + 4, False)]
    return None


def table_bound(ins, prog):
    """Finds `cmp rX, #n` among the few instructions before a table jump."""
    idx_reg = None
    ops = ins.operands
    if ins.id == A.ARM_INS_LDR:
        idx_reg = ops[1].mem.index
    else:
        idx_reg = ops[2].reg
    for back in range(1, 5):
        p = prog.decode_one(ins.addr - 4 * back, False)
        if p and p.id == A.ARM_INS_CMP and p.operands[0].reg == idx_reg and \
                p.operands[1].type == A.ARM_OP_IMM:
            return p.operands[1].imm
    return None


def table_targets_thumb(ins, prog):
    n = None
    idx = ins.operands[0].mem.index
    for back in range(2, 12, 2):
        p = prog.decode_one(ins.addr - back, True)
        if p and p.id == A.ARM_INS_CMP and p.operands[0].reg == idx and p.operands[1].type == A.ARM_OP_IMM:
            n = p.operands[1].imm
            break
    if n is None:
        return []
    base = ins.addr + 4
    out = []
    for i in range(n + 1):
        if ins.id == A.ARM_INS_TBB:
            off = prog.code[base + i - TEXT_BASE]
        else:
            off = struct.unpack_from('<H', prog.code, base + 2 * i - TEXT_BASE)[0]
        out.append((base + 2 * off, True))
    return out


def load(extract_dir, functions_csv, extra_entries=None):
    d = pathlib.Path(extract_dir)
    code = (d / 'code.bin').read_bytes()
    layout = json.loads((d / 'layout.json').read_text())
    entries = {}
    with open(functions_csv) as f:
        for row in csv.DictReader(f):
            entries[int(row['address'], 16)] = row['mode'] == 'thumb'
    if extra_entries:
        for line in pathlib.Path(extra_entries).read_text().splitlines():
            line = line.split('#')[0].strip()
            if line:
                a = int(line, 16)
                entries.setdefault(a & ~1, bool(a & 1))
    return Program(code, layout, entries)
