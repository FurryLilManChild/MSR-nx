// MSR-nx: hid and gsp::Gpu services, first version: the requests that hand out handles and shared
// memory. Input and the GPU behind them come later. Layouts follow fearkov/zakuro (MIT) hid.rs and
// gsp.rs and 3dbrew.
#include "hle.h"
#include "msr_log.h"

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

int svc_gsp(Ipc* q) {
    switch (q->cmd) {
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
    case 0x000B: case 0x000C: case 0x001C: case 0x001E: case 0x001F:
        msr_log("gsp: cmd %04X acknowledged (no GPU yet)", q->cmd);
        ipc_reply(q, 0, 0);
        return 1;
    }
    return 0;
}
