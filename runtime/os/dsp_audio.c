// MSR-nx: the 3DS audio firmware's work, done natively: 24 voices that play the game's sample
// buffers (PCM8, PCM16, DSP-ADPCM), report their state back, and a mixer that makes one stereo
// frame of 160 samples at 32728 Hz every 4.89 ms.
//
// Shared memory layout and behaviour from 3dbrew "DSP Memory Region" and fearkov/zakuro (MIT)
// services/dsp_voices.rs and dsp_mixer.rs, read as reference. The code here is ours.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "msr_log.h"
#include "pica.h"

#define VOICES 24
#define FRAME 160
#define REGION 0x20000u
#define CONFIG_SIZE 192u
#define STATUS_SIZE 12u

// DSP data memory word address -> guest address
static u32 dsp_addr(u32 w) { return 0x1FF00000u + 0x40000u + (w << 1); }

static u32 rd_dsp32(u32 a) { return (rd16(a) << 16) | rd16(a + 2); }
static void wr_dsp32(u32 a, u32 v) { wr16(a, v >> 16); wr16(a + 2, v & 0xFFFF); }

enum { F_PCM8, F_PCM16, F_ADPCM };

typedef struct {
    u32 addr, len;
    int fmt, stereo, loop, from_queue, has_played, has_adpcm;
    u16 id;
    s16 yn[2];
    u32 play_pos;
} Buffer;

typedef struct {
    int enabled, stereo, fmt, interp;   // interp: 0 polyphase (played linear), 1 linear, 2 none
    u16 sync;
    double rate, frac;
    float gain[3][4], ramp_from[3][4];
    int ramp[3];
    s16 coef[16], hist[2];
    Buffer queue[8];
    int nq;
    Buffer playing;
    int has_playing;
    s16* decoded;
    u32 decoded_cap;
    u32 remaining, length, position;
    u16 cur_id, last_id;
    int buffer_update, ran_out;
    int simple_on, biquad_on;
    s32 sb0, sa1, sy1[2];
    s32 bb[3], ba[2], bx[2][2], by[2][2];
    s16 out[FRAME][2];
} Voice;

static Voice g_v[VOICES];
static float g_volume[3];
static int g_aux[2], g_mono;

// Output ring for the platform's audio (Switch audren / PC), stereo s16 at 32728 Hz.
#define RING (32768 * 2)
s16 msr_audio_ring[RING * 2];
volatile u32 msr_audio_write;   // in stereo samples, grows forever
static FILE* g_wav;
static u32 g_wav_samples;

static void voice_reset(Voice* v) {
    s16* d = v->decoded;
    u32 cap = v->decoded_cap;
    memset(v, 0, sizeof *v);
    v->decoded = d;
    v->decoded_cap = cap;
    v->rate = 1.0;
    v->fmt = F_PCM16;
    v->sb0 = 1 << 15;
    v->bb[0] = 1 << 14;
}

static void decode_adpcm(Voice* v, const u8* data, u32 count) {
    if (v->decoded_cap < count) {
        v->decoded = (s16*)realloc(v->decoded, count * 2);
        v->decoded_cap = count;
    }
    s32 y1 = v->hist[0], y2 = v->hist[1];
    u32 n = 0;
    for (u32 f = 0; n < count; f++) {
        const u8* fr = data + 8 * f;
        s32 scale = 1 << (fr[0] & 0xF);
        int pred = (fr[0] >> 4) & 7;
        s32 c1 = v->coef[pred * 2], c2 = v->coef[pred * 2 + 1];
        for (int b = 1; b < 8 && n < count; b++)
            for (int h = 0; h < 2 && n < count; h++) {
                s32 nib = h ? fr[b] & 0xF : fr[b] >> 4;
                s32 xn = ((nib << 28) >> 28) * scale;
                s32 val = ((xn << 11) + 0x400 + c1 * y1 + c2 * y2) >> 11;
                if (val > 32767) val = 32767;
                if (val < -32768) val = -32768;
                y2 = y1;
                y1 = val;
                v->decoded[n++] = (s16)val;
            }
    }
    v->hist[0] = (s16)y1;
    v->hist[1] = (s16)y2;
}

static int dequeue(Voice* v) {
    if (!v->nq) return 0;
    int best = 0;
    for (int k = 1; k < v->nq; k++) if (v->queue[k].id < v->queue[best].id) best = k;
    Buffer b = v->queue[best];
    if (b.loop) v->queue[best].has_played = 1;
    else { v->queue[best] = v->queue[--v->nq]; }
    u32 start = b.has_played ? 0 : (b.play_pos < b.len ? b.play_pos : b.len);
    v->length = b.len;
    v->remaining = b.len - start;
    v->position = start;
    v->frac = 0;
    v->cur_id = b.id;
    v->last_id = 0;
    v->buffer_update = b.from_queue && !b.has_played;
    if (b.has_adpcm) { v->hist[0] = b.yn[0]; v->hist[1] = b.yn[1]; }
    if (b.fmt == F_ADPCM) {
        u32 va = pica_vaddr(b.addr & ~3u);
        if (va) decode_adpcm(v, g_mem + va, b.len);
    }
    v->playing = b;
    v->has_playing = 1;
    return 1;
}

static void read_sample(Voice* v, u32 i, s16* l, s16* r) {
    Buffer* b = &v->playing;
    if (i >= b->len) i = b->len ? b->len - 1 : 0;
    if (b->fmt == F_ADPCM) { *l = *r = v->decoded && i < v->decoded_cap ? v->decoded[i] : 0; return; }
    u32 va = pica_vaddr(b->addr & ~3u);
    if (!va) { *l = *r = 0; return; }
    if (b->fmt == F_PCM8) {
        if (b->stereo) { *l = (s16)((s8)rd8(va + 2 * i) * 256); *r = (s16)((s8)rd8(va + 2 * i + 1) * 256); }
        else *l = *r = (s16)((s8)rd8(va + i) * 256);
    } else {
        if (b->stereo) { *l = (s16)rd16(va + 4 * i); *r = (s16)rd16(va + 4 * i + 2); }
        else *l = *r = (s16)rd16(va + 2 * i);
    }
}

static void play_frame(Voice* v) {
    memset(v->out, 0, sizeof v->out);
    int follows = v->ran_out && v->nq;
    v->ran_out = 0;
    if (v->remaining == 0 && !follows) {
        if (dequeue(v)) return;   // the firmware starts a new buffer on the next frame
        v->enabled = 0;
        v->buffer_update = 1;
        v->last_id = v->cur_id;
        v->cur_id = 0;
        v->position = 0;
        return;
    }
    v->position = v->length - v->remaining;
    int o = 0;
    while (o < FRAME) {
        if (v->remaining == 0 && !dequeue(v)) break;
        while (o < FRAME && v->remaining) {
            u32 at = v->length - v->remaining;
            s16 l0, r0;
            read_sample(v, at, &l0, &r0);
            if (v->interp != 2 && v->frac > 0) {
                s16 l1, r1;
                read_sample(v, at + 1, &l1, &r1);
                l0 = (s16)(l0 + (l1 - l0) * v->frac);
                r0 = (s16)(r0 + (r1 - r0) * v->frac);
            }
            v->out[o][0] = l0;
            v->out[o][1] = r0;
            o++;
            double in = v->frac + v->rate;
            u32 used = (u32)in;
            if (used > v->remaining) used = v->remaining;
            v->frac = in - used;
            if (v->frac < 0) v->frac = 0;
            v->remaining -= used;
        }
    }
    v->ran_out = v->remaining == 0 && o == FRAME;
    // filters
    for (int i = 0; i < o; i++)
        for (int ch = 0; ch < 2; ch++) {
            s32 x = v->out[i][ch];
            if (v->simple_on) {
                x = (v->sb0 * x + v->sa1 * v->sy1[ch]) >> 15;
                x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
                v->sy1[ch] = x;
            }
            if (v->biquad_on) {
                s32 x0 = x;
                x = (v->bb[0] * x0 + v->bb[1] * v->bx[0][ch] + v->bb[2] * v->bx[1][ch] + v->ba[0] * v->by[0][ch] + v->ba[1] * v->by[1][ch]) >> 14;
                x = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
                v->bx[1][ch] = v->bx[0][ch]; v->bx[0][ch] = x0;
                v->by[1][ch] = v->by[0][ch]; v->by[0][ch] = x;
            }
            v->out[i][ch] = (s16)x;
        }
}

static float rdf(u32 a) { u32 w = rd32(a); float f; memcpy(&f, &w, 4); return isfinite(f) ? f : 0; }

static void parse_config(Voice* v, u32 base, u32 coefs) {
    u32 fl = rd32(base);
    if (!fl) return;
    if (fl & (1u << 29)) voice_reset(v);
    if (fl & (1u << 4)) { v->nq = 0; v->ran_out = 0; }
    if (fl & (1u << 16)) { v->enabled = rd8(base + 0xA0) != 0; v->ran_out = 0; }
    if (fl & (1u << 28)) v->sync = rd16(base + 0xA2);
    if (fl & (1u << 18)) { float r = rdf(base + 0x34); v->rate = r > 0 ? r : 1.0; }
    if (fl & (1u << 17)) v->interp = rd8(base + 0x38) == 1 ? 1 : rd8(base + 0x38) == 2 ? 2 : 0;
    if (fl & (1u << 2)) for (int i = 0; i < 16; i++) v->coef[i] = (s16)rd16(coefs + 2 * i);
    for (int m = 0; m < 3; m++)
        if (fl & (1u << (25 + m))) {
            v->ramp[m] = 1;
            memcpy(v->ramp_from[m], v->gain[m], sizeof v->gain[m]);
            for (int c = 0; c < 4; c++) v->gain[m][c] = rdf(base + 0x04 + (m * 4 + c) * 4);
        }
    if (fl & (1u << 22)) { u16 e = rd16(base + 0x3A); v->simple_on = e & 1; v->biquad_on = (e >> 1) & 1; }
    if (fl & (1u << 23)) { v->simple_on = 1; v->sb0 = (s16)rd16(base + 0x3C); v->sa1 = (s16)rd16(base + 0x3E); }
    if (fl & (1u << 24)) {
        v->biquad_on = 1;
        v->ba[0] = (s16)rd16(base + 0x42); v->ba[1] = (s16)rd16(base + 0x40);
        v->bb[0] = (s16)rd16(base + 0x48); v->bb[1] = (s16)rd16(base + 0x46); v->bb[2] = (s16)rd16(base + 0x44);
    }
    u16 f1 = rd16(base + 0xB4);
    if (fl & ((1u << 0) | (1u << 30))) { u32 f = (f1 >> 2) & 3; v->fmt = f == 0 ? F_PCM8 : f == 2 ? F_ADPCM : F_PCM16; }
    if (fl & ((1u << 1) | (1u << 30))) v->stereo = (f1 & 3) == 2;
    u32 play_pos = (fl & (1u << 21)) ? rd_dsp32(base + 0xA4) : 0;
    if (fl & (1u << 3)) {
        u32 len = rd_dsp32(base + 0xB0), played = v->length - v->remaining;
        v->length = len > played ? len : played;
        v->remaining = v->length - played;
        if (v->has_playing) v->playing.len = v->length;
    }
    if ((fl & (1u << 30)) && v->nq < 8) {
        u16 f2 = rd16(base + 0xBC);
        Buffer b = {rd_dsp32(base + 0xAC), rd_dsp32(base + 0xB0), v->fmt, v->stereo, (f2 >> 1) & 1, 0, 0, f2 & 1,
                    rd16(base + 0xBE), {(s16)rd16(base + 0xB8), (s16)rd16(base + 0xBA)}, play_pos};
        v->queue[v->nq++] = b;
    }
    if (fl & (1u << 19)) {
        u16 q = rd16(base + 0x4A);
        for (int s = 0; s < 4; s++) {
            if (!(q & (1 << s)) || v->nq >= 8) continue;
            u32 bb = base + 0x4C + 20 * s;
            u32 len = rd_dsp32(bb + 4);
            if (!len) continue;
            Buffer b = {rd_dsp32(bb), len, v->fmt, v->stereo, rd8(bb + 15) != 0, 1, 0, rd8(bb + 14) != 0,
                        rd16(bb + 16), {(s16)rd16(bb + 10), (s16)rd16(bb + 12)}, 0};
            v->queue[v->nq++] = b;
        }
        wr16(base + 0x4A, 0);
    }
    wr32(base, 0);
}

static void write_status(Voice* v, u32 base) {
    wr8(base, (u8)v->enabled);
    wr8(base + 1, (u8)v->buffer_update);
    v->buffer_update = 0;
    wr16(base + 2, v->sync);
    wr_dsp32(base + 4, v->position);
    wr16(base + 8, v->cur_id);
    wr16(base + 10, v->last_id);
}

static int current_region(void) {
    u16 a = rd16(dsp_addr(0xBFFF)), b = rd16(dsp_addr(0xBFFF) + REGION);
    if (a == 0xFFFF && b != 0xFFFE) return 1;
    if (b == 0xFFFF && a != 0xFFFE) return 0;
    return a > b ? 0 : 1;
}

void dsp_audio_reset(void) {
    for (int k = 0; k < VOICES; k++) voice_reset(&g_v[k]);
    memset(g_volume, 0, sizeof g_volume);
}

// One audio frame of the firmware.
void dsp_audio_frame(void) {
    static int inited;
    if (!inited) { inited = 1; dsp_audio_reset(); const char* w = getenv("MSR_WAV"); if (w) g_wav = fopen(w, "wb"); }
    int rd = current_region(), wrr = 1 - rd;
    u32 cfg = dsp_addr(0x9E92) + rd * REGION, coef = dsp_addr(0xA792) + rd * REGION;
    u32 st = dsp_addr(0x8680) + wrr * REGION;
    static s32 mix[3][4][FRAME];
    memset(mix, 0, sizeof mix);
    for (int k = 0; k < VOICES; k++) {
        Voice* v = &g_v[k];
        int was = v->enabled;
        parse_config(v, cfg + k * CONFIG_SIZE, coef + k * 32);
        if (v->enabled != was) msr_log("dsp: voice %d %s (%d queued, rate %g)", k, v->enabled ? "on" : "off", v->nq, v->rate);
        if (v->enabled) play_frame(v);
        if (v->enabled)
            for (int m = 0; m < 3; m++)
                for (int c = 0; c < 4; c++) {
                    float g1 = v->gain[m][c], g0 = v->ramp[m] ? v->ramp_from[m][c] : g1;
                    if (g0 == 0 && g1 == 0) continue;
                    for (int i = 0; i < FRAME; i++) {
                        float g = g0 + (g1 - g0) * i / (FRAME - 1);
                        mix[m][c][i] += (s32)(g * v->out[i][c & 1]);
                    }
                }
        v->ramp[0] = v->ramp[1] = v->ramp[2] = 0;
        write_status(v, st + k * STATUS_SIZE);
    }
    // mixer configuration
    u32 mc = dsp_addr(0x9430) + rd * REGION;
    u32 mf = rd32(mc);
    if (mf) {
        if (mf & (1u << 16)) g_volume[0] = rdf(mc + 0x04);
        for (int a = 0; a < 2; a++) {
            if (mf & (1u << (24 + a))) g_volume[a + 1] = rdf(mc + 0x08 + 4 * a);
            if (mf & (1u << (8 + a))) g_aux[a] = rd16(mc + 0x28 + 2 * a) != 0;
        }
        if (mf & (1u << 26)) g_mono = rd16(mc + 0x16) == 0;
        wr32(mc, 0);
    }
    // aux busses: the game processes them itself (effects) and hands them back a frame later
    u32 inter_rd = dsp_addr(0x9492) + rd * REGION, inter_wr = dsp_addr(0x9492) + wrr * REGION;
    static s32 used[3][4][FRAME];
    memcpy(used[0], mix[0], sizeof used[0]);
    for (int a = 0; a < 2; a++) {
        u32 off = a * 4 * FRAME * 4;
        if (g_aux[a]) {
            for (int c = 0; c < 4; c++)
                for (int i = 0; i < FRAME; i++) {
                    used[a + 1][c][i] = (s32)rd32(inter_rd + off + (c * FRAME + i) * 4);
                    wr32(inter_wr + off + (c * FRAME + i) * 4, (u32)mix[a + 1][c][i]);
                }
        } else memcpy(used[a + 1], mix[a + 1], sizeof used[a + 1]);
    }
    s16 frame[FRAME][2];
    memset(frame, 0, sizeof frame);
    for (int m = 0; m < 3; m++) {
        float vol = g_volume[m];
        if (vol == 0) continue;
        for (int i = 0; i < FRAME; i++) {
            float a = used[m][0][i] * vol, b = used[m][1][i] * vol, c = used[m][2][i] * vol, d = used[m][3][i] * vol;
            s32 l, r;
            if (g_mono) { l = r = (s32)((a + b + c + d) / 2); } else { l = (s32)(a + c); r = (s32)(b + d); }
            l += frame[i][0]; r += frame[i][1];
            frame[i][0] = (s16)(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
            frame[i][1] = (s16)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
        }
    }
    u32 fs = dsp_addr(0x8540) + wrr * REGION;
    for (int i = 0; i < FRAME; i++) { wr16(fs + 4 * i, (u16)frame[i][0]); wr16(fs + 4 * i + 2, (u16)frame[i][1]); }
    for (int i = 0; i < FRAME; i++) {
        u32 p = (msr_audio_write + i) % RING;
        msr_audio_ring[2 * p] = frame[i][0];
        msr_audio_ring[2 * p + 1] = frame[i][1];
    }
    msr_audio_write += FRAME;
    if (g_wav) {
        if (!g_wav_samples) { u8 hdr[44] = {0}; fwrite(hdr, 1, 44, g_wav); }
        fwrite(frame, 4, FRAME, g_wav);
        g_wav_samples += FRAME;
        // keep the header right as we go (the probe may be stopped at any time)
        long pos = ftell(g_wav);
        u32 data = g_wav_samples * 4, riff = 36 + data, rate = 32728, br = rate * 4;
        u8 h[44] = {'R', 'I', 'F', 'F', (u8)riff, (u8)(riff >> 8), (u8)(riff >> 16), (u8)(riff >> 24), 'W', 'A', 'V', 'E',
                    'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0, (u8)rate, (u8)(rate >> 8), 0, 0,
                    (u8)br, (u8)(br >> 8), (u8)(br >> 16), 0, 4, 0, 16, 0, 'd', 'a', 't', 'a',
                    (u8)data, (u8)(data >> 8), (u8)(data >> 16), (u8)(data >> 24)};
        if ((g_wav_samples / FRAME) % 64 == 0) { fseek(g_wav, 0, SEEK_SET); fwrite(h, 1, 44, g_wav); fseek(g_wav, pos, SEEK_SET); fflush(g_wav); }
    }
}
