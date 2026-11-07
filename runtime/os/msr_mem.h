#pragma once
#include "msr_cpu.h"

// 3DS memory permissions and states (3dbrew "Memory Management": MemoryPermission, MemoryState).
#define MSR_PERM_R   1u
#define MSR_PERM_W   2u
#define MSR_PERM_X   4u
#define MSR_PERM_RW  3u
#define MSR_PERM_RX  5u

#define MSR_STATE_FREE        0u
#define MSR_STATE_IO          2u
#define MSR_STATE_STATIC      3u
#define MSR_STATE_CODE        4u
#define MSR_STATE_PRIVATE     5u
#define MSR_STATE_SHARED      6u
#define MSR_STATE_CONTINUOUS  7u

typedef struct {
    u32 base, size, perm, state;
    const char* name;
} MsrRegion;

int msr_mem_init(void);
int msr_mem_map(u32 addr, u32 size, u32 perm, u32 state, const char* name);
int msr_mem_commit(u32 addr, u32 size);   // read/write private memory
MsrRegion msr_mem_query(u32 addr);

// 3DS address map as used by applications (3dbrew "Memory layout"; checked against the probe).
#define MSR_CODE_BASE    0x00100000u
#define MSR_HEAP_BASE    0x08000000u
#define MSR_STACK_TOP    0x10000000u
#define MSR_LINEAR_BASE  0x14000000u
#define MSR_TLS_BASE     0x1FF82000u
