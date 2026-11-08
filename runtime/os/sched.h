#pragma once
#include "msr_cpu.h"

void sched_init_main(Cpu* main_cpu, int prio);
Cpu* sched_main_cpu(void);
Cpu* sched_current_cpu(void);
u32 sched_current_id(void);
u64 sched_now_ns(void);
void sched_advance_ns(u64 ns);
void sched_switch(void);
u32 sched_create_thread(Cpu* c, u32 prio, u32 entry, u32 arg, u32 stack_top, s32 core);
void sched_exit_thread(void);
u32 sched_wait(Cpu* c, const u32* handles, int n, int wait_all, s64 timeout_ns, int* index);
void sched_sleep(Cpu* c, s64 ns);
u32 sched_arbitrate(Cpu* c, u32 arbiter, u32 addr, u32 type, s32 value, s64 timeout_ns);
void sched_after_signal(Cpu* c);
u32 sched_release_mutex(Cpu* c, u32 h);
u32 sched_create_mutex(int locked);
void sched_summary(void);
void sched_add_timer(u64 period_ns, void (*fn)(void));
int sched_poll_timers(void);
void sched_signal_event(Cpu* c, u32 h);
void sched_wake(u32 h);
u32 sched_release_semaphore(Cpu* c, u32 h, s32 count, u32* old);
int sched_get_priority(u32 h);
void sched_set_priority(Cpu* c, u32 h, int prio);
u32 sched_thread_id(u32 h);
