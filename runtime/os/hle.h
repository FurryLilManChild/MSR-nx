// MSR-nx: native 3DS system services (kernel objects and IPC services).
//
// Command layouts follow 3dbrew and fearkov/zakuro (MIT, crates/zakuro-core/src/services), which
// MSR-nx reads as reference; the code here is ours.
#pragma once
#include "msr_cpu.h"

// ---- kernel objects ---------------------------------------------------------------------------
enum {
    OBJ_NONE, OBJ_EVENT, OBJ_MUTEX, OBJ_SEMAPHORE, OBJ_TIMER, OBJ_ARBITER, OBJ_SESSION, OBJ_PORT,
    OBJ_MEMBLOCK, OBJ_THREAD, OBJ_RESLIMIT, OBJ_PROCESS
};

typedef struct {
    int type;
    char name[16];      // service name for sessions, a label otherwise
    u32 signalled;      // events, timers, semaphores (count)
    u32 sticky;         // event reset type: 1 = stays signalled until cleared
    u32 addr, size;     // memory blocks
    u32 refs;
} KObject;

u32 k_new(int type, const char* name);          // returns a handle
KObject* k_get(u32 handle);                      // 0 for an invalid handle
void k_close(u32 handle);
u32 k_dup(u32 handle);
void k_signal(u32 handle);

// ---- IPC ----------------------------------------------------------------------------------------
#define IPC_HDR(cmd, normal, translate) (((u32)(cmd) << 16) | ((normal) << 6) | (translate))

typedef struct {
    Cpu* c;
    u32 buf;     // guest address of the command buffer (TLS + 0x80)
    u32 cmd;
    u32 handle;  // the session the request came on
} Ipc;

MSR_INLINE u32 ipc_get(Ipc* q, int i) { return rd32(q->buf + 4 * i); }
MSR_INLINE void ipc_set(Ipc* q, int i, u32 v) { wr32(q->buf + 4 * i, v); }
// Success reply with n plain words after the result.
void ipc_reply(Ipc* q, const u32* words, int n);
void ipc_error(Ipc* q, u32 result);

// Each service handler returns 1 if it handled the command.
int svc_srv(Ipc* q);
int svc_apt(Ipc* q);
int svc_hid(Ipc* q);
int svc_gsp(Ipc* q);
int svc_fs(Ipc* q);
int svc_cfg(Ipc* q);
int svc_dsp(Ipc* q);
void hle_init(void);
