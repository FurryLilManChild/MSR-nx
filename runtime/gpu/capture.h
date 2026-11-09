// MSR-nx: one-frame captures for comparing the Switch renderer with the reference renderer.
#pragma once
#include "msr_cpu.h"

void msr_capture_request(const char* path);   // capture the next whole frame into path
void msr_capture_latch(void);                 // a top-screen picture was latched (frame boundary)
void msr_capture_gx(u32 id, const u32* d);    // a GX command is about to run
extern int msr_capture_active;
int msr_replay(const char* path, const char* out_dir);   // the probe: replays a capture
