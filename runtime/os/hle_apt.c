// MSR-nx: srv: and APT services.
//
// APT command numbers and replies follow fearkov/zakuro (MIT) apt.rs and 3dbrew "NS and APT
// Services". The game is the only application: no applets, no sleep, no home menu.
#include <string.h>
#include "hle.h"
#include "msr_log.h"

#define APT_SIGNAL_WAKEUP 1u
#define ERR_NO_PARAMETER  0xC8A0CFFCu

static u32 g_lock, g_notify_event, g_param_event;
static int g_param_pending;
static u32 g_param_dest;
static u32 g_cpu_limit;

void ipc_reply(Ipc* q, const u32* words, int n) {
    ipc_set(q, 0, IPC_HDR(q->cmd, n + 1, 0));
    ipc_set(q, 1, 0);
    for (int k = 0; k < n; k++) ipc_set(q, 2 + k, words[k]);
}

void ipc_error(Ipc* q, u32 result) {
    ipc_set(q, 0, IPC_HDR(q->cmd, 1, 0));
    ipc_set(q, 1, result);
}

int svc_srv(Ipc* q) {
    switch (q->cmd) {
    case 0x0001:  // RegisterClient
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0002: {  // EnableNotification -> semaphore handle
        u32 h = k_new(OBJ_SEMAPHORE, "srv:notify");
        ipc_set(q, 0, IPC_HDR(0x0002, 1, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x0);   // copy-handle descriptor, one handle
        ipc_set(q, 3, h);
        return 1;
    }
    case 0x0005: {  // GetServiceHandle(name[8], length, flags)
        char name[9] = {0};
        for (int k = 0; k < 8; k++) name[k] = (char)rd8(q->buf + 4 + k);
        u32 h = k_new(OBJ_SESSION, name);
        msr_receipt("service: %s -> handle %X", name, h);
        ipc_set(q, 0, IPC_HDR(0x0005, 1, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x10);  // move-handle descriptor, one handle
        ipc_set(q, 3, h);
        return 1;
    }
    }
    return 0;
}

int svc_apt(Ipc* q) {
    switch (q->cmd) {
    case 0x0001: {  // GetLockHandle(flags) -> attributes, power state, lock mutex
        if (!g_lock) g_lock = k_new(OBJ_MUTEX, "APT:lock");
        ipc_set(q, 0, IPC_HDR(0x0001, 3, 2));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, ipc_get(q, 1));
        ipc_set(q, 3, 0);
        ipc_set(q, 4, 0);       // copy handles, one
        ipc_set(q, 5, g_lock);
        return 1;
    }
    case 0x0002: {  // Initialize(app id, attributes) -> notification and parameter events
        g_notify_event = k_new(OBJ_EVENT, "APT:notify");
        g_param_event = k_new(OBJ_EVENT, "APT:param");
        // The application starts by receiving a Wakeup parameter.
        g_param_pending = 1;
        g_param_dest = ipc_get(q, 1);
        k_signal(g_param_event);
        ipc_set(q, 0, IPC_HDR(0x0002, 1, 3));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0x04000000);  // copy handles, two
        ipc_set(q, 3, g_notify_event);
        ipc_set(q, 4, g_param_event);
        return 1;
    }
    case 0x0003: case 0x0004: case 0x0015: case 0x0017: case 0x0019: case 0x001B: case 0x001F:
    case 0x003E: case 0x003F: case 0x0043: case 0x0055:
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0005: {  // GetAppletManInfo
        u32 w[4] = {0, 0, 0x101, 0x300};
        ipc_reply(q, w, 4);
        return 1;
    }
    case 0x0006: {  // GetAppletInfo -> title id, media, registered, loaded, attributes
        u32 w[6] = {0x001BB200u, 0x00040000u, 0, 1, 1, 0};
        ipc_reply(q, w, 6);
        return 1;
    }
    case 0x0009: {  // IsRegistered(app id)
        u32 app = ipc_get(q, 1);
        u32 w[1] = {app != 0x200};  // no library applet is ever loaded
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x000B: {  // InquireNotification -> none
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x000D: case 0x000E: {  // ReceiveParameter / GlanceParameter
        if (!g_param_pending) {
            ipc_error(q, ERR_NO_PARAMETER);
            return 1;
        }
        if (q->cmd == 0x000D) g_param_pending = 0;
        // The static buffer descriptor the caller set up is in its TLS at 0x180.
        u32 ptr = rd32(q->c->tls + 0x184);
        ipc_set(q, 0, IPC_HDR(q->cmd, 4, 4));
        ipc_set(q, 1, 0);
        ipc_set(q, 2, 0);                  // sender
        ipc_set(q, 3, APT_SIGNAL_WAKEUP);  // signal
        ipc_set(q, 4, 0);                  // size
        ipc_set(q, 5, 0x10);               // move handle
        ipc_set(q, 6, 0);
        ipc_set(q, 7, 0x2);                // static buffer descriptor, size 0, id 0
        ipc_set(q, 8, ptr);
        msr_receipt("apt: delivered the Wakeup parameter (to %X)", g_param_dest);
        return 1;
    }
    case 0x000F: {  // CancelParameter
        g_param_pending = 0;
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0035: case 0x0046: case 0x0047: {
        u32 w[2] = {0, 0};
        ipc_reply(q, w, 2);
        return 1;
    }
    case 0x004B: {  // AppletUtility
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x004F:  // SetAppCpuTimeLimit
        g_cpu_limit = ipc_get(q, 2);
        ipc_reply(q, 0, 0);
        return 1;
    case 0x0050: {
        u32 w[1] = {g_cpu_limit};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0056: {
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0058: {  // GetProgramId
        u32 w[2] = {0x001BB200u, 0x00040000u};
        ipc_reply(q, w, 2);
        return 1;
    }
    case 0x0101: case 0x0102: {  // CheckNew3DSApp / CheckNew3DS: an original 3DS
        u32 w[1] = {0};
        ipc_reply(q, w, 1);
        return 1;
    }
    case 0x0103: {  // GetTargetPlatform
        u32 w[1] = {1};
        ipc_reply(q, w, 1);
        return 1;
    }
    }
    return 0;
}
