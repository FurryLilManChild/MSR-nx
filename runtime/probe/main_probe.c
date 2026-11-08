// MSR-nx probe: loads the game's code into the guest window and runs it from the entry point
// until it stops (a kernel call limit, a fatal error, a crash or ExitProcess). Writes msr.log and
// receipt.txt into the output folder.
//
// Usage: msr_probe <extract folder with code.bin and exheader.bin> <output folder>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "sched.h"
#include "boot.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif

void msr_run(Cpu* c, u32 entry, u32 stop);
void msr_svc_summary(void);
void msr_dump_state(Cpu* c);
extern unsigned msr_svc_limit;


#ifdef MSR_TRACE
const char* msr_trace_ring[64];
unsigned msr_trace_pos;
#endif

void msr_dump_trace(void) {
#ifdef MSR_TRACE
    msr_receipt("last functions entered (oldest first):");
    for (unsigned k = 0; k < 64; k++) {
        const char* n = msr_trace_ring[(msr_trace_pos + k) & 63];
        if (n) msr_receipt("  %s", n);
    }
#endif
}

static void crashed(unsigned long code, const void* where, int is_write) {
    u64 a = (u64)(uintptr_t)where;
    u64 base = (u64)(uintptr_t)g_mem;
    if (a >= base && a < base + (1ull << 32))
        msr_receipt("crash: %s guest address %08X (not committed memory)", is_write ? "write to" : "read of",
                    (u32)(a - base));
    else
        msr_receipt("crash: host exception %lX at host address %p", code, where);
    msr_dump_trace();
    msr_svc_summary();
    msr_log_close();
}

#ifdef _WIN32
static LONG WINAPI on_exception(EXCEPTION_POINTERS* ep) {
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    const void* where = r->ExceptionAddress;
    int is_write = 0;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        is_write = r->ExceptionInformation[0] == 1;
        where = (const void*)r->ExceptionInformation[1];
    }
    crashed(r->ExceptionCode, where, is_write);
    ExitProcess(3);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
static void on_signal(int sig, siginfo_t* si, void* ctx) {
    (void)ctx;
    crashed((unsigned long)sig, si->si_addr, 0);
    _exit(3);
}
#endif


int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: msr_probe <extract folder> <output folder>\n");
        return 1;
    }
    msr_log_open(argv[2]);
    msr_receipt("MSR-nx probe 14 (PICA200 command processor, shader interpreter, reference renderer)");
#ifdef _WIN32
    AddVectoredExceptionHandler(1, on_exception);
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
#endif
    snprintf(msr_extract_dir, sizeof msr_extract_dir, "%s", argv[1]);
    {
        extern char msr_frames_dir[512];
        snprintf(msr_frames_dir, sizeof msr_frames_dir, "%s/frames", argv[2]);
#ifdef _WIN32
        _mkdir(msr_frames_dir);
#else
        mkdir(msr_frames_dir, 0777);
#endif
    }
    {
        extern u64 msr_time_limit_ns;
        extern unsigned msr_frame_every;
        const char* e = getenv("MSR_SECONDS");
        if (e) msr_time_limit_ns = (u64)atoi(e) * 1000000000ull;
        e = getenv("MSR_FRAME_EVERY");
        if (e) msr_frame_every = (unsigned)atoi(e);
    }
    { extern char msr_save_dir[512]; snprintf(msr_save_dir, sizeof msr_save_dir, "%s/save", argv[2]); }
    Cpu* c = msr_boot(argv[1]);
    if (!c) return 1;
    msr_run(c, MSR_CODE_BASE, 0xFFFFFFF0u);
    msr_receipt("the entry function returned");
    msr_dump_state(c);
    sched_summary();
    msr_log_close();
    return 0;
}
