// MSR-nx: dsp::DSP, first version. The game's audio firmware is never run: the component upload
// is accepted, the audio pipe answers like the firmware does after a state change, and the
// semaphore and interrupt events get signalled. Mixing the voices comes with the audio work.
//
// Command layouts and the firmware's reply follow 3dbrew "DSP Services" and fearkov/zakuro (MIT)
// services/dsp.rs; the code here is ours.
#include <string.h>
#include "hle.h"
#include "msr_log.h"
#include "sched.h"

void dsp_audio_frame(void);
void dsp_audio_reset(void);

#define DSP_RAM 0x1FF00000u   // where the DSP's memory appears to the application

enum { PIPE_AUDIO = 2, PIPE_BINARY = 3, PIPE_COUNT = 8 };

static struct {
    int loaded, running;
    u32 semaphore_event;      // 0 until the game asks for it
    u32 interrupt_events[8];
    int n_interrupt;
    u32 semaphore_mask, semaphore_value;
    u8 pipe[PIPE_COUNT][64];
    u32 pipe_len[PIPE_COUNT];
} g_dsp;

// Where the firmware's structures live in DSP data memory, as the real firmware reports them.
static const u16 k_struct_addr[15] = {
    0xBFFF, 0x9E92, 0x8680, 0xA792, 0x9430, 0x8400, 0x8540, 0x9492,
    0x8710, 0x8410, 0xA912, 0xAA12, 0xAAD2, 0xAC52, 0xAC5C,
};

static void signal_semaphore(void) {
    g_dsp.semaphore_value = g_dsp.semaphore_mask;
    if (g_dsp.semaphore_event) sched_wake(g_dsp.semaphore_event);
    for (int k = 0; k < g_dsp.n_interrupt; k++) sched_wake(g_dsp.interrupt_events[k]);
}

static void write_pipe(u32 channel, u32 src, u32 size) {
    if (channel != PIPE_AUDIO || size == 0) {
        msr_log("dsp: pipe %u takes %u bytes (ignored)", channel, size);
        return;
    }
    u8 state = rd8(src);
    msr_log("dsp: audio pipe state %u", state);
    memset(g_dsp.pipe_len, 0, sizeof g_dsp.pipe_len);
    if (state == 0) {          // initialize: the firmware answers with its structure addresses
        u8* p = g_dsp.pipe[PIPE_AUDIO];
        p[0] = 15; p[1] = 0;
        for (int k = 0; k < 15; k++) { p[2 + 2 * k] = (u8)k_struct_addr[k]; p[3 + 2 * k] = (u8)(k_struct_addr[k] >> 8); }
        g_dsp.pipe_len[PIPE_AUDIO] = 32;
        g_dsp.running = 1;
        dsp_audio_reset();
        msr_receipt("dsp: audio firmware initialized (stand-in, no mixing yet)");
    } else if (state == 1) g_dsp.running = 0;
    else if (state == 2) g_dsp.running = 1;
    else if (state == 3) g_dsp.running = 0;
    signal_semaphore();
}

// The audio firmware finishes a frame of 160 samples at 32728 Hz and signals.
static void dsp_frame(void) {
    if (g_dsp.running) { dsp_audio_frame(); signal_semaphore(); }
}

int svc_dsp(Ipc* q) {
    static int timer;
    if (!timer) { timer = 1; sched_add_timer(4888780, dsp_frame); }
    switch (q->cmd) {
    case 0x0001: {  // RecvData(register)
        u32 w[1] = {g_dsp.running ? 0u : 1u};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0002: case 0x0004: case 0x000B: {  // RecvDataIsReady, SendDataIsEmpty, CheckSemaphoreRequest
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0003: case 0x0005: case 0x0007: case 0x0009: case 0x000A:
    case 0x0013: case 0x0014: case 0x0020:
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0008: {  // GetSemaphore
        u32 w[1] = {g_dsp.semaphore_value};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0006: case 0x001F: case 0x0021: {  // RecvFifoEx, GetHeadphoneStatus, GetIsDspOccupied
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x000C: {  // ConvertProcessAddressFromDspDram(address)
        u32 w[1] = {DSP_RAM + (ipc_get(q, 1) << 1) + 0x40000};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x000D:  // WriteProcessPipe(channel, size, descriptor, buffer)
        write_pipe(ipc_get(q, 1), ipc_get(q, 4), ipc_get(q, 2));
        ipc_reply(q, 0, 0);
        return 1;
    case 0x000E: case 0x0010: {  // ReadPipe / ReadPipeIfPossible(channel, peer, size)
        u32 ch = ipc_get(q, 1) % PIPE_COUNT, size = ipc_get(q, 3) & 0xFFFF;
        u32 dst = rd32(q->c->tls + 0x184);   // static buffer 0
        u32 n = g_dsp.pipe_len[ch] < size ? g_dsp.pipe_len[ch] : size;
        for (u32 k = 0; k < n; k++) wr8(dst + k, g_dsp.pipe[ch][k]);
        memmove(g_dsp.pipe[ch], g_dsp.pipe[ch] + n, g_dsp.pipe_len[ch] - n);
        g_dsp.pipe_len[ch] -= n;
        msr_log("dsp: pipe %u gave %u of %u bytes to %08X", ch, n, size, dst);
        ipc_set(q, 0, IPC_HDR(q->cmd, 2, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, n);
        ipc_set(q, 3, (n << 14) | 2);
        ipc_set(q, 4, dst);
        return 1;
    }
    case 0x000F: {  // GetPipeReadableSize(channel, peer)
        u32 w[1] = {g_dsp.pipe_len[ipc_get(q, 1) % PIPE_COUNT]};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0011: {  // LoadComponent(size, program mask, data mask, descriptor, buffer) -> loaded
        g_dsp.loaded = 1;
        msr_receipt("dsp: the game uploaded its audio firmware (%X bytes); accepted, not run", ipc_get(q, 1));
        ipc_set(q, 0, IPC_HDR(0x0011, 2, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 1);
        ipc_set(q, 3, (ipc_get(q, 1) << 4) | 0xA);
        ipc_set(q, 4, ipc_get(q, 5));
        return 1;
    }
    case 0x0012:  // UnloadComponent
        g_dsp.loaded = g_dsp.running = 0;
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0015: {  // RegisterInterruptEvents(interrupt, channel, descriptor, event)
        u32 h = ipc_get(q, 4);
        if (h && g_dsp.n_interrupt < 8) g_dsp.interrupt_events[g_dsp.n_interrupt++] = h;
        ipc_reply(q, 0, 0);
        return 1;
    }
    case 0x0016:  // GetSemaphoreEventHandle
        if (!g_dsp.semaphore_event) g_dsp.semaphore_event = k_new(OBJ_EVENT, "DSP:semaphore");
        else k_dup(g_dsp.semaphore_event);   // one more reference for the handle given out
        ipc_set(q, 0, IPC_HDR(0x0016, 1, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0);
        ipc_set(q, 3, g_dsp.semaphore_event);
        return 1;
    case 0x0017:  // SetSemaphoreMask
        g_dsp.semaphore_mask = ipc_get(q, 1);
        ipc_reply(q, 0, 0);
        return 1;
    }
    return 0;
}
