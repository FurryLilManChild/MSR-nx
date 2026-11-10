// MSR-nx: guest threads and the 3DS scheduler.
//
// Every guest thread runs on its own host thread with its own Cpu and its own C call stack, so
// generated code keeps real calls and returns. Only one guest thread runs at a time, as on the
// 3DS application core: a global lock is the baton. The 3DS kernel never time-slices between
// priorities, so neither do we: the highest-priority ready thread runs until it blocks in a kernel
// call; a kernel call that readies a higher-priority thread hands over at once.
//
// Wait semantics follow 3dbrew and fearkov/zakuro (MIT) kernel/svc.rs and sync.rs.
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hle.h"
#include "msr_log.h"
#include "msr_mem.h"
#include "sched.h"

#define MAX_THREADS 32
#define RESULT_TIMEOUT 0x09401BFEu

enum { T_FREE, T_READY, T_WAIT, T_DEAD };
enum { W_NONE, W_OBJECTS, W_ARBITER, W_SLEEP };

typedef struct GThread {
    Cpu cpu;
    int state, prio, index;
    u32 handle, id;
    pthread_t host;
    pthread_cond_t cv;
    // wait
    int wait_kind, wait_all, nwait;
    u32 wait_handles[64];
    u32 arb_addr, arb_handle;
    u64 deadline;            // virtual ns, 0 = none
    u32 wake_result;
    int wake_index;
} GThread;

static GThread g_threads[MAX_THREADS];
static GThread* g_cur;
static pthread_mutex_t g_baton = PTHREAD_MUTEX_INITIALIZER;
static u64 g_now_ns;
static u32 g_next_id = 1;
static unsigned g_switches;

void msr_run(Cpu* c, u32 entry, u32 stop);
extern unsigned msr_trace_pos;

// Real-time mode (the Switch): the clock is the wall clock, and with every thread waiting the
// scheduler sleeps until the next event instead of jumping the clock to it.
int sched_realtime;
static u64 g_rt_start;
static u64 wall_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}
static void rt_update(void) {
    if (!sched_realtime) return;
    if (!g_rt_start) g_rt_start = wall_ns();
    u64 t = wall_ns() - g_rt_start;
    if (t > g_now_ns) g_now_ns = t;
}

u64 sched_now_ns(void) { rt_update(); return g_now_ns; }
void sched_advance_ns(u64 ns) { if (!sched_realtime) g_now_ns += ns; else rt_update(); }
Cpu* sched_current_cpu(void) { return g_cur ? &g_cur->cpu : 0; }
u32 sched_current_id(void) { return g_cur ? g_cur->id : 0; }

static GThread* thread_of_handle(u32 h) {
    if (h == 0xFFFF8001u) return g_cur;
    for (int k = 0; k < MAX_THREADS; k++)
        if (g_threads[k].state != T_FREE && g_threads[k].handle == h) return &g_threads[k];
    return 0;
}

// ---- object state ---------------------------------------------------------------------------
// KObject fields: events use signalled/sticky (reset type), mutexes use addr = owner thread
// index + 1 and size = lock count, threads use signalled (set when the thread ends).

static int obj_ready(u32 h, GThread* t) {
    KObject* o = k_get(h);
    if (!o) {
        GThread* th = thread_of_handle(h);
        return th && th->state == T_DEAD;
    }
    switch (o->type) {
    case OBJ_EVENT: case OBJ_SEMAPHORE: case OBJ_TIMER: return o->signalled != 0;
    case OBJ_MUTEX: return o->addr == 0 || o->addr == (u32)t->index + 1;
    case OBJ_THREAD: {
        GThread* th = thread_of_handle(h);
        return th ? th->state == T_DEAD : 1;
    }
    case OBJ_SESSION: case OBJ_PORT: return 1;
    }
    return 1;
}

static void obj_acquire(u32 h, GThread* t) {
    KObject* o = k_get(h);
    if (!o) return;
    switch (o->type) {
    case OBJ_EVENT:
        if (o->sticky == 0) o->signalled = 0;          // one-shot
        break;
    case OBJ_SEMAPHORE:
        if (o->signalled) o->signalled--;
        break;
    case OBJ_MUTEX:
        o->addr = (u32)t->index + 1;
        o->size++;
        break;
    }
}

// Tries to satisfy a thread's object wait; returns 1 and finishes the wait if it can.
static int try_finish_wait(GThread* t) {
    if (t->wait_all) {
        for (int k = 0; k < t->nwait; k++)
            if (!obj_ready(t->wait_handles[k], t)) return 0;
        for (int k = 0; k < t->nwait; k++) obj_acquire(t->wait_handles[k], t);
        t->wake_index = 0;
    } else {
        int hit = -1;
        for (int k = 0; k < t->nwait; k++)
            if (obj_ready(t->wait_handles[k], t)) { hit = k; break; }
        if (hit < 0) return 0;
        obj_acquire(t->wait_handles[hit], t);
        t->wake_index = hit;
    }
    t->wake_result = 0;
    t->state = T_READY;
    t->wait_kind = W_NONE;
    return 1;
}

// Re-checks every thread waiting on objects (after a signal, release or thread end).
static void recheck_waiters(void) {
    // Highest priority first, so a mutex or one-shot event goes to the right thread.
    for (;;) {
        GThread* best = 0;
        for (int k = 0; k < MAX_THREADS; k++) {
            GThread* t = &g_threads[k];
            if (t->state != T_WAIT || t->wait_kind != W_OBJECTS) continue;
            int ready = 0;
            if (t->wait_all) {
                ready = 1;
                for (int j = 0; j < t->nwait; j++) if (!obj_ready(t->wait_handles[j], t)) ready = 0;
            } else {
                for (int j = 0; j < t->nwait; j++) if (obj_ready(t->wait_handles[j], t)) ready = 1;
            }
            if (ready && (!best || t->prio < best->prio)) best = t;
        }
        if (!best) break;
        try_finish_wait(best);
    }
    // Pulse events and the like: clear what nobody took.
}

// ---- scheduling ------------------------------------------------------------------------------

static GThread* pick(void) {
    GThread* best = 0;
    int start = g_cur ? g_cur->index : 0;
    for (int n = 1; n <= MAX_THREADS; n++) {
        GThread* t = &g_threads[(start + n) % MAX_THREADS];  // round robin among equals
        if (t->state == T_READY && (!best || t->prio < best->prio)) best = t;
    }
    return best;
}

static void dump_threads(void) {
    for (int k = 0; k < MAX_THREADS; k++) {
        GThread* t = &g_threads[k];
        if (t->state == T_FREE) continue;
        msr_receipt("  thread %u (handle %X) prio %X state %s, pc %08X lr %08X%s", t->id, t->handle, t->prio,
                    t->state == T_READY ? "ready" : t->state == T_WAIT ? "waiting" : "ended",
                    t->cpu.pc, t->cpu.r[14], t == g_cur ? " (current)" : "");
        if (t->state == T_WAIT && t->wait_kind == W_OBJECTS)
            for (int j = 0; j < t->nwait; j++) {
                KObject* o = k_get(t->wait_handles[j]);
                msr_receipt("    waits on %X (%s)", t->wait_handles[j], o ? o->name : "thread");
            }
        if (t->state == T_WAIT && t->wait_kind == W_ARBITER)
            msr_receipt("    waits on address %08X (holds %08X)", t->arb_addr, rd32(t->arb_addr));
    }
}

// ---- timers: periodic hardware events (vblank, audio frames) on the virtual clock ------------
#define MAX_TIMERS 8
static struct { u64 period, next; void (*fn)(void); } g_timers[MAX_TIMERS];
static int g_ntimers;

void sched_add_timer(u64 period_ns, void (*fn)(void)) {
    if (g_ntimers == MAX_TIMERS) return;
    g_timers[g_ntimers].period = period_ns;
    g_timers[g_ntimers].next = g_now_ns + period_ns;
    g_timers[g_ntimers].fn = fn;
    g_ntimers++;
}

// Fires every timer that is due. Returns how many fired.
int sched_poll_timers(void) {
    int fired = 0;
    rt_update();
    for (int k = 0; k < g_ntimers; k++)
        while (g_timers[k].next <= g_now_ns) {
            g_timers[k].next += g_timers[k].period;
            if (sched_realtime && g_timers[k].next <= g_now_ns)   // running late: drop the missed ones
                g_timers[k].next = g_now_ns + g_timers[k].period;
            g_timers[k].fn();
            fired++;
        }
    return fired;
}

static u64 next_timer(void) {
    u64 t = 0;
    for (int k = 0; k < g_ntimers; k++) if (!t || g_timers[k].next < t) t = g_timers[k].next;
    return t;
}

// Gives the CPU to the best ready thread and returns when this thread runs again.
// The port menu pauses the game: the running thread holds here at its next switch, and the clock
// leaves out the paused time.
volatile int sched_pause_request;
// The frame rate (port menu): the game's frame limit in ms and whether its fixed 1/30 s interface
// steps take the measured frame time instead (tools/gen/generate.py PATCHES). 33 and 0: the 3DS's 30.
volatile u32 msr_frame_limit_ms = 33;
volatile int msr_fps60;
static void pause_point(void) {
    if (!sched_pause_request || !sched_realtime) return;
    u64 t0 = wall_ns();
    while (sched_pause_request) {
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, 0);
    }
    g_rt_start += wall_ns() - t0;
}

int sched_async_pending(void);   // hle_hid_gsp.c: interrupts from the graphics thread
int sched_async_deliver(void);
int sched_async_busy(void);
unsigned long long msr_idle_ns;  // time with every guest thread waiting (speed report)
void sched_switch(void) {
    GThread* me = g_cur;
    for (;;) {
        pause_point();
        sched_async_deliver();
        GThread* next = pick();
        if (me->state == T_READY && (!next || next->prio > me->prio || next == me)) next = me;
        if (!next && !sched_realtime && (sched_async_busy() || sched_async_pending())) {
            // the probe's own clock: wait for the graphics thread before moving time on
            while (sched_async_busy()) { struct timespec ts = {0, 100000}; nanosleep(&ts, 0); }
            continue;
        }
        if (!next) {
            // Nothing ready: jump the clock to the earliest timeout.
            u64 soonest = 0;
            for (int k = 0; k < MAX_THREADS; k++) {
                GThread* t = &g_threads[k];
                if (t->state == T_WAIT && t->deadline && (!soonest || t->deadline < soonest)) soonest = t->deadline;
            }
            u64 tn = next_timer();
            extern u64 msr_time_limit_ns;
            if (g_now_ns > msr_time_limit_ns) {
                msr_receipt("probe: game time limit reached with every thread waiting");
                dump_threads();
                msr_fatal(&me->cpu, "time limit");
            }
            u64 target = soonest && (!tn || soonest < tn) ? soonest : tn;
            if (sched_realtime && target) {   // sleep (holding the baton: nothing else can run) until then
                rt_update();
                int woken = 0;
                u64 w0 = wall_ns();
                while (target > g_now_ns) {   // in short steps: the graphics thread may finish something first
                    if (sched_async_pending()) { woken = 1; break; }
                    u64 d = target - g_now_ns;
                    if (d > 500000) d = 500000;
                    struct timespec ts = {0, (long)d};
                    nanosleep(&ts, 0);
                    rt_update();
                }
                msr_idle_ns += wall_ns() - w0;
                if (woken) continue;   // deliver it and see who is ready
                if (g_now_ns < target) g_now_ns = target;
            }
            if (tn && (!soonest || tn < soonest)) {
                if (tn > g_now_ns) g_now_ns = tn;
                sched_poll_timers();
                continue;
            }
            if (!soonest) {
                msr_receipt("probe: every thread is waiting and nothing can wake them");
                dump_threads();
                msr_fatal(&me->cpu, "all threads waiting");
            }
            if (soonest > g_now_ns) g_now_ns = soonest;
            for (int k = 0; k < MAX_THREADS; k++) {
                GThread* t = &g_threads[k];
                if (t->state == T_WAIT && t->deadline && t->deadline <= g_now_ns) {
                    t->state = T_READY;
                    t->wait_kind = W_NONE;
                    t->wake_result = RESULT_TIMEOUT;
                    t->wake_index = -1;
                }
            }
            continue;
        }
        if (next == me) return;
        g_switches++;
        g_cur = next;
        pthread_cond_signal(&next->cv);
        while (g_cur != me) pthread_cond_wait(&me->cv, &g_baton);
        if (me->state == T_READY) return;
    }
}

static void* thread_main(void* arg) {
    GThread* t = (GThread*)arg;
    pthread_mutex_lock(&g_baton);
    while (g_cur != t) pthread_cond_wait(&t->cv, &g_baton);
    msr_log("thread %u starts at %08X", t->id, t->cpu.pc);
    msr_run(&t->cpu, t->cpu.pc, 0xFFFFFFF4u);
    // The entry function returned: the thread ends like ExitThread.
    sched_exit_thread();
    return 0;
}

void sched_init_main(Cpu* main_cpu, int prio) {
    GThread* t = &g_threads[0];
    memset(t, 0, sizeof *t);
    t->state = T_READY;
    t->prio = prio;
    t->index = 0;
    t->id = g_next_id++;
    t->handle = 0xFFFF8001u;
    pthread_cond_init(&t->cv, 0);
    t->cpu = *main_cpu;   // the main thread runs on this copy from here on
    g_cur = t;
    pthread_mutex_lock(&g_baton);
}

Cpu* sched_main_cpu(void) { return &g_threads[0].cpu; }

// The Switch runs the main guest thread on its own host thread (for a big stack): the thread that
// called sched_init_main lets go of the baton, the game thread takes it.
void sched_handoff_main(void) { pthread_mutex_unlock(&g_baton); }
void sched_enter_main(void) { pthread_mutex_lock(&g_baton); }
size_t sched_thread_stack = 8u << 20;

u32 sched_create_thread(Cpu* c, u32 prio, u32 entry, u32 arg, u32 stack_top, s32 core) {
    int k;
    for (k = 1; k < MAX_THREADS; k++) if (g_threads[k].state == T_FREE) break;
    if (k == MAX_THREADS) return 0;
    GThread* t = &g_threads[k];
    memset(t, 0, sizeof *t);
    t->index = k;
    t->prio = (int)prio;
    t->id = g_next_id++;
    t->handle = k_new(OBJ_THREAD, "thread");
    t->cpu.r[0] = arg;
    t->cpu.r[13] = stack_top;
    t->cpu.r[14] = 0xFFFFFFF4u;
    t->cpu.pc = entry;
    t->cpu.tls = MSR_TLS_BASE + 0x200u * (u32)k;
    t->cpu.fpscr = c->fpscr;
    t->state = T_READY;
    pthread_cond_init(&t->cv, 0);
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, sched_thread_stack);   // generated code uses large frames
    pthread_create(&t->host, &at, thread_main, t);
    pthread_attr_destroy(&at);
    msr_receipt("thread: created thread %u at %08X (arg %08X, stack %08X, prio %X, core %d) -> %X",
                t->id, entry, arg, stack_top, prio, core, t->handle);
    return t->handle;
}

// The calling thread ends. Never returns to the caller.
void sched_exit_thread(void) {
    GThread* me = g_cur;
    me->state = T_DEAD;
    KObject* o = k_get(me->handle);
    if (o) o->signalled = 1;
    msr_log("thread %u ended", me->id);
    recheck_waiters();
    if (me->index == 0) {
        msr_receipt("probe: the main thread ended");
        msr_fatal(&me->cpu, "main thread ended");
    }
    sched_switch();   // never comes back: nothing makes a dead thread ready
    pthread_mutex_unlock(&g_baton);
    pthread_exit(0);
}

// ---- kernel calls ------------------------------------------------------------------------------

static void begin_wait(GThread* t, s64 timeout_ns) {
    t->state = T_WAIT;
    t->deadline = timeout_ns >= 0 ? g_now_ns + (u64)timeout_ns : 0;
    if (t->deadline == 0 && timeout_ns >= 0) t->deadline = 1;
}

// WaitSynchronization1 / N. Returns the result; *index gets the woken handle's position.
u32 sched_wait(Cpu* c, const u32* handles, int n, int wait_all, s64 timeout_ns, int* index) {
    GThread* t = g_cur;
    t->wait_kind = W_OBJECTS;
    t->wait_all = wait_all;
    t->nwait = n > 64 ? 64 : n;
    memcpy(t->wait_handles, handles, sizeof(u32) * t->nwait);
    if (try_finish_wait(t)) { *index = t->wake_index; return 0; }
    if (timeout_ns == 0) { t->wait_kind = W_NONE; *index = -1; return RESULT_TIMEOUT; }
    begin_wait(t, timeout_ns);
    sched_switch();
    *index = t->wake_index;
    return t->wake_result;
}

void sched_sleep(Cpu* c, s64 ns) {
    GThread* t = g_cur;
    if (ns <= 0) {
        // Yield: let another ready thread of the same priority run.
            sched_switch();
            return;
    }
    t->wait_kind = W_SLEEP;
    begin_wait(t, ns);
    sched_switch();
}

u32 sched_arbitrate(Cpu* c, u32 arbiter, u32 addr, u32 type, s32 value, s64 timeout_ns) {
    if (type == 0) {  // Signal: wake up to value threads (all if negative) waiting on addr
        int woken = 0;
        for (;;) {
            if (value >= 0 && woken >= value) break;
            GThread* best = 0;
            for (int k = 0; k < MAX_THREADS; k++) {
                GThread* t = &g_threads[k];
                if (t->state == T_WAIT && t->wait_kind == W_ARBITER && t->arb_addr == addr &&
                    (!best || t->prio < best->prio)) best = t;
            }
            if (!best) break;
            best->state = T_READY;
            best->wait_kind = W_NONE;
            best->wake_result = 0;
            woken++;
        }
        if (woken) sched_after_signal(c);
        return 0;
    }
    s32 cur = (s32)rd32(addr);
    if (cur >= value) return 0;
    if (type == 2 || type == 4) wr32(addr, (u32)(cur - 1));
    GThread* t = g_cur;
    t->wait_kind = W_ARBITER;
    t->arb_addr = addr;
    t->arb_handle = arbiter;
    begin_wait(t, (type == 3 || type == 4) ? timeout_ns : -1);
    sched_switch();
    return t->wake_result;
}

// After a signal, release or creation: wake waiters and hand over to a higher-priority thread.
void sched_after_signal(Cpu* c) {
    recheck_waiters();
    GThread* next = pick();
    if (next && next != g_cur && next->prio < g_cur->prio) {
        sched_switch();
    }
}

u32 sched_release_mutex(Cpu* c, u32 h) {
    KObject* o = k_get(h);
    if (!o || o->type != OBJ_MUTEX) return 0xD8E007F7u;   // invalid handle
    if (o->addr != (u32)g_cur->index + 1) return 0xD8E0041Fu;   // not the owner
    if (--o->size == 0) o->addr = 0;
    sched_after_signal(c);
    return 0;
}

// A service signals an event in the middle of a request: wake waiters, switch later.
void sched_wake(u32 h) {
    KObject* o = k_get(h);
    if (!o) return;
    o->signalled = 1;
    recheck_waiters();
    if (o->sticky == 2) o->signalled = 0;
}

// Events: reset type 0 one-shot, 1 sticky, 2 pulse (kept in KObject.sticky).
void sched_signal_event(Cpu* c, u32 h) {
    KObject* o = k_get(h);
    if (!o) return;
    o->signalled = 1;
    recheck_waiters();
    if (o->sticky == 2) o->signalled = 0;
    sched_after_signal(c);
}

u32 sched_release_semaphore(Cpu* c, u32 h, s32 count, u32* old) {
    KObject* o = k_get(h);
    if (!o || o->type != OBJ_SEMAPHORE) return 0xD8E007F7u;
    *old = o->signalled;
    if (o->signalled + (u32)count > o->size) return 0xD8E007FDu;   // over the maximum
    o->signalled += (u32)count;
    sched_after_signal(c);
    return 0;
}

int sched_get_priority(u32 h) {
    GThread* t = thread_of_handle(h);
    return t ? t->prio : -1;
}

void sched_set_priority(Cpu* c, u32 h, int prio) {
    GThread* t = thread_of_handle(h);
    if (t) t->prio = prio;
    sched_after_signal(c);
}

u32 sched_thread_id(u32 h) {
    GThread* t = thread_of_handle(h);
    return t ? t->id : 0;
}

u32 sched_create_mutex(int locked) {
    u32 h = k_new(OBJ_MUTEX, "mutex");
    if (locked) {
        KObject* o = k_get(h);
        o->addr = (u32)g_cur->index + 1;
        o->size = 1;
    }
    return h;
}

void sched_summary(void) {
    msr_receipt("threads (%u switches):", g_switches);
    dump_threads();
}
