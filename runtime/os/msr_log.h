// MSR-nx: logging and fatal errors.
#pragma once
#include "msr_cpu.h"

void msr_log_open(const char* dir);
void msr_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void msr_receipt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void msr_fatal(Cpu* c, const char* why);
void msr_log_close(void);
