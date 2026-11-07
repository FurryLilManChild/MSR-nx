#pragma once
#include "msr_cpu.h"

int msr_mem_init(void);
int msr_mem_commit(u32 addr, u32 size);

// 3DS address map as used by applications (3dbrew "Memory layout"; checked against the probe).
#define MSR_CODE_BASE    0x00100000u
#define MSR_HEAP_BASE    0x08000000u
#define MSR_STACK_TOP    0x10000000u
#define MSR_LINEAR_BASE  0x14000000u
#define MSR_TLS_BASE     0x1FF82000u
