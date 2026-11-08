// MSR-nx: the 4 GiB guest window. Reserved once; regions the game uses are committed on demand
// and recorded (base, size, permissions, 3DS memory state) so QueryMemory can answer truthfully.
#include <string.h>
#include "msr_cpu.h"
#include "msr_log.h"
#include "msr_mem.h"

#if defined(__SWITCH__)
#include <malloc.h>
#include <switch.h>
// The Switch has no 4 GiB reserve-and-commit: a 512 MiB window (all a 3DS application can address
// below 0x20000000) is reserved in the stack region, and each region is backed by heap pages mapped
// in with svcMapMemory. A bitmap keeps track of mapped pages, so a region mapped twice is fine.
#define NX_WINDOW 0x20000000u
static u8 g_mapped[NX_WINDOW >> 15];   // one bit per 4 KiB page
#elif defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

u8* g_mem;

#define MAX_REGIONS 256
static MsrRegion g_regions[MAX_REGIONS];
static int g_region_count;

int msr_mem_init(void) {
#if defined(__SWITCH__)
    virtmemLock();
    g_mem = (u8*)virtmemFindStack(NX_WINDOW, 0x10000);
    if (g_mem) virtmemAddReservation(g_mem, NX_WINDOW);
    virtmemUnlock();
#elif defined(_WIN32)
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
#if defined(__SWITCH__)
    int ok = end <= NX_WINDOW;
    for (u32 p = start; ok && p < end;) {
        if (g_mapped[p >> 15] & (1 << ((p >> 12) & 7))) { p += 0x1000; continue; }
        u32 q = p;
        while (q < end && !(g_mapped[q >> 15] & (1 << ((q >> 12) & 7)))) q += 0x1000;
        void* src = memalign(0x1000, q - p);
        if (!src) { ok = 0; break; }
        memset(src, 0, q - p);
        if (R_FAILED(svcMapMemory(g_mem + p, src, q - p))) { ok = 0; break; }
        for (u32 r = p; r < q; r += 0x1000) g_mapped[r >> 15] |= 1 << ((r >> 12) & 7);
        p = q;
    }
#elif defined(_WIN32)
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
