// MSR-nx: logs. msr.log gets everything; receipt.txt gets the probe's summary lines.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "msr_log.h"

static FILE* g_log;
static FILE* g_receipt;
int msr_full_log = 1;   // the detailed msr.log; the Switch build turns it off unless asked for

void msr_log_open(const char* dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/msr.log", dir);
    if (msr_full_log) g_log = fopen(path, "w");
    snprintf(path, sizeof path, "%s/receipt.txt", dir);
    g_receipt = fopen(path, "w");
}

// The Switch build (msr_log_buffered) keeps the game's threads off the SD card: lines go into
// memory and the main loop writes them out about once a second (msr_log_pump). An SD write can take
// milliseconds now and then, which made frames miss their slot when the game's thread waited on it.
int msr_log_buffered;
#define LOGBUF (512u << 10)
typedef struct { char data[LOGBUF]; unsigned n; } LogBuf;
static LogBuf g_buf[2];   // 0: the receipt, 1: msr.log
static pthread_mutex_t g_buf_lock = PTHREAD_MUTEX_INITIALIZER;

static void buf_write(FILE* f, LogBuf* b) {
    if (f && b->n) { fwrite(b->data, 1, b->n, f); fflush(f); }
    b->n = 0;
}
void msr_log_pump(void) {
    static LogBuf out[2];   // swapped out under the lock, written outside it
    pthread_mutex_lock(&g_buf_lock);
    for (int k = 0; k < 2; k++) { memcpy(out[k].data, g_buf[k].data, g_buf[k].n); out[k].n = g_buf[k].n; g_buf[k].n = 0; }
    pthread_mutex_unlock(&g_buf_lock);
    buf_write(g_receipt, &out[0]);
    buf_write(g_log, &out[1]);
}

static void vlog(FILE* f, const char* fmt, va_list ap) {
    if (!f) return;
    if (msr_log_buffered && (f == g_receipt || f == g_log)) {
        char line[1024];
        int n = vsnprintf(line, sizeof line - 1, fmt, ap);
        if (n < 0) return;
        if (n > (int)sizeof line - 2) n = sizeof line - 2;
        line[n++] = '\n';
        LogBuf* b = &g_buf[f == g_log];
        pthread_mutex_lock(&g_buf_lock);
        if (b->n + n > LOGBUF) { fwrite(b->data, 1, b->n, f); b->n = 0; }   // full: write it out here
        memcpy(b->data + b->n, line, n);
        b->n += n;
        pthread_mutex_unlock(&g_buf_lock);
        return;
    }
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
    if (!msr_log_buffered) {
        va_start(ap, fmt);
        vlog(stdout, fmt, ap);   // the window shows the receipt too
        va_end(ap);
    }
    va_start(ap, fmt);
    vlog(g_log, fmt, ap);
    va_end(ap);
}

void msr_dump_state(Cpu* c);
void msr_dump_trace(void);

void msr_fatal(Cpu* c, const char* why) {
    msr_receipt("stopped: %s", why);
    if (c) msr_dump_state(c);
    msr_dump_trace();
    msr_log_close();
    exit(2);
}

void msr_log_close(void) {
    msr_log_pump();
    if (g_log) fclose(g_log);
    if (g_receipt) fclose(g_receipt);
    g_log = g_receipt = 0;
}

unsigned long long msr_perf_ns[PERF_COUNT];
unsigned msr_perf_n[PERF_COUNT];

void msr_perf_report(double seconds) {
    static const char* names[PERF_COUNT] = {"transfers", "copies", "readbacks", "surface uploads",
                                            "texture uploads", "vertex fetch", "GPU waits", "present"};
    char line[512];
    int n = 0;
    for (int k = 0; k < PERF_COUNT; k++) {
        if (!msr_perf_n[k]) continue;
        n += snprintf(line + n, sizeof line - n, "%s%s %u in %.0f ms", n ? ", " : "", names[k], msr_perf_n[k],
                      msr_perf_ns[k] / 1e6);
        if (n >= (int)sizeof line) break;
        msr_perf_ns[k] = 0;
        msr_perf_n[k] = 0;
    }
    msr_receipt("  time per %.1f s: %s", seconds, n ? line : "(nothing measured)");
}
