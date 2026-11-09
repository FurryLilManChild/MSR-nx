// MSR-nx: one-frame captures.
//
// On request (the Switch build: hold ZL + ZR + minus), the next frame is captured: at its start
// the PICA state, the lighting tables and VRAM; during it every GX command, with the bytes of each
// command list as it was submitted; at its end every region of guest memory, VRAM again and the
// framebuffer setup. The probe replays the frame on the reference renderer (MSR_REPLAY) and writes
// its VRAM, so tools/capture/compare.py can set the Switch's pictures of every render target and
// glow stage next to the reference's.
//
// Records: u32 type, u32 size, payload. 1 Pica state, 2 lighting tables, 3 VRAM at the start, 4 GX
// command (id and 7 words), 5 command list (address, bytes), 6 memory region at the end (base,
// bytes), 7 VRAM at the end, 8 framebuffer info.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "capture.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "pica.h"

#define VRAM_BASE 0x1F000000u
#define VRAM_SIZE (6u << 20)

int msr_capture_active;
static int g_state;   // 0 idle, 1 requested, 2 recording
static FILE* g_f;
static char g_path[300];

static void rec(u32 type, const void* a, u32 na, const void* b, u32 nb) {
    if (!g_f) return;
    u32 h[2] = {type, na + nb};
    fwrite(h, 4, 2, g_f);
    if (na) fwrite(a, 1, na, g_f);
    if (nb) fwrite(b, 1, nb, g_f);
}

static void vram_record(u32 type) {
    if (pica_sync_hook) pica_sync_hook(VRAM_BASE, VRAM_SIZE);   // the GPU's VRAM reaches guest memory
    rec(type, g_mem + VRAM_BASE, VRAM_SIZE, 0, 0);
}

void msr_capture_request(const char* path) {
    if (g_state) return;
    snprintf(g_path, sizeof g_path, "%s", path);
    g_state = 1;
}

void msr_capture_latch(void) {
    {   // MSR_CAPTURE_AT=N (probe): capture the frame after the Nth top-screen picture, into MSR_CAPTURE
        static int at = -2;
        static unsigned latches;
        if (at == -2) { const char* e = getenv("MSR_CAPTURE_AT"); at = e ? atoi(e) : -1; }
        if (at >= 0 && latches++ == (unsigned)at) msr_capture_request(getenv("MSR_CAPTURE") ? getenv("MSR_CAPTURE") : "capture.bin");
    }
    if (g_state == 1) {
        g_f = fopen(g_path, "wb");
        if (!g_f) { msr_receipt("capture: could not open %s", g_path); g_state = 0; return; }
        fwrite("MSRCAP01", 1, 8, g_f);
        rec(1, &g_pica, sizeof g_pica, 0, 0);
        rec(2, pica_lut, sizeof pica_lut, 0, 0);
        vram_record(3);
        g_state = 2;
        msr_capture_active = 1;
        { extern int msr_draw_this_frame; msr_draw_this_frame = 1; }   // the reference renderer draws this one
        msr_receipt("capture: recording one frame");
    } else if (g_state == 2) {
        u32 base[256], size[256];
        int n = msr_mem_regions(base, size, 256);
        vram_record(7);
        for (int k = 0; k < n; k++) {
            if (base[k] >= VRAM_BASE && base[k] < VRAM_BASE + VRAM_SIZE) continue;
            u32 hdr[2] = {base[k], size[k]};
            rec(6, hdr, 8, g_mem + base[k], size[k]);
        }
        typedef struct { u32 left, right, stride, format; unsigned swaps; } Fb;   // hle_hid_gsp.c MsrFramebuffer
        extern Fb msr_fb[2];
        rec(8, msr_fb, sizeof(Fb) * 2, 0, 0);
        long bytes = ftell(g_f);
        fclose(g_f);
        g_f = 0;
        g_state = 0;
        msr_capture_active = 0;
        msr_receipt("capture: saved %s (%ld bytes)", g_path, bytes);
    }
}

void msr_capture_gx(u32 id, const u32* d) {
    if (g_state != 2) return;
    u32 w[8] = {id};
    memcpy(w + 1, d, 7 * 4);
    rec(4, w, 32, 0, 0);
    if (id == 1 && d[1] && d[1] < (64u << 20)) {
        u32 hdr[2] = {d[0], d[1]};
        rec(5, hdr, 8, g_mem + d[0], d[1]);
    }
}

// ---- replay (the probe) ------------------------------------------------------------------------
int msr_replay(const char* path, const char* out_dir) {
    FILE* f = fopen(path, "rb");
    if (!f) { msr_receipt("replay: cannot open %s", path); return 1; }
    char magic[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "MSRCAP01", 8)) { msr_receipt("replay: not a capture"); fclose(f); return 1; }
    // first pass: memory at the end and VRAM at the start go in before anything runs
    u8* buf = 0;
    size_t cap = 0;
    long gx_at = 0;
    for (;;) {
        u32 h[2];
        if (fread(h, 4, 2, f) != 2) break;
        if (h[1] > cap) { cap = h[1]; buf = (u8*)realloc(buf, cap); }
        if (fread(buf, 1, h[1], f) != h[1]) break;
        if (h[0] == 1 && h[1] == sizeof g_pica) memcpy(&g_pica, buf, h[1]);
        else if (h[0] == 2 && h[1] == sizeof pica_lut) { memcpy(pica_lut, buf, h[1]); pica_lut_version++; }
        else if (h[0] == 3 && h[1] == VRAM_SIZE) memcpy(g_mem + VRAM_BASE, buf, VRAM_SIZE);
        else if (h[0] == 6) {
            u32 base, size;
            memcpy(&base, buf, 4);
            memcpy(&size, buf + 4, 4);
            msr_mem_map(base, size, 3, 0, "replay");
            memcpy(g_mem + base, buf + 8, size);
        } else if (h[0] == 7 && h[1] == VRAM_SIZE) {
            char p[600];
            snprintf(p, sizeof p, "%s/switch_vram_end.bin", out_dir);
            FILE* o = fopen(p, "wb");
            if (o) { fwrite(buf, 1, VRAM_SIZE, o); fclose(o); }
        } else if (h[0] == 8) {
            char p[600];
            snprintf(p, sizeof p, "%s/fb_info.bin", out_dir);
            FILE* o = fopen(p, "wb");
            if (o) { fwrite(buf, 1, h[1], o); fclose(o); }
        }
        if (h[0] == 4 && !gx_at) gx_at = ftell(f) - 8 - (long)h[1];
    }
    // second pass: the frame's GX commands, with each command list's bytes as submitted
    extern void gx_execute(u32 id, u32* d);
    extern int msr_draw_this_frame;
    msr_draw_this_frame = 1;
    unsigned cmds = 0;
    fseek(f, gx_at, SEEK_SET);
    for (;;) {
        u32 h[2];
        if (fread(h, 4, 2, f) != 2) break;
        if (h[1] > cap) { cap = h[1]; buf = (u8*)realloc(buf, cap); }
        if (fread(buf, 1, h[1], f) != h[1]) break;
        if (h[0] == 5) {
            u32 addr, size;
            memcpy(&addr, buf, 4);
            memcpy(&size, buf + 4, 4);
            memcpy(g_mem + addr, buf + 8, size);
        } else if (h[0] == 4) {
            u32 w[8];
            memcpy(w, buf, 32);
            gx_execute(w[0], w + 1);
            cmds++;
        } else if (h[0] == 6 || h[0] == 7) break;   // the end of the frame
    }
    fclose(f);
    free(buf);
    char p[600];
    snprintf(p, sizeof p, "%s/replay_vram_end.bin", out_dir);
    FILE* o = fopen(p, "wb");
    if (o) { fwrite(g_mem + VRAM_BASE, 1, VRAM_SIZE, o); fclose(o); }
    msr_receipt("replay: %u GX commands replayed; VRAM written to %s", cmds, p);
    return 0;
}
