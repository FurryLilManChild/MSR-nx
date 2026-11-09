// MSR-nx: audio time stretching (see audio_stretch.c).
#pragma once
#include <stdint.h>
typedef int16_t s16;
typedef uint32_t u32;

void stretch_reset(void);
// Fills n stereo samples at out_rate from the DSP's ring (ring_size stereo samples; write is the
// writer's running count).
void stretch_fill(s16* out, u32 n, double out_rate, const s16* ring, u32 ring_size, u32 write);
float stretch_tempo(void);   // the current playback tempo (1 = real time)
