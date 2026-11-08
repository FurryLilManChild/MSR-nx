#pragma once
#include "msr_cpu.h"

extern char msr_extract_dir[512];
Cpu* msr_boot(const char* dir);
