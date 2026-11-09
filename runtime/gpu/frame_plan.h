// MSR-nx: skipping GPU work that never reaches a shown screen (see frame_plan.c).
#pragma once
#include "msr_cpu.h"

enum { FP_DRAW = 1, FP_FILL, FP_TRANSFER, FP_COPY, FP_DMA };
// Records one operation; rd and wr are [lo, hi) guest address pairs. Returns 0 to skip it.
int fp_op(u32 type, u64 sig, const u32* rd, unsigned nrd, const u32* wr, unsigned nwr, int full_write);
// End of a frame: out lists the memory that is shown ([lo, hi) pairs).
void fp_frame_end(const u32* out, unsigned nout);
void fp_set_enabled(int on);
extern unsigned fp_skipped_ops, fp_total_ops;
