// MSR-nx: hid and gsp::Gpu services, first version: the requests that hand out handles and shared
// memory. Input and the GPU behind them come later. Layouts follow fearkov/zakuro (MIT) hid.rs and
// gsp.rs and 3dbrew.
#include <string.h>
#include "hle.h"
#include "msr_log.h"
#include "sched.h"
#include "pica.h"

static u32 g_hid_mem, g_hid_events[5];
static u32 g_gsp_mem, g_gsp_event;
static int g_gsp_registered;

int svc_hid(Ipc* q) {
    switch (q->cmd) {
    case 0x000A: {  // GetIPCHandles -> shared memory, 5 events (pad 0, pad 1, accel, gyro, debug)
        if (!g_hid_mem) {
            g_hid_mem = k_new(OBJ_MEMBLOCK, "hid:mem");
            k_get(g_hid_mem)->size = 0x1000;
            for (int k = 0; k < 5; k++) g_hid_events[k] = k_new(OBJ_EVENT, "hid:event");
        }
        ipc_set(q, 0, IPC_HDR(0x000A, 1, 7));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x14000000);  // copy handles, six
        ipc_set(q, 3, g_hid_mem);
        for (int k = 0; k < 5; k++) ipc_set(q, 4 + k, g_hid_events[k]);
        msr_receipt("hid: GetIPCHandles -> shared memory %X", g_hid_mem);
        return 1;
    }
    case 0x0011: case 0x0012: case 0x0013: case 0x0014:  // sensors on/off
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0015: {  // GetGyroscopeLowRawToDpsCoefficient -> float
        u32 w[1] = {0x41136666u};  // 9.2f
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0016: {  // GetGyroscopeLowCalibrateParam -> 20 bytes of zero offsets
        u32 w[5] = {0, 0, 0, 0, 0};
        ipc_reply(q, w, 5);
        return 1;
    }
    case 0x0017: {  // GetSoundVolume -> full
        u32 w[1] = {0x3F};
        ipc_reply(q, w, 1);
        return 1;
    }
    }
    return 0;
}

// ---- GSP: the interrupt relay queue and GX commands ------------------------------------------
// Shared memory layout (3dbrew "GSP Shared Memory"): interrupt queues at 0x40 * thread index,
// framebuffer info at 0x200, GX command rings at 0x800 + 0x200 * thread index.
enum { IRQ_PSC0, IRQ_PSC1, IRQ_VBLANK_TOP, IRQ_VBLANK_BOTTOM, IRQ_PPF, IRQ_P3D, IRQ_DMA };

static u32 gsp_shared(void) {
    KObject* o = g_gsp_mem ? k_get(g_gsp_mem) : 0;
    return o ? o->addr : 0;
}

static void gsp_interrupt(int id) {
    u32 qa = gsp_shared();
    if (!qa || !g_gsp_event) return;
    u32 index = rd8(qa), count = rd8(qa + 1);
    if (count >= 0x34) { wr8(qa + 2, 1); return; }   // full: the error flag, the interrupt is lost
    wr8(qa + 0xC + (index + count) % 0x34, (u8)id);
    wr8(qa + 1, (u8)(count + 1));
    sched_wake(g_gsp_event);
}

unsigned msr_gx_counts[8];

static void gx_command(u32 entry) {
    u32 id = rd32(entry) & 0xFF, d[7];
    for (int k = 0; k < 7; k++) d[k] = rd32(entry + 4 + 4 * k);
    if (id < 8) msr_gx_counts[id]++;
    switch (id) {
    case 0x00:  // RequestDma(source, destination, size)
        if (d[2] && d[0] && d[1]) memmove(g_mem + d[1], g_mem + d[0], d[2]);
        msr_log("gx: dma %08X -> %08X size %X", d[0], d[1], d[2]);
        gsp_interrupt(IRQ_DMA);
        break;
    case 0x01:  // ProcessCommandList(address, size, flags)
        msr_log("gx: command list %08X size %X", d[0], d[1]);
        pica_run_list(d[0], d[1]);
        gsp_interrupt(IRQ_P3D);
        break;
    case 0x02:  // MemoryFill(start0, value0, end0, start1, value1, end1, control)
        for (int b = 0; b < 2; b++) {
            u32 start = d[b * 3], value = d[b * 3 + 1], end = d[b * 3 + 2];
            if (!start || end <= start) continue;
            u32 ctl = d[6] >> (16 * b), width = (ctl >> 8) & 3;   // bit 8: 24 bit, bit 9: 32 bit, neither: 16
            if (width == 0) for (u32 a = start; a + 2 <= end; a += 2) wr16(a, (u16)value);
            else if (width & 2) for (u32 a = start; a + 4 <= end; a += 4) wr32(a, value);
            else for (u32 a = start; a + 3 <= end; a += 3) { wr8(a, (u8)value); wr8(a + 1, (u8)(value >> 8)); wr8(a + 2, (u8)(value >> 16)); }
            msr_log("gx: fill %08X..%08X with %08X (width %u)", start, end, value, width);
            gsp_interrupt(b ? IRQ_PSC1 : IRQ_PSC0);
        }
        break;
    case 0x03:  // DisplayTransfer(input, output, input size, output size, flags)
        msr_log("gx: display transfer %08X -> %08X (%08X %08X flags %08X)", d[0], d[1], d[2], d[3], d[4]);
        pica_display_transfer(d[0], d[1], d[2], d[3], d[4]);
        gsp_interrupt(IRQ_PPF);
        break;
    case 0x04:  // TextureCopy(input, output, size, input gap, output gap, flags)
        msr_log("gx: texture copy %08X -> %08X size %X", d[0], d[1], d[2]);
        pica_texture_copy(d[0], d[1], d[2], d[3], d[4], d[5]);
        gsp_interrupt(IRQ_PPF);
        break;
    case 0x05:  // FlushCacheRegions
        break;
    default:
        msr_log("gx: unknown command %02X", id);
    }
}

static void gx_run_queue(void) {
    u32 base = gsp_shared();
    if (!base) return;
    u32 ring = base + 0x800;   // thread index 0
    for (;;) {
        u32 ctl = rd32(ring), index = ctl & 0xFF, count = (ctl >> 8) & 0xFF;
        if (!count) break;
        u32 entry = ring + 0x20 + (index % 15) * 0x20;
        wr32(ring, (ctl & 0xFFFF0000u) | ((index + 1) % 15) | ((count - 1) << 8));
        gx_command(entry);
    }
}

// The screens refresh at about 60 Hz: both vblank interrupts once a frame.
// Framebuffer info for client 0 at shared memory 0x200 + 0x40 * screen: byte 0 the buffer index,
// byte 1 "dirty"; two 0x1C-byte entries from +4 (active, left address, right address, stride,
// format, select, attribute). A game queues a swap by writing an entry and setting dirty; the GSP
// takes it at the next vblank and clears dirty. The renderer will read msr_fb[].
typedef struct { u32 left, right, stride, format; unsigned swaps; } MsrFramebuffer;
MsrFramebuffer msr_fb[2];

static void gsp_latch_framebuffers(void) {
    u32 base = gsp_shared();
    if (!base) return;
    for (int screen = 0; screen < 2; screen++) {
        u32 rec = base + 0x200 + 0x40 * screen;
        u32 hdr = rd32(rec);
        if (!(hdr & 0xFF00)) continue;
        u32 e = rec + 4 + 0x1C * (hdr & 1);
        msr_fb[screen].left = rd32(e + 4);
        msr_fb[screen].right = rd32(e + 8);
        msr_fb[screen].stride = rd32(e + 12);
        msr_fb[screen].format = rd32(e + 16);
        if (msr_fb[screen].swaps++ < 4)
            msr_log("gsp: screen %d shows %08X (stride %u, format %X)", screen, msr_fb[screen].left,
                    msr_fb[screen].stride, msr_fb[screen].format);
        wr8(rec + 1, 0);
        pica_frame_shown(screen, msr_fb[screen].left, msr_fb[screen].stride, msr_fb[screen].format);
    }
}

static void gsp_vblank(void) {
    gsp_latch_framebuffers();
    gsp_interrupt(IRQ_VBLANK_TOP);
    gsp_interrupt(IRQ_VBLANK_BOTTOM);
}

int svc_gsp(Ipc* q) {
    static int timer;
    if (!timer) { timer = 1; sched_add_timer(16715000, gsp_vblank); }   // 59.83 Hz
    switch (q->cmd) {
    case 0x000C:  // TriggerCmdReqQueue
        gx_run_queue();
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0013: {  // RegisterInterruptRelayQueue(flags, event) -> thread index, shared memory
        g_gsp_event = ipc_get(q, 3);
        if (!g_gsp_mem) {
            g_gsp_mem = k_new(OBJ_MEMBLOCK, "gsp:mem");
            k_get(g_gsp_mem)->size = 0x1000;
        }
        ipc_set(q, 0, IPC_HDR(0x0013, 2, 2));
        ipc_set(q, 1, g_gsp_registered ? 0 : 0x2A07);  // first registration says so
        ipc_set(q, 2, 0);   // thread index
        ipc_set(q, 3, 0);   // copy handles, one
        ipc_set(q, 4, g_gsp_mem);
        g_gsp_registered = 1;
        msr_receipt("gsp: RegisterInterruptRelayQueue (event %X) -> shared memory %X", g_gsp_event, g_gsp_mem);
        return 1;
    }
    case 0x0016: case 0x0017: case 0x0014:  // AcquireRight / ReleaseRight / Unregister...
    case 0x0001: case 0x0002: case 0x0003:  // WriteHWRegs (and masked, repeat)
    case 0x0005: case 0x0008: case 0x0009: case 0x0007:  // SetBufferSwap, cache ops
    case 0x000B: case 0x001C: case 0x001E: case 0x001F:
        msr_log("gsp: cmd %04X acknowledged (no GPU yet)", q->cmd);
        ipc_reply(q, 0, 0);
        return 1;
    }
    return 0;
}
