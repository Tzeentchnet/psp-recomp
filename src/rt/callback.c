/* Kernel callbacks, ported from PPSSPP Core/HLE/sceKernelThread.cpp (v1.20.4), plus the power
 * callback slots from scePower.cpp.
 *
 * A callback belongs to the thread that created it. Notifying it (sr_cb_notify: power, memory
 * stick, UMD events, or sceKernelNotifyCallback) bumps its count and stores the argument; it runs
 * on its thread when that thread calls sceKernelCheckCallback or waits in a ...CB call. It runs
 * with a0 = notify count, a1 = notify argument, a2 = the argument given at creation, on the
 * thread's own stack: like PPSSPP's mipscall it reserves 128 bytes below sp and saves a0-t7, t8,
 * t9 and ra there first. A callback that returns non-zero is deleted.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CB 64

typedef struct {
    int used;
    uint32_t uid, thread_uid, entry, common_arg;
    uint32_t notify_count, notify_arg;
    char name[32];
} Callback;

/* Creation order, which is the order PPSSPP's per-thread callback list checks them in. */
static Callback s_cb[MAX_CB];

static Callback *cb_find(uint32_t uid) {
    for (int i = 0; i < MAX_CB; i++) if (s_cb[i].used && s_cb[i].uid == uid) return &s_cb[i];
    return NULL;
}

static int s_log = -1;
static int cblog(void) { if (s_log < 0) s_log = getenv("SR_CBLOG") ? 1 : 0; return s_log; }

int sr_cb_notify(uint32_t uid, uint32_t arg) {
    Callback *cb = cb_find(uid);
    if (!cb) return 0;
    cb->notify_count++;
    cb->notify_arg = arg;
    if (cblog()) fprintf(stderr, "CB: notify 0x%x %s arg=0x%x count=%u\n", uid, cb->name, arg, cb->notify_count);
    sched_cb_notify_thread(cb->thread_uid);
    return 1;
}

int sr_cb_pending(uint32_t thread_uid) {
    for (int i = 0; i < MAX_CB; i++)
        if (s_cb[i].used && s_cb[i].thread_uid == thread_uid && s_cb[i].notify_count) return 1;
    return 0;
}

static void run_cb(CpuState *s, Callback *cb) {
    uint32_t uid = cb->uid, entry = cb->entry;
    uint32_t a0 = cb->notify_count, a1 = cb->notify_arg, a2 = cb->common_arg;
    cb->notify_count = 0;
    cb->notify_arg = 0;
    if (cblog()) fprintf(stderr, "CB: run 0x%x %s entry=%08x (%u, 0x%x, 0x%x)\n", uid, cb->name, entry, a0, a1, a2);
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    uint32_t sp = s->r[29] - 32 * 4;
    for (int i = 4; i <= 15; i++) MEM_W32(sp + (uint32_t)i * 4, s->r[i]);
    MEM_W32(sp + 24 * 4, s->r[24]);
    MEM_W32(sp + 25 * 4, s->r[25]);
    MEM_W32(sp + 31 * 4, s->r[31]);
    s->r[29] = sp;
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[31] = 0;
    dispatch(s, entry);
    uint32_t ret = s->r[2];
    memcpy(s, &save, sizeof(CpuState));
    if (ret != 0) {
        Callback *c = cb_find(uid);
        if (c) {
            if (cblog()) fprintf(stderr, "CB: 0x%x returned 0x%x, deleted\n", uid, ret);
            sr_unregister_uid(uid);
            c->used = 0;
        }
    }
}

/* Runs every notified callback of the thread, including ones notified while these run.
 * Returns how many ran. */
int sr_cb_run_pending(CpuState *s, uint32_t thread_uid) {
    int n = 0;
    for (int guard = 0; guard < 1000; guard++) {
        Callback *next = NULL;
        for (int i = 0; i < MAX_CB; i++)
            if (s_cb[i].used && s_cb[i].thread_uid == thread_uid && s_cb[i].notify_count) { next = &s_cb[i]; break; }
        if (!next) break;
        run_cb(s, next);
        n++;
    }
    return n;
}

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])

static uint32_t h_CreateCallback(CpuState *s) {
    if (!A0) return 0x80020001u;               /* SCE_KERNEL_ERROR_ERROR: no name */
    if (A1 & 0xF0000000u) return 0x800200D3u;  /* ILLEGAL_ADDR */
    Callback *cb = NULL;
    for (int i = 0; i < MAX_CB; i++) if (!s_cb[i].used) { cb = &s_cb[i]; break; }
    if (!cb) return 0x80020190u;               /* NO_MEMORY */
    memset(cb, 0, sizeof(*cb));
    cb->used = 1;
    cb->uid = sr_alloc_uid_type(SR_TMID_CALLBACK);
    cb->thread_uid = sched_current_uid();
    cb->entry = A1;
    cb->common_arg = A2;
    int i = 0;
    for (; i < 31 && MEM_R8(A0 + (uint32_t)i); i++) cb->name[i] = (char)MEM_R8(A0 + (uint32_t)i);
    cb->name[i] = 0;
    if (cblog()) fprintf(stderr, "CB: create 0x%x %s entry=%08x arg=%08x thread=0x%x\n",
                         cb->uid, cb->name, cb->entry, cb->common_arg, cb->thread_uid);
    return cb->uid;
}

static uint32_t h_DeleteCallback(CpuState *s) {
    Callback *cb = cb_find(A0);
    if (!cb) return 0x80020197u;               /* UNKNOWN_CBID */
    sr_unregister_uid(cb->uid);
    cb->used = 0;
    return 0;
}

static uint32_t h_NotifyCallback(CpuState *s) {
    return sr_cb_notify(A0, A1) ? 0 : 0x80020197u;
}

static uint32_t h_CancelCallback(CpuState *s) {
    Callback *cb = cb_find(A0);
    if (!cb) return 0x80020197u;
    cb->notify_arg = 0;                        /* PPSSPP: only the argument is reset */
    return 0;
}

static uint32_t h_GetCallbackCount(CpuState *s) {
    Callback *cb = cb_find(A0);
    return cb ? cb->notify_count : 0x80020197u;
}

/* sceKernelCheckCallback: run the current thread's notified callbacks; 1 if any ran. */
static uint32_t h_CheckCallback(CpuState *s) {
    return sr_cb_run_pending(s, sched_current_uid()) ? 1 : 0;
}

/* scePower callback slots (16 public). Registering notifies the callback at once with
 * AC power | battery present | battery full, as PPSSPP does. */
static uint32_t s_power_slots[32];

static uint32_t h_PowerRegisterCallback(CpuState *s) {
    int slot = (int)A0;
    uint32_t cbid = A1;
    if (slot < -1 || slot >= 32) return 0x80000102u;   /* INVALID_SLOT */
    if (slot >= 16) return 0x80000023u;                /* PRIV_REQUIRED */
    if (cbid == 0) return 0x80000100u;                 /* INVALID_CB */
    int ret = -1;
    if (slot == -1) {
        for (int i = 0; i < 16; i++)
            if (s_power_slots[i] == 0 && ret == -1) { s_power_slots[i] = cbid; ret = i; }
        if (ret == -1) return 0x80000022u;             /* SLOTS_FULL */
    } else {
        if (s_power_slots[slot] != 0) return 0x80000020u;   /* TAKEN_SLOT */
        s_power_slots[slot] = cbid;
        ret = 0;
    }
    sr_cb_notify(cbid, 0x00001000u | 0x00000080u | 0x00000064u);
    return (uint32_t)ret;
}

static uint32_t h_PowerUnregisterCallback(CpuState *s) {
    int slot = (int)A0;
    if (slot < 0 || slot >= 32) return 0x80000102u;
    if (slot >= 16) return 0x80000023u;
    if (!s_power_slots[slot]) return 0x80000025u;      /* EMPTY_SLOT */
    s_power_slots[slot] = 0;
    return 0;
}

void sr_cb_register(void) {
    sr_hle_register(0xe81caf8f, "sceKernelCreateCallback", h_CreateCallback);
    sr_hle_register(0xedba5844, "sceKernelDeleteCallback", h_DeleteCallback);
    sr_hle_register(0xc11ba8c4, "sceKernelNotifyCallback", h_NotifyCallback);
    sr_hle_register(0xba4051d6, "sceKernelCancelCallback", h_CancelCallback);
    sr_hle_register(0x2a3d44ff, "sceKernelGetCallbackCount", h_GetCallbackCount);
    sr_hle_register(0x349d6d6c, "sceKernelCheckCallback", h_CheckCallback);
    sr_hle_register(0x04b7766e, "scePowerRegisterCallback", h_PowerRegisterCallback);
    sr_hle_register(0xdfa8baf8, "scePowerUnregisterCallback", h_PowerUnregisterCallback);
}
