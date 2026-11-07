// MSR-nx: the 4 GiB guest window. Reserved once; regions the game uses are committed on demand
// and recorded (base, size, permissions, 3DS memory state) so QueryMemory can answer truthfully.
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

u8* g_mem;

#define MAX_REGIONS 256
static MsrRegion g_regions[MAX_REGIONS];
static int g_region_count;

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

static void add_region(u32 base, u32 size, u32 perm, u32 state, const char* name) {
    // Keep the table sorted and merge an exact neighbour of the same kind (heap growth).
    for (int k = 0; k < g_region_count; k++) {
        MsrRegion* r = &g_regions[k];
        if (r->base + r->size == base && r->perm == perm && r->state == state) {
            r->size += size;
            return;
        }
    }
    if (g_region_count == MAX_REGIONS) return;
    int k = g_region_count++;
    while (k > 0 && g_regions[k - 1].base > base) {
        g_regions[k] = g_regions[k - 1];
        k--;
    }
    g_regions[k].base = base;
    g_regions[k].size = size;
    g_regions[k].perm = perm;
    g_regions[k].state = state;
    g_regions[k].name = name;
}

int msr_mem_map(u32 addr, u32 size, u32 perm, u32 state, const char* name) {
    u32 start = addr & ~0xFFFu;
    u64 end = ((u64)addr + size + 0xFFF) & ~(u64)0xFFF;
#ifdef _WIN32
    void* p = VirtualAlloc(g_mem + start, (SIZE_T)(end - start), MEM_COMMIT, PAGE_READWRITE);
    int ok = p != NULL;
#else
    int ok = mprotect(g_mem + start, (size_t)(end - start), PROT_READ | PROT_WRITE) == 0;
#endif
    msr_log("memory: map %08X..%08X perm %X state %X (%s) %s", start, (u32)(end - 1), perm, state,
            name, ok ? "ok" : "FAILED");
    if (ok) add_region(start, (u32)(end - start), perm, state, name);
    return ok;
}

int msr_mem_commit(u32 addr, u32 size) {
    return msr_mem_map(addr, size, MSR_PERM_RW, MSR_STATE_PRIVATE, "commit");
}

MsrRegion msr_mem_query(u32 addr) {
    MsrRegion out;
    u32 free_base = 0;
    for (int k = 0; k < g_region_count; k++) {
        MsrRegion* r = &g_regions[k];
        if (addr >= r->base && addr - r->base < r->size) return *r;
        if (r->base > addr) {
            out.base = free_base;
            out.size = r->base - free_base;
            out.perm = 0;
            out.state = MSR_STATE_FREE;
            out.name = "free";
            return out;
        }
        free_base = r->base + r->size;
    }
    out.base = free_base;
    out.size = (u32)(0x100000000ull - free_base);
    out.perm = 0;
    out.state = MSR_STATE_FREE;
    out.name = "free";
    return out;
}
