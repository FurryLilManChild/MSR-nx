// MSR-nx probe: loads the game's code into the guest window and runs it from the entry point
// until it stops (a kernel call limit, a fatal error, a crash or ExitProcess). Writes msr.log and
// receipt.txt into the output folder.
//
// Usage: msr_probe <extract folder with code.bin and exheader.bin> <output folder>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"

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

static Cpu g_cpu;

#ifdef MSR_TRACE
const char* msr_trace_ring[64];
unsigned msr_trace_pos;
#endif

static void dump_trace(void) {
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
    dump_trace();
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

static u8* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* p = (u8*)malloc(*size);
    if (fread(p, 1, *size, f) != *size) { free(p); p = 0; }
    fclose(f);
    return p;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: msr_probe <extract folder> <output folder>\n");
        return 1;
    }
    msr_log_open(argv[2]);
    msr_receipt("MSR-nx probe 2 (generated code, kernel call logging, QueryMemory, stall detection)");
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
    char path[600];
    size_t code_size = 0, ex_size = 0;
    snprintf(path, sizeof path, "%s/code.bin", argv[1]);
    u8* code = read_file(path, &code_size);
    snprintf(path, sizeof path, "%s/exheader.bin", argv[1]);
    u8* ex = read_file(path, &ex_size);
    if (!code || !ex || ex_size < 0x400) {
        msr_receipt("stopped: could not read code.bin and exheader.bin from %s", argv[1]);
        return 1;
    }
    u32 data_addr, data_pages, bss, stack;
    memcpy(&data_addr, ex + 0x30, 4);
    memcpy(&data_pages, ex + 0x34, 4);
    memcpy(&bss, ex + 0x3C, 4);
    memcpy(&stack, ex + 0x1C, 4);
    msr_receipt("code.bin %u bytes, data at %08X (%u pages), bss %X, stack %X",
                (unsigned)code_size, data_addr, data_pages, bss, stack);
    if (!msr_mem_init()) return 1;
    u32 text_addr, text_pages, ro_addr, ro_pages;
    memcpy(&text_addr, ex + 0x10, 4);
    memcpy(&text_pages, ex + 0x14, 4);
    memcpy(&ro_addr, ex + 0x20, 4);
    memcpy(&ro_pages, ex + 0x24, 4);
    msr_mem_map(text_addr, text_pages * 0x1000, MSR_PERM_RX, MSR_STATE_CODE, "text");
    msr_mem_map(ro_addr, ro_pages * 0x1000, MSR_PERM_R, MSR_STATE_CODE, "rodata");
    msr_mem_map(data_addr, data_pages * 0x1000 + bss, MSR_PERM_RW, MSR_STATE_PRIVATE, "data+bss");
    memcpy(g_mem + MSR_CODE_BASE, code, code_size);
    u32 stack_size = stack ? (stack + 0xFFF) & ~0xFFFu : 0x10000;
    msr_mem_map(MSR_STACK_TOP - stack_size, stack_size, MSR_PERM_RW, MSR_STATE_PRIVATE, "stack");
    msr_mem_map(0x1FF80000u, 0x2000, MSR_PERM_R, MSR_STATE_SHARED, "config+shared page");
    msr_mem_map(MSR_TLS_BASE, 0x1000, MSR_PERM_RW, MSR_STATE_PRIVATE, "tls");

    Cpu* c = &g_cpu;
    memset(c, 0, sizeof *c);
    c->r[13] = MSR_STACK_TOP;
    c->tls = MSR_TLS_BASE;
    const u32 stop = 0xFFFFFFF0u;
    c->r[14] = stop;
    msr_receipt("running from entry %08X", MSR_CODE_BASE);
    msr_run(c, MSR_CODE_BASE, stop);
    msr_receipt("the entry function returned");
    msr_dump_state(c);
    msr_log_close();
    return 0;
}
