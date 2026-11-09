// MSR-nx: audio time stretching between the 3DS DSP's mix and the Switch's audio output.
//
// The DSP mix (stereo, 32728 Hz) arrives as fast as the game runs. When the game slows down it
// arrives slower than the speaker plays it, and the gaps used to be filled with silence (crackling).
// Here the stream is played back at a tempo that follows the buffer level: below the target the
// sound is stretched (WSOLA: overlapping pieces, each placed where it best matches the previous
// one, so the pitch stays the same), above it the backlog plays slightly faster. A full stall fades
// out instead of cutting off. The result is resampled to the output rate.
//
// Platform-neutral: tools/tests/stretch_test.c runs it on the CPU with an uneven producer.
#include <math.h>
#include <string.h>
#include "audio_stretch.h"

#define IN_RATE 32728.0
#define SEQ 982         // piece length, 30 ms at 32728 Hz
#define OVL 196         // overlap between pieces, 6 ms
#define SEEK 327        // how far ahead a piece may be moved to match, 10 ms
#define TARGET 3000     // input samples to keep in hand (about 90 ms)
#define TEMPO_MIN 0.30f
#define TEMPO_MAX 1.60f

static struct {
    double rpos;           // read position in the input ring (stereo samples, grows like the writer)
    float ovl[OVL][2];     // the tail of the last piece, to blend into the next
    int have_ovl;
    float out[SEQ * 2][2]; // stretched samples at the input rate, waiting to be resampled
    unsigned out_n, out_r;
    double frac;           // resampler position between out[out_r] and out[out_r + 1]
    float tempo, gain;
    float rate;            // how fast the game is producing sound, relative to real time (smoothed)
    u32 last_write;
    int started;
} S;

void stretch_reset(void) { memset(&S, 0, sizeof S); S.tempo = 1.0f; S.rate = 1.0f; }

static inline float in_s(const s16* ring, u32 ring_size, u32 pos, int ch) { return ring[2 * (pos % ring_size) + ch]; }

// Picks where in [base, base + SEEK) the next piece starts so it lines up with the stored tail.
static u32 best_offset(const s16* ring, u32 ring_size, u32 base) {
    if (!S.have_ovl) return 0;
    float best = -1e30f;
    u32 bo = 0;
    for (u32 o = 0; o < SEEK; o += 2) {   // every other position: plenty at this rate
        float c = 0, e = 1e-3f;
        for (u32 i = 0; i < OVL; i += 2) {
            float m = (in_s(ring, ring_size, base + o + i, 0) + in_s(ring, ring_size, base + o + i, 1)) * 0.5f;
            float t = (S.ovl[i][0] + S.ovl[i][1]) * 0.5f;
            c += m * t;
            e += m * m;
        }
        c /= sqrtf(e);   // normalized, so loud places are not favoured
        if (c > best) { best = c; bo = o; }
    }
    return bo;
}

// One piece of stretched output (SEQ - OVL samples) from input at rpos, then moves rpos on by the
// tempo. Needs SEEK + SEQ input samples available.
static void make_piece(const s16* ring, u32 ring_size) {
    u32 base = (u32)S.rpos, off = best_offset(ring, ring_size, base), p = base + off;
    unsigned w = S.out_n;
    for (u32 i = 0; i < OVL; i++) {   // blend the stored tail into the start of this piece
        float t = (float)i / OVL;
        for (int c = 0; c < 2; c++) {
            float v = in_s(ring, ring_size, p + i, c);
            S.out[w][c] = S.have_ovl ? S.ovl[i][c] * (1 - t) + v * t : v;
        }
        w++;
    }
    for (u32 i = OVL; i < SEQ - OVL; i++, w++)
        for (int c = 0; c < 2; c++) S.out[w][c] = in_s(ring, ring_size, p + i, c);
    for (u32 i = 0; i < OVL; i++)
        for (int c = 0; c < 2; c++) S.ovl[i][c] = in_s(ring, ring_size, p + SEQ - OVL + i, c);
    S.have_ovl = 1;
    S.out_n = w;
    S.rpos += S.tempo * (SEQ - OVL);
}

void stretch_fill(s16* out, u32 n, double out_rate, const s16* ring, u32 ring_size, u32 write) {
    double step = IN_RATE / out_rate;
    // the game's sound rate over this buffer, smoothed over about 300 ms
    float produced = (float)(write - S.last_write), wanted = (float)(n * step);
    S.last_write = write;
    if (produced < wanted * 4) S.rate += (produced / wanted - S.rate) * 0.1f;
    for (u32 i = 0; i < n; i++) {
        // keep at least two stretched samples ahead of the resampler
        while (S.out_r + 1 >= S.out_n) {
            if (S.out_r > 0) {   // drop what was played
                memmove(S.out, S.out + S.out_r, (S.out_n - S.out_r) * sizeof S.out[0]);
                S.out_n -= S.out_r;
                S.out_r = 0;
            }
            double level = (double)write - S.rpos;
            if (!S.started) {   // wait for a little sound before starting
                if (level < TARGET) break;
                S.started = 1;
            }
            if (level > ring_size * 0.75 || level < -1.0) {   // far behind (or confused): jump close to the writer
                S.rpos = (double)write - TARGET;
                S.have_ovl = 0;
                level = TARGET;
            }
            if (level < SEEK + SEQ) break;   // nothing to stretch: fade out below
            // tempo: the game's sound rate, nudged toward keeping TARGET samples in hand
            float want = S.rate * (float)(1.0 + 0.5 * (level - TARGET) / TARGET);
            if (want < TEMPO_MIN) want = TEMPO_MIN;
            if (want > TEMPO_MAX) want = TEMPO_MAX;
            S.tempo += (want - S.tempo) * 0.25f;
            make_piece(ring, ring_size);
        }
        float l, r;
        if (S.out_r + 1 < S.out_n) {
            float t = (float)S.frac;
            l = S.out[S.out_r][0] * (1 - t) + S.out[S.out_r + 1][0] * t;
            r = S.out[S.out_r][1] * (1 - t) + S.out[S.out_r + 1][1] * t;
            S.frac += step;
            while (S.frac >= 1.0) { S.frac -= 1.0; S.out_r++; }
            if (S.gain < 1.0f) S.gain += 1.0f / 480;   // 10 ms fade back in after a stall
        } else {
            // a full stall: hold the last sample and fade it out (no click), then start over
            l = S.out_n ? S.out[S.out_n - 1][0] : 0;
            r = S.out_n ? S.out[S.out_n - 1][1] : 0;
            if (S.gain > 0) S.gain -= 1.0f / 480;
            if (S.gain <= 0) { S.gain = 0; S.started = 0; S.have_ovl = 0; }
        }
        float g = S.gain < 0 ? 0 : S.gain > 1 ? 1 : S.gain;
        l *= g;
        r *= g;
        out[2 * i] = (s16)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[2 * i + 1] = (s16)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}

float stretch_tempo(void) { return S.tempo; }
