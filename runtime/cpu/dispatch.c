// MSR-nx: lookups for calls and jumps through registers, and the top-level run loop.
#include <stdio.h>
#include <stdlib.h>
#include "msr_cpu.h"
#include "msr_log.h"

extern const unsigned msr_fn_count;
extern const u32 msr_fn_addr[];
extern void (*const msr_fn_ptr[])(Cpu* __restrict);

typedef void (*GuestFn)(Cpu* __restrict);

// Binary search over the sorted entry list (bit 0 of each entry marks Thumb).
static GuestFn lookup(u32 target) {
    u32 a = target & ~1u;
    unsigned lo = 0, hi = msr_fn_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        u32 m = msr_fn_addr[mid] & ~1u;
        if (m == a) return msr_fn_ptr[mid];
        if (m < a) lo = mid + 1; else hi = mid;
    }
    return 0;
}

void msr_call_indirect(Cpu* c, u32 target) {
    GuestFn f = lookup(target);
    if (!f) {
        msr_log("fatal: call to %08X, not a known function (from lr %08X)", target, c->r[14]);
        msr_fatal(c, "unknown call target");
    }
    f(c);
}

void msr_jump_indirect(Cpu* c, u32 target) {
    GuestFn f = lookup(target);
    if (f) { f(c); return; }        // a tail call through a register
    c->pc = target;                  // otherwise a return through a register (bx r3 and the like)
}

void msr_unknown(Cpu* c, u32 addr, const char* what) {
    msr_log("fatal: %s at %08X", what, addr);
    msr_fatal(c, what);
}

// Runs guest code from c->pc until the guest returns to `stop`. A function that returns to an
// address other than its call's expected one unwinds to here; we look the address up and go on.
void msr_run(Cpu* c, u32 entry, u32 stop) {
    c->pc = entry;
    for (;;) {
        u32 pc = c->pc;
        if (pc == stop) return;
        GuestFn f = lookup(pc);
        if (!f) {
            msr_log("fatal: control reached %08X, not a function entry (lr %08X sp %08X)",
                    pc, c->r[14], c->r[13]);
            msr_fatal(c, "lost control");
        }
        c->r[15] = pc;
        f(c);
    }
}
