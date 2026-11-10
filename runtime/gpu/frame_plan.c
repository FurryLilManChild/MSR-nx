// MSR-nx: skips GPU work whose result never reaches a screen the Switch shows.
//
// Samus Returns renders every frame twice, once per eye of the 3DS's stereoscopic screen, even with
// the 3D slider at 0, and the Switch shows only the left eye. Each frame's GPU operations (draws,
// fills, display transfers, texture copies, DMA) are recorded with the memory they read and write.
// At the end of the frame a backward pass marks the operations that feed the displayed framebuffers
// (or memory the next frame reads before writing). The next frame skips the others, as long as it
// issues the same operation in the same order (an operation more or fewer in between is allowed for:
// the lists are realigned); anything new runs. The probe checks that the shown pictures are byte-identical with and
// without skipping (MSR_NOSKIP=1 runs everything).
#include <stdlib.h>
#include <string.h>
#include "frame_plan.h"
#include "msr_log.h"

#define MAX_OPS 1024
#define MAX_RANGES 6
#define SET_MAX 512

typedef struct {
    u32 lo, hi;
} Range;

typedef struct {
    u32 type;
    u64 sig;
    Range rd[MAX_RANGES], wr[MAX_RANGES];
    u8 nrd, nwr;
    u8 full_write;   // writes replace everything in wr (not a partial draw)
    u8 skipped;
} Op;

static Op g_ops[MAX_OPS];
static unsigned g_nops;
static u8 g_plan_live[MAX_OPS];
static u64 g_plan_sig[MAX_OPS];
static unsigned g_plan_n;
static int g_diverged;
static unsigned g_diverge_at;
static int g_enabled = -1;
static unsigned g_cursor;      // where this frame is in last frame's list
static int g_by_position = -1; // MSR_FPPOS=1: the old matching, by position only
#define REALIGN_WINDOW 48
unsigned fp_realigned;
unsigned fp_skipped_ops, fp_total_ops;

static int enabled(void) {
    if (g_enabled < 0) {
        const char* e = getenv("MSR_NOSKIP");
        g_enabled = !(e && e[0] == '1');
        e = getenv("MSR_FPPOS");
        g_by_position = e && e[0] == '1';
    }
    return g_enabled;
}

void fp_set_enabled(int on) { g_enabled = on; }

int fp_op(u32 type, u64 sig, const u32* rd, unsigned nrd, const u32* wr, unsigned nwr, int full_write) {
    unsigned i = g_nops;
    int run = 1;
    // An operation is skipped only when last frame's operation at the same position was the same
    // one and fed nothing shown. Positions that differ (the double-buffered framebuffer address
    // alternates, for one) simply run.
    if (enabled()) {
        unsigned j = g_cursor;
        if (j < g_plan_n && g_plan_sig[j] == sig) { run = g_plan_live[j]; g_cursor = j + 1; }
        else {
            if (!g_diverged) { g_diverged = 1; g_diverge_at = i; }
            // Realign: last frame's list with an operation more or fewer earlier on (one object
            // more on screen) is the same list from here on. Look a little way ahead for this one;
            // not found, it is new this frame and runs, and the place in last frame's list stays.
            if (g_by_position != 1) {
                unsigned end = j + REALIGN_WINDOW < g_plan_n ? j + REALIGN_WINDOW : g_plan_n;
                for (unsigned k = j + 1; k < end; k++)
                    if (g_plan_sig[k] == sig) { run = g_plan_live[k]; g_cursor = k + 1; fp_realigned++; break; }
            } else if (j < g_plan_n) g_cursor = j + 1;
        }
    }
    if (i < MAX_OPS) {
        Op* o = &g_ops[i];
        o->type = type;
        o->sig = sig;
        o->nrd = (u8)(nrd > MAX_RANGES ? MAX_RANGES : nrd);
        o->nwr = (u8)(nwr > MAX_RANGES ? MAX_RANGES : nwr);
        for (unsigned k = 0; k < o->nrd; k++) { o->rd[k].lo = rd[2 * k]; o->rd[k].hi = rd[2 * k + 1]; }
        for (unsigned k = 0; k < o->nwr; k++) { o->wr[k].lo = wr[2 * k]; o->wr[k].hi = wr[2 * k + 1]; }
        o->full_write = (u8)full_write;
        o->skipped = (u8)!run;
        g_nops++;
    } else g_diverged = 1;
    fp_total_ops++;
    if (!run) fp_skipped_ops++;
    return run;
}

// ---- interval sets ------------------------------------------------------------------------------
typedef struct { Range r[SET_MAX]; unsigned n; int overflow; } Set;

static void set_add(Set* s, u32 lo, u32 hi) {
    if (lo >= hi) return;
    // merge with every overlapping or touching range
    unsigned w = 0;
    for (unsigned k = 0; k < s->n; k++) {
        Range c = s->r[k];
        if (c.hi < lo || c.lo > hi) { s->r[w++] = c; continue; }
        if (c.lo < lo) lo = c.lo;
        if (c.hi > hi) hi = c.hi;
    }
    s->n = w;
    if (s->n < SET_MAX) s->r[s->n++] = (Range){lo, hi};
    else s->overflow = 1;
}

static void set_sub(Set* s, u32 lo, u32 hi) {
    if (lo >= hi) return;
    Range keep[SET_MAX + 2];
    unsigned w = 0;
    for (unsigned k = 0; k < s->n; k++) {
        Range c = s->r[k];
        if (c.hi <= lo || c.lo >= hi) { keep[w++] = c; continue; }
        if (c.lo < lo) keep[w++] = (Range){c.lo, lo};
        if (c.hi > hi) keep[w++] = (Range){hi, c.hi};
    }
    if (w > SET_MAX) { w = SET_MAX; s->overflow = 1; }
    memcpy(s->r, keep, w * sizeof(Range));
    s->n = w;
}

static int set_hits(const Set* s, u32 lo, u32 hi) {
    for (unsigned k = 0; k < s->n; k++)
        if (s->r[k].lo < hi && s->r[k].hi > lo) return 1;
    return 0;
}

// The frame is over: work out which of its operations mattered, for the next frame.
void fp_frame_end(const u32* out, unsigned nout) {
    static Set live, written;
    unsigned n = g_nops < MAX_OPS ? g_nops : MAX_OPS;
    live.n = 0; live.overflow = 0;
    written.n = 0; written.overflow = 0;
    for (unsigned k = 0; k < nout; k++) set_add(&live, out[2 * k], out[2 * k + 1]);
    // memory this frame read before writing it came from the previous frame; keep it alive at the end
    for (unsigned i = 0; i < n; i++) {
        Op* o = &g_ops[i];
        for (unsigned k = 0; k < o->nrd; k++) {
            // the parts of the read not yet written this frame
            static Set part;
            part.n = 0; part.overflow = 0;
            set_add(&part, o->rd[k].lo, o->rd[k].hi);
            for (unsigned j = 0; j < written.n && part.n; j++) set_sub(&part, written.r[j].lo, written.r[j].hi);
            for (unsigned j = 0; j < part.n; j++) set_add(&live, part.r[j].lo, part.r[j].hi);
        }
        for (unsigned k = 0; k < o->nwr; k++) set_add(&written, o->wr[k].lo, o->wr[k].hi);
    }
    int ok = !live.overflow && !written.overflow && g_nops <= MAX_OPS;
    static Set end_live;
    end_live = live;
    for (unsigned i = n; i-- > 0;) {
        Op* o = &g_ops[i];
        int needed = 0;
        for (unsigned k = 0; k < o->nwr && !needed; k++) needed = set_hits(&live, o->wr[k].lo, o->wr[k].hi);
        if (o->nwr == 0) needed = 1;   // nothing known about what it writes: keep it
        g_plan_live[i] = (u8)needed;
        g_plan_sig[i] = o->sig;
        if (!needed) continue;
        if (o->full_write)
            for (unsigned k = 0; k < o->nwr; k++) set_sub(&live, o->wr[k].lo, o->wr[k].hi);
        for (unsigned k = 0; k < o->nrd; k++) set_add(&live, o->rd[k].lo, o->rd[k].hi);
        if (live.overflow) ok = 0;
    }
    unsigned prev_n = g_plan_n;
    g_plan_n = ok ? n : 0;
    {   // MSR_FPDEBUG=N: the plan for frame N in the receipt
        static int dbg = -2;
        static unsigned frame;
        if (dbg == -2) { const char* e = getenv("MSR_FPDEBUG"); dbg = e ? atoi(e) : -1; }
        if ((int)frame == dbg) {
            msr_receipt("fp: frame %u, %u ops, plan %s, %s at %u (previous plan %u ops)", frame, n, ok ? "ok" : "overflow", g_diverged ? "diverged" : "matched", g_diverge_at, prev_n);
            for (unsigned k = 0; k < end_live.n; k++) msr_receipt("fp: live at end %08X-%08X", end_live.r[k].lo, end_live.r[k].hi);
            for (unsigned i = 0; i < n; i++) {
                Op* o = &g_ops[i];
                msr_receipt("fp: %3u t%u %s%s full%u wr %08X-%08X rd %08X-%08X %08X-%08X %08X-%08X", i, o->type,
                            g_plan_live[i] ? "LIVE" : "dead", o->skipped ? " (skipped)" : "", o->full_write,
                            o->nwr ? o->wr[0].lo : 0, o->nwr ? o->wr[0].hi : 0, o->nrd > 0 ? o->rd[0].lo : 0,
                            o->nrd > 0 ? o->rd[0].hi : 0, o->nrd > 1 ? o->rd[1].lo : 0, o->nrd > 1 ? o->rd[1].hi : 0,
                            o->nrd > 2 ? o->rd[2].lo : 0, o->nrd > 2 ? o->rd[2].hi : 0);
            }
        }
        frame++;
    }
    g_nops = 0;
    g_cursor = 0;
    g_diverged = 0;
}

