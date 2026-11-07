// MSR-nx: the 4 GiB guest window. Reserved once; regions the game uses are committed on demand.
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

u8* g_mem;

int msr_mem_init(void) {
#ifdef _WIN32
    g_mem = (u8*)VirtualAlloc(NULL, (SIZE_T)1 << 32, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(NULL, (size_t)1 << 32, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    g_mem = p == MAP_FAILED ? 0 : (u8*)p;
#endif
    if (!g_mem) {
        msr_receipt("memory: could not reserve the 4 GiB guest window");
        return 0;
    }
    msr_log("memory: guest window at %p", (void*)g_mem);
    return 1;
}

int msr_mem_commit(u32 addr, u32 size) {
    u32 start = addr & ~0xFFFu;
    u64 end = ((u64)addr + size + 0xFFF) & ~(u64)0xFFF;
#ifdef _WIN32
    void* p = VirtualAlloc(g_mem + start, (SIZE_T)(end - start), MEM_COMMIT, PAGE_READWRITE);
    int ok = p != NULL;
#else
    int ok = mprotect(g_mem + start, (size_t)(end - start), PROT_READ | PROT_WRITE) == 0;
#endif
    msr_log("memory: commit %08X..%08X %s", start, (u32)(end - 1), ok ? "ok" : "FAILED");
    return ok;
}
