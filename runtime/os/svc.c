// MSR-nx: 3DS kernel calls, first version (probe).
//
// Goal of this version: find out what the game asks the system for, in order. Calls that are
// simple to do right (memory, handles, ticks, ids) are done right. Service requests through IPC
// are logged and answered "success" with zeroed outputs, which is enough to see the next request
// but is not real behaviour. The probe receipt says which answers were stand-ins.
//
// SVC numbers and register conventions follow 3dbrew's "SVC" page; every call is logged with its
// raw number and arguments, so a wrong assumption here shows up in the log.
#include <stdio.h>
#include <string.h>
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"

#include "hle.h"
#include "sched.h"

static u32 g_heap_used, g_linear_used;
static unsigned g_svc_count, g_ipc_count;
static unsigned g_svc_hist[0x80];
unsigned msr_svc_limit = 2000000;
u64 msr_time_limit_ns = 20000000000ull;   // the probe stops after 20 s of game time
u64 msr_ticks;          // advanced by the probe clock

static u32 new_handle(const char* name) {
    static const struct { const char* name; int type; } kinds[] = {
        {"event", OBJ_EVENT}, {"mutex", OBJ_MUTEX}, {"thread", OBJ_THREAD},
        {"memblock", OBJ_MEMBLOCK}, {"arbiter", OBJ_ARBITER}, {"reslimit", OBJ_RESLIMIT},
    };
    for (unsigned k = 0; k < sizeof kinds / sizeof kinds[0]; k++)
        if (!strcmp(name, kinds[k].name)) return k_new(kinds[k].type, name);
    return k_new(OBJ_SESSION, name);   // ports and services
}

static const char* handle_name(u32 h) {
    if (h == 0xFFFF8000u) return "current-process";
    if (h == 0xFFFF8001u) return "current-thread";
    KObject* o = k_get(h);
    return o ? o->name : "?";
}

static void read_cstr(u32 addr, char* out, int n) {
    int k = 0;
    for (; k < n - 1; k++) {
        char ch = (char)rd8(addr + k);
        if (!ch) break;
        out[k] = ch;
    }
    out[k] = 0;
}

static void ipc(Cpu* c, u32 handle) {
    u32 buf = c->tls + 0x80;
    u32 hdr = rd32(buf);
    u32 cmd = hdr >> 16;
    const char* svc_name = handle_name(handle);
    g_ipc_count++;
    Ipc q = {c, buf, cmd, handle};
    int done = 0;
    if (!strcmp(svc_name, "srv:")) done = svc_srv(&q);
    else if (!strncmp(svc_name, "APT:", 4)) done = svc_apt(&q);
    else if (!strncmp(svc_name, "hid:", 4)) done = svc_hid(&q);
    else if (!strcmp(svc_name, "gsp::Gpu")) done = svc_gsp(&q);
    else if (!strncmp(svc_name, "fs:", 3)) done = svc_fs(&q);
    else if (!strncmp(svc_name, "cfg:", 4)) done = svc_cfg(&q);
    else if (!strcmp(svc_name, "dsp::DSP")) done = svc_dsp(&q);
    else if (!strcmp(svc_name, "err:f") && cmd == 0x0001) {
        // ThrowFatalError: the game gave up. Words 1..: error type, result, pc, ...
        msr_receipt("stopped: the game reported a fatal error through err:f (type %X, result %08X, pc %08X, lr? %08X)",
                    rd32(buf + 4) & 0xFF, rd32(buf + 8), rd32(buf + 12), rd32(buf + 16));
        msr_fatal(c, "err:f fatal error");
    }
    if (done) {
        msr_log("ipc %s cmd %04X handled", svc_name, cmd);
        return;
    }
    u32 normal = (hdr >> 6) & 0x3F, translate = hdr & 0x3F;
    char args[160] = {0};
    int off = 0;
    for (u32 k = 1; k <= normal + translate && k <= 8; k++)
        off += snprintf(args + off, sizeof args - off, " %08X", rd32(buf + 4 * k));
    msr_receipt("ipc %s cmd %04X (stand-in success):%s", svc_name, cmd, args);
    // Stand-in reply: success and zeroed outputs.
    wr32(buf, (cmd << 16) | (8 << 6));
    for (int k = 1; k <= 8; k++) wr32(buf + 4 * k, 0);
}

void msr_svc(Cpu* c, u32 imm) {
    u32* r = c->r;
    g_svc_count++;
    g_svc_hist[imm & 0x7F]++;
    if (g_svc_count > msr_svc_limit) {
        msr_receipt("probe: %u kernel calls reached, stopping", msr_svc_limit);
        msr_fatal(c, "probe limit");
    }
    if (sched_now_ns() > msr_time_limit_ns) {
        msr_receipt("probe: %llu s of game time reached, stopping", (unsigned long long)(msr_time_limit_ns / 1000000000ull));
        sched_summary();
        msr_fatal(c, "time limit");
    }
    if (sched_poll_timers()) sched_after_signal(c);
    msr_log("svc %02X r0=%08X r1=%08X r2=%08X r3=%08X lr=%08X", imm, r[0], r[1], r[2], r[3], r[14]);
    {
        // Stall detection: the same call with the same arguments from the same place, over and over.
        static u32 last[6];
        static unsigned repeats;
        u32 cur[6] = {imm, r[0], r[1], r[2], r[3], r[14]};
        if (!memcmp(cur, last, sizeof cur)) {
            if (++repeats >= 200) {
                msr_receipt("probe: svc %02X repeated 200 times with the same arguments from lr %08X: "
                            "the game is waiting on something the runtime does not provide yet", imm, r[14]);
                msr_fatal(c, "stalled");
            }
        } else {
            repeats = 0;
            memcpy(last, cur, sizeof cur);
        }
    }
    switch (imm) {
    case 0x01: {  // ControlMemory(op, addr0, addr1, size, perm) -> r1 = address
        u32 op = r[0], addr0 = r[1], size = r[3];
        u32 base = op & 0x10000 ? MSR_LINEAR_BASE : MSR_HEAP_BASE;
        if ((op & 0xFF) == 3) {  // COMMIT
            u32 a = addr0 ? addr0 : base + (op & 0x10000 ? g_linear_used : g_heap_used);
            if (!addr0) { if (op & 0x10000) g_linear_used += size; else g_heap_used += size; }
            msr_mem_map(a, size, MSR_PERM_RW, op & 0x10000 ? MSR_STATE_CONTINUOUS : MSR_STATE_PRIVATE,
                        op & 0x10000 ? "linear heap" : "heap");
            msr_receipt("memory: ControlMemory commit %08X size %X%s", a, size, op & 0x10000 ? " (linear)" : "");
            r[0] = 0; r[1] = a;
        } else {
            msr_receipt("memory: ControlMemory op %X addr %08X size %X (not done)", op, addr0, size);
            r[0] = 0; r[1] = addr0;
        }
        return;
    }
    case 0x02: {  // QueryMemory(addr in r2) -> r1 base, r2 size, r3 perm, r4 state, r5 page flags
        MsrRegion q = msr_mem_query(r[2]);
        msr_log("svc QueryMemory %08X -> %08X size %X perm %X state %X (%s)", r[2], q.base, q.size,
                q.perm, q.state, q.name);
        r[0] = 0; r[1] = q.base; r[2] = q.size; r[3] = q.perm; r[4] = q.state; r[5] = 0;
        return;
    }
    case 0x03:
        msr_receipt("stopped: the game called ExitProcess");
        msr_fatal(0, "ExitProcess");
    case 0x08: {  // CreateThread(priority, entry, arg, stack top, core) -> r1 = handle
        u32 h = sched_create_thread(c, r[0], r[1], r[2], r[3], (s32)r[4]);
        r[0] = h ? 0 : 0xC860180Au; r[1] = h;
        if (h) sched_after_signal(c);
        return;
    }
    case 0x09:  // ExitThread
        sched_exit_thread();
        return;
    case 0x0A:  // SleepThread(ns in r0:r1)
        sched_sleep(c, (s64)((u64)r[1] << 32 | r[0]));
        return;
    case 0x0B: {  // GetThreadPriority(handle in r1) -> r1
        int p = sched_get_priority(r[1]);
        r[0] = p < 0 ? 0xD8E007F7u : 0; r[1] = (u32)p;
        return;
    }
    case 0x0C:  // SetThreadPriority(handle, priority)
        sched_set_priority(c, r[0], (int)r[1]);
        r[0] = 0;
        return;
    case 0x13:  // CreateMutex(initially locked in r1) -> r1
        r[1] = sched_create_mutex(r[1] != 0); r[0] = 0;
        return;
    case 0x14:  // ReleaseMutex(handle)
        r[0] = sched_release_mutex(c, r[0]);
        return;
    case 0x15: {  // CreateSemaphore(initial r1, max r2) -> r1
        u32 h = k_new(OBJ_SEMAPHORE, "semaphore");
        k_get(h)->signalled = r[1];
        k_get(h)->size = r[2];
        r[0] = 0; r[1] = h;
        return;
    }
    case 0x16: {  // ReleaseSemaphore(handle r1, count r2) -> r1 previous count
        u32 old = 0;
        r[0] = sched_release_semaphore(c, r[1], (s32)r[2], &old); r[1] = old;
        return;
    }
    case 0x17: {  // CreateEvent(reset type in r1) -> r1
        u32 h = k_new(OBJ_EVENT, "event");
        k_get(h)->sticky = r[1];
        r[0] = 0; r[1] = h;
        return;
    }
    case 0x18:  // SignalEvent
        sched_signal_event(c, r[0]);
        r[0] = 0;
        return;
    case 0x19: {  // ClearEvent
        KObject* o = k_get(r[0]);
        if (o) o->signalled = 0;
        r[0] = 0;
        return;
    }
    case 0x1E: r[0] = 0; r[1] = new_handle("memblock"); return;
    case 0x1F:  // MapMemoryBlock(handle, addr, perm, other perm)
        msr_receipt("memory: MapMemoryBlock %X (%s) at %08X", r[0], handle_name(r[0]), r[1]);
        if (r[1]) {
            KObject* o = k_get(r[0]);
            u32 size = o && o->size ? o->size : 0x10000;
            msr_mem_map(r[1], size, MSR_PERM_RW, MSR_STATE_SHARED, handle_name(r[0]));
            if (o) o->addr = r[1];
        }
        r[0] = 0;
        return;
    case 0x20: r[0] = 0; return;
    case 0x21: r[0] = 0; r[1] = new_handle("arbiter"); return;
    case 0x22:  // ArbitrateAddress(arbiter, address, type, value, timeout ns in r4:r5)
        r[0] = sched_arbitrate(c, r[0], r[1], r[2], (s32)r[3], (s64)((u64)r[5] << 32 | r[4]));
        return;
    case 0x23: k_close(r[0]); r[0] = 0; return;
    case 0x24: {  // WaitSynchronization1(handle, timeout ns in r2:r3)
        u32 h = r[0];
        int idx;
        r[0] = sched_wait(c, &h, 1, 0, (s64)((u64)r[3] << 32 | r[2]), &idx);
        return;
    }
    case 0x25: {  // WaitSynchronizationN(timeout lo r0, handles r1, count r2, wait all r3, timeout hi r4) -> r1 index
        u32 hs[64];
        int n = (s32)r[2] > 64 ? 64 : (s32)r[2], idx = -1;
        for (int k = 0; k < n; k++) hs[k] = rd32(r[1] + 4 * k);
        r[0] = sched_wait(c, hs, n, r[3] != 0, (s64)((u64)r[4] << 32 | r[0]), &idx);
        r[1] = (u32)idx;
        return;
    }
    case 0x27: r[0] = 0; r[1] = k_dup(r[1]); return;
    case 0x28:  // GetSystemTick
        sched_advance_ns(100000);  // the virtual clock moves 0.1 ms per read
        msr_ticks = sched_now_ns() / 1000 * 268111856ull / 1000000;
        r[0] = (u32)msr_ticks; r[1] = (u32)(msr_ticks >> 32);
        return;
    case 0x2B: r[0] = 0; r[1] = 0; r[2] = 0; return;
    case 0x2D: {  // ConnectToPort(out handle, name)
        char name[16];
        read_cstr(r[1], name, sizeof name);
        u32 h = new_handle(name);
        msr_receipt("port: ConnectToPort \"%s\" -> %X", name, h);
        r[0] = 0; r[1] = h;
        return;
    }
    case 0x32:  // SendSyncRequest(handle)
        ipc(c, r[0]);
        r[0] = 0;
        sched_after_signal(c);   // a service may have woken a higher-priority thread
        return;
    case 0x35: r[0] = 0; r[1] = 1; return;
    case 0x37: r[0] = 0; r[1] = sched_thread_id(r[1]); return;  // GetThreadId
    case 0x38: r[0] = 0; r[1] = new_handle("reslimit"); return;
    case 0x39: case 0x3A: r[0] = 0; return;
    case 0x3C:
        msr_receipt("stopped: the game called Break (reason %X)", r[0]);
        msr_fatal(c, "Break");
    default:
        msr_receipt("svc %02X not handled (stand-in success)", imm);
        r[0] = 0;
        return;
    }
}

void msr_svc_summary(void) {
    msr_receipt("kernel calls: %u total, %u service requests", g_svc_count, g_ipc_count);
    for (int k = 0; k < 0x80; k++)
        if (g_svc_hist[k]) msr_receipt("  svc %02X x%u", k, g_svc_hist[k]);
}

void msr_dump_state(Cpu* c) {
    msr_receipt("state: pc %08X lr %08X sp %08X", c->pc, c->r[14], c->r[13]);
    for (int k = 0; k < 13; k += 4)
        msr_receipt("  r%d %08X r%d %08X r%d %08X r%d %08X", k, c->r[k], k + 1, c->r[k + 1],
                    k + 2, c->r[k + 2], k + 3, c->r[k + 3]);
    msr_svc_summary();
}
