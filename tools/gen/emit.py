"""Translates decoded ARM11 (ARMv6K ARM, Thumb-1, VFPv2) instructions into C.

Each instruction becomes a few C statements over locals r0..r14 (r13 = sp, r14 = lr), flags N Z
C V Q, GE, and the Cpu pointer c for VFP registers and FPSCR. Control flow is handed back to the
caller through an `Emit` context, so the same code serves whole functions and the one-instruction
test harness.
"""
import struct

import capstone
from capstone import arm as A

COND = {
    A.ARM_CC_EQ: 'Z', A.ARM_CC_NE: '!Z', A.ARM_CC_HS: 'C', A.ARM_CC_LO: '!C',
    A.ARM_CC_MI: 'N', A.ARM_CC_PL: '!N', A.ARM_CC_VS: 'V', A.ARM_CC_VC: '!V',
    A.ARM_CC_HI: '(C && !Z)', A.ARM_CC_LS: '(!C || Z)', A.ARM_CC_GE: '(N == V)',
    A.ARM_CC_LT: '(N != V)', A.ARM_CC_GT: '(!Z && N == V)', A.ARM_CC_LE: '(Z || N != V)',
}

GPR = {getattr(A, 'ARM_REG_R%d' % i): i for i in range(13)}
GPR[A.ARM_REG_SP] = 13
GPR[A.ARM_REG_LR] = 14
GPR[A.ARM_REG_PC] = 15
# capstone names r9..r12 sb, sl, fp, ip
for name, i in (('SB', 9), ('SL', 10), ('FP', 11), ('IP', 12)):
    if hasattr(A, 'ARM_REG_' + name):
        GPR[getattr(A, 'ARM_REG_' + name)] = i

SREG = {getattr(A, 'ARM_REG_S%d' % i): i for i in range(32)}
DREG = {getattr(A, 'ARM_REG_D%d' % i): i for i in range(32)}

SFT_IMM = {A.ARM_SFT_LSL: 'lsl', A.ARM_SFT_LSR: 'lsr', A.ARM_SFT_ASR: 'asr', A.ARM_SFT_ROR: 'ror'}
SFT_REG = {A.ARM_SFT_LSL_REG: 'lsl', A.ARM_SFT_LSR_REG: 'lsr', A.ARM_SFT_ASR_REG: 'asr',
           A.ARM_SFT_ROR_REG: 'ror'}


class Unsupported(Exception):
    pass


def h(v):
    return '0x%Xu' % (v & 0xFFFFFFFF)


class Emit:
    """Collects C lines for one instruction. Control transfers go through the hooks."""

    def __init__(self, ins, hooks):
        self.ins = ins
        self.hooks = hooks
        self.out = []
        self.tmp = 0

    def line(self, s):
        self.out.append(s)

    def new(self, ctype='u32'):
        self.tmp += 1
        return 't%d' % self.tmp

    # ---- operands -------------------------------------------------------------------------
    def pc_value(self):
        return self.ins.addr + (4 if self.ins.thumb else 8)

    def r(self, reg):
        i = GPR.get(reg)
        if i is None:
            raise Unsupported('register %s' % reg)
        if i == 15:
            return h(self.pc_value())
        return 'r%d' % i

    def ri(self, reg):
        i = GPR.get(reg)
        if i is None:
            raise Unsupported('register %s' % reg)
        return i

    def shifted(self, op, carry):
        """Value of a register operand with its shift. carry: name of a carry variable to update
        (logical flag-setting instructions) or None."""
        x = self.r(op.reg)
        st, sv = op.shift.type, op.shift.value
        if st == A.ARM_SFT_INVALID or (st == A.ARM_SFT_LSL and sv == 0):
            return x
        if st == A.ARM_SFT_RRX:
            if carry:
                return 'rrx_c(%s, &%s)' % (x, carry)
            return '((%s >> 1) | (C << 31))' % x
        if st in SFT_IMM:
            fn = SFT_IMM[st]
            if carry:
                return '%s_c(%s, %d, &%s)' % (fn, x, sv, carry)
            return '%s_(%s, %d)' % (fn, x, sv)
        if st in SFT_REG:
            fn = SFT_REG[st]
            amt = '(%s & 0xFF)' % self.r(sv)
            if fn == 'ror' and not carry:
                return 'ror_(%s, %s)' % (x, amt)
            if carry:
                return '%s_c(%s, %s, &%s)' % (fn, x, amt, carry)
            return '%s_(%s, %s)' % (fn, x, amt)
        raise Unsupported('shift %d' % st)

    def imm_carry(self):
        """Carry out of an ARM modified immediate: bit 31 of the value when rotated."""
        if self.ins.thumb:
            return None
        word = struct.unpack('<I', self.ins.raw)[0]
        if (word >> 25) & 1 and ((word >> 8) & 0xF):
            imm = ((word & 0xFF) >> (2 * ((word >> 8) & 0xF))) | \
                ((word & 0xFF) << (32 - 2 * ((word >> 8) & 0xF)))
            return (imm & 0xFFFFFFFF) >> 31
        return None

    def dp_imm(self):
        """The immediate of an ARM data-processing instruction, from the encoding. capstone splits
        a non-canonical rotation (`add r1, pc, #112, #30`) into two operands, and op.imm alone is
        then the unrotated byte."""
        if self.ins.thumb:
            return None
        word = struct.unpack('<I', self.ins.raw)[0]
        if (word >> 26) & 3 or not (word >> 25) & 1:
            return None
        rot = 2 * ((word >> 8) & 0xF)
        v = word & 0xFF
        return ((v >> rot) | (v << (32 - rot))) & 0xFFFFFFFF if rot else v

    def operand(self, op, carry=None):
        if op.type == A.ARM_OP_IMM:
            if carry:
                ic = self.imm_carry()
                if ic is not None:
                    self.line('%s = %d;' % (carry, ic))
            v = self.dp_imm()
            return h(op.imm if v is None else v)
        if op.type == A.ARM_OP_REG:
            return self.shifted(op, carry)
        raise Unsupported('operand type %d' % op.type)

    # ---- writes -----------------------------------------------------------------------------
    def set_reg(self, reg, expr, is_load=False):
        """Writes a GPR. A write to pc is a branch."""
        i = self.ri(reg)
        if i == 15:
            t = self.new()
            self.line('u32 %s = %s;' % (t, expr))
            self.hooks.write_pc(self, t, is_load)
            return
        self.line('r%d = %s;' % (i, expr))

    def nz(self, v):
        self.line('N = %s >> 31; Z = %s == 0;' % (v, v))


def emit(ins, hooks):
    """Returns the C lines for one instruction, wrapped in its condition."""
    e = Emit(ins, hooks)
    body(e)
    cc = ins.cc
    lines = e.out
    if cc in COND:
        return ['if (%s) {' % COND[cc]] + ['  ' + l for l in lines] + ['}']
    return ['{'] + ['  ' + l for l in lines] + ['}']


DP3 = {A.ARM_INS_ADD, A.ARM_INS_ADC, A.ARM_INS_SUB, A.ARM_INS_SBC, A.ARM_INS_RSB, A.ARM_INS_RSC,
       A.ARM_INS_AND, A.ARM_INS_ORR, A.ARM_INS_EOR, A.ARM_INS_BIC}
LOGIC = {A.ARM_INS_AND, A.ARM_INS_ORR, A.ARM_INS_EOR, A.ARM_INS_BIC}
SHIFTS = {A.ARM_INS_LSL: 'lsl', A.ARM_INS_LSR: 'lsr', A.ARM_INS_ASR: 'asr', A.ARM_INS_ROR: 'ror'}


def sets_flags(ins):
    """ARM: the S bit (bit 20) of data processing and multiply encodings. Thumb-1: the mnemonic
    (capstone's update_flags also marks instructions that only read the flags)."""
    if ins.thumb:
        base = ins.mnem.split('.')[0]
        return base.endswith('s')
    word = struct.unpack('<I', ins.raw)[0]
    return bool((word >> 20) & 1)


def body(e):
    ins = e.ins
    i = ins.id
    ops = ins.operands
    fl = sets_flags(ins)

    if i == A.ARM_INS_HINT or i == A.ARM_INS_NOP or i == A.ARM_INS_PLD:
        return

    # ---- data processing --------------------------------------------------------------------
    if i in DP3:
        if len(ops) == 2:  # Thumb two-operand form: rd, rm  ->  rd = rd op rm
            dst, a_op, b_op = ops[0], ops[0], ops[1]
        else:
            dst, a_op, b_op = ops[0], ops[1], ops[2]
        a = e.r(a_op.reg)
        if i in LOGIC:
            cv = 'C' if fl else None
            b = e.operand(b_op, cv)
            tb = e.new()
            e.line('u32 %s = %s;' % (tb, b))
            expr = {A.ARM_INS_AND: '%s & %s', A.ARM_INS_ORR: '%s | %s', A.ARM_INS_EOR: '%s ^ %s',
                    A.ARM_INS_BIC: '%s & ~%s'}[i] % (a, tb)
            t = e.new()
            e.line('u32 %s = %s;' % (t, expr))
            if fl:
                e.nz(t)
            e.set_reg(dst.reg, t)
            return
        b = e.operand(b_op)
        tb = e.new()
        e.line('u32 %s = %s;' % (tb, b))
        if i in (A.ARM_INS_RSB, A.ARM_INS_RSC):
            a, tb = tb, a
        t = e.new()
        if fl:
            cin = {A.ARM_INS_ADD: '0', A.ARM_INS_SUB: '1', A.ARM_INS_RSB: '1',
                   A.ARM_INS_ADC: 'C', A.ARM_INS_SBC: 'C', A.ARM_INS_RSC: 'C'}[i]
            fn = 'adds_' if i in (A.ARM_INS_ADD, A.ARM_INS_ADC) else 'subs_'
            e.line('u32 %s = %s(%s, %s, %s, &N, &Z, &C, &V);' % (t, fn, a, tb, cin))
        else:
            expr = {A.ARM_INS_ADD: '%s + %s', A.ARM_INS_SUB: '%s - %s', A.ARM_INS_RSB: '%s - %s',
                    A.ARM_INS_ADC: '%s + %s + C', A.ARM_INS_SBC: '%s - %s - !C',
                    A.ARM_INS_RSC: '%s - %s - !C'}[i] % (a, tb)
            e.line('u32 %s = %s;' % (t, expr))
        e.set_reg(dst.reg, t)
        return

    if i in (A.ARM_INS_CMP, A.ARM_INS_CMN):
        a = e.r(ops[0].reg)
        b = e.operand(ops[1])
        fn = 'subs_' if i == A.ARM_INS_CMP else 'adds_'
        cin = '1' if i == A.ARM_INS_CMP else '0'
        e.line('(void)%s(%s, %s, %s, &N, &Z, &C, &V);' % (fn, a, b, cin))
        return
    if i in (A.ARM_INS_TST, A.ARM_INS_TEQ):
        a = e.r(ops[0].reg)
        b = e.operand(ops[1], 'C')
        t = e.new()
        e.line('u32 %s = %s %s %s;' % (t, a, '&' if i == A.ARM_INS_TST else '^', b))
        e.nz(t)
        return

    if i == getattr(A, 'ARM_INS_MOVS', -1):
        fl = True
        i = A.ARM_INS_MOV
    if i in (A.ARM_INS_MOV, A.ARM_INS_MVN):
        cv = 'C' if fl else None
        v = e.operand(ops[1], cv)
        t = e.new()
        e.line('u32 %s = %s%s;' % (t, '~' if i == A.ARM_INS_MVN else '', v))
        if fl:
            e.nz(t)
        e.set_reg(ops[0].reg, t)
        return
    if i == A.ARM_INS_ADR:
        base = ((ins.addr + 4) & ~3) if ins.thumb else ins.addr + 8
        off = ops[1].imm
        if not ins.thumb:   # add/sub rd, pc, #imm: take the value and the sign from the encoding
            word = struct.unpack('<I', ins.raw)[0]
            off = e.dp_imm() if ((word >> 21) & 0xF) == 0x4 else -e.dp_imm()
        e.set_reg(ops[0].reg, h((base + off) & 0xFFFFFFFF))
        return
    if i == A.ARM_INS_MOVW:
        e.set_reg(ops[0].reg, h(ops[1].imm))
        return
    if i == A.ARM_INS_MOVT:
        d = e.r(ops[0].reg)
        e.set_reg(ops[0].reg, '(%s & 0xFFFFu) | %s' % (d, h(ops[1].imm << 16)))
        return
    if i in SHIFTS or i == A.ARM_INS_RRX:
        # lsl rd, rm, #n / lsl rd, rm, rs (capstone's alias of mov with a shift).
        fn = SHIFTS.get(i)
        if len(ops) == 2 and i != A.ARM_INS_RRX and ops[1].shift.type != A.ARM_SFT_INVALID:
            # ARM: lsl rd, rm, #n reported as (rd, rm with the shift attached)
            cv = 'C' if fl else None
            t = e.new()
            e.line('u32 %s = %s;' % (t, e.shifted(ops[1], cv)))
            if fl:
                e.nz(t)
            e.set_reg(ops[0].reg, t)
            return
        if len(ops) == 2 and i != A.ARM_INS_RRX:  # Thumb: lsls rd, rs  (rd = rd shift rs)
            src, amt_op = ops[0], ops[1]
        elif i == A.ARM_INS_RRX:
            src, amt_op = ops[1], None
        else:
            src, amt_op = ops[1], ops[2]
        x = e.r(src.reg)
        t = e.new()
        if i == A.ARM_INS_RRX:
            if fl:
                e.line('u32 %s = rrx_c(%s, &C);' % (t, x))
            else:
                e.line('u32 %s = (%s >> 1) | (C << 31);' % (t, x))
        else:
            if amt_op.type == A.ARM_OP_IMM:
                amt = str(amt_op.imm)
            else:
                amt = '(%s & 0xFF)' % e.r(amt_op.reg)
            if fl:
                e.line('u32 %s = %s_c(%s, %s, &C);' % (t, fn, x, amt))
            else:
                e.line('u32 %s = %s_(%s, %s);' % (t, fn, x, amt))
        if fl:
            e.nz(t)
        e.set_reg(ops[0].reg, t)
        return

    if i in (A.ARM_INS_MUL, A.ARM_INS_MLA):
        if len(ops) == 2:  # Thumb muls rd, rm: rd = rd * rm
            a, b, acc = e.r(ops[0].reg), e.r(ops[1].reg), None
        else:
            a, b = e.r(ops[1].reg), e.r(ops[2].reg)
            acc = e.r(ops[3].reg) if i == A.ARM_INS_MLA else None
        t = e.new()
        e.line('u32 %s = %s * %s%s;' % (t, a, b, (' + ' + acc) if acc else ''))
        if fl:
            e.nz(t)
        e.set_reg(ops[0].reg, t)
        return
    if i in (A.ARM_INS_UMULL, A.ARM_INS_SMULL, A.ARM_INS_UMLAL, A.ARM_INS_SMLAL):
        lo, hi = ops[0].reg, ops[1].reg
        a, b = e.r(ops[2].reg), e.r(ops[3].reg)
        t = e.new()
        if i in (A.ARM_INS_UMULL, A.ARM_INS_UMLAL):
            prod = '(u64)%s * (u64)%s' % (a, b)
        else:
            prod = '(u64)((s64)(s32)%s * (s64)(s32)%s)' % (a, b)
        if i in (A.ARM_INS_UMLAL, A.ARM_INS_SMLAL):
            prod += ' + (((u64)%s << 32) | %s)' % (e.r(hi), e.r(lo))
        e.line('u64 %s = %s;' % (t, prod))
        if fl:
            e.line('N = (u32)(%s >> 63); Z = %s == 0;' % (t, t))
        e.set_reg(lo, '(u32)%s' % t)
        e.set_reg(hi, '(u32)(%s >> 32)' % t)
        return
    if i == A.ARM_INS_SMULBB:
        e.set_reg(ops[0].reg, '(u32)((s32)(s16)%s * (s32)(s16)%s)' % (e.r(ops[1].reg), e.r(ops[2].reg)))
        return
    if i == A.ARM_INS_CLZ:
        x = e.r(ops[1].reg)
        e.set_reg(ops[0].reg, '%s ? (u32)__builtin_clz(%s) : 32u' % (x, x))
        return
    if i in (A.ARM_INS_UXTB, A.ARM_INS_UXTH, A.ARM_INS_SXTB, A.ARM_INS_SXTH):
        x = e.shifted(ops[1], None)
        cast = {A.ARM_INS_UXTB: '(u32)(u8)', A.ARM_INS_UXTH: '(u32)(u16)',
                A.ARM_INS_SXTB: '(u32)(s32)(s8)', A.ARM_INS_SXTH: '(u32)(s32)(s16)'}[i]
        e.set_reg(ops[0].reg, '%s(%s)' % (cast, x))
        return
    if i == A.ARM_INS_SXTAH:
        x = e.shifted(ops[2], None)
        e.set_reg(ops[0].reg, '%s + (u32)(s32)(s16)(%s)' % (e.r(ops[1].reg), x))
        return
    if i == A.ARM_INS_REV:
        e.set_reg(ops[0].reg, '__builtin_bswap32(%s)' % e.r(ops[1].reg))
        return
    if i == A.ARM_INS_PKHBT:
        b = e.shifted(ops[2], None)
        e.set_reg(ops[0].reg, '(%s & 0xFFFFu) | ((%s) & 0xFFFF0000u)' % (e.r(ops[1].reg), b))
        return
    if i in (A.ARM_INS_UQSUB8, A.ARM_INS_QSUB8):
        a, b = e.r(ops[1].reg), e.r(ops[2].reg)
        t = e.new()
        e.line('u32 %s = 0;' % t)
        for k in range(4):
            sh = 8 * k
            if i == A.ARM_INS_UQSUB8:
                e.line('{ int d = (int)((%s >> %d) & 0xFF) - (int)((%s >> %d) & 0xFF); '
                       'if (d < 0) d = 0; %s |= (u32)d << %d; }' % (a, sh, b, sh, t, sh))
            else:
                e.line('{ int d = (int)(s8)(%s >> %d) - (int)(s8)(%s >> %d); '
                       'if (d < -128) d = -128; if (d > 127) d = 127; %s |= (u32)(u8)d << %d; }'
                       % (a, sh, b, sh, t, sh))
        e.set_reg(ops[0].reg, t)
        return
    if i == A.ARM_INS_QDSUB:
        # qdsub rd, rm, rn: rd = sat(rm - sat(2 * rn))
        m, n = e.r(ops[1].reg), e.r(ops[2].reg)
        t = e.new()
        e.line('s64 %s = 2 * (s64)(s32)%s; if (%s > 0x7FFFFFFF) { %s = 0x7FFFFFFF; Q = 1; } '
               'if (%s < -0x80000000LL) { %s = -0x80000000LL; Q = 1; }' % (t, n, t, t, t, t))
        u = e.new()
        e.line('s64 %s = (s64)(s32)%s - %s; if (%s > 0x7FFFFFFF) { %s = 0x7FFFFFFF; Q = 1; } '
               'if (%s < -0x80000000LL) { %s = -0x80000000LL; Q = 1; }' % (u, m, t, u, u, u, u))
        e.set_reg(ops[0].reg, '(u32)%s' % u)
        return

    # ---- loads and stores -------------------------------------------------------------------
    if i in LDST:
        return ldst(e)
    if i in (A.ARM_INS_LDM, A.ARM_INS_LDMIB, A.ARM_INS_LDMDA, A.ARM_INS_LDMDB, A.ARM_INS_STM,
             A.ARM_INS_STMIB, A.ARM_INS_STMDA, A.ARM_INS_STMDB, A.ARM_INS_PUSH, A.ARM_INS_POP):
        return ldm_stm(e)

    # ---- branches ---------------------------------------------------------------------------
    if i == A.ARM_INS_B:
        e.hooks.branch(e, ops[0].imm & 0xFFFFFFFF)
        return
    if i in (A.ARM_INS_BL, A.ARM_INS_BLX):
        ret = (ins.addr + ins.size) | (1 if ins.thumb else 0)
        if ops[0].type == A.ARM_OP_IMM:
            t = ops[0].imm & 0xFFFFFFFF
            thumb = ins.thumb if i == A.ARM_INS_BL else not ins.thumb
            e.line('r14 = %s;' % h(ret))
            e.hooks.call(e, t | (1 if thumb else 0), ret)
        else:
            tg = e.new()
            e.line('u32 %s = %s;' % (tg, e.r(ops[0].reg)))
            e.line('r14 = %s;' % h(ret))
            e.hooks.call_indirect(e, tg, ret)
        return
    if i == A.ARM_INS_BX:
        tg = e.new()
        e.line('u32 %s = %s;' % (tg, e.r(ops[0].reg)))
        e.hooks.bx(e, tg, ops[0].reg == A.ARM_REG_LR)
        return

    if i == A.ARM_INS_SVC:
        e.hooks.svc(e, ops[0].imm)
        return
    if i == A.ARM_INS_MRC:
        # mrc p15, 0, rd, c13, c0, 3: the TLS pointer.
        if ops[0].type == A.ARM_OP_PIMM and ops[0].imm == 15 and ops[3].imm == 13 and ops[5].imm == 3:
            e.set_reg(ops[2].reg, 'c->tls')
            return
        raise Unsupported('mrc %s' % ins.op_str)
    if i == A.ARM_INS_MCR:
        # mcr p15, 0, rd, c7, c10, 5 (data memory barrier) and friends: nothing to do.
        if ops[0].imm == 15 and ops[3].imm == 7:
            return
        raise Unsupported('mcr %s' % ins.op_str)
    if i == A.ARM_INS_CLREX:
        e.line('c->excl_valid = 0;')
        return
    if i == A.ARM_INS_MRS:
        e.set_reg(ops[0].reg, '(N << 31) | (Z << 30) | (C << 29) | (V << 28) | (Q << 27) | (GE << 16) | 0x10u')
        return
    if i == A.ARM_INS_MSR:
        x = e.operand(ops[1])
        t = e.new()
        e.line('u32 %s = %s;' % (t, x))
        e.line('N = %s >> 31; Z = (%s >> 30) & 1; C = (%s >> 29) & 1; V = (%s >> 28) & 1; '
               'Q = (%s >> 27) & 1;' % (t, t, t, t, t))
        if 'g' in ins.op_str.split(',')[0]:
            e.line('GE = (%s >> 16) & 0xF;' % t)
        return
    if i == A.ARM_INS_UDF:
        e.hooks.unknown(e, 'udf')
        return

    if i in VFP_IDS:
        return vfp(e)
    raise Unsupported('instruction %s %s' % (ins.mnem, ins.op_str))


LDST = {A.ARM_INS_LDR: ('rd32', 4, None), A.ARM_INS_LDRB: ('rd8', 1, None),
        A.ARM_INS_LDRH: ('rd16', 2, None), A.ARM_INS_LDRSB: ('rd8', 1, '(u32)(s32)(s8)'),
        A.ARM_INS_LDRSH: ('rd16', 2, '(u32)(s32)(s16)'), A.ARM_INS_LDRT: ('rd32', 4, None),
        A.ARM_INS_LDRBT: ('rd8', 1, None), A.ARM_INS_STR: ('wr32', 4, None),
        A.ARM_INS_STRB: ('wr8', 1, None), A.ARM_INS_STRH: ('wr16', 2, None),
        A.ARM_INS_STRBT: ('wr8', 1, None), A.ARM_INS_LDRD: ('rd32', 8, None),
        A.ARM_INS_STRD: ('wr32', 8, None), A.ARM_INS_LDREX: ('rd32', 4, 'ex'),
        A.ARM_INS_LDREXB: ('rd8', 1, 'ex'), A.ARM_INS_LDREXH: ('rd16', 2, 'ex'),
        A.ARM_INS_LDREXD: ('rd32', 8, 'ex'), A.ARM_INS_STREX: ('wr32', 4, 'ex'),
        A.ARM_INS_STREXB: ('wr8', 1, 'ex'), A.ARM_INS_STREXH: ('wr16', 2, 'ex'),
        A.ARM_INS_STREXD: ('wr32', 8, 'ex')}


def index_value(e, op):
    """An index register operand (with shift) of a load or store; negated when subtracted."""
    x = e.r(op.reg)
    st, sv = op.shift.type, op.shift.value
    if st not in (A.ARM_SFT_INVALID, 0) and not (st == A.ARM_SFT_LSL and sv == 0):
        if st == A.ARM_SFT_RRX:
            x = '((%s >> 1) | (C << 31))' % x
        else:
            x = '%s_(%s, %d)' % (SFT_IMM[st], x, sv)
    return x


def ldst(e):
    ins = e.ins
    fn, size, ext = LDST[ins.id]
    ops = ins.operands
    load = fn.startswith('rd')
    if ext == 'ex' and not load:
        status, regs, mem_i = ops[0], [ops[1]] + ([ops[2]] if size == 8 else []), 3 if size == 8 else 2
    else:
        status = None
        regs = [ops[0]] + ([ops[1]] if size == 8 else [])
        mem_i = 2 if size == 8 else 1
    mop = ops[mem_i]
    m = mop.mem
    if m.base == A.ARM_REG_PC:
        base_val = h((ins.addr + 4) & ~3) if ins.thumb else h(ins.addr + 8)
    else:
        base_val = e.r(m.base)
    # Offset
    off = None
    if ins.thumb:
        post = False
    else:
        post = not ((struct.unpack('<I', ins.raw)[0] >> 24) & 1)
    extra = ops[mem_i + 1:]  # post-indexed offset operand, if any
    if post and extra:
        o = extra[0]
        if o.type == A.ARM_OP_IMM:
            off = h(-o.imm if o.subtracted else o.imm)
        else:
            v = index_value(e, o)
            off = '(0u - %s)' % v if o.subtracted else v
    elif m.index not in (0, A.ARM_REG_INVALID):
        idx = e.r(m.index)
        st, sv = mop.shift.type, mop.shift.value
        if st not in (A.ARM_SFT_INVALID,) and not (st == A.ARM_SFT_LSL and sv == 0):
            if st == A.ARM_SFT_RRX:
                idx = '((%s >> 1) | (C << 31))' % idx
            else:
                idx = '%s_(%s, %d)' % (SFT_IMM[st], idx, sv)
        off = '(0u - %s)' % idx if (mop.subtracted or m.scale == -1) else idx
    elif m.disp:
        off = h(m.disp)
    addr = e.new()
    if post:
        e.line('u32 %s = %s;' % (addr, base_val))
    else:
        e.line('u32 %s = %s%s;' % (addr, base_val, (' + ' + off) if off else ''))
    wb = ins.writeback or post
    if ext == 'ex':
        if load:
            e.line('c->excl_addr = %s; c->excl_valid = 1;' % addr)
        else:
            e.line('if (c->excl_valid && c->excl_addr == %s) {' % addr)
    vals = []
    if load:
        for k, rop in enumerate(regs):
            t = e.new()
            a = addr if k == 0 else '%s + 4' % addr
            v = '%s(%s)' % (fn, a)
            if ext and ext != 'ex':
                v = '%s%s' % (ext, v)
            e.line('u32 %s = %s;' % (t, v))
            vals.append((rop, t))
    else:
        for k, rop in enumerate(regs):
            a = addr if k == 0 else '%s + 4' % addr
            if rop.reg == A.ARM_REG_PC:
                v = h(ins.addr + 8 if not ins.thumb else ins.addr + 4)  # ARM11 stores pc + 8
            else:
                v = e.r(rop.reg)
            e.line('%s(%s, %s);' % (fn, a, v))
    if ext == 'ex' and not load:
        e.line('  %s = 0; c->excl_valid = 0; } else { %s = 1; }' % (e.r(status.reg), e.r(status.reg)))
    # Writeback before the loaded register (a load into the base register wins).
    if wb and m.base != A.ARM_REG_PC:
        if post:
            e.set_reg(m.base, '%s + %s' % (addr, off or '0'))
        else:
            e.set_reg(m.base, addr)
    for rop, t in vals:
        e.set_reg(rop.reg, t, is_load=True)


def reglist_ops(ins, start):
    return [o for o in ins.operands[start:]]


def ldm_stm(e):
    ins = e.ins
    i = ins.id
    ops = ins.operands
    if i in (A.ARM_INS_PUSH, A.ARM_INS_POP):
        base_reg = A.ARM_REG_SP
        regs = ops
        wb = True
        mode = 'db' if i == A.ARM_INS_PUSH else 'ia'
        load = i == A.ARM_INS_POP
    else:
        base_reg = ops[0].reg
        regs = ops[1:]
        wb = ins.writeback
        mode = {A.ARM_INS_LDM: 'ia', A.ARM_INS_STM: 'ia', A.ARM_INS_LDMIB: 'ib', A.ARM_INS_STMIB: 'ib',
                A.ARM_INS_LDMDA: 'da', A.ARM_INS_STMDA: 'da', A.ARM_INS_LDMDB: 'db',
                A.ARM_INS_STMDB: 'db'}[i]
        load = i in (A.ARM_INS_LDM, A.ARM_INS_LDMIB, A.ARM_INS_LDMDA, A.ARM_INS_LDMDB)
    if '^' in ins.op_str:
        raise Unsupported('user-mode ldm/stm')
    n = len(regs)
    base = e.r(base_reg)
    start = {'ia': '%s', 'ib': '%s + 4', 'da': '%s - %d + 4', 'db': '%s - %d'}[mode]
    a = e.new()
    if mode in ('da', 'db'):
        e.line('u32 %s = %s;' % (a, start % (base, 4 * n)))
    else:
        e.line('u32 %s = %s;' % (a, start % base))
    newbase = {'ia': '%s + %d', 'ib': '%s + %d', 'da': '%s - %d', 'db': '%s - %d'}[mode] % (base, 4 * n)
    nb = e.new()
    e.line('u32 %s = %s;' % (nb, newbase))
    sorted_regs = sorted(regs, key=lambda o: GPR[o.reg])
    if load:
        vals = []
        for k, o in enumerate(sorted_regs):
            t = e.new()
            e.line('u32 %s = rd32(%s + %d);' % (t, a, 4 * k))
            vals.append((o, t))
        if wb and not any(o.reg == base_reg for o in regs):
            e.set_reg(base_reg, nb)
        pc_val = None
        for o, t in vals:
            if o.reg == A.ARM_REG_PC:
                pc_val = t
            else:
                e.set_reg(o.reg, t)
        if pc_val:
            e.hooks.write_pc(e, pc_val, True)
    else:
        for k, o in enumerate(sorted_regs):
            if o.reg == A.ARM_REG_PC:
                v = h(ins.addr + (4 if ins.thumb else 8))
            else:
                v = e.r(o.reg)
            e.line('wr32(%s + %d, %s);' % (a, 4 * k, v))
        if wb:
            e.set_reg(base_reg, nb)


VFP_IDS = set()
for _n in ('VLDR', 'VSTR', 'VMOV', 'VADD', 'VSUB', 'VMUL', 'VDIV', 'VMLA', 'VMLS', 'VNMUL', 'VNMLS',
           'VNMLA', 'VNEG', 'VABS', 'VSQRT', 'VCMP', 'VCMPE', 'VMRS', 'VMSR', 'VCVT', 'VPUSH', 'VPOP',
           'VLDMIA', 'VSTMIA', 'VLDMDB', 'VSTMDB', 'FMSTAT'):
    if hasattr(A, 'ARM_INS_' + _n):
        VFP_IDS.add(getattr(A, 'ARM_INS_' + _n))


def fsize(ins):
    """'f32' or 'f64' from the data type suffix."""
    m = ins.mnem
    if '.f64' in m:
        return 'f64'
    return 'f32'


def vreg(e, op):
    """C expression reading a VFP register (float or double)."""
    if op.reg in SREG:
        return 'sf(c, %d)' % SREG[op.reg], 'f32', SREG[op.reg]
    if op.reg in DREG:
        return 'df(c, %d)' % DREG[op.reg], 'f64', DREG[op.reg]
    raise Unsupported('vfp register')


def vset(e, op, expr):
    if op.reg in SREG:
        e.line('set_sf(c, %d, %s);' % (SREG[op.reg], expr))
    elif op.reg in DREG:
        e.line('set_df(c, %d, %s);' % (DREG[op.reg], expr))
    else:
        raise Unsupported('vfp dest')


def vfp(e):
    ins = e.ins
    i = ins.id
    ops = ins.operands
    if i in (A.ARM_INS_VLDR, A.ARM_INS_VSTR):
        m = ops[1].mem
        base = h((ins.addr + 8) & ~3) if m.base == A.ARM_REG_PC else e.r(m.base)
        a = e.new()
        e.line('u32 %s = %s + %s;' % (a, base, h(m.disp)))
        r = ops[0].reg
        if r in SREG:
            if i == A.ARM_INS_VLDR:
                e.line('c->vfp.s[%d] = rd32(%s);' % (SREG[r], a))
            else:
                e.line('wr32(%s, c->vfp.s[%d]);' % (a, SREG[r]))
        else:
            d = DREG[r]
            if i == A.ARM_INS_VLDR:
                e.line('c->vfp.s[%d] = rd32(%s); c->vfp.s[%d] = rd32(%s + 4);' % (2 * d, a, 2 * d + 1, a))
            else:
                e.line('wr32(%s, c->vfp.s[%d]); wr32(%s + 4, c->vfp.s[%d]);' % (a, 2 * d, a, 2 * d + 1))
        return
    if i in (A.ARM_INS_VPUSH, A.ARM_INS_VPOP, A.ARM_INS_VLDMIA, A.ARM_INS_VSTMIA,
             getattr(A, 'ARM_INS_VLDMDB', -1), getattr(A, 'ARM_INS_VSTMDB', -1)):
        if i in (A.ARM_INS_VPUSH, A.ARM_INS_VPOP):
            base_reg, regs, wb = A.ARM_REG_SP, ops, True
            db = i == A.ARM_INS_VPUSH
            load = i == A.ARM_INS_VPOP
        else:
            base_reg, regs, wb = ops[0].reg, ops[1:], ins.writeback
            db = i in (getattr(A, 'ARM_INS_VLDMDB', -1), getattr(A, 'ARM_INS_VSTMDB', -1))
            load = i in (A.ARM_INS_VLDMIA, getattr(A, 'ARM_INS_VLDMDB', -1))
        words = []
        for o in regs:
            if o.reg in SREG:
                words.append(SREG[o.reg])
            else:
                d = DREG[o.reg]
                words += [2 * d, 2 * d + 1]
        n = len(words)
        base = e.r(base_reg)
        a = e.new()
        e.line('u32 %s = %s%s;' % (a, base, (' - %d' % (4 * n)) if db else ''))
        for k, w in enumerate(words):
            if load:
                e.line('c->vfp.s[%d] = rd32(%s + %d);' % (w, a, 4 * k))
            else:
                e.line('wr32(%s + %d, c->vfp.s[%d]);' % (a, 4 * k, w))
        if wb:
            e.set_reg(base_reg, '%s %s %d' % (base, '-' if db else '+', 4 * n))
        return
    if i == A.ARM_INS_VMOV:
        return vmov(e)
    if i in (A.ARM_INS_VMRS, A.ARM_INS_FMSTAT):
        if i == A.ARM_INS_FMSTAT or ops[0].reg == A.ARM_REG_APSR_NZCV:
            e.line('N = c->fpscr >> 31; Z = (c->fpscr >> 30) & 1; C = (c->fpscr >> 29) & 1; '
                   'V = (c->fpscr >> 28) & 1;')
        else:
            e.set_reg(ops[0].reg, 'c->fpscr')
        return
    if i == A.ARM_INS_VMSR:
        e.line('c->fpscr = %s;' % e.r(ops[1].reg))
        return
    if i in (A.ARM_INS_VCMP, A.ARM_INS_VCMPE):
        a, ty, _ = vreg(e, ops[0])
        if len(ops) > 1 and ops[1].type == A.ARM_OP_FP:
            b = '0.0'
        elif len(ops) > 1 and ops[1].type == A.ARM_OP_IMM:
            b = '0.0'
        elif len(ops) > 1:
            b, _, _ = vreg(e, ops[1])
        else:
            b = '0.0'
        e.line('c->fpscr = (c->fpscr & 0x0FFFFFFFu) | vcmp_flags((double)%s, (double)%s);' % (a, b))
        return
    if i == A.ARM_INS_VCVT:
        return vcvt(e)
    # Arithmetic
    d_op = ops[0]
    dv, ty, _ = vreg(e, d_op)
    T = 'float' if ty == 'f32' else 'double'
    if i in (A.ARM_INS_VNEG, A.ARM_INS_VABS, A.ARM_INS_VSQRT):
        x, _, _ = vreg(e, ops[1])
        if i == A.ARM_INS_VNEG:
            expr = '-%s' % x
        elif i == A.ARM_INS_VABS:
            expr = '__builtin_fabs%s(%s)' % ('f' if ty == 'f32' else '', x)
        else:
            expr = '__builtin_sqrt%s(%s)' % ('f' if ty == 'f32' else '', x)
        vset(e, d_op, expr)
        return
    if len(ops) == 2:
        n_op, m_op = ops[0], ops[1]
    else:
        n_op, m_op = ops[1], ops[2]
    n, _, _ = vreg(e, n_op)
    m, _, _ = vreg(e, m_op)
    p = e.new()
    if i == A.ARM_INS_VADD:
        vset(e, d_op, '%s + %s' % (n, m)); return
    if i == A.ARM_INS_VSUB:
        vset(e, d_op, '%s - %s' % (n, m)); return
    if i == A.ARM_INS_VMUL:
        vset(e, d_op, '%s * %s' % (n, m)); return
    if i == A.ARM_INS_VDIV:
        vset(e, d_op, '%s / %s' % (n, m)); return
    if i == A.ARM_INS_VNMUL:
        vset(e, d_op, '-(%s * %s)' % (n, m)); return
    e.line('%s %s = %s * %s;' % (T, p, n, m))
    if i == A.ARM_INS_VMLA:
        vset(e, d_op, '%s + %s' % (dv, p)); return
    if i == A.ARM_INS_VMLS:
        vset(e, d_op, '%s - %s' % (dv, p)); return
    if i == A.ARM_INS_VNMLA:
        vset(e, d_op, '-%s - %s' % (dv, p)); return
    if i == A.ARM_INS_VNMLS:
        vset(e, d_op, '%s - %s' % (p, dv)); return
    raise Unsupported('vfp %s' % ins.mnem)


def vmov(e):
    ins = e.ins
    ops = ins.operands
    if len(ops) == 2:
        d, s = ops
        if d.reg in GPR and s.reg in SREG:
            e.set_reg(d.reg, 'c->vfp.s[%d]' % SREG[s.reg]); return
        if d.reg in SREG and s.type == A.ARM_OP_REG and s.reg in GPR:
            e.line('c->vfp.s[%d] = %s;' % (SREG[d.reg], e.r(s.reg))); return
        if d.reg in SREG and s.type == A.ARM_OP_REG and s.reg in SREG:
            e.line('c->vfp.s[%d] = c->vfp.s[%d];' % (SREG[d.reg], SREG[s.reg])); return
        if d.reg in DREG and s.type == A.ARM_OP_REG and s.reg in DREG:
            e.line('c->vfp.dw[%d] = c->vfp.dw[%d];' % (DREG[d.reg], DREG[s.reg])); return
        if s.type == A.ARM_OP_FP:
            # vmov.f32 sN, #imm: encode from the instruction bits (exact) rather than the double.
            word = struct.unpack('<I', ins.raw)[0]
            imm8 = ((word >> 12) & 0xF0) | (word & 0xF)
            a, b, cd = imm8 >> 7, (imm8 >> 6) & 1, imm8 & 0x3F
            if d.reg in SREG:
                bits = (a << 31) | ((b ^ 1) << 30) | ((0x1F if b else 0) << 25) | (cd << 19)
                e.line('c->vfp.s[%d] = %s;' % (SREG[d.reg], h(bits)))
            else:
                bits = (a << 63) | ((b ^ 1) << 62) | ((0xFF if b else 0) << 54) | (cd << 48)
                e.line('c->vfp.dw[%d] = 0x%XULL;' % (DREG[d.reg], bits))
            return
    if len(ops) == 3:
        a, b, c = ops
        if a.reg in DREG:  # vmov dN, rL, rH
            d = DREG[a.reg]
            e.line('c->vfp.s[%d] = %s; c->vfp.s[%d] = %s;' % (2 * d, e.r(b.reg), 2 * d + 1, e.r(c.reg)))
            return
        if c.reg in DREG:  # vmov rL, rH, dN
            d = DREG[c.reg]
            t1, t2 = e.new(), e.new()
            e.line('u32 %s = c->vfp.s[%d], %s = c->vfp.s[%d];' % (t1, 2 * d, t2, 2 * d + 1))
            e.set_reg(a.reg, t1)
            e.set_reg(b.reg, t2)
            return
    if len(ops) == 4:
        a, b, cc, dd = ops
        if a.reg in SREG:  # vmov sN, sN+1, rL, rH
            e.line('c->vfp.s[%d] = %s; c->vfp.s[%d] = %s;' % (SREG[a.reg], e.r(cc.reg), SREG[b.reg], e.r(dd.reg)))
            return
        t1, t2 = e.new(), e.new()
        e.line('u32 %s = c->vfp.s[%d], %s = c->vfp.s[%d];' % (t1, SREG[cc.reg], t2, SREG[dd.reg]))
        e.set_reg(a.reg, t1)
        e.set_reg(b.reg, t2)
        return
    raise Unsupported('vmov %s' % ins.op_str)


def vcvt(e):
    ins = e.ins
    ops = ins.operands
    parts = ins.mnem.split('.')
    if len(parts) < 3:
        raise Unsupported('vcvt %s' % ins.mnem)
    to, frm = parts[-2], parts[-1]
    d, s = ops[0], ops[1]
    if len(ops) > 2:
        raise Unsupported('fixed-point vcvt')
    if frm in ('s32', 'u32'):
        src = '(s32)c->vfp.s[%d]' % SREG[s.reg] if frm == 's32' else 'c->vfp.s[%d]' % SREG[s.reg]
        if to == 'f32':
            e.line('set_sf(c, %d, (float)%s);' % (SREG[d.reg], src))
        else:
            e.line('set_df(c, %d, (double)%s);' % (DREG[d.reg], src))
        return
    x, _, _ = vreg(e, s)
    if to in ('s32', 'u32'):
        # vcvt (round toward zero); vcvtr would use FPSCR rounding.
        if ins.mnem.startswith('vcvtr'):
            raise Unsupported('vcvtr')
        fn = 'vcvt_s32' if to == 's32' else 'vcvt_u32'
        e.line('c->vfp.s[%d] = (u32)%s((double)%s);' % (SREG[d.reg], fn, x))
        return
    if to == 'f64' and frm == 'f32':
        e.line('set_df(c, %d, (double)%s);' % (DREG[d.reg], x)); return
    if to == 'f32' and frm == 'f64':
        e.line('set_sf(c, %d, (float)%s);' % (SREG[d.reg], x)); return
    raise Unsupported('vcvt %s' % ins.mnem)
