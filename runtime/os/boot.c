// MSR-nx: loading the game and setting up its address space, shared by the probe and the Switch.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "sched.h"

char msr_extract_dir[512];

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

// Loads code.bin and exheader.bin from dir, maps the address space and makes the main thread.
// Returns the main thread's Cpu, ready for msr_run from MSR_CODE_BASE, or 0.
Cpu* msr_boot(const char* dir) {
    snprintf(msr_extract_dir, sizeof msr_extract_dir, "%s", dir);
    char path[600];
    size_t code_size = 0, ex_size = 0;
    snprintf(path, sizeof path, "%s/code.bin", dir);
    u8* code = read_file(path, &code_size);
    snprintf(path, sizeof path, "%s/exheader.bin", dir);
    u8* ex = read_file(path, &ex_size);
    if (!code || !ex || ex_size < 0x400) {
        msr_receipt("stopped: could not read code.bin and exheader.bin from %s", dir);
        return 0;
    }
    u32 data_addr, data_pages, bss, stack;
    memcpy(&data_addr, ex + 0x30, 4);
    memcpy(&data_pages, ex + 0x34, 4);
    memcpy(&bss, ex + 0x3C, 4);
    memcpy(&stack, ex + 0x1C, 4);
    msr_receipt("code.bin %u bytes, data at %08X (%u pages), bss %X, stack %X",
                (unsigned)code_size, data_addr, data_pages, bss, stack);
    if (!msr_mem_init()) return 0;
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
    // Config memory and the shared page, the values an Old 3DS shows (3dbrew "Configuration Memory")
    wr32(0x1FF80030u, 0);             // APPMEMTYPE: 64 MB, Old 3DS
    wr32(0x1FF80040u, 0x04000000u);   // APPMEMALLOC
    wr32(0x1FF80044u, 0x02C00000u);   // SYSMEMALLOC
    wr32(0x1FF80048u, 0x01400000u);   // BASEMEMALLOC
    wr32(0x1FF81004u, 1);             // RUNNING_HW: retail
    wr32(0x1FF81080u, 0);             // 3D_SLIDERSTATE: 0.0f
    msr_mem_map(0x1F000000u, 0x600000, MSR_PERM_RW, MSR_STATE_SHARED, "vram");
    msr_mem_map(0x1FF00000u, 0x80000, MSR_PERM_RW, MSR_STATE_SHARED, "dsp ram");
    msr_mem_map(MSR_TLS_BASE, 0x4000, MSR_PERM_RW, MSR_STATE_PRIVATE, "tls (32 threads)");

    static Cpu boot_cpu;
    Cpu* c = &boot_cpu;
    memset(c, 0, sizeof *c);
    c->r[13] = MSR_STACK_TOP;
    c->tls = MSR_TLS_BASE;
    const u32 stop = 0xFFFFFFF0u;
    c->r[14] = stop;
    int prio = ex[0x20F] ? ex[0x20F] : 0x30;   // ARM11 local caps: main thread priority
    sched_init_main(c, prio);
    c = sched_main_cpu();
    msr_receipt("running from entry %08X (main thread priority %X)", MSR_CODE_BASE, prio);
    free(code);
    free(ex);
    return c;
}
