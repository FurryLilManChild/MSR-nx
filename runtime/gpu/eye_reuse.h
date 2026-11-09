// MSR-nx: right eye reuse (eye_reuse.c): the right eye's draws skipped when they repeat the left's.
#pragma once
#include "pica.h"

void eye_reuse_set(int on);
int eye_reuse_on(void);
void eye_reuse_copier(void (*copy)(u32 src, u32 dst, u32 size));   // VRAM to VRAM, the renderer's DMA
int eye_draw(int run, u32 c, u32 d, u32 csz, u32 dsz, int cw, int dw, int dr, const u32* tex, unsigned ntex);
void eye_draw_done(void);   // after a draw eye_draw let run: its target registers back
void eye_before_read(u32 lo, u32 hi);
void eye_written(u32 lo, u32 hi, u64 tok);   // tok 0: unknown content
u64 eye_token_of(u32 lo, u32 hi);
void eye_note_transfer(u32 src, u32 dst, u32 src_bytes);
void eye_note_framebuffers(u32 left_va, u32 right_va);
void eye_frame_end(void);
extern unsigned eye_skipped, eye_drawn_right, eye_materialized, eye_failures;
