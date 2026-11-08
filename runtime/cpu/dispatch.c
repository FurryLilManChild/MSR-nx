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
        if (m == a) {
            if ((msr_fn_addr[mid] & 1u) != (target & 1u) && (target & 1u)) {
                // A Thumb target (bit 0 set) that our list has as ARM, or the reverse through a
                // pop/ldr pc. Running it in the wrong mode is never right.
                msr_log("fatal: target %08X is %s code, but the function list has it as %s",
                        target, (target & 1u) ? "Thumb" : "ARM", (msr_fn_addr[mid] & 1u) ? "Thumb" : "ARM");
                return 0;
            }
            return msr_fn_ptr[mid];
        }
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

static unsigned g_unwinds;

void msr_unwind(Cpu* c, u32 call_site, u32 expected) {
    if (g_unwinds++ < 200)
        msr_log("unwind: call at %08X expected a return to %08X, got %08X (lr %08X sp %08X)",
                call_site, expected, c->pc, c->r[14], c->r[13]);
    if (g_unwinds == 1)
        msr_receipt("first unexpected return: call at %08X expected %08X, got %08X",
                    call_site, expected, c->pc);
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
        msr_log("run: entering %08X from the top level", pc);
        f(c);
    }
}

void msr_nan_seen(const char* fn, int reg, int dbl) {
    static const char* seen[256];
    static int n;
    for (int k = 0; k < n; k++) if (seen[k] == fn) return;
    if (n < 256) seen[n++] = fn;
    msr_log("nan: %s wrote NaN to %c%d", fn, dbl ? 'd' : 's', reg);
}
