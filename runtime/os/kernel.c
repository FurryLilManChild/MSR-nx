// MSR-nx: kernel object table and handles.
#include <stdio.h>
#include <string.h>
#include "hle.h"
#include "msr_log.h"

#define MAX_OBJ 2048
#define HANDLE_BASE 0x100u

static KObject g_obj[MAX_OBJ];   // handle = HANDLE_BASE + index (one handle per object here)

u32 k_new(int type, const char* name) {
    for (u32 k = 1; k < MAX_OBJ; k++) {
        if (g_obj[k].type == OBJ_NONE) {
            memset(&g_obj[k], 0, sizeof g_obj[k]);
            g_obj[k].type = type;
            g_obj[k].refs = 1;
            snprintf(g_obj[k].name, sizeof g_obj[k].name, "%s", name ? name : "");
            return HANDLE_BASE + k;
        }
    }
    msr_log("kernel: out of objects");
    return 0;
}

KObject* k_get(u32 h) {
    if (h < HANDLE_BASE + 1 || h >= HANDLE_BASE + MAX_OBJ) return 0;
    KObject* o = &g_obj[h - HANDLE_BASE];
    return o->type == OBJ_NONE ? 0 : o;
}

void k_close(u32 h) {
    KObject* o = k_get(h);
    if (o && --o->refs == 0) o->type = OBJ_NONE;
}

u32 k_dup(u32 h) {
    // One handle per object keeps this simple: a duplicate shares the object.
    KObject* o = k_get(h);
    if (o) o->refs++;
    return h;
}

void k_signal(u32 h) {
    KObject* o = k_get(h);
    if (o) o->signalled = 1;
}
