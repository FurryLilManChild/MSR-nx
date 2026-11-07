"""Shared pieces for generated C: local register declarations and Cpu sync code."""

GPRS = ['r%d' % i for i in range(15)]
FLAGS = ['N', 'Z', 'C', 'V', 'Q', 'GE']
FLAG_FIELDS = {'N': 'n', 'Z': 'z', 'C': 'c', 'V': 'v', 'Q': 'q', 'GE': 'ge'}


def declare_in():
    return ['DECLARE_REGS;']


def declare_in_text():
    """Locals loaded from Cpu at function entry."""
    regs = ' '.join('u32 r%d = c->r[%d];' % (i, i) for i in range(15))
    flags = ' '.join('u32 %s = c->%s;' % (f, FLAG_FIELDS[f]) for f in FLAGS)
    return [regs, flags]


def sync_out():
    return 'SYNC_OUT;'


def sync_in():
    return 'SYNC_IN;'


def sync_out_text():
    regs = ' '.join('c->r[%d] = r%d;' % (i, i) for i in range(15))
    flags = ' '.join('c->%s = %s;' % (FLAG_FIELDS[f], f) for f in FLAGS)
    return regs + ' ' + flags


def sync_in_text():
    regs = ' '.join('r%d = c->r[%d];' % (i, i) for i in range(15))
    flags = ' '.join('%s = c->%s;' % (f, FLAG_FIELDS[f]) for f in FLAGS)
    return regs + ' ' + flags
