// MSR-nx: checks runtime/os/audio_stretch.c with a 440 Hz tone produced unevenly (full speed, half
// speed, bursts, a stall): the output must have no gaps or jumps while sound is available, and its
// pitch must stay at 440 Hz.
// Build: gcc -O2 -I runtime/os tools/tests/stretch_test.c runtime/os/audio_stretch.c -lm -o stretch_test
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio_stretch.h"

#define RING 32768
static s16 ring[RING * 2];
static u32 writes;
static double phase;

static void produce(u32 n) {
    for (u32 i = 0; i < n; i++) {
        s16 v = (s16)(8000 * sin(phase));
        phase += 2 * M_PI * 440.0 / 32728.0;
        ring[2 * (writes % RING)] = v;
        ring[2 * (writes % RING) + 1] = v;
        writes++;
    }
}

int main(void) {
    stretch_reset();
    static s16 out[1440 * 2];
    // 30 ms output buffers; the producer runs at a speed that changes over time
    double speed[] = {1.0, 1.0, 1.0, 0.5, 0.5, 0.5, 0.3, 1.6, 1.0, 0.0, 1.0, 1.0};
    int fails = 0;
    double prev = 0;
    long gaps = 0, jumps = 0, crossings = 0, samples = 0;
    double carry = 0;
    for (int seg = 0; seg < 12; seg++) {
        for (int b = 0; b < 100; b++) {   // 3 seconds per segment
            carry += 32728.0 * 0.030 * speed[seg];
            u32 k = (seg == 4 && b % 3 != 2) ? 0 : (u32)carry;
            carry -= k;
            produce(k);
            stretch_fill(out, 1440, 48000.0, ring, RING, writes);
            for (int i = 0; i < 1440; i++) {
                double v = out[2 * i];
                if (seg >= 1 && speed[seg] > 0 && seg != 10) {
                    if (fabs(v - prev) > 8000 * 2 * M_PI * 440 / 48000 * 1.8) jumps++;   // steeper than the tone allows
                    if (v == 0 && prev == 0) gaps++;
                    if ((prev < 0 && v >= 0)) crossings++;
                    samples++;
                }
                prev = v;
            }
        }
        printf("segment %d speed %.1f: tempo %.2f\n", seg, speed[seg], stretch_tempo());
    }
    double hz = crossings / (samples / 48000.0);
    printf("pitch %.1f Hz (440 expected), %ld jumps, %ld silent samples\n", hz, jumps, gaps);
    if (fabs(hz - 440) > 15 || jumps > 50 || gaps > 2000) fails = 1;
    printf(fails ? "FAILED\n" : "passed\n");
    return fails;
}
