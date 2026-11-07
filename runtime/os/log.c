// MSR-nx: logs. msr.log gets everything; receipt.txt gets the probe's summary lines.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "msr_log.h"

static FILE* g_log;
static FILE* g_receipt;

void msr_log_open(const char* dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/msr.log", dir);
    g_log = fopen(path, "w");
    snprintf(path, sizeof path, "%s/receipt.txt", dir);
    g_receipt = fopen(path, "w");
}

static void vlog(FILE* f, const char* fmt, va_list ap) {
    if (!f) return;
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    fflush(f);
}

void msr_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(g_log, fmt, ap);
    va_end(ap);
}

void msr_receipt(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(g_receipt, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vlog(stdout, fmt, ap);   // the window shows the receipt too
    va_end(ap);
    va_start(ap, fmt);
    vlog(g_log, fmt, ap);
    va_end(ap);
}

void msr_dump_state(Cpu* c);

void msr_fatal(Cpu* c, const char* why) {
    msr_receipt("stopped: %s", why);
    if (c) msr_dump_state(c);
    msr_log_close();
    exit(2);
}

void msr_log_close(void) {
    if (g_log) fclose(g_log);
    if (g_receipt) fclose(g_receipt);
    g_log = g_receipt = 0;
}
