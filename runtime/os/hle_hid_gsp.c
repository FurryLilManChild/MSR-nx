// MSR-nx: hid and gsp::Gpu services, first version: the requests that hand out handles and shared
// memory. Input and the GPU behind them come later. Layouts follow fearkov/zakuro (MIT) hid.rs and
// gsp.rs and 3dbrew.
#include <stdio.h>
#include <stdlib.h>
#include "frame_plan.h"
#include "eye_reuse.h"
#include "capture.h"
#include <string.h>
#include "hle.h"
#include "msr_log.h"
#include "sched.h"
#include "pica.h"
#include <pthread.h>
#include <time.h>

static u32 g_hid_mem, g_hid_events[5];
static u32 g_gsp_mem, g_gsp_event;
static int g_gsp_registered;

// ---- hid: the pad and touch screen sections of the shared memory, written every frame --------
// Layout (3dbrew "HID Shared Memory", fearkov/zakuro hid.rs): each section starts with the tick its
// ring last started over at, the one before, and the index of the newest entry; the pad section
// keeps the held buttons at 0x1C and eight 0x10-byte entries from 0x28 (held, pressed, released,
// circle pad x, y); the touch section at 0xA8 keeps eight 8-byte entries from 0xC8.
u32 msr_buttons;                 // set by the platform layer (Switch input, probe script)
s16 msr_circle_x, msr_circle_y;
int msr_touch_down;
u16 msr_touch_x, msr_touch_y;

static void hid_section_header(u32 sec, u32 index, u64* ticks) {
    u64 now = sched_now_ns() / 1000 * 268111856ull / 1000000;
    if (index == 0) { ticks[1] = ticks[0]; ticks[0] = now; }
    wr32(sec, (u32)ticks[0]); wr32(sec + 4, (u32)(ticks[0] >> 32));
    wr32(sec + 8, (u32)ticks[1]); wr32(sec + 12, (u32)(ticks[1] >> 32));
    wr32(sec + 0x10, index);
}

static void hid_update(void) {
    static u32 prev, pad_i, touch_i;
    static u64 pad_ticks[2], touch_ticks[2];
    KObject* o = g_hid_mem ? k_get(g_hid_mem) : 0;
    if (!o || !o->addr) return;
    u32 base = o->addr;
    u32 b = msr_buttons & 0xFFF;
    if (msr_circle_x > 75) b |= 1u << 28;
    if (msr_circle_x < -75) b |= 1u << 29;
    if (msr_circle_y > 75) b |= 1u << 30;
    if (msr_circle_y < -75) b |= 1u << 31;
    u32 pressed = b & ~prev, released = prev & ~b;
    prev = b;
    pad_i = (pad_i + 1) % 8;
    hid_section_header(base, pad_i, pad_ticks);
    wr32(base + 0x1C, b);
    u32 e = base + 0x28 + 0x10 * pad_i;
    wr32(e, b); wr32(e + 4, pressed); wr32(e + 8, released);
    wr16(e + 12, (u16)msr_circle_x); wr16(e + 14, (u16)msr_circle_y);
    touch_i = (touch_i + 1) % 8;
    hid_section_header(base + 0xA8, touch_i, touch_ticks);
    u32 t = base + 0xA8 + 0x20 + 8 * touch_i;
    wr16(t, msr_touch_down ? msr_touch_x : 0); wr16(t + 2, msr_touch_down ? msr_touch_y : 0);
    wr32(t + 4, msr_touch_down ? 1 : 0);
    sched_wake(g_hid_events[0]);
    sched_wake(g_hid_events[1]);
}

// Probe input script, MSR_INPUT="START@6.0-6.3,A@9-9.2": buttons held over game-time ranges.
static void probe_script(void) {
    static const char* names[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
    const char* e = getenv("MSR_INPUT");
    if (!e) return;
    double now = sched_now_ns() / 1e9;
    u32 b = 0;
    s16 cx = 0, cy = 0;
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", e);
    for (char* tok = strtok(buf, ","); tok; tok = strtok(0, ",")) {
        char name[16]; double t0, t1;
        if (sscanf(tok, "%15[A-Z]@%lf-%lf", name, &t0, &t1) != 3) continue;
        for (int k = 0; k < 12; k++)
            if (!strcmp(name, names[k]) && now >= t0 && now < t1) b |= 1u << k;
        if (now >= t0 && now < t1) {   // circle pad: CRIGHT, CLEFT, CUP, CDOWN
            if (!strcmp(name, "CRIGHT")) cx = 150;
            if (!strcmp(name, "CLEFT")) cx = -150;
            if (!strcmp(name, "CUP")) cy = 150;
            if (!strcmp(name, "CDOWN")) cy = -150;
        }
    }
    msr_circle_x = cx;
    msr_circle_y = cy;
    if (b != (msr_buttons & 0xFFF)) msr_log("input: buttons %03X at %.2f s", b, now);
    msr_buttons = b;
}

static void hid_frame(void) {
    probe_script();
    hid_update();
}

int svc_hid(Ipc* q) {
    static int timer;
    if (!timer) { timer = 1; sched_add_timer(16715000, hid_frame); }
    switch (q->cmd) {
    case 0x000A: {  // GetIPCHandles -> shared memory, 5 events (pad 0, pad 1, accel, gyro, debug)
        if (!g_hid_mem) {
            g_hid_mem = k_new(OBJ_MEMBLOCK, "hid:mem");
            k_get(g_hid_mem)->size = 0x1000;
            for (int k = 0; k < 5; k++) g_hid_events[k] = k_new(OBJ_EVENT, "hid:event");
        }
        ipc_set(q, 0, IPC_HDR(0x000A, 1, 7));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x14000000);  // copy handles, six
        ipc_set(q, 3, g_hid_mem);
        for (int k = 0; k < 5; k++) ipc_set(q, 4 + k, g_hid_events[k]);
        msr_receipt("hid: GetIPCHandles -> shared memory %X", g_hid_mem);
        return 1;
    }
    case 0x0011: case 0x0012: case 0x0013: case 0x0014:  // sensors on/off
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0015: {  // GetGyroscopeLowRawToDpsCoefficient -> float
        u32 w[1] = {0x41136666u};  // 9.2f
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0016: {  // GetGyroscopeLowCalibrateParam -> 20 bytes of zero offsets
        u32 w[5] = {0, 0, 0, 0, 0};
        ipc_reply(q, w, 5);
        return 1;
    }
    case 0x0017: {  // GetSoundVolume -> full
        u32 w[1] = {0x3F};
        ipc_reply(q, w, 1);
        return 1;
    }
    }
    return 0;
}

// ---- GSP: the interrupt relay queue and GX commands ------------------------------------------
// Shared memory layout (3dbrew "GSP Shared Memory"): interrupt queues at 0x40 * thread index,
// framebuffer info at 0x200, GX command rings at 0x800 + 0x200 * thread index.
enum { IRQ_PSC0, IRQ_PSC1, IRQ_VBLANK_TOP, IRQ_VBLANK_BOTTOM, IRQ_PPF, IRQ_P3D, IRQ_DMA };

static u32 gsp_shared(void) {
    KObject* o = g_gsp_mem ? k_get(g_gsp_mem) : 0;
    return o ? o->addr : 0;
}

// ---- the graphics thread ----------------------------------------------------------------------
// On the 3DS the GPU works through the game's GX commands while the CPU goes on with the game.
// Here the same: the commands go to a host thread of their own (on its own core), which runs them
// in order (PICA command lists through the renderer, transfers, copies, fills, DMA) and hands the
// completion interrupts back; the guest side delivers them in order whenever it switches threads
// or waits. The framebuffer latch's renderer half goes through the same queue so it stays in
// order with the drawing. msr_gx_async (main_nx.c, the port menu) turns it on; the probe keeps
// everything on the guest thread.
volatile int msr_gx_async;            // wanted
static int g_async;                   // in use (changed only with the queue empty)
static __thread int t_worker;         // this host thread is the graphics thread
enum { JOB_GX, JOB_LATCH };
typedef struct { u32 kind, id, d[7]; u32 screen, left, right, stride, format, lv, rv, nout; u32 out[16]; } GxJob;
#define JOBS 1024
static GxJob g_jobs[JOBS];
static volatile u32 g_job_head, g_job_tail;   // head: next to write (guest), tail: next to run (worker)
static volatile int g_worker_busy;
static pthread_mutex_t g_job_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_job_cv = PTHREAD_COND_INITIALIZER;
#define IRQS 1024
static volatile u8 g_irq_ring[IRQS];
static volatile u32 g_irq_head, g_irq_tail;   // head: worker writes, tail: guest delivers
unsigned long long msr_gx_busy_ns;            // the graphics thread's time on commands (speed report)
static unsigned long long mono_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec; }

static void gsp_interrupt_now(int id);
static void gsp_interrupt(int id) {
    if (t_worker) {   // the guest side delivers it
        u32 h = g_irq_head;
        while (h - __atomic_load_n(&g_irq_tail, __ATOMIC_ACQUIRE) >= IRQS) { struct timespec ts = {0, 100000}; nanosleep(&ts, 0); }
        g_irq_ring[h % IRQS] = (u8)id;
        __atomic_store_n(&g_irq_head, h + 1, __ATOMIC_RELEASE);
        return;
    }
    gsp_interrupt_now(id);
}
// sched.c: interrupts finished on the graphics thread, delivered on the guest side; 1 if any.
// the graphics thread still has queued work or is at it
int sched_async_busy(void) {
    return __atomic_load_n(&g_job_head, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_job_tail, __ATOMIC_ACQUIRE) || g_worker_busy;
}
int sched_async_pending(void) { return __atomic_load_n(&g_irq_head, __ATOMIC_ACQUIRE) != g_irq_tail; }
int sched_async_deliver(void) {
    int n = 0;
    while (sched_async_pending()) {
        u32 t = g_irq_tail;
        int id = g_irq_ring[t % IRQS];
        __atomic_store_n(&g_irq_tail, t + 1, __ATOMIC_RELEASE);
        gsp_interrupt_now(id);
        n++;
    }
    return n;
}

static void gsp_interrupt_now(int id) {
    u32 qa = gsp_shared();
    if (!qa || !g_gsp_event) return;
    u32 index = rd8(qa), count = rd8(qa + 1);
    if (count >= 0x34) { wr8(qa + 2, 1); return; }   // full: the error flag, the interrupt is lost
    wr8(qa + 0xC + (index + count) % 0x34, (u8)id);
    wr8(qa + 1, (u8)(count + 1));
    sched_wake(g_gsp_event);
}

unsigned msr_gx_counts[8];

unsigned gx_stat[4];
// MSR_GXTRACE=N: every GX command and draw of top-screen pictures N and N+1 goes to the receipt.
int msr_gx_trace(void) {
    static int start = -2;
    if (start == -2) { const char* e = getenv("MSR_GXTRACE"); start = e ? atoi(e) : -1; }
    extern unsigned msr_top_swaps(void);
    return start >= 0 && msr_top_swaps() >= (unsigned)start && msr_top_swaps() < (unsigned)start + 2;
}   // DMA, fills, display transfers, texture copies

void gx_execute(u32 id, u32* d);
static void gx_command(u32 entry) {
    u32 id = rd32(entry) & 0xFF, d[7];
    for (int k = 0; k < 7; k++) d[k] = rd32(entry + 4 + 4 * k);
    msr_capture_gx(id, d);
    gx_execute(id, d);
}

// A VRAM to VRAM copy outside the game's own commands: right eye reuse making the right eye real.
static void eye_copy(u32 src, u32 dst, u32 n) {
    u32 dd[3] = {src, dst, n};
    if (pica_gx_hook && pica_gx_hook(0, dd)) return;
    if (pica_sync_hook) pica_sync_hook(src, n);
    memmove(g_mem + dst, g_mem + src, n);
    if (pica_invalidate_hook) pica_invalidate_hook(dst, n);
}

static u64 tmix(u64 h, u32 v) { for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 0x100000001B3ull; } return h; }

// One GX command (also replayed from a frame capture by the probe).
void gx_execute(u32 id, u32* d) {
    { static int set; if (!set) { set = 1; eye_reuse_copier(eye_copy); } }
    if (id < 8) msr_gx_counts[id]++;
    switch (id) {
    case 0x00:  // RequestDma(source, destination, size)
        gx_stat[0]++;
        { u32 rr[2] = {d[0], d[0] + d[2]}, ww[2] = {d[1], d[1] + d[2]};
          fp_op(FP_DMA, ((u64)d[0] << 32) ^ d[1], rr, 1, ww, 1, 1); }   // always runs: the game's own data
        eye_before_read(d[0], d[0] + d[2]);
        eye_written(d[1], d[1] + d[2], (d[0] >= 0x1F000000u && d[0] < 0x1F600000u) ? tmix(eye_token_of(d[0], d[0] + d[2]), d[2]) : 0);
        if (!(pica_gx_hook && pica_gx_hook(0, d))) {   // the GPU renderer may do it on its side
            if (pica_sync_hook) pica_sync_hook(d[0], d[2]);
            if (d[2] && d[0] && d[1]) memmove(g_mem + d[1], g_mem + d[0], d[2]);
            if (pica_invalidate_hook) pica_invalidate_hook(d[1], d[2]);
        }
        msr_log("gx: dma %08X -> %08X size %X", d[0], d[1], d[2]);
        gsp_interrupt(IRQ_DMA);
        break;
    case 0x01:  // ProcessCommandList(address, size, flags)
        msr_log("gx: command list %08X size %X", d[0], d[1]);
        pica_run_list(d[0], d[1]);
        gsp_interrupt(IRQ_P3D);
        break;
    case 0x02:  // MemoryFill(start0, value0, end0, start1, value1, end1, control)
        gx_stat[1]++;
        for (int b = 0; b < 2; b++) {
            u32 start = d[b * 3], value = d[b * 3 + 1], end = d[b * 3 + 2];
            if (!start || end <= start) continue;
            { u32 ww[2] = {start, end};
              int run = fp_op(FP_FILL, ((u64)start << 32) ^ end, 0, 0, ww, 1, 1);
              eye_written(start, end, run ? tmix(tmix(tmix(0xF111u, value), (d[6] >> (16 * b + 8)) & 3), end - start) : 0);
              if (!run) { gsp_interrupt(b ? IRQ_PSC1 : IRQ_PSC0); continue; } }
            if (pica_gx_hook) {
                u32 a[4] = {start, end, value, (d[6] >> (16 * b + 8)) & 3};
                if (pica_gx_hook(2, a)) { gsp_interrupt(b ? IRQ_PSC1 : IRQ_PSC0); continue; }
            }
            if (pica_fill_hook) pica_fill_hook(start, end, value, (d[6] >> (16 * b + 8)) & 3);
            u32 ctl = d[6] >> (16 * b), width = (ctl >> 8) & 3;   // bit 8: 24 bit, bit 9: 32 bit, neither: 16
            if (width == 0) for (u32 a = start; a + 2 <= end; a += 2) wr16(a, (u16)value);
            else if (width & 2) for (u32 a = start; a + 4 <= end; a += 4) wr32(a, value);
            else for (u32 a = start; a + 3 <= end; a += 3) { wr8(a, (u8)value); wr8(a + 1, (u8)(value >> 8)); wr8(a + 2, (u8)(value >> 16)); }
            { msr_log("gx: fill %08X..%08X with %08X (width %u)", start, end, value, width); if (msr_gx_trace()) msr_receipt("trace: fill %08X..%08X with %08X (width %u)", start, end, value, width); }
            gsp_interrupt(b ? IRQ_PSC1 : IRQ_PSC0);
        }
        break;
    case 0x03:  // DisplayTransfer(input, output, input size, output size, flags)
        gx_stat[2]++;
        { msr_log("gx: display transfer %08X -> %08X (%08X %08X flags %08X)", d[0], d[1], d[2], d[3], d[4]); if (msr_gx_trace()) msr_receipt("trace: display transfer %08X -> %08X (%08X %08X flags %08X)", d[0], d[1], d[2], d[3], d[4]); }
        {
            static const u32 bpp[8] = {4, 3, 2, 2, 2, 4, 4, 4};
            u32 fi = (d[4] >> 8) & 7, fo = (d[4] >> 12) & 7, down = (d[4] >> 24) & 3;
            u32 ow = (d[3] & 0xFFFF) / (down ? 2 : 1), oh = (d[3] >> 16) / (down == 2 ? 2 : 1);
            u32 rr[2] = {d[0], d[0] + (d[2] & 0xFFFF) * (d[2] >> 16) * bpp[fi]}, ww[2] = {d[1], d[1] + ow * oh * bpp[fo]};
            eye_note_transfer(d[0], d[1], rr[1] - rr[0]);
            int run = fp_op(FP_TRANSFER, ((u64)d[0] << 32) ^ d[1] ^ ((u64)d[4] << 16), rr, 1, ww, 1, 1);
            if (run) eye_before_read(rr[0], rr[1]);
            eye_written(ww[0], ww[1], run ? tmix(tmix(tmix(tmix(eye_token_of(rr[0], rr[1]), d[2]), d[3]), d[4]), 3) : 0);
            if (!run) { gsp_interrupt(IRQ_PPF); break; }
        }
        if (pica_gx_hook && pica_gx_hook(3, d)) { gsp_interrupt(IRQ_PPF); break; }
        if (pica_sync_hook) pica_sync_hook(d[0], (d[2] & 0xFFFF) * (d[2] >> 16) * 4);
        { MSR_PERF_BEGIN(PERF_TRANSFER); pica_display_transfer(d[0], d[1], d[2], d[3], d[4]); MSR_PERF_END(PERF_TRANSFER); }
        if (pica_invalidate_hook) pica_invalidate_hook(d[1], (d[3] & 0xFFFF) * (d[3] >> 16) * 4);
        gsp_interrupt(IRQ_PPF);
        break;
    case 0x04:  // TextureCopy(input, output, size, input gap, output gap, flags)
        gx_stat[3]++;
        { msr_log("gx: texture copy %08X -> %08X size %X gaps %08X %08X flags %08X", d[0], d[1], d[2], d[3], d[4], d[5]); if (msr_gx_trace()) msr_receipt("trace: texture copy %08X -> %08X size %X gaps %08X %08X flags %08X", d[0], d[1], d[2], d[3], d[4], d[5]); }
        {
            u32 iw = (d[3] & 0xFFFF) * 16, ig = (d[3] >> 16) * 16, ow = (d[4] & 0xFFFF) * 16, og = (d[4] >> 16) * 16;
            u32 in_span = iw ? d[2] + d[2] / iw * ig : d[2], out_span = ow ? d[2] + d[2] / ow * og : d[2];
            u32 rr[2] = {d[0], d[0] + in_span}, ww[2] = {d[1], d[1] + out_span};
            int run = fp_op(FP_COPY, ((u64)d[0] << 32) ^ d[1] ^ ((u64)d[2] << 8), rr, 1, ww, 1, ig == 0 && og == 0);
            if (run) eye_before_read(rr[0], rr[1]);
            eye_written(ww[0], ww[1], run ? tmix(tmix(tmix(tmix(tmix(eye_token_of(rr[0], rr[1]), d[2]), d[3]), d[4]), d[5]), 4) : 0);
            if (!run) { gsp_interrupt(IRQ_PPF); break; }
            if (pica_gx_hook && pica_gx_hook(4, d)) { gsp_interrupt(IRQ_PPF); break; }
            if (pica_sync_hook) pica_sync_hook(d[0], in_span);
            { MSR_PERF_BEGIN(PERF_TEXCOPY); pica_texture_copy(d[0], d[1], d[2], d[3], d[4], d[5]); MSR_PERF_END(PERF_TEXCOPY); }
            if (pica_invalidate_hook) pica_invalidate_hook(d[1], out_span);
        }
        gsp_interrupt(IRQ_PPF);
        break;
    case 0x05:  // FlushCacheRegions
        break;
    default:
        msr_log("gx: unknown command %02X", id);
    }
}

static void latch_renderer(const GxJob* j);
static void job_run(const GxJob* j) {
    if (j->kind == JOB_GX) {
        u32 d[7];
        memcpy(d, j->d, sizeof d);
        msr_capture_gx(j->id, d);
        gx_execute(j->id, d);
    } else latch_renderer(j);
}
static void* gx_worker(void* arg) {
    (void)arg;
    t_worker = 1;
#ifdef __SWITCH__
    { extern void msr_thread_to_core(int core); msr_thread_to_core(1); }
#endif
    for (;;) {
        pthread_mutex_lock(&g_job_mu);
        while (__atomic_load_n(&g_job_head, __ATOMIC_ACQUIRE) == g_job_tail) { g_worker_busy = 0; pthread_cond_wait(&g_job_cv, &g_job_mu); }
        g_worker_busy = 1;
        pthread_mutex_unlock(&g_job_mu);
        unsigned long long t0 = mono_ns();
        while (__atomic_load_n(&g_job_head, __ATOMIC_ACQUIRE) != g_job_tail) {
            job_run(&g_jobs[g_job_tail % JOBS]);
            __atomic_store_n(&g_job_tail, g_job_tail + 1, __ATOMIC_RELEASE);
        }
        msr_gx_busy_ns += mono_ns() - t0;
    }
    return 0;
}
static void job_push(const GxJob* j) {
    static int started;
    if (!started) {
        started = 1;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 8u << 20);
        pthread_t th;
        pthread_create(&th, &at, gx_worker, 0);
    }
    while (__atomic_load_n(&g_job_head, __ATOMIC_ACQUIRE) - __atomic_load_n(&g_job_tail, __ATOMIC_ACQUIRE) >= JOBS) {
        sched_async_deliver();
        struct timespec ts = {0, 100000};
        nanosleep(&ts, 0);
    }
    g_jobs[g_job_head % JOBS] = *j;
    __atomic_store_n(&g_job_head, g_job_head + 1, __ATOMIC_RELEASE);
    pthread_mutex_lock(&g_job_mu);
    pthread_cond_signal(&g_job_cv);
    pthread_mutex_unlock(&g_job_mu);
}
// Waits for the graphics thread to finish everything queued (and delivers its interrupts).
static void gx_drain(void) {
    while (__atomic_load_n(&g_job_head, __ATOMIC_ACQUIRE) != __atomic_load_n(&g_job_tail, __ATOMIC_ACQUIRE) || g_worker_busy) {
        struct timespec ts = {0, 200000};
        nanosleep(&ts, 0);
    }
    sched_async_deliver();
}
static void async_mode_check(void) {
    if (msr_gx_async != g_async) {   // only with nothing queued
        if (g_async) gx_drain();
        g_async = msr_gx_async;
        msr_receipt("gpu: graphics thread %s", g_async ? "on (GX commands run on their own core)" : "off (on the game's thread)");
    }
}

static void gx_run_queue(void) {
    u32 base = gsp_shared();
    if (!base) return;
    u32 ring = base + 0x800;   // thread index 0
    for (;;) {
        u32 ctl = rd32(ring), index = ctl & 0xFF, count = (ctl >> 8) & 0xFF;
        if (!count) break;
        u32 entry = ring + 0x20 + (index % 15) * 0x20;
        wr32(ring, (ctl & 0xFFFF0000u) | ((index + 1) % 15) | ((count - 1) << 8));
        async_mode_check();
        if (g_async) {
            GxJob j = {JOB_GX, rd32(entry) & 0xFF};
            for (int k = 0; k < 7; k++) j.d[k] = rd32(entry + 4 + 4 * k);
            job_push(&j);
        } else gx_command(entry);
    }
}

// The screens refresh at about 60 Hz: both vblank interrupts once a frame.
// Framebuffer info for client 0 at shared memory 0x200 + 0x40 * screen: byte 0 the buffer index,
// byte 1 "dirty"; two 0x1C-byte entries from +4 (active, left address, right address, stride,
// format, select, attribute). A game queues a swap by writing an entry and setting dirty; the GSP
// takes it at the next vblank and clears dirty. The renderer will read msr_fb[].
typedef struct { u32 left, right, stride, format; unsigned swaps; } MsrFramebuffer;
MsrFramebuffer msr_fb[2];
unsigned msr_frame_gaps[3];             // pictures, ones more than 18 ms after the last, more than 35 ms
unsigned long long msr_frame_worst_ns;  // the longest gap
unsigned msr_top_swaps(void) { return msr_fb[0].swaps; }

static void latch_renderer(const GxJob* j) {
    int screen = (int)j->screen;
    MsrFramebuffer* f = &msr_fb[screen];
    if (screen == 0) eye_note_framebuffers(j->lv, j->rv);
    f->left = j->left; f->right = j->right; f->stride = j->stride; f->format = j->format;
    if (f->swaps < 4)
        msr_log("gsp: screen %d shows %08X (stride %u, format %X)", screen, f->left, f->stride, f->format);
    pica_frame_shown(screen, f->left, f->stride, f->format);
    if (pica_latch_hook) pica_latch_hook(screen);
    __atomic_add_fetch(&f->swaps, 1, __ATOMIC_RELEASE);
    if (screen == 0) {
        msr_capture_latch();
        fp_frame_end(j->out, j->nout);
        eye_frame_end();
    }
}

static void gsp_latch_framebuffers(void) {
    u32 base = gsp_shared();
    if (!base) return;
    for (int screen = 0; screen < 2; screen++) {
        u32 rec = base + 0x200 + 0x40 * screen;
        u32 hdr = rd32(rec);
        if (!(hdr & 0xFF00)) continue;
        u32 e = rec + 4 + 0x1C * (hdr & 1);
        // the guest's view (g_fbg); msr_fb, which the renderer and the Switch's screens read, is
        // set by latch_renderer in order with the drawing
        static MsrFramebuffer g_fbg[2];
        g_fbg[screen].left = rd32(e + 4);
        g_fbg[screen].right = rd32(e + 8);
        g_fbg[screen].stride = rd32(e + 12);
        g_fbg[screen].format = rd32(e + 16);
        wr8(rec + 1, 0);
        GxJob j = {JOB_LATCH};
        j.screen = screen; j.left = g_fbg[screen].left; j.right = g_fbg[screen].right;
        j.stride = g_fbg[screen].stride; j.format = g_fbg[screen].format;
        if (screen == 0) {
            u32 lv = pica_vaddr(j.left), rv = pica_vaddr(j.right);
            j.lv = lv ? lv : j.left; j.rv = rv ? rv : j.right;
        }
        {
            // what the Switch shows: the left image of each screen, in every buffer the game has
            // shown it in (games double-buffer, so the next picture goes to the one not shown now)
            static u32 seen[2][4], nseen[2];
            u32 a = g_fbg[screen].left, v = pica_vaddr(a);
            if (v) a = v;
            int known = 0;
            for (u32 k = 0; k < nseen[screen]; k++) if (seen[screen][k] == a) known = 1;
            if (!known && a && nseen[screen] < 4) seen[screen][nseen[screen]++] = a;
            if (screen == 0) {
                unsigned n = 0;
                for (int sc = 0; sc < 2; sc++)
                    for (u32 k = 0; k < nseen[sc]; k++) {
                        u32 b = (sc ? 320u : 400u) * (g_fbg[sc].stride ? g_fbg[sc].stride : 240 * 3);
                        j.out[2 * n] = seen[sc][k];
                        j.out[2 * n + 1] = seen[sc][k] + b;
                        n++;
                    }
                j.nout = n;
            }
        }
        if (screen == 0) {   // the time between the game's top-screen pictures (speed report)
            static unsigned long long last;
            extern volatile int sched_resumed;
            unsigned long long now = mono_ns();
            if (sched_resumed) { sched_resumed = 0; last = 0; }   // after the port menu
            if (last) {
                unsigned long long gap = now - last;
                msr_frame_gaps[0]++;
                if (gap > 18000000ull) msr_frame_gaps[1]++;   // missed a 60 Hz slot
                if (gap > 35000000ull) msr_frame_gaps[2]++;   // missed two or more
                if (gap > msr_frame_worst_ns) msr_frame_worst_ns = gap;
            }
            last = now;
        }
        if (g_async) job_push(&j); else latch_renderer(&j);
    }
}

// The shared page's clock (3dbrew "Configuration Memory", 0x1FF81000): two DATETIME blocks at
// 0x20 and 0x40, the selector at 0x00 naming the current one; each holds milliseconds since
// 1900-01-01 and the system tick it was taken at. The kernel refreshes it about once a second.
static void shared_page_clock(void) {
    static u32 sel;
    u64 now_ms = sched_now_ns() / 1000000ull;
    u64 base_ms = 3971548800000ull;   // 2025-11-08 00:00 UTC in ms since 1900
    u64 tick = sched_now_ns() / 1000 * 268111856ull / 1000000;
    sel ^= 1;
    u32 blk = 0x1FF81020u + 0x20 * sel;
    wr32(blk, (u32)(base_ms + now_ms)); wr32(blk + 4, (u32)((base_ms + now_ms) >> 32));
    wr32(blk + 8, (u32)tick); wr32(blk + 12, (u32)(tick >> 32));
    wr32(blk + 16, 0xFFFFFFFFu);   // drift: none
    wr32(0x1FF81000u, sel);
}

static void gsp_vblank(void) {
    static unsigned n;
    if (n++ % 60 == 0) shared_page_clock();
    gsp_latch_framebuffers();
    gsp_interrupt(IRQ_VBLANK_TOP);
    gsp_interrupt(IRQ_VBLANK_BOTTOM);
}

int svc_gsp(Ipc* q) {
    static int timer;
    if (!timer) { timer = 1; sched_add_timer(16715000, gsp_vblank); }   // 59.83 Hz
    switch (q->cmd) {
    case 0x000C:  // TriggerCmdReqQueue
        gx_run_queue();
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0013: {  // RegisterInterruptRelayQueue(flags, event) -> thread index, shared memory
        g_gsp_event = ipc_get(q, 3);
        if (!g_gsp_mem) {
            g_gsp_mem = k_new(OBJ_MEMBLOCK, "gsp:mem");
            k_get(g_gsp_mem)->size = 0x1000;
        }
        ipc_set(q, 0, IPC_HDR(0x0013, 2, 2));
        ipc_set(q, 1, g_gsp_registered ? 0 : 0x2A07);  // first registration says so
        ipc_set(q, 2, 0);   // thread index
        ipc_set(q, 3, 0);   // copy handles, one
        ipc_set(q, 4, g_gsp_mem);
        g_gsp_registered = 1;
        msr_receipt("gsp: RegisterInterruptRelayQueue (event %X) -> shared memory %X", g_gsp_event, g_gsp_mem);
        return 1;
    }
    case 0x0016: case 0x0017: case 0x0014:  // AcquireRight / ReleaseRight / Unregister...
    case 0x0001: case 0x0002: case 0x0003:  // WriteHWRegs (and masked, repeat)
    case 0x0005: case 0x0008: case 0x0009: case 0x0007:  // SetBufferSwap, cache ops
    case 0x000B: case 0x001C: case 0x001E: case 0x001F:
        msr_log("gsp: cmd %04X acknowledged (no GPU yet)", q->cmd);
        ipc_reply(q, 0, 0);
        return 1;
    }
    return 0;
}
