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

static u32 g_heap_used, g_linear_used;
static unsigned g_svc_count, g_ipc_count;
static unsigned g_svc_hist[0x80];
unsigned msr_svc_limit = 20000;
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
        u32 h = new_handle("thread");
        msr_receipt("thread: CreateThread entry %08X arg %08X stack %08X prio %X core %d -> %X (not started in the probe)",
                    r[1], r[2], r[3], r[0], (s32)r[4], h);
        r[0] = 0; r[1] = h;
        return;
    }
    case 0x09:
        msr_receipt("stopped: ExitThread");
        msr_fatal(c, "ExitThread");
    case 0x0A:  // SleepThread
        msr_ticks += ((u64)r[1] << 32 | r[0]) / 4;
        r[0] = 0;
        return;
    case 0x13: r[0] = 0; r[1] = new_handle("mutex"); return;
    case 0x14: r[0] = 0; return;
    case 0x17: r[0] = 0; r[1] = new_handle("event"); return;
    case 0x18: case 0x19: r[0] = 0; return;
    case 0x1E: r[0] = 0; r[1] = new_handle("memblock"); return;
    case 0x1F:  // MapMemoryBlock(handle, addr, perm, other perm)
        msr_receipt("memory: MapMemoryBlock %X (%s) at %08X", r[0], handle_name(r[0]), r[1]);
        if (r[1]) {
            KObject* o = k_get(r[0]);
            u32 size = o && o->size ? o->size : 0x10000;
            msr_mem_map(r[1], size, MSR_PERM_RW, MSR_STATE_SHARED, handle_name(r[0]));
        }
        r[0] = 0;
        return;
    case 0x20: r[0] = 0; return;
    case 0x21: r[0] = 0; r[1] = new_handle("arbiter"); return;
    case 0x22: r[0] = 0; return;
    case 0x23: k_close(r[0]); r[0] = 0; return;
    case 0x24:  // WaitSynchronization1
        msr_log("svc WaitSynchronization1 %X (%s) (stand-in: signalled)", r[0], handle_name(r[0]));
        r[0] = 0;
        return;
    case 0x25:  // WaitSynchronizationN -> r1 = index
        r[0] = 0; r[1] = 0;
        return;
    case 0x27: r[0] = 0; r[1] = k_dup(r[1]); return;
    case 0x28:  // GetSystemTick
        msr_ticks += 268111856ull / 1000;  // the probe clock moves 1 ms per read
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
        return;
    case 0x35: r[0] = 0; r[1] = 1; return;
    case 0x37: r[0] = 0; r[1] = 1; return;
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
