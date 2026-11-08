// MSR-nx: logging and fatal errors.
#pragma once
#include "msr_cpu.h"

void msr_log_open(const char* dir);
void msr_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void msr_receipt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void msr_fatal(Cpu* c, const char* why);
void msr_log_close(void);

// Speed meter: time spent in the parts of the platform layer that might slow the game down. The
// Switch build writes a line to the receipt every few seconds; the probe prints totals at the end.
#include <time.h>
enum { PERF_TRANSFER, PERF_TEXCOPY, PERF_READBACK, PERF_SURF_UPLOAD, PERF_TEX_UPLOAD, PERF_VERTEX, PERF_GPU_WAIT,
       PERF_PRESENT, PERF_COUNT };
extern unsigned long long msr_perf_ns[PERF_COUNT];
extern unsigned msr_perf_n[PERF_COUNT];
static inline unsigned long long msr_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}
#define MSR_PERF_BEGIN(id) unsigned long long msr_perf_t0_##id = msr_now_ns()
#define MSR_PERF_END(id) (msr_perf_ns[id] += msr_now_ns() - msr_perf_t0_##id, msr_perf_n[id]++)
void msr_perf_report(double seconds);   // one receipt line for the interval, then resets the meters
