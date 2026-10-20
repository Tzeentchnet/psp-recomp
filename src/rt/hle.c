/* High-level emulation (Phase 6): NID -> handler dispatch for the recompiled game's imports.
 *
 * The PRX import table maps each .sceStub.text stub to a (library, NID); tools/imports.py
 * resolves it and the codegen emits sr_syscall(s, NID) in each stub. A handler reads its
 * arguments from $a0-$a3 (and the stack for further args) and returns the $v0 value.
 *
 * Kernel object UIDs come from one counter, like PPSSPP's KernelObjectPool, and the driver
 * continues it from the reference trace's "# kernel next_uid". Memory comes from a port of
 * PPSSPP's BlockAllocator (kmem.c), so partition blocks, FPLs and thread stacks land at the
 * addresses PPSSPP gives for the same call sequence.
 *
 * After each handler, the caller-saved temp registers are poisoned to 0xDEADBEEF exactly as
 * PPSSPP's SetDeadbeefRegs does, so traces stay aligned on the registers the game actually
 * keeps (it never relies on caller-saved registers surviving a call).
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "audio.h"
#include "atrac_decode.h"
#include "iso.h"
#include "pgf.h"
#include "sas_reverb.h"
#include "nid_names.h"   /* sr_nid_name(): names unknown NIDs in the trap below */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <setjmp.h>
#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

uint32_t sr_last_nid = 0;

/* ---- handler table ---- */

typedef struct { uint32_t nid; const char *name; HleFn fn; } HleEntry;
#define HLE_CAP 512
static HleEntry s_hle[HLE_CAP];
static int s_hle_n = 0;

void sr_hle_register(uint32_t nid, const char *name, HleFn fn) {
    if (s_hle_n < HLE_CAP) { s_hle[s_hle_n].nid = nid; s_hle[s_hle_n].name = name; s_hle[s_hle_n].fn = fn; s_hle_n++; }
}

static HleEntry *hle_find(uint32_t nid) {
    for (int i = 0; i < s_hle_n; i++) if (s_hle[i].nid == nid) return &s_hle[i];
    return NULL;
}

/* ---- argument / return helpers ---- */

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
/* 5th+ integer arguments are passed on the caller's stack at sp+16, sp+20, ... */
/* Argument 5+idx of an HLE call. PSP userland is MIPS EABI: arguments 1..8 arrive in
 * a0-a3,t0-t3 (r4..r11) and only 9+ go to the stack (sp+0) -- same as PPSSPP's PARAM(n).
 * This used to read the o32 stack home slots (sp+16+), which EABI callers never write:
 * every >4-argument HLE call received stack garbage (found via sceMpegRingbufferConstruct
 * getting its fill callback from there -- the movie player jumped into the ring buffer). */
static uint32_t stack_arg(CpuState *s, int idx) {
    return idx < 4 ? s->r[8 + idx] : MEM_R32(s->r[29] + (uint32_t)(idx - 4) * 4);
}

/* ---- kernel object UIDs ---- */

/* Single shared UID pool matching PPSSPP: uid = 0x110, 0x111, ... incrementing by 1. */
static uint32_t s_uid = 0x110;
static uint32_t s_boot_uid_gap;
typedef struct { uint32_t uid, type; } UidObject;
static UidObject *s_uid_objects;
static uint32_t s_uid_object_count, s_uid_object_capacity;

uint32_t sr_alloc_uid(void) { return s_uid++; }
void sr_register_uid_type(uint32_t uid, uint32_t type) {
    for (uint32_t i = 0; i < s_uid_object_count; i++) {
        if (s_uid_objects[i].uid == uid) {
            s_uid_objects[i].type = type;
            return;
        }
    }
    if (s_uid_object_count == s_uid_object_capacity) {
        uint32_t next = s_uid_object_capacity ? s_uid_object_capacity * 2 : 256;
        UidObject *objects = (UidObject *)realloc(s_uid_objects, (size_t)next * sizeof(*objects));
        if (!objects) {
            fprintf(stderr, "kernel UID registry: out of host memory\n");
            abort();
        }
        s_uid_objects = objects;
        s_uid_object_capacity = next;
    }
    s_uid_objects[s_uid_object_count].uid = uid;
    s_uid_objects[s_uid_object_count].type = type;
    s_uid_object_count++;
}
uint32_t sr_alloc_uid_type(uint32_t type) {
    uint32_t uid = sr_alloc_uid();
    sr_register_uid_type(uid, type);
    return uid;
}
void sr_unregister_uid(uint32_t uid) {
    for (uint32_t i = 0; i < s_uid_object_count; i++)
        if (s_uid_objects[i].uid == uid) {
            s_uid_objects[i].type = 0;
            return;
        }
}
uint32_t sr_uid_list(uint32_t type, uint32_t *out, uint32_t capacity) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < s_uid_object_count; i++)
        if (s_uid_objects[i].type == type) {
            if (out && total < capacity) out[total] = s_uid_objects[i].uid;
            total++;
        }
    return total;
}
/* Continue UID numbering where the oracle's kernel is (trace header "# kernel next_uid="), so
 * handles returned to the game match the reference trace. */
void sr_set_next_uid(uint32_t next) {
    s_uid = next;
    s_boot_uid_gap = 0;
}
uint32_t sr_get_next_uid(void) { return s_uid; }
void sr_set_boot_uid_gap(uint32_t count) { s_boot_uid_gap = count; }
void sr_boot_uid_delay_checkpoint(void) {
    if (!s_boot_uid_gap) return;
    uint32_t first = s_uid;
    s_uid += s_boot_uid_gap;
    if (getenv("SR_THLOG"))
        fprintf(
            stderr, "bootloader exit consumed UID%s 0x%x..0x%x\n",
            s_boot_uid_gap == 1 ? "" : "s", first, s_uid - 1);
    s_boot_uid_gap = 0;
}

/* User memory, partition blocks, FPLs and thread stacks live in kmem.c (PPSSPP's BlockAllocator). */

/* ---- handlers ---- */

uint32_t g_sdk_version = 0;   /* read by gelist.c (SDK-dependent GE behaviour) */

static uint32_t h_GetCompiledSdkVersion(CpuState *s) { (void)s; return g_sdk_version; }
static uint32_t h_SetCompiledSdkVersion(CpuState *s) { g_sdk_version = A0; return 0; }
/* sceKernelSetCompilerVersion is a separate value; it must not overwrite the SDK version. */
static uint32_t g_compiler_version = 0;
static uint32_t h_SetCompilerVersion(CpuState *s) { g_compiler_version = A0; return 0; }

/* sceUtilityGetSystemParamInt(id, int *out): write the system setting and return 0. PPSSPP's
 * defaults (Core/HLE/sceUtility.cpp registry): English (1), Western button order, 24h clock.
 * A no-op that leaves *out untouched makes the game read garbage for the language and load the
 * wrong region assets. IDs follow PSP_SYSTEMPARAM_ID_INT_*. */
static uint32_t h_GetSystemParamInt(CpuState *s) {
    uint32_t id = A0, out = A1, v;
    switch (id) {
        case 2:  v = 1;  break;   /* ADHOC_CHANNEL: automatic */
        case 3:  v = 0;  break;   /* WLAN_POWERSAVE: off */
        case 4:  v = 1;  break;   /* DATE_FORMAT: MMDDYYYY */
        case 5:  v = 0;  break;   /* TIME_FORMAT: 24h */
        case 6:  v = 0;  break;   /* TIMEZONE offset (minutes) */
        case 7:  v = 0;  break;   /* DAYLIGHTSAVINGS: off */
        case 8:  v = 1;  break;   /* LANGUAGE: English */
        case 9:  v = 1;  break;   /* BUTTON_PREFERENCE: cross = enter (Western) */
        default: v = 0;  break;
    }
    if (out) MEM_W32(out, v);
    if (getenv("SR_LANGLOG")) fprintf(stderr, "GetSystemParamInt id=%u -> %u\n", id, v);
    return 0;
}
static uint32_t h_GetSystemParamString(CpuState *s) {
    static const char nickname[] = "shadow";
    uint32_t id = A0, out = A1, size = A2;
    if (id != 1) return 0x80110103u;   /* INVALID_SYSTEM_PARAM_ID */
    if (size <= strlen(nickname)) return 0x80110102u;   /* STRING_TOO_LONG */
    if (!out || !sr_inrange(out) || size - 1 > UINT32_MAX - out ||
        !sr_inrange(out + size - 1)) {
        return UINT32_MAX;
    }
    for (uint32_t i = 0; i < size; i++) MEM_W8(out + i, 0);
    for (uint32_t i = 0; i < sizeof(nickname) - 1; i++)
        MEM_W8(out + i, (uint8_t)nickname[i]);
    return 0;
}
/* sceCtrlGetIdleCancelThreshold(int *idlereset, int *idleback): both thresholds "disabled". */
static uint32_t h_CtrlGetIdleCancelThreshold(CpuState *s) {
    if (A0) MEM_W32(A0, 0xFFFFFFFFu);   /* -1 = idle cancel disabled (PPSSPP default) */
    if (A1) MEM_W32(A1, 0xFFFFFFFFu);
    return 0;
}

/* ThreadManForUser, backed by the fiber scheduler (src/rt/sched.c). */
static uint32_t h_CreateThread(CpuState *s) {
    /* a0=name, a1=entry, a2=priority, a3=stackSize, t0=attr, t1=option. Returns the thread UID or
     * an error; the stack comes from the user partition (kmem.c). */
    if (!A0) return 0x80020001u;                      /* SCE_KERNEL_ERROR_ERROR: no name */
    char name[32];
    int i = 0;
    for (; i < 31 && MEM_R8(A0 + (uint32_t)i); i++) name[i] = (char)MEM_R8(A0 + (uint32_t)i);
    name[i] = 0;
    return sched_create_thread(name, A1, (int)A2, A3, s->r[8]);
}
static uint32_t h_StartThread(CpuState *s) {
    /* a0=thid, a1=arglen, a2=argp. Mark ready, then preempt if it is higher priority -- PSP
     * runs a higher-priority started thread immediately. */
    uint32_t rc = sched_start_thread(A0, A1, A2);
    if (rc) return rc;
    sched_preempt();
    return 0;
}
static uint32_t h_ExitThread(CpuState *s) { sched_exit_current((int32_t)A0); return 0; }
static uint32_t h_ExitDeleteThread(CpuState *s) { sched_exit_delete_current((int32_t)A0); return 0; }
static uint32_t h_DeleteThread(CpuState *s) { return (uint32_t)sched_delete_thread(A0); }
static uint32_t h_DelayThread(CpuState *s) {
    sr_boot_uid_delay_checkpoint();
    sched_delay_current(A0);
    return 0;
}
static uint32_t h_ChangeThreadPriority(CpuState *s) { sched_set_priority(A0, (int)A1); return 0; }
static uint32_t h_TerminateDeleteThread(CpuState *s) {
    if (A0 == 0 || A0 == sched_current_uid()) return 0x80020197u;   /* ILLEGAL_THID */
    sched_terminate_thread(A0);
    return (uint32_t)sched_delete_thread(A0);
}
static uint32_t h_GetThreadIdSched(CpuState *s) { (void)s; return sched_current_uid(); }
static uint32_t h_GetThreadPriority(CpuState *s) { (void)s; return (uint32_t)sched_current_priority(); }
static uint32_t h_SleepThread(CpuState *s) { (void)s;
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "SLEEP uid=0x%x\n", sched_current_uid()); }
    sched_thread_sleep(); return 0; }
static uint32_t h_WakeupThread(CpuState *s) {
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "WAKEUP target=0x%x (from 0x%x)\n", A0, sched_current_uid()); }
    sched_thread_wakeup(A0); sched_preempt(); return 0; }
static uint32_t h_CancelWakeupThread(CpuState *s) {
    int old = sched_thread_cancel_wakeup(A0);
    if (getenv("SR_WAKELOG")) { static int n=0; if (n++<8) fprintf(stderr, "CANCEL_WAKE target=0x%x old=%d (from 0x%x)\n", A0, old, sched_current_uid()); }
    return old < 0 ? 0x800201a0u : (uint32_t)old;
}
static uint32_t h_WaitThreadEnd(CpuState *s) {
    uint32_t uid = A0;
    while (!sched_is_dormant(uid)) sched_block_on(uid);
    return 0;
}
static uint32_t h_ReferThreadRunStatus(CpuState *s) {
    uint32_t out = A1;
    if (out) {
        SrThreadRunStatus rs;
        int rc = sched_thread_run_status(A0, &rs);
        if (rc < 0) return 0x800201a0u;
        for (uint32_t i = 0; i < 0x2c; i++) MEM_W8(out + i, 0);
        MEM_W32(out + 0x00, rs.size);
        MEM_W32(out + 0x04, rs.status);
        MEM_W32(out + 0x08, rs.currentPriority);
        MEM_W32(out + 0x0c, rs.waitType);
        MEM_W32(out + 0x10, rs.waitId);
        MEM_W32(out + 0x14, rs.wakeupCount);
        MEM_W32(out + 0x18, rs.runClocksLow);
        MEM_W32(out + 0x1c, rs.runClocksHigh);
        MEM_W32(out + 0x20, rs.intrPreemptCount);
        MEM_W32(out + 0x24, rs.threadPreemptCount);
        MEM_W32(out + 0x28, rs.releaseCount);
        if (getenv("SR_WAKELOG")) { static int n=0; if (n++<12) fprintf(stderr,
            "REFER_RUN target=0x%x status=%u pri=%u waitType=%u waitId=0x%x wakeups=%u\n",
            A0, rs.status, rs.currentPriority, rs.waitType, rs.waitId, rs.wakeupCount); }
    }
    return 0;
}
static void hle_buf_w32(uint8_t *buf, uint32_t off, uint32_t value) {
    memcpy(buf + off, &value, sizeof(value));
}
static uint32_t h_ReferThreadStatus(CpuState *s) {
    uint32_t out = A1;
    if (!sr_inrange(out)) return 0x800200d3u;   /* ILLEGAL_ADDR */
    uint32_t wanted = MEM_R32(out);
    uint32_t native_size = g_sdk_version > 0x02060010u ? 108u : 104u;
    if (wanted > native_size) return 0x800201bcu;   /* ILLEGAL_SIZE */

    SrThreadStatus ts;
    if (sched_thread_status(A0, &ts) < 0) return 0x80020198u;   /* UNKNOWN_THID */

    uint8_t buf[108] = {0};
    hle_buf_w32(buf, 0x00, native_size);
    memcpy(buf + 0x04, ts.name, sizeof(ts.name));
    hle_buf_w32(buf, 0x24, ts.attr);
    hle_buf_w32(buf, 0x28, ts.status);
    hle_buf_w32(buf, 0x2c, ts.entry);
    hle_buf_w32(buf, 0x30, ts.stack);
    hle_buf_w32(buf, 0x34, ts.stackSize);
    hle_buf_w32(buf, 0x38, ts.gpReg);
    hle_buf_w32(buf, 0x3c, (uint32_t)ts.initialPriority);
    hle_buf_w32(buf, 0x40, (uint32_t)ts.currentPriority);
    hle_buf_w32(buf, 0x44, ts.waitType);
    hle_buf_w32(buf, 0x48, ts.waitId);
    hle_buf_w32(buf, 0x4c, (uint32_t)ts.wakeupCount);
    hle_buf_w32(buf, 0x50, (uint32_t)ts.exitStatus);
    hle_buf_w32(buf, 0x54, ts.runClocksLow);
    hle_buf_w32(buf, 0x58, ts.runClocksHigh);
    hle_buf_w32(buf, 0x5c, ts.intrPreemptCount);
    hle_buf_w32(buf, 0x60, ts.threadPreemptCount);
    hle_buf_w32(buf, 0x64, ts.releaseCount);
    for (uint32_t i = 0; i < wanted; i++) MEM_W8(out + i, buf[i]);
    return 0;
}
static uint32_t h_GetThreadExitStatus(CpuState *s) {
    return (uint32_t)sched_thread_exit_status(A0);
}
static uint32_t h_GetThreadmanIdList(CpuState *s) {
    uint32_t type = A0, out = A1, capacity = A2, count_out = A3;
    if (capacity >= 0x08000000u) return 0x800200d3u;   /* ILLEGAL_ADDR */
    if (capacity) {
        uint64_t last = (uint64_t)out + (uint64_t)capacity * 4u - 1u;
        if (last > UINT32_MAX || !sr_inrange(out) || !sr_inrange((uint32_t)last))
            return 0x800200d2u;   /* ILLEGAL_ARGUMENT */
    }

    uint32_t total;
    if (type >= 1 && type <= 14)
        total = sr_uid_list(type, NULL, 0);
    else if (type >= 64 && type <= 67)
        total = sched_thread_id_list(type, NULL, 0);
    else
        return 0x800201bbu;       /* ILLEGAL_TYPE */

    uint32_t written = total < capacity ? total : capacity;
    uint32_t *ids = written ? (uint32_t *)malloc((size_t)written * sizeof(*ids)) : NULL;
    if (written && !ids) {
        fprintf(stderr, "sceKernelGetThreadmanIdList: out of host memory\n");
        abort();
    }
    if (type >= 1 && type <= 14)
        sr_uid_list(type, ids, written);
    else
        sched_thread_id_list(type, ids, written);
    for (uint32_t i = 0; i < written; i++) MEM_W32(out + i * 4u, ids[i]);
    free(ids);
    if (count_out && sr_inrange(count_out)) MEM_W32(count_out, total);
    return written;
}
static uint32_t h_SysClock2USec(CpuState *s) {
    if (!sr_inrange(A0) || !sr_inrange(A0 + 7u)) return 0x800200d3u;
    uint64_t clock = (uint64_t)MEM_R32(A0) | ((uint64_t)MEM_R32(A0 + 4u) << 32);
    if (A1 && sr_inrange(A1)) MEM_W32(A1, (uint32_t)(clock / 1000000u));
    if (A2 && sr_inrange(A2)) MEM_W32(A2, (uint32_t)(clock % 1000000u));
    return 0;
}
static uint32_t h_ok(CpuState *s) { (void)s; return 0; }

/* ...CB variants: the same wait, with the thread's notified callbacks run during it. */
static uint32_t with_callbacks(CpuState *s, HleFn fn) {
    sched_cb_wait(1);
    uint32_t r = fn(s);
    sched_cb_wait(0);
    return r;
}
static uint32_t h_DelayThreadCB(CpuState *s) { return with_callbacks(s, h_DelayThread); }
static uint32_t h_SleepThreadCB(CpuState *s) { return with_callbacks(s, h_SleepThread); }
static uint32_t h_WaitThreadEndCB(CpuState *s) { return with_callbacks(s, h_WaitThreadEnd); }

/* ModuleMgrForUser */
static uint32_t h_module_uid(CpuState *s) { (void)s; return sr_alloc_uid(); }
static uint32_t h_GetModuleId(CpuState *s) { (void)s; return 0x112; }   /* main module's id (stable) */

/* Kernel_Library: interrupt suspend/resume. Suspend returns the prior state (use 1); resume
 * is void. */
static uint32_t h_CpuSuspendIntr(CpuState *s) { (void)s; return 1; }

/* Boot-path setup calls. These return success (or the value the reference run returns) so the boot
 * proceeds down the same branch. They do not yet model the subsystem behind them; the import
 * sequence vs the reference run is what validates that the returned value drives the same path. */
static uint32_t h_UmdCheckMedium(CpuState *s) { (void)s; return 1; }      /* medium present */
static uint32_t h_DmacMemcpy(CpuState *s) {
    /* a0=dst, a1=src, a2=size. A real DMA copy in guest memory. */
    uint32_t dst = A0, src = A1, n = A2;
    sr_gpu_vram_read(src, n);
    sr_gpu_vram_prepare_write(dst, n);
    if (n && n - 1u <= UINT32_MAX - src &&
        n - 1u <= UINT32_MAX - dst &&
        sr_inrange(src) && sr_inrange(src + n - 1u) &&
        sr_inrange(dst) && sr_inrange(dst + n - 1u)) {
        memmove(SR_HOST(dst), SR_HOST(src), n);
    } else {
        for (uint32_t i = 0; i < n; i++)
            MEM_W8(dst + i, MEM_R8(src + i));
    }
    sr_gpu_vram_dirty(dst, n);   /* DMA into a GPU-cached framebuffer must invalidate it */
    return 0;
}
/* sceLibFont. The PSP firmware fonts (flash0 font PGFs) are not on the game ISO, so we load the
 * real PGF fonts bundled with the PPSSPP reference assets and rasterise glyphs with a port of
 * PPSSPP's PGF reader (src/rt/pgf.c). ltn0.pgf covers Latin + punctuation/symbols (the menus);
 * jpn0.pgf is used as a fallback for any CJK code points. If neither file is found we fall back to
 * a tiny synthetic 5x7 font so text still appears. */
static PGF *s_pgf_ltn = NULL, *s_pgf_jpn = NULL;
static int s_pgf_tried = 0;
static void font_load(void) {
    if (s_pgf_tried) return;
    s_pgf_tried = 1;
    char path[600];
    const char *dir = getenv("SR_FONTDIR");
    if (dir) { snprintf(path, sizeof path, "%s/ltn0.pgf", dir); s_pgf_ltn = pgf_open(path); }
    if (!s_pgf_ltn) s_pgf_ltn = pgf_open("font/ltn0.pgf");
    if (!s_pgf_ltn) s_pgf_ltn = pgf_open("third_party/ppsspp/assets/flash0/font/ltn0.pgf");
    if (dir) { snprintf(path, sizeof path, "%s/jpn0.pgf", dir); s_pgf_jpn = pgf_open(path); }
    if (!s_pgf_jpn) s_pgf_jpn = pgf_open("font/jpn0.pgf");
    if (!s_pgf_jpn) s_pgf_jpn = pgf_open("third_party/ppsspp/assets/flash0/font/jpn0.pgf");
    if (getenv("SR_FONTLOG"))
        fprintf(stderr, "font_load: ltn0=%s jpn0=%s\n", s_pgf_ltn ? "ok" : "MISSING", s_pgf_jpn ? "ok" : "MISSING");
}
/* Pick the font that actually has a glyph for `cc` (Latin first, then CJK fallback). */
static const PGF *font_for(int cc) {
    font_load();
    if (s_pgf_ltn && pgf_has_char(s_pgf_ltn, cc)) return s_pgf_ltn;
    if (s_pgf_jpn && pgf_has_char(s_pgf_jpn, cc)) return s_pgf_jpn;
    return NULL;
}

static uint32_t h_FontNewLib(CpuState *s) {
    if (A1) MEM_W32(A1, 0);            /* *errorCode = 0 */
    return 0x0f000001;                 /* non-NULL library handle */
}
static uint32_t h_FontOpen(CpuState *s) {
    /* (lib, index, mode, errorCode*) */
    if (A3) MEM_W32(A3, 0);
    return 0x0f000101;                 /* non-NULL font handle */
}
static uint32_t h_FontGetFontInfo(CpuState *s) {
    font_load();
    const PGF *p = s_pgf_ltn ? s_pgf_ltn : s_pgf_jpn;
    if (A1) { for (int i = 0; i < 0x108; i++) MEM_W8(A1 + (uint32_t)i, 0); if (p) pgf_get_font_info(p, A1); }
    return 0;
}
static uint32_t h_FontGetCharInfo(CpuState *s) {
    uint32_t ci = A2; if (!ci) return 0;
    uint32_t cc = A1 & 0xffff;
    const PGF *p = font_for((int)cc);
    if (p) { pgf_get_char_info(p, (int)cc, 0, ci); return 0; }
    /* No PGF available: synthetic 8x11 metrics fallback. */
    for (int i = 0; i < 0x3c; i++) MEM_W8(ci + (uint32_t)i, 0);
    int draw = (cc > 32);                      /* space and controls: empty */
    MEM_W32(ci + 0, draw ? 8 : 0);             /* bitmapWidth */
    MEM_W32(ci + 4, draw ? 11 : 0);            /* bitmapHeight */
    MEM_W32(ci + 8, 0);                        /* bitmapLeft */
    MEM_W32(ci + 12, 0);                       /* bitmapTop */
    MEM_W32(ci + 16, 8 << 6);                  /* sfp26Width */
    MEM_W32(ci + 20, 11 << 6);                 /* sfp26Height */
    MEM_W32(ci + 24, 0);                       /* ascender */
    MEM_W32(ci + 28, (uint32_t)(-(11 << 6)));  /* descender */
    MEM_W32(ci + 32, 0);                       /* bearingHX */
    MEM_W32(ci + 36, 0);                       /* bearingHY */
    MEM_W32(ci + 48, 8 << 6);                  /* sfp26AdvanceH */
    MEM_W32(ci + 52, 12 << 6);                 /* sfp26AdvanceV */
    return 0;
}

static const uint8_t *font5x7(uint32_t cc) {
    if (cc >= 'a' && cc <= 'z') cc -= 32;
    switch (cc) {
        case 'A': { static const uint8_t r[7] = {14,17,17,31,17,17,17}; return r; }
        case 'B': { static const uint8_t r[7] = {30,17,17,30,17,17,30}; return r; }
        case 'C': { static const uint8_t r[7] = {14,17,16,16,16,17,14}; return r; }
        case 'D': { static const uint8_t r[7] = {30,17,17,17,17,17,30}; return r; }
        case 'E': { static const uint8_t r[7] = {31,16,16,30,16,16,31}; return r; }
        case 'F': { static const uint8_t r[7] = {31,16,16,30,16,16,16}; return r; }
        case 'G': { static const uint8_t r[7] = {14,17,16,23,17,17,15}; return r; }
        case 'H': { static const uint8_t r[7] = {17,17,17,31,17,17,17}; return r; }
        case 'I': { static const uint8_t r[7] = {14,4,4,4,4,4,14}; return r; }
        case 'J': { static const uint8_t r[7] = {7,2,2,2,18,18,12}; return r; }
        case 'K': { static const uint8_t r[7] = {17,18,20,24,20,18,17}; return r; }
        case 'L': { static const uint8_t r[7] = {16,16,16,16,16,16,31}; return r; }
        case 'M': { static const uint8_t r[7] = {17,27,21,21,17,17,17}; return r; }
        case 'N': { static const uint8_t r[7] = {17,25,21,19,17,17,17}; return r; }
        case 'O': { static const uint8_t r[7] = {14,17,17,17,17,17,14}; return r; }
        case 'P': { static const uint8_t r[7] = {30,17,17,30,16,16,16}; return r; }
        case 'Q': { static const uint8_t r[7] = {14,17,17,17,21,18,13}; return r; }
        case 'R': { static const uint8_t r[7] = {30,17,17,30,20,18,17}; return r; }
        case 'S': { static const uint8_t r[7] = {15,16,16,14,1,1,30}; return r; }
        case 'T': { static const uint8_t r[7] = {31,4,4,4,4,4,4}; return r; }
        case 'U': { static const uint8_t r[7] = {17,17,17,17,17,17,14}; return r; }
        case 'V': { static const uint8_t r[7] = {17,17,17,17,17,10,4}; return r; }
        case 'W': { static const uint8_t r[7] = {17,17,17,21,21,21,10}; return r; }
        case 'X': { static const uint8_t r[7] = {17,17,10,4,10,17,17}; return r; }
        case 'Y': { static const uint8_t r[7] = {17,17,10,4,4,4,4}; return r; }
        case 'Z': { static const uint8_t r[7] = {31,1,2,4,8,16,31}; return r; }
        case '0': { static const uint8_t r[7] = {14,17,19,21,25,17,14}; return r; }
        case '1': { static const uint8_t r[7] = {4,12,4,4,4,4,14}; return r; }
        case '2': { static const uint8_t r[7] = {14,17,1,2,4,8,31}; return r; }
        case '3': { static const uint8_t r[7] = {30,1,1,14,1,1,30}; return r; }
        case '4': { static const uint8_t r[7] = {2,6,10,18,31,2,2}; return r; }
        case '5': { static const uint8_t r[7] = {31,16,16,30,1,1,30}; return r; }
        case '6': { static const uint8_t r[7] = {6,8,16,30,17,17,14}; return r; }
        case '7': { static const uint8_t r[7] = {31,1,2,4,8,8,8}; return r; }
        case '8': { static const uint8_t r[7] = {14,17,17,14,17,17,14}; return r; }
        case '9': { static const uint8_t r[7] = {14,17,17,15,1,2,12}; return r; }
        case '.': { static const uint8_t r[7] = {0,0,0,0,0,12,12}; return r; }
        case ',': { static const uint8_t r[7] = {0,0,0,0,0,12,8}; return r; }
        case ':': { static const uint8_t r[7] = {0,12,12,0,12,12,0}; return r; }
        case '/': { static const uint8_t r[7] = {1,1,2,4,8,16,16}; return r; }
        case '-': { static const uint8_t r[7] = {0,0,0,31,0,0,0}; return r; }
        case '+': { static const uint8_t r[7] = {0,4,4,31,4,4,0}; return r; }
        case '!': { static const uint8_t r[7] = {4,4,4,4,4,0,4}; return r; }
        case '?': { static const uint8_t r[7] = {14,17,1,2,4,0,4}; return r; }
        case '(' : { static const uint8_t r[7] = {2,4,8,8,8,4,2}; return r; }
        case ')' : { static const uint8_t r[7] = {8,4,2,2,2,4,8}; return r; }
        case '\'' : { static const uint8_t r[7] = {4,4,8,0,0,0,0}; return r; }
        default: return NULL;
    }
}

static void font_write_pixel(uint32_t base, uint32_t fmt, int bpl, int bufW, int bufH, int px, int py, uint8_t val) {
    static const int pxBytes[5] = { 0, 0, 1, 3, 4 };
    if (fmt > 4 || px < 0 || px >= bufW || py < 0 || py >= bufH) return;
    int pb = pxBytes[fmt];
    uint32_t a = base + (uint32_t)(py * bpl) + (uint32_t)(pb == 0 ? px / 2 : px * pb);
    switch (fmt) {
        case 0: case 1: {
            uint8_t old = MEM_R8(a);
            uint8_t pix = val >> 4;
            if ((px & 1) != (int)fmt) MEM_W8(a, (uint8_t)((pix << 4) | (old & 0x0F)));
            else MEM_W8(a, (uint8_t)((old & 0xF0) | pix));
            break;
        }
        case 2: MEM_W8(a, val); break;
        case 3: MEM_W8(a, val); MEM_W8(a + 1, val); MEM_W8(a + 2, val); break;
        case 4: MEM_W32(a, (uint32_t)val | ((uint32_t)val << 8) | ((uint32_t)val << 16) | ((uint32_t)val << 24)); break;
    }
}

static uint32_t h_FontGetCharGlyphImage(CpuState *s) {
    uint32_t cc = A1 & 0xffff, gi = A2;
    if (!gi || cc <= 32) return 0;
    const PGF *p = font_for((int)cc);
    if (p) { pgf_draw_glyph(p, (int)cc, 0, gi); return 0; }
    /* No PGF available: synthetic 5x7 fallback below. */
    uint32_t fmt = MEM_R32(gi + 0);
    int x = (int)MEM_R32(gi + 4) >> 6, y = (int)MEM_R32(gi + 8) >> 6;
    int bufW = (int)(MEM_R16(gi + 12)), bufH = (int)(MEM_R16(gi + 14));
    int bpl = (int)(MEM_R16(gi + 16));
    uint32_t base = MEM_R32(gi + 20);
    if (getenv("SR_FONTLOG")) { static int n = 0; if (n++ < 12)
        fprintf(stderr, "glyph cc=0x%04x fmt=%u xy=(%d,%d) buf=0x%08x %dx%d bpl=%d\n", cc, fmt, x, y, base, bufW, bufH, bpl); }
    const uint8_t *rows = font5x7(cc);
    if (!rows && getenv("SR_FONTLOG")) { static unsigned char seen[65536]; if (!seen[cc]) { seen[cc] = 1;
        fprintf(stderr, "MISSING glyph cc=0x%04x '%c'\n", cc, (cc >= 32 && cc < 127) ? (char)cc : '?'); } }
    for (int yy = 0; yy < 11; yy++) for (int xx = 0; xx < 8; xx++) {
        int gx = xx - 1, gy = yy - 2;
        int on = rows && gx >= 0 && gx < 5 && gy >= 0 && gy < 7 && (rows[gy] & (1u << (4 - gx)));
        font_write_pixel(base, fmt, bpl, bufW, bufH, x + xx, y + yy, on ? 0xFF : 0x00);
    }
    return 0;
}
static uint32_t h_FontFindOptimumFont(CpuState *s) {
    if (A2) MEM_W32(A2, 0);            /* *errorCode */
    return 0;                          /* font index 0 */
}
/* ---- sceMpeg (PSMF video): faithful port in src/rt/mpeg.c (from PPSSPP) ----
 * These thin wrappers marshal MIPS args to the ported mpeg_* functions. The port implements the
 * real PSMF analysis, ring-buffer accounting, handle/context creation, stream registration, and
 * AU getters with timestamp progression + end-of-stream, so the movie playback loop runs and
 * completes exactly as on PPSSPP. The SDL3 build decodes AVC video through Windows Media
 * Foundation (h264_mf.c); ATRAC movie audio is still modelled as silence. */
uint32_t mpeg_init(void);
uint32_t mpeg_finish(void);
uint32_t mpeg_query_mem_size(uint32_t outAddr);
uint32_t mpeg_ringbuffer_query_mem_size(uint32_t packets);
uint32_t mpeg_ringbuffer_construct(uint32_t ring, uint32_t numPackets, uint32_t data, uint32_t size, uint32_t cbAddr, uint32_t cbArg);
uint32_t mpeg_create(uint32_t mpegAddr, uint32_t dataPtr, uint32_t size, uint32_t ringAddr, uint32_t frameWidth, uint32_t mode, uint32_t ddrTop);
uint32_t mpeg_delete(uint32_t mpegAddr);
uint32_t mpeg_query_stream_offset(uint32_t mpegAddr, uint32_t bufferAddr, uint32_t offsetAddr);
uint32_t mpeg_query_stream_size(uint32_t bufferAddr, uint32_t sizeAddr);
uint32_t mpeg_regist_stream(uint32_t mpegAddr, uint32_t streamType, uint32_t streamNum);
uint32_t mpeg_unregist_stream(uint32_t mpegAddr, uint32_t sid);
uint32_t mpeg_ringbuffer_available_size(uint32_t ring);
uint32_t mpeg_ringbuffer_put(CpuState *s, uint32_t ring, uint32_t numPackets, uint32_t available);
uint32_t mpeg_get_avc_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_get_atrac_au(uint32_t mpegAddr, uint32_t sid, uint32_t auAddr, uint32_t attrAddr);
uint32_t mpeg_avc_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t initAddr);
uint32_t mpeg_atrac_decode(uint32_t mpegAddr, uint32_t auAddr, uint32_t bufferAddr, uint32_t init);
uint32_t mpeg_avc_decode_stop(uint32_t mpegAddr, uint32_t frameWidth, uint32_t bufferAddr, uint32_t statusAddr);
uint32_t mpeg_malloc_avc_es_buf(uint32_t mpegAddr);
uint32_t mpeg_free_avc_es_buf(uint32_t mpegAddr, uint32_t esBuf);
uint32_t mpeg_init_au(uint32_t mpegAddr, uint32_t esBuffer, uint32_t auAddr);
uint32_t mpeg_query_atrac_es_size(uint32_t mpegAddr, uint32_t esSizeAddr, uint32_t outSizeAddr);
uint32_t mpeg_avc_decode_mode(uint32_t mpegAddr, uint32_t modeAddr);

static uint32_t h_MpegInit(CpuState *s) { (void)s; return mpeg_init(); }
static uint32_t h_MpegMallocAvcEsBuf(CpuState *s) { return mpeg_malloc_avc_es_buf(A0); }
static uint32_t h_MpegFreeAvcEsBuf(CpuState *s) { return mpeg_free_avc_es_buf(A0, A1); }
static uint32_t h_MpegInitAu(CpuState *s) { return mpeg_init_au(A0, A1, A2); }
static uint32_t h_MpegQueryAtracEsSize(CpuState *s) { return mpeg_query_atrac_es_size(A0, A1, A2); }
static uint32_t h_MpegFinish(CpuState *s) { (void)s; return mpeg_finish(); }
static uint32_t h_MpegAvcDecodeMode(CpuState *s) { return mpeg_avc_decode_mode(A0, A1); }
/* sceMpegQueryMemSize() takes no args and RETURNS the context size in v0 (PPSSPP MpegRequiredMem:
 * 0x10000 for lib version >= 0x0105, which ACX uses). The earlier wrapper wrote it to a pointer and
 * returned 0, so the game allocated a 0-byte mpeg buffer and sceMpegCreate failed with NO_MEMORY. */
static uint32_t h_MpegQueryMemSize(CpuState *s) { (void)s; return 0x10000u; }
static uint32_t h_MpegCreate(CpuState *s) {
    uint32_t r = mpeg_create(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1), stack_arg(s, 2));
    if (getenv("SR_MPEGLOG")) fprintf(stderr, "MpegCreate mpegAddr=0x%x data=0x%x size=0x%x ring=0x%x fw=%u -> 0x%x\n",
        A0, A1, A2, A3, stack_arg(s,0), r);
    return r;
}
static uint32_t h_MpegDelete(CpuState *s) { return mpeg_delete(A0); }
static uint32_t h_MpegRingbufferQueryMemSize(CpuState *s) { return mpeg_ringbuffer_query_mem_size(A0); }
static uint32_t h_MpegRingbufferConstruct(CpuState *s) { return mpeg_ringbuffer_construct(A0, A1, A2, A3, stack_arg(s, 0), stack_arg(s, 1)); }
static uint32_t h_MpegRingbufferAvailable(CpuState *s) { return mpeg_ringbuffer_available_size(A0); }
static uint32_t h_MpegRingbufferPut(CpuState *s) { return mpeg_ringbuffer_put(s, A0, A1, A2); }
static uint32_t h_MpegRegistStream(CpuState *s) { return mpeg_regist_stream(A0, A1, A2); }
static uint32_t h_MpegUnRegistStream(CpuState *s) { return mpeg_unregist_stream(A0, A1); }
static uint32_t h_MpegQueryStreamOffset(CpuState *s) { return mpeg_query_stream_offset(A0, A1, A2); }
static uint32_t h_MpegQueryStreamSize(CpuState *s) { return mpeg_query_stream_size(A0, A1); }
/* PPSSPP returns these via hleDelayResult so the playback thread yields (it does not busy-poll the
 * ring). Mirror that: delay the calling thread a frame-ish, longer when there is no data yet, so the
 * feeder/display threads run and the movie paces instead of hanging the scheduler. */
static uint32_t h_MpegGetAvcAu(CpuState *s) {
    uint32_t r = mpeg_get_avc_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
static uint32_t h_MpegGetAtracAu(CpuState *s) {
    uint32_t r = mpeg_get_atrac_au(A0, A1, A2, A3);
    sched_delay_current(r == 0x80618001u ? 8000u : 3000u);
    return r;
}
/* PPSSPP charges real decode latency (sceMpeg.cpp: avcDecodeDelayMs=5400, atracDecodeDelayMs=3000,
 * passed to hleDelayResult in microseconds). Besides pacing, the delay is a guaranteed yield. */
static uint32_t h_MpegAvcDecode(CpuState *s) {
    uint32_t r = mpeg_avc_decode(A0, A1, A2, A3, stack_arg(s, 0));
    sched_delay_current(5400);
    return r;
}
static uint32_t h_MpegAtracDecode(CpuState *s) {
    uint32_t r = mpeg_atrac_decode(A0, A1, A2, A3);
    sched_delay_current(3000);
    return r;
}
static uint32_t h_MpegAvcDecodeStop(CpuState *s) { return mpeg_avc_decode_stop(A0, A1, A2, A3); }

/* sceAtrac3plus: RIFF bookkeeping plus the standalone PPSSPP/FFmpeg decoder. */
#define ATRAC_SAMPLES_PER_FRAME 2048
#define ATRAC3_SAMPLES_PER_FRAME 1024
#define ATRAC_CODEC_PLUS 0x1000
#define ATRAC_CODEC_AT3  0x1001
#define ATRAC_INTERNAL_DECODE_ERROR 0x807f00fdu

typedef struct {
    int used;
    uint32_t buf, size, fileSize;
    int endSample, posSample, loopNum;
    int codec, channels, blockAlign;
    uint32_t dataOffset, dataBytes, frameOffset;
    int firstSampleOffset, skipSamples, primeFrames;
    int loopStart, loopEnd;
    uint32_t internalError;
    int reportedAudio;
    int streamed;
    uint32_t ringRead, ringWrite, buffered, fileReadOffset;
    uint32_t resetFileOffset, lastWritable;
    int resetSample;
    SrAtracDecoder *decoder;
} Atrac;

static Atrac s_atrac[8];
static int16_t s_atrac_decode[ATRAC_SAMPLES_PER_FRAME * 2];
static uint8_t s_atrac_frame[0x4000];

static int atrac_log(void) {
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("SR_ATRACLOG") ? 1 : 0;
    return enabled;
}

static int atrac_samples_per_frame(const Atrac *a) {
    return a->codec == ATRAC_CODEC_AT3
        ? ATRAC3_SAMPLES_PER_FRAME : ATRAC_SAMPLES_PER_FRAME;
}

static int atrac_first_sample_full(const Atrac *a) {
    return a->firstSampleOffset +
        (a->codec == ATRAC_CODEC_AT3 ? 0x45 : 0x170);
}

static int atrac_target_frame(const Atrac *a, int sample) {
    return 1 + (a->firstSampleOffset + sample) / atrac_samples_per_frame(a);
}

static uint32_t atrac_file_offset_for_sample(const Atrac *a, int sample) {
    int frame = atrac_target_frame(a, sample);
    if (frame > 0) frame--;
    return a->dataOffset + (uint32_t)frame * (uint32_t)a->blockAlign;
}

static void atrac_destroy_decoder(Atrac *a) {
    sr_atrac_decoder_destroy(a->decoder);
    a->decoder = NULL;
}

static uint32_t atrac_parse_riff(Atrac *a) {
    if (a->size < 72 || !sr_inrange(a->buf) ||
        a->buf > UINT32_MAX - (a->size - 1u) ||
        !sr_inrange(a->buf + a->size - 1u))
        return 0x80630011u;
    if (MEM_R32(a->buf) != 0x46464952u || MEM_R32(a->buf + 8) != 0x45564157u)
        return 0x80630006u;

    a->fileSize = MEM_R32(a->buf + 4) + 8u;
    uint32_t offset = 12;
    int found_format = 0;
    int found_data = 0;
    while (offset <= a->size - 8u) {
        uint32_t id = MEM_R32(a->buf + offset);
        uint32_t raw_size = MEM_R32(a->buf + offset + 4u);
        uint32_t chunk_size = (raw_size + 1u) & ~1u;
        uint32_t data = offset + 8u;
        if (data > a->size || chunk_size > a->size - data) {
            if (id == 0x61746164u) {
                a->dataOffset = data;
                a->dataBytes = raw_size;
                found_data = 1;
            }
            break;
        }

        if (id == 0x20746d66u && raw_size >= 16) {
            uint32_t fmt = MEM_R16(a->buf + data);
            a->codec = fmt == 0x0270u ? ATRAC_CODEC_AT3 :
                fmt == 0xfffeu ? ATRAC_CODEC_PLUS : 0;
            a->channels = MEM_R16(a->buf + data + 2u);
            uint32_t sample_rate = MEM_R32(a->buf + data + 4u);
            a->blockAlign = MEM_R16(a->buf + data + 12u);
            if (!a->codec || (a->channels != 1 && a->channels != 2) ||
                sample_rate != 44100 || !a->blockAlign)
                return 0x80630008u;
            found_format = 1;
        } else if (id == 0x74636166u && raw_size >= 4) {
            a->endSample = (int)MEM_R32(a->buf + data);
            if (raw_size >= 8)
                a->firstSampleOffset = (int)MEM_R32(a->buf + data + 4u);
        } else if (id == 0x6c706d73u && raw_size >= 56 &&
                   MEM_R32(a->buf + data + 28u) != 0) {
            a->loopStart = (int)MEM_R32(a->buf + data + 44u);
            a->loopEnd = (int)MEM_R32(a->buf + data + 48u);
        } else if (id == 0x61746164u) {
            a->dataOffset = data;
            a->dataBytes = raw_size;
            found_data = 1;
            break;
        }
        offset = data + chunk_size;
    }

    if (!found_format || !found_data) return 0x80630006u;
    if (a->dataOffset >= a->size ||
        (uint32_t)a->blockAlign > a->size - a->dataOffset)
        return 0x80630011u;
    if (a->endSample <= 0) {
        uint32_t frames = a->dataBytes / (uint32_t)a->blockAlign;
        int samples = (int)frames * atrac_samples_per_frame(a) -
            atrac_first_sample_full(a);
        a->endSample = samples > 0 ? samples : atrac_samples_per_frame(a);
    }
    if (a->loopStart >= 0 && a->loopEnd >= 0) {
        int first_sample = atrac_first_sample_full(a);
        a->loopStart = a->loopStart > first_sample
            ? a->loopStart - first_sample : 0;
        a->loopEnd = a->loopEnd > first_sample
            ? a->loopEnd - first_sample : 0;
    }
    if (a->loopStart >= a->loopEnd) a->loopStart = a->loopEnd = -1;
    return 0;
}

static const uint8_t *atrac_frame_data(Atrac *a) {
    uint32_t frame_bytes = (uint32_t)a->blockAlign;
    if (frame_bytes > sizeof(s_atrac_frame)) return NULL;
    if (!a->streamed) {
        if (a->frameOffset > a->size || frame_bytes > a->size - a->frameOffset)
            return NULL;
        return (const uint8_t *)SR_HOST(a->buf + a->frameOffset);
    }
    if (a->buffered < frame_bytes) return NULL;
    if (a->ringRead + frame_bytes <= a->size)
        return (const uint8_t *)SR_HOST(a->buf + a->ringRead);
    uint32_t first = a->size - a->ringRead;
    memcpy(s_atrac_frame, SR_HOST(a->buf + a->ringRead), first);
    memcpy(s_atrac_frame + first, SR_HOST(a->buf), frame_bytes - first);
    return s_atrac_frame;
}

static void atrac_consume_frame(Atrac *a) {
    uint32_t frame_bytes = (uint32_t)a->blockAlign;
    if (!a->streamed) {
        a->frameOffset += frame_bytes;
        return;
    }
    a->ringRead = (a->ringRead + frame_bytes) % a->size;
    a->buffered -= frame_bytes;
    a->frameOffset = a->ringRead;
}

static uint32_t atrac_prepare_decoder(Atrac *a, int sample) {
    atrac_destroy_decoder(a);
    a->decoder = sr_atrac_decoder_create(a->codec, a->channels, a->blockAlign);
    if (!a->decoder) {
        a->internalError = ATRAC_INTERNAL_DECODE_ERROR;
        return 0x80630008u;
    }

    int samples_per_frame = atrac_samples_per_frame(a);
    int source_sample = a->firstSampleOffset + sample;
    int target_frame = 1 + source_sample / samples_per_frame;
    a->skipSamples =
        (atrac_first_sample_full(a) + sample) % samples_per_frame;
    if (a->streamed) {
        a->frameOffset = a->ringRead;
        a->primeFrames = sample == 0 ? target_frame : 1;
    } else {
        int first_prime = sample == 0 ? 0 : target_frame > 0 ? target_frame - 1 : 0;
        a->frameOffset =
            a->dataOffset + (uint32_t)first_prime * (uint32_t)a->blockAlign;
        a->primeFrames = target_frame - first_prime;
    }
    a->posSample = sample;
    a->internalError = 0;
    return 0;
}

static uint32_t h_AtracSetDataAndGetID(CpuState *s) {
    int id = -1; for (int i = 0; i < 8; i++) if (!s_atrac[i].used) { id = i; break; }
    if (id < 0) return 0xFFFFFFFFu;
    Atrac *a = &s_atrac[id];
    atrac_destroy_decoder(a);
    memset(a, 0, sizeof(*a));
    a->used = 1;
    a->buf = A0;
    a->size = A1;
    a->loopStart = a->loopEnd = -1;
    uint32_t result = atrac_parse_riff(a);
    if (!result &&
        (a->fileSize > a->size || a->dataBytes > a->size - a->dataOffset)) {
        a->streamed = 1;
        a->ringRead = a->dataOffset;
        a->ringWrite = 0;
        a->buffered = a->size - a->dataOffset;
        a->fileReadOffset = a->size;
        a->frameOffset = a->ringRead;
    }
    if (!result) result = atrac_prepare_decoder(a, 0);
    if (result) {
        atrac_destroy_decoder(a);
        memset(a, 0, sizeof(*a));
        return result;
    }
    if (atrac_log())
        fprintf(stderr,
                "ATRAC: set id=%d codec=0x%x channels=%d frame=%d "
                "buffer=0x%08x bytes=%u data=%u/%u samples=%d loop=%d..%d%s\n",
                id, a->codec, a->channels, a->blockAlign, a->buf, a->size,
                a->dataOffset, a->dataBytes, a->endSample, a->loopStart, a->loopEnd,
                a->streamed ? " streamed" : "");
    return (uint32_t)id;
}
static Atrac *atrac_of(uint32_t id) { return id < 8 && s_atrac[id].used ? &s_atrac[id] : 0; }
static uint32_t h_AtracReleaseAtracID(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (a) {
        atrac_destroy_decoder(a);
        memset(a, 0, sizeof(*a));
    }
    return 0;
}

static uint32_t h_AtracDecodeData(CpuState *s) {
    /* a0=id, a1=outSamples, a2=*decodedSamples, a3=*finishFlag, sp+16=*remainFrames. */
    Atrac *a = atrac_of(A0); if (!a) return 0x80630002u;   /* bad ID */
    uint32_t out = A1, decAddr = A2, finAddr = A3, remAddr = stack_arg(s, 0);
    int decoded = 0;
    if (!a->decoder) return 0x80630024u;
    while (a->primeFrames > 0) {
        const uint8_t *frame = atrac_frame_data(a);
        if (!frame) {
            a->internalError = 0x80630023u;
            return 0x80630023u;
        }
        int primed_samples = 0;
        int primed = sr_atrac_decoder_decode(
            a->decoder, frame, a->blockAlign, s_atrac_decode, 2,
            ATRAC_SAMPLES_PER_FRAME, &primed_samples);
        if (primed < 0) {
            a->internalError = (uint32_t)primed;
            return 0x80630024u;
        }
        atrac_consume_frame(a);
        a->primeFrames--;
    }
    const uint8_t *frame = atrac_frame_data(a);
    if (!frame) {
        a->internalError = 0x80630023u;
        return 0x80630023u;
    }

    int consumed = sr_atrac_decoder_decode(
        a->decoder, frame, a->blockAlign, s_atrac_decode, 2,
        ATRAC_SAMPLES_PER_FRAME, &decoded);
    if (consumed < 0) {
        a->internalError = (uint32_t)consumed;
        if (atrac_log())
            fprintf(stderr, "ATRAC: decode error id=%u frame=0x%x code=%d\n",
                    A0, a->frameOffset, consumed);
        return 0x80630024u;
    }
    atrac_consume_frame(a);

    int skip = a->skipSamples < decoded ? a->skipSamples : decoded;
    a->skipSamples -= skip;
    int n = decoded - skip;
    if (n > a->endSample - a->posSample) n = a->endSample - a->posSample;
    if (n < 0) n = 0;
    if (out && n > 0) {
        uint32_t bytes = (uint32_t)n * 4u;
        if (out > UINT32_MAX - (bytes - 1u) ||
            !sr_inrange(out) || !sr_inrange(out + bytes - 1u))
            return 0x800200d3u;
        int peak = 0;
        for (int i = 0; i < n; i++) {
            int16_t left = s_atrac_decode[(i + skip) * 2];
            int16_t right = s_atrac_decode[(i + skip) * 2 + 1];
            MEM_W16(out + (uint32_t)i * 4u, (uint16_t)left);
            MEM_W16(out + (uint32_t)i * 4u + 2u, (uint16_t)right);
            int left_abs = left < 0 ? -(int)left : left;
            int right_abs = right < 0 ? -(int)right : right;
            if (left_abs > peak) peak = left_abs;
            if (right_abs > peak) peak = right_abs;
        }
        if (peak >= 256 && !a->reportedAudio) {
            a->reportedAudio = 1;
            if (atrac_log())
                fprintf(stderr, "ATRAC: audible id=%u peak=%d sample=%d\n",
                        A0, peak, a->posSample);
        }
    }
    a->posSample += n;
    int finished = 0;
    if (a->posSample >= a->endSample) {
        if (a->loopNum == 0 || a->loopStart < 0) {
            finished = 1;
            if (atrac_log()) fprintf(stderr, "ATRAC: end id=%u samples=%d\n", A0, a->endSample);
        } else {
            if (a->streamed) {
                a->ringRead = a->ringWrite = a->buffered = 0;
                a->fileReadOffset = atrac_file_offset_for_sample(a, a->loopStart);
                a->frameOffset = 0;
                a->lastWritable = 0;
            }
            uint32_t loop_result = atrac_prepare_decoder(a, a->loopStart);
            if (loop_result) return loop_result;
            if (a->loopNum > 0) a->loopNum--;
            if (atrac_log())
                fprintf(stderr, "ATRAC: loop id=%u remaining=%d\n", A0, a->loopNum);
        }
    }
    if (decAddr) MEM_W32(decAddr, (uint32_t)n);
    if (finAddr) MEM_W32(finAddr, (uint32_t)finished);
    if (remAddr) MEM_W32(remAddr, 0xFFFFFFFFu);            /* ALLDATA_IS_ON_MEMORY */
    return 0;
}
static uint32_t h_AtracGetRemainFrame(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    uint32_t frames = a->streamed
        ? a->buffered / (uint32_t)a->blockAlign : 0xFFFFFFFFu;
    if (A1) MEM_W32(A1, frames);
    return 0;
}
static uint32_t h_AtracGetStreamDataInfo(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    uint32_t writable = 0;
    uint32_t write_pointer = a->buf;
    uint32_t read_offset = 0;
    if (a->streamed && a->fileReadOffset < a->fileSize) {
        uint32_t free_bytes = a->size - a->buffered;
        uint32_t contiguous = a->size - a->ringWrite;
        uint32_t remaining = a->fileSize - a->fileReadOffset;
        writable = free_bytes < contiguous ? free_bytes : contiguous;
        if (writable > remaining) writable = remaining;
        write_pointer = a->buf + a->ringWrite;
        read_offset = a->fileReadOffset;
    }
    a->lastWritable = writable;
    if (A1) MEM_W32(A1, write_pointer);
    if (A2) MEM_W32(A2, writable);
    if (A3) MEM_W32(A3, read_offset);
    if (atrac_log() && a->streamed)
        fprintf(stderr, "ATRAC: stream id=%u write=0x%08x bytes=%u file=%u\n",
                A0, write_pointer, writable, read_offset);
    return 0;
}
static uint32_t h_AtracAddStreamData(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    if (!a->streamed) return 0x80630009u;
    uint32_t bytes = A1;
    if (bytes > a->lastWritable || bytes > a->size - a->buffered)
        return 0x80630018u;
    a->ringWrite = (a->ringWrite + bytes) % a->size;
    a->buffered += bytes;
    a->fileReadOffset += bytes;
    a->lastWritable = 0;
    if (atrac_log())
        fprintf(stderr, "ATRAC: add id=%u bytes=%u buffered=%u file=%u\n",
                A0, bytes, a->buffered, a->fileReadOffset);
    return 0;
}
static uint32_t h_AtracGetNextDecodePosition(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return 0x80630002u;
    if (a->posSample >= a->endSample) return 0x80630024u;
    if (A1) MEM_W32(A1, (uint32_t)a->posSample);
    return 0;
}
static uint32_t h_AtracGetSoundSample(CpuState *s) {
    Atrac *a = atrac_of(A0); if (!a) return 0x80630002u;
    if (A1) MEM_W32(A1, (uint32_t)a->endSample);
    if (A2) MEM_W32(A2, (uint32_t)a->loopStart);
    if (A3) MEM_W32(A3, (uint32_t)a->loopEnd);
    return 0;
}
static uint32_t h_AtracGetLoopStatus(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    if (A1) MEM_W32(A1, (uint32_t)a->loopNum);
    if (A2) MEM_W32(A2, a->loopStart >= 0 ? 1u : 0u);
    return 0;
}
static uint32_t h_AtracSetLoopNum(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    if (a->loopStart < 0 && A1 != 0) return 0x80630021u;
    a->loopNum = (int)A1;
    if (atrac_log()) fprintf(stderr, "ATRAC: set loop id=%u count=%d\n", A0, a->loopNum);
    return 0;
}
static uint32_t h_AtracGetInternalErrorInfo(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    if (A1 && A1 <= UINT32_MAX - 3u && sr_inrange(A1) && sr_inrange(A1 + 3u))
        MEM_W32(A1, a->internalError);
    return 0;
}
static uint32_t h_AtracGetBufferInfoForResetting(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    if ((uint32_t)A1 > (uint32_t)a->endSample) return 0x80630015u;
    if (!A2 || A2 > UINT32_MAX - 31u || !sr_inrange(A2) || !sr_inrange(A2 + 31u))
        return 0x800200d3u;

    for (uint32_t off = 0; off < 32u; off += 4u) MEM_W32(A2 + off, 0);
    if (a->streamed) {
        uint32_t file_offset = atrac_file_offset_for_sample(a, (int)A1);
        uint32_t writable =
            file_offset < a->fileSize ? a->fileSize - file_offset : 0;
        if (writable > a->size) writable = a->size;
        uint32_t minimum = writable < (uint32_t)a->blockAlign
            ? writable : (uint32_t)a->blockAlign;
        MEM_W32(A2 + 0, a->buf);
        MEM_W32(A2 + 4, writable);
        MEM_W32(A2 + 8, minimum);
        MEM_W32(A2 + 12, file_offset);
        a->resetFileOffset = file_offset;
        a->resetSample = (int)A1;
    } else {
        MEM_W32(A2 + 0, a->buf);
        MEM_W32(A2 + 16, a->buf);
    }
    if (atrac_log())
        fprintf(stderr, "ATRAC: reset info id=%u sample=%u file=%u writable=%u\n",
                A0, A1, MEM_R32(A2 + 12), MEM_R32(A2 + 4));
    return 0;
}
static uint32_t h_AtracResetPlayPosition(CpuState *s) {
    Atrac *a = atrac_of(A0);
    if (!a) return 0x80630002u;
    if (A1 > (uint32_t)a->endSample) return 0x80630015u;
    if (a->streamed) {
        if (a->resetSample != (int)A1 || A2 > a->size ||
            a->resetFileOffset > a->fileSize ||
            A2 > a->fileSize - a->resetFileOffset)
            return 0x80630016u;
        a->ringRead = 0;
        a->ringWrite = A2 % a->size;
        a->buffered = A2;
        a->fileReadOffset = a->resetFileOffset + A2;
        a->frameOffset = 0;
        a->lastWritable = 0;
    }
    uint32_t result = atrac_prepare_decoder(a, (int)A1);
    if (atrac_log())
        fprintf(stderr, "ATRAC: reset id=%u sample=%d result=0x%08x\n",
                A0, a->posSample, result);
    return result;
}

/* sceUtility dialogs (savedata/msg/osk). Faithful to PPSSPP's PSPDialog status machine
 * (Core/Dialog/PSPDialog.cpp): status enum NONE=0, INITIALIZE=1, RUNNING=2, FINISHED=3,
 * SHUTDOWN=4. InitStart -> INITIALIZE; GetStatus returns the current status and then auto-advances
 * INITIALIZE->RUNNING and SHUTDOWN->NONE; the (real-hardware) utility thread completes the
 * autoload, modelled here by RUNNING->FINISHED after a few polls; ShutdownStart -> SHUTDOWN. The
 * earlier guess jumped straight to RUNNING and to NONE, skipping INITIALIZE(1) and SHUTDOWN(4),
 * which a game that waits to observe those states would hang on. result is the common-header
 * field at param+0x1c. */
static void guest_cstr(uint32_t addr, char *out, int max);

/* sceUtilitySavedata: real persistence on a virtual memory stick (src/rt/savedata.c). */
uint32_t sr_savedata_execute(uint32_t param);

static int s_dlg_status = 0, s_dlg_tick = 0; static uint32_t s_dlg_param = 0, s_dlg_result = 0;
static int s_osk_current_clear(void);   /* fwd: savedata/msg dialogs take the slot from the OSK */
static uint32_t h_SavedataInitStart(CpuState *s) {
    s_dlg_param = A0; s_dlg_status = 1; s_dlg_tick = 0;   /* INITIALIZE */
    s_osk_current_clear();
    s_dlg_result = sr_savedata_execute(A0);               /* fill result structs now (PPSSPP IO action) */
    if (getenv("SR_DLGLOG")) { static int n=0; fprintf(stderr, "** SavedataInitStart #%d **\n", ++n);
        uint32_t p = A0;
        char gn[16], sn[24]; guest_cstr(p+0x3c, gn, sizeof(gn)); guest_cstr(p+0x4c, sn, sizeof(sn));
        fprintf(stderr, "SavedataInitStart param=0x%08x size=%u mode=%d gameName='%s' saveName='%s' result=0x%08x\n",
            p, MEM_R32(p+0), (int)MEM_R32(p+0x30), gn, sn, s_dlg_result);
    }
    return 0;
}
static uint32_t h_MsgDialogInitStart(CpuState *s) {
    s_dlg_param = A0; s_dlg_status = 1; s_dlg_tick = 0; s_dlg_result = 0;
    s_osk_current_clear();
    return 0;
}
static uint32_t h_DlgGetStatus(CpuState *s) {
    (void)s;
    int ret = s_dlg_status;
    if (s_dlg_status == 1) {                              /* INITIALIZE -> RUNNING */
        s_dlg_status = 2; s_dlg_tick = 0;
    } else if (s_dlg_status == 2) {                       /* utility-thread completes the op */
        if (++s_dlg_tick > 3) { s_dlg_status = 3; if (s_dlg_param) MEM_W32(s_dlg_param + 0x1c, s_dlg_result); }
    } else if (s_dlg_status == 4) {                       /* SHUTDOWN -> NONE */
        s_dlg_status = 0;
    }
    if (getenv("SR_DLGLOG")) {                            /* log transitions only (unbounded) */
        static int last = -1;
        if (s_dlg_status != last) {
            fprintf(stderr, "DlgGetStatus: ret=%d -> status=%d (result=0x%08x)\n", ret, s_dlg_status, s_dlg_result);
            last = s_dlg_status;
        }
    }
    return (uint32_t)ret;
}
static uint32_t h_SavedataUpdate(CpuState *s) {
    (void)s;
    if (s_dlg_status == 2) {
        s_dlg_status = 3;
        if (s_dlg_param) MEM_W32(s_dlg_param + 0x1c, s_dlg_result);
        if (getenv("SR_DLGLOG")) fprintf(stderr, "SavedataUpdate: FINISHED result=0x%08x written to 0x%08x\n",
                                         s_dlg_result, s_dlg_param + 0x1c);
    }
    return 0;
}
static uint32_t h_DlgShutdown(CpuState *s) {
    (void)s;
    s_dlg_status = 4;
    if (getenv("SR_DLGLOG")) fprintf(stderr, "DlgShutdownStart\n");
    return 0;
}

/* ---- sceUtilityOsk: the on-screen keyboard, backed by a native input box (osk_win.c).
 * Same PSPDialog status machine as the other utilities. When the dialog reaches RUNNING the
 * native modal input box collects the text (the game is parked polling OskGetStatus, exactly
 * as it would be while the real OSK overlay is up), then the result is written back into each
 * SceUtilityOskData field as UTF-16 and the status advances to FINISHED. */
int sr_osk_input(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap);
static int s_osk_status = 0;
static uint32_t s_osk_param = 0;
/* PPSSPP keeps a "current dialog type": OskGetStatus is WRONG_TYPE only while a DIFFERENT
 * utility dialog owns the slot. After an OSK shuts down it stays the current dialog and
 * GetStatus returns NONE(0) — a game spinning "while (OskGetStatus() != 0)" after name entry
 * hangs forever if we keep returning WRONG_TYPE there. */
static int s_osk_current = 0;
static int s_osk_current_clear(void) { s_osk_current = 0; return 0; }

static void osk_read_utf16(uint32_t addr, wchar_t *out, int max) {
    int i = 0;
    if (addr) for (; i < max - 1; i++) {
        uint16_t c = MEM_R16(addr + (uint32_t)i * 2);
        if (!c) break;
        out[i] = (wchar_t)c;
    }
    out[i] = 0;
}

static void osk_run(void) {
    uint32_t p = s_osk_param;
    if (!p) return;
    int nf = (int)MEM_R32(p + 0x30);                       /* fieldCount */
    uint32_t fields = MEM_R32(p + 0x34);                   /* SceUtilityOskData[] */
    if (nf < 1 || nf > 8 || !fields) return;
    for (int i = 0; i < nf; i++) {
        uint32_t f = fields + (uint32_t)i * 0x34;
        uint32_t descA = MEM_R32(f + 0x1c), inA = MEM_R32(f + 0x20);
        uint32_t outLen = MEM_R32(f + 0x24), outA = MEM_R32(f + 0x28);
        uint32_t outLimit = MEM_R32(f + 0x30);
        wchar_t desc[128], intext[256], out[256];
        osk_read_utf16(descA, desc, 128);
        osk_read_utf16(inA, intext, 256);
        int cap = outLen ? (int)outLen : 256;              /* u16 units incl. terminator */
        if (outLimit && (int)outLimit + 1 < cap) cap = (int)outLimit + 1;
        if (cap > 256) cap = 256;
        wcscpy(out, intext);
        int ok = sr_osk_input(desc, intext, out, cap);
        if (outA) {
            const wchar_t *w = ok ? out : intext;
            int j = 0;
            for (; w[j] && j < cap - 1; j++) MEM_W16(outA + (uint32_t)j * 2, (uint16_t)w[j]);
            MEM_W16(outA + (uint32_t)j * 2, 0);
        }
        MEM_W32(f + 0x2c, ok ? 2u : 1u);                   /* result: CHANGED / CANCELLED */
        if (getenv("SR_DLGLOG"))
            fprintf(stderr, "osk: field %d desc='%ls' in='%ls' -> %s '%ls'\n",
                    i, desc, intext, ok ? "ok" : "cancel", ok ? out : intext);
    }
    MEM_W32(p + 0x1c, 0);                                  /* common result */
}

static uint32_t h_OskInitStart(CpuState *s) {
    s_osk_param = A0;
    s_osk_status = 1;                                      /* INITIALIZE */
    s_osk_current = 1;                                     /* OSK owns the dialog slot */
    return 0;
}
/* sceUtilityOskGetStatus: with no OSK ever started (e.g. polled every boot frame while a
 * savedata dialog is up) PPSSPP returns SCE_ERROR_UTILITY_WRONG_TYPE (0x80110005). Once an
 * OSK ran, NONE(0) is a real status games wait for after shutdown. */
static uint32_t h_OskGetStatus(CpuState *s) {
    (void)s;
    if (!s_osk_current) return 0x80110005u;
    int ret = s_osk_status;
    if (s_osk_status == 1) s_osk_status = 2;               /* INITIALIZE -> RUNNING */
    else if (s_osk_status == 2) { osk_run(); s_osk_status = 3; }   /* RUNNING -> FINISHED */
    else if (s_osk_status == 4) { s_osk_status = 0; s_osk_param = 0; }
    return (uint32_t)ret;
}
static uint32_t h_OskUpdate(CpuState *s) { (void)s; return 0; }
static uint32_t h_OskShutdown(CpuState *s) { (void)s; if (s_osk_status) s_osk_status = 4; return 0; }

static uint32_t h_VolatileMemLock(CpuState *s) {
    /* sceKernelVolatileMemLock(type, void **paddr, int *psize): hand the app the 4MB volatile
     * partition. PPSSPP returns base 0x08400000, size 0x00400000; the game uses it as the
     * destination scratch buffer for decompressing/copying loaded assets, so the out-params
     * must be filled or the copy targets NULL. */
    if (A1) MEM_W32(A1, 0x08400000u);
    if (A2) MEM_W32(A2, 0x00400000u);
    return 0;
}
static uint32_t h_UmdDriveStat(CpuState *s) { (void)s; return 0x32; }     /* PRESENT|READY|READABLE (matches PPSSPP reference) */

static uint32_t s_vcount_fwd;  /* mirror of s_vcount for clock/input timing (set in sr_vblank_tick) */
/* Virtual microsecond clock. It is anchored to the frame (VBLANK) counter so it advances at the
 * real PSP rate of one frame per 1/60 s (16667 us). A small per-call increment keeps it strictly
 * monotonic within a frame. Earlier this advanced only +100us per call, so time barely moved while
 * the game ran hundreds of frames -- any "wait N seconds of system time" (logo/loading screens)
 * never elapsed. s_vcount_fwd mirrors the frame counter (set in sr_vblank_tick). */
static uint64_t g_time_usec = 0;
static uint64_t now_usec(void) {
    uint64_t base = (uint64_t)s_vcount_fwd * 16667ull;
    if (base > g_time_usec) g_time_usec = base;
    else g_time_usec += 1;
    return g_time_usec;
}
static uint32_t h_GetSystemTimeLow(CpuState *s) { (void)s; return (uint32_t)now_usec(); }
static uint32_t h_GetSystemTimeWide(CpuState *s) { uint64_t t = now_usec(); s->r[3] = (uint32_t)(t >> 32); return (uint32_t)t; }
static uint32_t h_LibcTime(CpuState *s) {
    uint32_t t = (uint32_t)(now_usec() / 1000000) + 0x40000000u;   /* seconds since epoch-ish */
    if (A0) MEM_W32(A0, t);
    return t;
}
static uint32_t h_LibcClock(CpuState *s) { (void)s; return (uint32_t)now_usec(); }
/* sceKernelGetSystemTime(SceKernelSysClock *): microseconds since boot. */
static uint32_t h_GetSystemTime(CpuState *s) { uint64_t t = now_usec(); if (A0) { MEM_W32(A0, (uint32_t)t); MEM_W32(A0 + 4, (uint32_t)(t >> 32)); } return 0; }
/* sceRtcGetCurrentTick(u64 *): microseconds since 0001-01-01, like PPSSPP's __RtcGetCurrentTick
 * (host time at start + time since boot). */
#define RTC_MAGIC_OFFSET 62135596800000000ull   /* 0001-01-01 to 1970-01-01 in microseconds */
static uint64_t rtc_base_us(void) {
    static uint64_t base = 0;
    if (!base) base = (uint64_t)time(NULL) * 1000000ull + RTC_MAGIC_OFFSET;
    return base;
}
static uint32_t h_RtcGetCurrentTick(CpuState *s) {
    uint64_t t = rtc_base_us() + now_usec();
    if (A0) { MEM_W32(A0, (uint32_t)t); MEM_W32(A0 + 4, (uint32_t)(t >> 32)); }
    return 0;
}
/* sceRtcGetTime_t(const pspTime *date, u32 *time): the date as seconds since 1970 (UTC). */
static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
static uint32_t h_RtcGetTime_t(CpuState *s) {
    uint32_t in = A0, out = A1;
    if (!in || !out) return 0;
    int year = MEM_R16(in), month = MEM_R16(in + 2), day = MEM_R16(in + 4);
    int hour = MEM_R16(in + 6), minute = MEM_R16(in + 8), sec = MEM_R16(in + 10);
    int64_t t = days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 + minute * 60 + sec;
    MEM_W32(out, (uint32_t)t);
    return 0;
}
/* sceRtcGetWin32FileTime(pspTime *in, u64 *out): out gets 100ns FILETIME units; only deltas
 * matter to the game, so the virtual clock scaled by 10 suffices. The input struct is const. */
static uint32_t h_RtcGetWin32FileTime(CpuState *s) {
    uint64_t ft = now_usec() * 10u;
    if (A1) { MEM_W32(A1, (uint32_t)ft); MEM_W32(A1 + 4, (uint32_t)(ft >> 32)); }
    return 0;
}
static uint32_t h_RtcGetTickResolution(CpuState *s) { (void)s; return 1000000; }
/* Fill a struct timeval {sec, usec} from the virtual clock (the old h_ok left the output
 * struct unwritten -- garbage timestamps). */
static uint32_t h_LibcGettimeofday(CpuState *s) {
    uint64_t t = now_usec();
    if (A0) { MEM_W32(A0, (uint32_t)(t / 1000000u) + 0x40000000u); MEM_W32(A0 + 4, (uint32_t)(t % 1000000u)); }
    if (A1) { MEM_W32(A1, 0); MEM_W32(A1 + 4, 0); }   /* timezone: UTC */
    return 0;
}
/* Fill a pspTime {u16 year,month,day,hour,min,sec; u32 usec} from the virtual clock. */
static uint32_t h_RtcGetCurrentClock(CpuState *s) {
    uint32_t out = A0;
    uint64_t t = now_usec();
    if (out) {
        uint64_t secs = t / 1000000u;
        MEM_W16(out + 0, 2007);                          /* year (any sane value) */
        MEM_W16(out + 2, 1 + (uint16_t)((secs / 2592000u) % 12));
        MEM_W16(out + 4, 1 + (uint16_t)((secs / 86400u) % 28));
        MEM_W16(out + 6, (uint16_t)((secs / 3600u) % 24));
        MEM_W16(out + 8, (uint16_t)((secs / 60u) % 60));
        MEM_W16(out + 10, (uint16_t)(secs % 60));
        MEM_W32(out + 12, (uint32_t)(t % 1000000u));
    }
    return 0;
}
static uint32_t h_StdFd(CpuState *s) { (void)s; return 1; }   /* a std file descriptor */
static uint32_t h_F919F628(CpuState *s) { (void)s; return 0x001a0b00; }   /* matches PPSSPP reference */

/* ---- IoFileMgrForUser: file IO from the game ISO (src/rt/iso.c) ---- */

static void guest_cstr(uint32_t addr, char *out, int max) {
    int i = 0;
    for (; i < max - 1; i++) { uint8_t c = MEM_R8(addr + (uint32_t)i); if (!c) break; out[i] = (char)c; }
    out[i] = 0;
}

/* sceKernelLoadModule(path, flags, opt): the module's code is NOT loaded or run (static
 * recompilation covers only the main EBOOT). Log the path loudly so any feature that
 * depends on a runtime-loaded module (e.g. a cutscene player) is identifiable. */
static uint32_t h_LoadModule(CpuState *s) {
    char path[256]; guest_cstr(A0, path, sizeof(path));
    fprintf(stderr, "sceKernelLoadModule(\"%s\") -> fake uid (module code not loaded)\n", path);
    return sr_alloc_uid();
}

/* sceKernelPrintf: surface the game's own debug output (format string only; the engine's
 * messages are mostly plain text and any %-args print as-is, which is still informative). */
static uint32_t h_KernelPrintf(CpuState *s) {
    char msg[512]; guest_cstr(A0, msg, sizeof(msg));
    fprintf(stderr, "GAMELOG %s%s", msg, (msg[0] && msg[strlen(msg)-1] == '\n') ? "" : "\n");
    return 0;
}

typedef enum {
    IO_ASYNC_NONE,
    IO_ASYNC_OPEN,
    IO_ASYNC_READ,
    IO_ASYNC_SEEK,
    IO_ASYNC_CLOSE,
} IoAsyncOp;

typedef struct {
    int used;
    uint32_t lba, size, off;
    FILE *host;
    IoAsyncOp async_op;
    uint32_t async_addr, async_size;
    int64_t async_offset;
    uint32_t async_whence, async_delay;
    int64_t async_res;
    int async_pending, async_ready, close_pending;
    int async_priority;
    uint32_t async_thread;
} Fd;
static Fd s_fds[64];
static int s_async_default_priority = -1;

#define IO_ASYNC_RESULT_OBJ(fd) (0x494f0000u + (fd))
#define IO_ASYNC_START_OBJ(fd)  (0x494f0100u + (fd))
#define SCE_KERNEL_ERROR_BADF       0x80020323u
#define SCE_KERNEL_ERROR_ASYNC_BUSY 0x80020329u
#define SCE_KERNEL_ERROR_NOASYNC    0x8002032au

static int io_find_slot(void) {
    for (int i = 3; i < 64; i++) if (!s_fds[i].used) return i;
    return -1;
}

static int io_async_busy(const Fd *f) {
    return f->async_pending || f->async_ready;
}

static void io_release_fd(uint32_t fd) {
    if (fd >= 64 || !s_fds[fd].used) return;
    Fd *f = &s_fds[fd];
    if (f->host) fclose(f->host);
    if (f->async_thread) sched_delete_hle_thread(f->async_thread);
    memset(f, 0, sizeof(*f));
}

static int host_root(char *out, int max) {
    const char *override = getenv("SR_FSDIR");
    int n;
    if (override && override[0]) {
        n = snprintf(out, (size_t)max, "%s", override);
    } else {
        const char *local = getenv("LOCALAPPDATA");
        if (!local || !local[0]) {
            static int warned;
            if (!warned++) fprintf(stderr, "VFS: LOCALAPPDATA is unset; set SR_FSDIR to override the writable root\n");
            return -1;
        }
        n = snprintf(out, (size_t)max, "%s\\KZLRecomp", local);
    }
    if (n < 0 || n >= max) {
        fprintf(stderr, "VFS: writable root path is too long\n");
        return -1;
    }
    return 0;
}

static int is_disc_path(const char *path) {
    return _strnicmp(path, "disc0:", 6) == 0 || _strnicmp(path, "umd0:", 5) == 0 ||
           strstr(path, "sce_lbn") != NULL;
}

/* Writable devices preserve their directory hierarchy below SR_FSDIR when set, otherwise below
 * %LOCALAPPDATA%\KZLRecomp. fatms0: and cache: alias ms0:, matching KZL's PSP device setup. */
static int host_path(const char *guest, char *out, int max) {
    char root[512];
    if (host_root(root, sizeof(root)) != 0) return -1;
    int n = snprintf(out, (size_t)max, "%s", root);
    if (n < 0 || n >= max) return -1;
    while (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/')) n--;
    out[n] = 0;

    const char *p = guest;
    const char *colon = strchr(guest, ':');
    char device[32] = "root";
    if (colon) {
        int len = (int)(colon - guest);
        if (len <= 0 || len >= (int)sizeof(device)) return -1;
        memcpy(device, guest, (size_t)len);
        device[len] = 0;
        if (_stricmp(device, "fatms0") == 0 || _stricmp(device, "cache") == 0)
            strcpy(device, "ms0");
        p = colon + 1;
    }
    if (n + 1 + (int)strlen(device) >= max) return -1;
    out[n++] = '\\';
    for (int i = 0; device[i]; i++) out[n++] = device[i];

    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != '/' && *end != '\\') end++;
        int len = (int)(end - p);
        if (len == 1 && p[0] == '.') { p = end; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') return -1;
        if (n + 1 + len >= max) return -1;
        out[n++] = '\\';
        for (int i = 0; i < len; i++) {
            unsigned char c = (unsigned char)p[i];
            out[n++] = (char)(c < 0x20 || strchr("<>:\"|?*", c) ? '_' : c);
        }
        p = end;
    }
    out[n] = 0;
    return 0;
}

/* Compatibility with scratch files produced before the VFS preserved directories. */
static int legacy_host_path(const char *guest, char *out, int max) {
    char root[512];
    if (host_root(root, sizeof(root)) != 0) return -1;
    int n = snprintf(out, (size_t)max, "%s\\", root);
    if (n < 0 || n >= max) return -1;
    for (int i = 0; guest[i] && n < max - 1; i++) {
        char c = guest[i];
        out[n++] = (char)((c == '/' || c == ':' || c == '\\' || c == ' ') ? '_' : c);
    }
    out[n] = 0;
    return guest[0] && n < max ? 0 : -1;
}

static void host_make_parents(char *path) {
    for (char *p = path; *p; p++) {
        if (*p != '\\' && *p != '/') continue;
        if (p == path + 2 && path[1] == ':') continue;
        char save = *p;
        *p = 0;
        if (path[0]) _mkdir(path);
        *p = save;
    }
}

static int host_existing_path(const char *guest, char *out, int max, struct _stat64 *st) {
    if (host_path(guest, out, max) != 0) return -1;
    if (_stat64(out, st) == 0) return 0;
    char legacy[512];
    if (legacy_host_path(guest, legacy, sizeof(legacy)) == 0 && _stat64(legacy, st) == 0) {
        snprintf(out, (size_t)max, "%s", legacy);
        return 0;
    }
    return -1;
}

static uint32_t h_IoOpen(CpuState *s) {
    /* a0=path, a1=flags, a2=mode. Returns an fd (>=0) or a negative error.
     * PSP flags: WRONLY=2, RDWR=3, APPEND=0x100, CREAT=0x200, TRUNC=0x400. */
    char path[256];
    guest_cstr(A0, path, sizeof(path));
    uint32_t flags = A1;
    if (getenv("SR_IOLOG")) fprintf(stderr, "Open(%s) flags=0x%x\n", path, flags);
    if (getenv("SR_PATHHEX")) {
        int bad = 0; for (int i = 0; path[i]; i++) if ((unsigned char)path[i] < 0x20 || (unsigned char)path[i] >= 0x7f) bad = 1;
        if (bad || path[0] == 0) {
            fprintf(stderr, "Open BAD path ptr=0x%08x bytes:", A0);
            for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", MEM_R8(A0 + (uint32_t)i));
            fprintf(stderr, "\n");
        }
    }
    int slot = io_find_slot();
    if (slot < 0) return 0x80010018;  /* too many open files */

    int writing = (flags & 0x0002) != 0;        /* WRONLY or RDWR */
    int creating = (flags & 0x0200) != 0;
    uint32_t lba, size;
    int in_iso = (is_disc_path(path) || strchr(path, ':') == NULL) &&
                 iso_lookup(path, &lba, &size) == 0;

    if (writing || creating || !in_iso) {
        /* Host-backed file (writable storage). */
        if ((writing || creating) && is_disc_path(path)) return 0x8001001eu;  /* read-only */
        char hp[512];
        if (host_path(path, hp, sizeof(hp)) != 0) return 0x80010016u;
        struct _stat64 st;
        if (_stat64(hp, &st) != 0) {
            char legacy[512];
            if (legacy_host_path(path, legacy, sizeof(legacy)) == 0 && _stat64(legacy, &st) == 0)
                snprintf(hp, sizeof(hp), "%s", legacy);
        }
        if (writing || creating) host_make_parents(hp);
        const char *mode;
        if (flags & 0x0400) mode = "w+b";            /* TRUNC */
        else if (flags & 0x0100) mode = "a+b";       /* APPEND */
        else if (writing || creating) mode = "r+b";  /* update; fall back to create below */
        else mode = "rb";                            /* read-only host file (e.g. a prior save) */
        FILE *fp = fopen(hp, mode);
        if (!fp && (writing || creating)) fp = fopen(hp, "w+b");  /* create if missing */
        if (!fp) {
            if (in_iso) goto from_iso;               /* read-only and only on disc: serve the ISO */
            fprintf(stderr, "sceIoOpen: not found: %s\n", path);
            return 0x80010002;
        }
        fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
        Fd *f = &s_fds[slot];
        memset(f, 0, sizeof(*f));
        f->used = 1; f->host = fp; f->lba = 0;
        f->size = (uint32_t)(sz < 0 ? 0 : sz); f->off = 0;
        f->async_priority = s_async_default_priority;
        return (uint32_t)slot;
    }
from_iso:
    memset(&s_fds[slot], 0, sizeof(s_fds[slot]));
    s_fds[slot].used = 1; s_fds[slot].host = NULL;
    s_fds[slot].lba = lba; s_fds[slot].size = size; s_fds[slot].off = 0;
    s_fds[slot].async_priority = s_async_default_priority;
    return (uint32_t)slot;
}
static uint32_t h_IoWrite(CpuState *s) {
    /* a0=fd, a1=src, a2=count. Returns bytes written. */
    uint32_t fd = A0, src = A1, count = A2;
    if (fd == 1 || fd == 2) return count;            /* stdout/stderr: pretend-consumed */
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    Fd *f = &s_fds[fd];
    if (io_async_busy(f)) return SCE_KERNEL_ERROR_ASYNC_BUSY;
    if (!f->host) return 0x80010013;                 /* read-only (ISO) fd: not writable */
    fseek(f->host, (long)f->off, SEEK_SET);
    uint8_t tmp[4096]; uint32_t done = 0;
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        for (uint32_t k = 0; k < n; k++) tmp[k] = (uint8_t)MEM_R8(src + done + k);
        done += (uint32_t)fwrite(tmp, 1, n, f->host);
        if (done < count) break;
    }
    fflush(f->host);
    f->off += done; if (f->off > f->size) f->size = f->off;
    return done;
}
static uint32_t io_read(uint32_t fd, uint32_t dst, uint32_t count) {
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    Fd *f = &s_fds[fd];
    if (f->off + count > f->size) count = f->size - f->off;
    /* Read in chunks straight into guest memory, from the host file or the ISO. */
    uint8_t tmp[4096];
    uint32_t done = 0;
    if (f->host) fseek(f->host, (long)f->off, SEEK_SET);
    while (done < count) {
        uint32_t n = count - done; if (n > sizeof(tmp)) n = sizeof(tmp);
        if (f->host) { size_t got = fread(tmp, 1, n, f->host); if (got < n) n = (uint32_t)got; }
        else iso_read(f->lba, f->off + done, tmp, n);
        for (uint32_t k = 0; k < n; k++) MEM_W8(dst + done + k, tmp[k]);
        done += n;
        if (n == 0) break;
    }
    f->off += done;
    if (getenv("SR_IOLOG")) {
        static int n = 0;
        if (n++ < 4000)
            fprintf(stderr, "Read fd=%u off=%u size=%u -> %u\n", fd, f->off - done, count, done);
    }
    return done;
}

static uint32_t h_IoRead(CpuState *s) {
    /* a0=fd, a1=dst, a2=count. Returns bytes read. */
    uint32_t fd = A0;
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    if (io_async_busy(&s_fds[fd])) return SCE_KERNEL_ERROR_ASYNC_BUSY;
    return io_read(fd, A1, A2);
}

static int64_t io_seek(Fd *f, int64_t off, uint32_t whence) {
    int64_t base = whence == 1 ? f->off : (whence == 2 ? f->size : 0);
    int64_t pos = base + off;
    if (pos < 0) pos = 0;
    if (pos > f->size) pos = f->size;
    f->off = (uint32_t)pos;
    return pos;
}

static uint32_t h_IoLseek32(CpuState *s) {
    /* a0=fd, a1=offset, a2=whence. Returns new position (32-bit). */
    uint32_t fd = A0; int32_t off = (int32_t)A1; uint32_t whence = A2;
    if (fd >= 64 || !s_fds[fd].used) return 0x80010009;
    Fd *f = &s_fds[fd];
    if (io_async_busy(f)) return SCE_KERNEL_ERROR_ASYNC_BUSY;
    return (uint32_t)io_seek(f, off, whence);
}
static uint32_t h_IoLseek(CpuState *s) {
    /* a0=fd, [a2:a3]=64-bit offset, [sp+16]=whence. Returns 64-bit pos in v0:v1. */
    uint32_t fd = A0;
    int64_t off = (int64_t)(((uint64_t)A3 << 32) | A2);
    uint32_t whence = stack_arg(s, 0);
    if (fd >= 64 || !s_fds[fd].used) { s->r[3] = 0xFFFFFFFF; return 0x80010009; }
    Fd *f = &s_fds[fd];
    if (io_async_busy(f)) { s->r[3] = 0xFFFFFFFF; return SCE_KERNEL_ERROR_ASYNC_BUSY; }
    int64_t pos = io_seek(f, off, whence);
    s->r[3] = (uint32_t)((uint64_t)pos >> 32);
    return (uint32_t)pos;
}
static uint32_t h_IoClose(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= 64 || !s_fds[fd].used) return SCE_KERNEL_ERROR_BADF;
    io_release_fd(fd);
    return 0;
}

static void io_async_execute(uint32_t fd, uint32_t *delay) {
    Fd *f = &s_fds[fd];
    *delay = f->async_delay;
    switch (f->async_op) {
        case IO_ASYNC_OPEN:
            break;
        case IO_ASYNC_READ: {
            uint32_t result = io_read(fd, f->async_addr, f->async_size);
            f->async_res = (int64_t)(int32_t)result;
            *delay = f->async_size / 100u;
            if (*delay < 100u) *delay = 100u;
            if (getenv("SR_IOLOG"))
                fprintf(stderr, "ReadAsync fd=%u dst=0x%x size=%u -> %d\n",
                        fd, f->async_addr, f->async_size, (int32_t)result);
            break;
        }
        case IO_ASYNC_SEEK:
            f->async_res = io_seek(f, f->async_offset, f->async_whence);
            *delay = 100;
            break;
        case IO_ASYNC_CLOSE:
            f->async_res = 0;
            *delay = 0;
            break;
        default:
            f->async_res = -1;
            *delay = 0;
            break;
    }
    f->async_op = IO_ASYNC_NONE;
}

static void io_async_worker(uint32_t fd) {
    for (;;) {
        if (fd >= 64 || !s_fds[fd].used) {
            sched_exit_delete_current(0);
            return;
        }
        uint32_t delay;
        io_async_execute(fd, &delay);
        sched_delay_current(delay);
        if (!s_fds[fd].used) {
            sched_exit_delete_current(0);
            return;
        }
        s_fds[fd].async_pending = 0;
        s_fds[fd].async_ready = 1;
        sched_wake(IO_ASYNC_RESULT_OBJ(fd));
        sched_block_on(IO_ASYNC_START_OBJ(fd));
    }
}

static uint32_t io_async_start(uint32_t fd) {
    Fd *f = &s_fds[fd];
    f->async_pending = 1;
    f->async_ready = 0;
    if (!sr_sched_on) {
        uint32_t delay;
        io_async_execute(fd, &delay);
        f->async_pending = 0;
        f->async_ready = 1;
        return 0;
    }

    int priority = f->async_priority == -1 ? sched_current_priority() : f->async_priority;
    if (!f->async_thread) {
        uint32_t uid = sched_create_hle_thread("SceIoAsync", priority, 0x200, io_async_worker, fd);
        if ((int32_t)uid < 0) {
            f->async_pending = 0;
            f->async_op = IO_ASYNC_NONE;
            return uid;
        }
        f->async_thread = uid;
    } else {
        sched_set_priority(f->async_thread, priority);
        sched_wake(IO_ASYNC_START_OBJ(fd));
    }
    sched_preempt();
    return 0;
}

static uint32_t h_IoOpenAsync(CpuState *s) {
    char path[256];
    guest_cstr(A0, path, sizeof(path));
    uint32_t result = h_IoOpen(s);
    uint32_t fd = result;
    if ((int32_t)result < 0) {
        int slot = io_find_slot();
        if (slot < 0) return result;
        fd = (uint32_t)slot;
        memset(&s_fds[fd], 0, sizeof(s_fds[fd]));
        s_fds[fd].used = 1;
        s_fds[fd].close_pending = 1;
        s_fds[fd].async_priority = s_async_default_priority;
        s_fds[fd].async_res = (int64_t)(int32_t)result;
    } else {
        s_fds[fd].async_res = (int64_t)(int32_t)fd;
    }
    Fd *f = &s_fds[fd];
    f->async_op = IO_ASYNC_OPEN;
    f->async_delay = strstr(path, "sce_lbn") ? 80u :
                     (is_disc_path(path) ? ((int32_t)result < 0 ? 6000u : 4000u) : 10000u);
    uint32_t start = io_async_start(fd);
    if ((int32_t)start < 0) {
        io_release_fd(fd);
        return start;
    }
    return fd;
}
static uint32_t h_IoReadAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= 64 || !s_fds[fd].used) return SCE_KERNEL_ERROR_BADF;
    Fd *f = &s_fds[fd];
    if (io_async_busy(f)) return SCE_KERNEL_ERROR_ASYNC_BUSY;
    f->async_op = IO_ASYNC_READ;
    f->async_addr = A1;
    f->async_size = A2;
    return io_async_start(fd);
}
/* sceIoLseekAsync(fd, s64 offset, whence): the same arguments as sceIoLseek (offset in a2:a3,
 * whence in t0), not sceIoLseek32's. Using the 32-bit decoder read the offset from a1, so every
 * seek went to 0 and KZL re-read the start of PSP.DAT for each 64 KB block. */
static uint32_t h_IoLseekAsync(CpuState *s) {
    uint32_t fd = A0;
    int64_t offset = (int64_t)(((uint64_t)A3 << 32) | A2);
    uint32_t whence = stack_arg(s, 0);
    if (fd >= 64 || !s_fds[fd].used) return SCE_KERNEL_ERROR_BADF;
    if (whence > 2) return 0x80010016u;
    Fd *f = &s_fds[fd];
    if (io_async_busy(f)) return SCE_KERNEL_ERROR_ASYNC_BUSY;
    f->async_op = IO_ASYNC_SEEK;
    f->async_offset = offset;
    f->async_whence = whence;
    return io_async_start(fd);
}

static uint32_t io_async_result(CpuState *s, int poll) {
    uint32_t fd = A0, resp = A1;
    if (fd >= 64 || !s_fds[fd].used) return SCE_KERNEL_ERROR_BADF;
    Fd *f = &s_fds[fd];
    if (f->async_pending) {
        if (poll) return 1;
        do {
            sched_block_on(IO_ASYNC_RESULT_OBJ(fd));
        } while (f->used && f->async_pending);
        if (!f->used) return SCE_KERNEL_ERROR_BADF;
    }
    if (!f->async_ready) return SCE_KERNEL_ERROR_NOASYNC;
    int64_t result = f->async_res;
    if (resp && sr_inrange(resp) && sr_inrange(resp + 7u)) {
        MEM_W32(resp, (uint32_t)result);
        MEM_W32(resp + 4, (uint32_t)((uint64_t)result >> 32));
    }
    int close_pending = f->close_pending;
    f->async_ready = 0;
    if (close_pending) io_release_fd(fd);
    return 0;
}

static uint32_t h_IoWaitAsync(CpuState *s) {
    if (sched_in_interrupt()) return 0x80020064u;
    return io_async_result(s, 0);
}

static uint32_t h_IoPollAsync(CpuState *s) {
    return io_async_result(s, 1);
}

static uint32_t h_IoCloseAsync(CpuState *s) {
    uint32_t fd = A0;
    if (fd >= 64 || !s_fds[fd].used) return SCE_KERNEL_ERROR_BADF;
    Fd *f = &s_fds[fd];
    if (io_async_busy(f)) return SCE_KERNEL_ERROR_ASYNC_BUSY;
    f->close_pending = 1;
    f->async_op = IO_ASYNC_CLOSE;
    return io_async_start(fd);
}
static uint32_t h_IoGetstat(CpuState *s) {
    /* a0=path, a1=SceIoStat*. SceIoStat layout: mode(+0), attr(+4), size(+8,64-bit),
     * ctime(+0x10), atime(+0x20), mtime(+0x30), st_private[6](+0x40). For UMD files PPSSPP
     * fills st_private[0] (+0x40) with the file's starting LBN; the game reads that to build
     * a raw "sce_lbn0x<LBN>" path. Omitting it made the game read garbage and fetch the wrong
     * sector (e.g. REGFILE.CDI at LBN 0x5f20 was read as 0x80). */
    char path[256];
    guest_cstr(A0, path, sizeof(path));
    uint32_t st = A1;
    if (!sr_inrange(st) || !sr_inrange(st + 0x57u)) return 0x800200d3u;

    uint32_t lba = 0, size = 0;
    int is_dir = 0, host = 0;
    time_t ctime_value = 0, atime_value = 0, mtime_value = 0;
    if ((is_disc_path(path) || strchr(path, ':') == NULL) &&
        iso_lookup_info(path, &lba, &size, &is_dir) == 0) {
        if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> lba=0x%x size=0x%x\n", path, lba, size);
    } else {
        char hp[512];
        struct _stat64 info;
        if (host_existing_path(path, hp, sizeof(hp), &info) != 0) {
            if (getenv("SR_STATLOG")) fprintf(stderr, "Getstat(%s) -> NOT FOUND\n", path);
            return 0x80010002u;
        }
        host = 1;
        is_dir = (info.st_mode & _S_IFDIR) != 0;
        size = (uint32_t)info.st_size;
        ctime_value = info.st_ctime;
        atime_value = info.st_atime;
        mtime_value = info.st_mtime;
    }

    for (int i = 0; i < 0x58; i++) MEM_W8(st + (uint32_t)i, 0);
    MEM_W32(st + 0, (is_dir ? 0x1000u : 0x2000u) | (host ? 0777u : 0555u));
    MEM_W32(st + 4, is_dir ? 0x10u : 0x20u);
    MEM_W32(st + 8, size);
    MEM_W32(st + 12, 0);
    time_t times[3] = { ctime_value, atime_value, mtime_value };
    for (int i = 0; i < 3; i++) {
        struct tm fallback = {0};
        fallback.tm_year = 104; fallback.tm_mon = 0; fallback.tm_mday = 1;
        struct tm *tmv = times[i] ? localtime(&times[i]) : &fallback;
        uint32_t p = st + 0x10u + (uint32_t)i * 0x10u;
        MEM_W16(p + 0, (uint16_t)(tmv->tm_year + 1900));
        MEM_W16(p + 2, (uint16_t)(tmv->tm_mon + 1));
        MEM_W16(p + 4, (uint16_t)tmv->tm_mday);
        MEM_W16(p + 6, (uint16_t)tmv->tm_hour);
        MEM_W16(p + 8, (uint16_t)tmv->tm_min);
        MEM_W16(p + 10, (uint16_t)tmv->tm_sec);
        MEM_W32(p + 12, 0);
    }
    MEM_W32(st + 0x40, lba);
    return 0;
}

typedef struct DirEntry {
    char name[256];
    uint32_t lba, size;
    int is_dir, host;
    time_t ctime_value, atime_value, mtime_value;
} DirEntry;
typedef struct DirHandle {
    int used;
    uint32_t uid;
    DirEntry *entries;
    int count, index;
} DirHandle;
#define DIR_HANDLE_CAP 64
static DirHandle s_dirs[DIR_HANDLE_CAP];

static int dir_add(DirHandle *dir, const char *name, uint32_t lba, uint32_t size, int is_dir,
                   int host, time_t ctime_value, time_t atime_value, time_t mtime_value) {
    DirEntry *entries = (DirEntry *)realloc(dir->entries, (size_t)(dir->count + 1) * sizeof(*entries));
    if (!entries) return -1;
    dir->entries = entries;
    DirEntry *entry = &dir->entries[dir->count++];
    memset(entry, 0, sizeof(*entry));
    snprintf(entry->name, sizeof(entry->name), "%s", name);
    entry->lba = lba;
    entry->size = size;
    entry->is_dir = is_dir;
    entry->host = host;
    entry->ctime_value = ctime_value;
    entry->atime_value = atime_value;
    entry->mtime_value = mtime_value;
    return 0;
}

static int load_iso_dir(const char *path, DirHandle *dir) {
    int count = 0;
    if (iso_list(path, NULL, 0, &count) != 0) return -1;
    IsoDirEntry *entries = count ? (IsoDirEntry *)malloc((size_t)count * sizeof(*entries)) : NULL;
    if (count && !entries) return -1;
    if (iso_list(path, entries, count, &count) != 0) { free(entries); return -1; }
    for (int i = 0; i < count; i++)
        if (dir_add(dir, entries[i].name, entries[i].lba, entries[i].size, entries[i].is_dir,
                    0, 0, 0, 0) != 0) {
            free(entries);
            return -1;
        }
    free(entries);
    return 0;
}

static int dir_has_name(const DirHandle *dir, const char *name) {
    for (int i = 0; i < dir->count; i++)
        if (_stricmp(dir->entries[i].name, name) == 0) return 1;
    return 0;
}

static int load_legacy_host_dir(const char *path, DirHandle *dir) {
    char mapped[512], pattern[512], root[512];
    if (legacy_host_path(path, mapped, sizeof(mapped)) != 0) return -1;
    if (host_root(root, sizeof(root)) != 0) return -1;
    char *base = strrchr(mapped, '\\');
    base = base ? base + 1 : mapped;
    char prefix[300];
    int prefix_len = snprintf(prefix, sizeof(prefix), "%s_", base);
    if (prefix_len <= 1 || prefix_len >= (int)sizeof(prefix)) return -1;
    if (snprintf(pattern, sizeof(pattern), "%s\\*", root) >= (int)sizeof(pattern)) return -1;

    struct _finddata_t data;
    intptr_t find = _findfirst(pattern, &data);
    if (find == -1) return -1;
    int found = 0;
    do {
        if (_strnicmp(data.name, prefix, (size_t)prefix_len) != 0) continue;
        const char *rest = data.name + prefix_len;
        if (!rest[0]) continue;
        const char *sep = strchr(rest, '_');
        int len = sep ? (int)(sep - rest) : (int)strlen(rest);
        if (len <= 0 || len > 255) continue;
        char name[256];
        memcpy(name, rest, (size_t)len);
        name[len] = 0;
        if (dir_has_name(dir, name)) continue;
        if (dir_add(dir, name, 0, sep ? 0u : (uint32_t)data.size, sep != NULL, 1,
                    data.time_create, data.time_access, data.time_write) != 0) {
            _findclose(find);
            return -1;
        }
        found = 1;
    } while (_findnext(find, &data) == 0);
    _findclose(find);
    return found ? 0 : -1;
}

static int load_host_dir(const char *path, DirHandle *dir) {
    char hp[512], pattern[520];
    if (host_path(path, hp, sizeof(hp)) != 0) return -1;
    struct _stat64 st;
    if (_stat64(hp, &st) != 0) return load_legacy_host_dir(path, dir);
    if (!(st.st_mode & _S_IFDIR)) return -1;
    if (snprintf(pattern, sizeof(pattern), "%s\\*", hp) >= (int)sizeof(pattern)) return -1;

    struct _finddata_t data;
    intptr_t find = _findfirst(pattern, &data);
    if (find == -1) return errno == ENOENT ? 0 : -1;
    do {
        if (!strcmp(data.name, ".") || !strcmp(data.name, "..")) continue;
        if (dir_add(dir, data.name, 0, (uint32_t)data.size, (data.attrib & _A_SUBDIR) != 0, 1,
                    data.time_create, data.time_access, data.time_write) != 0) {
            _findclose(find);
            return -1;
        }
    } while (_findnext(find, &data) == 0);
    _findclose(find);
    return 0;
}

static DirHandle *dir_find(uint32_t uid) {
    for (int i = 0; i < DIR_HANDLE_CAP; i++)
        if (s_dirs[i].used && s_dirs[i].uid == uid) return &s_dirs[i];
    return NULL;
}

static void write_dir_stat(uint32_t out, const DirEntry *entry) {
    for (uint32_t i = 0; i < 0x58; i++) MEM_W8(out + i, 0);
    MEM_W32(out + 0, (entry->is_dir ? 0x1000u : 0x2000u) | (entry->host ? 0777u : 0555u));
    MEM_W32(out + 4, entry->is_dir ? 0x10u : 0x20u);
    MEM_W32(out + 8, entry->size);
    MEM_W32(out + 12, 0);
    time_t times[3] = { entry->ctime_value, entry->atime_value, entry->mtime_value };
    for (int i = 0; i < 3; i++) {
        struct tm fallback = {0};
        fallback.tm_year = 104; fallback.tm_mon = 0; fallback.tm_mday = 1;
        struct tm *tmv = times[i] ? localtime(&times[i]) : &fallback;
        uint32_t p = out + 0x10u + (uint32_t)i * 0x10u;
        MEM_W16(p + 0, (uint16_t)(tmv->tm_year + 1900));
        MEM_W16(p + 2, (uint16_t)(tmv->tm_mon + 1));
        MEM_W16(p + 4, (uint16_t)tmv->tm_mday);
        MEM_W16(p + 6, (uint16_t)tmv->tm_hour);
        MEM_W16(p + 8, (uint16_t)tmv->tm_min);
        MEM_W16(p + 10, (uint16_t)tmv->tm_sec);
        MEM_W32(p + 12, 0);
    }
    MEM_W32(out + 0x40, entry->lba);
}

static uint32_t h_IoDopen(CpuState *s) {
    char path[256];
    guest_cstr(A0, path, sizeof(path));
    DirHandle loaded = {0};
    int disc = is_disc_path(path);
    int result = (disc || strchr(path, ':') == NULL) ? load_iso_dir(path, &loaded) : -1;
    if (result != 0 && !disc) result = load_host_dir(path, &loaded);
    if (result != 0) { free(loaded.entries); return 0x80010002u; }

    DirHandle *slot = NULL;
    for (int i = 0; i < DIR_HANDLE_CAP; i++)
        if (!s_dirs[i].used) { slot = &s_dirs[i]; break; }
    if (!slot) { free(loaded.entries); return 0x80010018u; }
    *slot = loaded;
    slot->used = 1;
    slot->uid = sr_alloc_uid();
    return slot->uid;
}

static uint32_t h_IoDread(CpuState *s) {
    DirHandle *dir = dir_find(A0);
    if (!dir) return 0x80010009u;
    uint32_t out = A1;
    if (!sr_inrange(out) || !sr_inrange(out + 0x15bu)) return 0x800200d3u;
    if (dir->index >= dir->count) {
        MEM_W8(out + 0x58, 0);
        return 0;
    }
    DirEntry *entry = &dir->entries[dir->index++];
    for (uint32_t i = 0; i < 0x15c; i++) MEM_W8(out + i, 0);
    write_dir_stat(out, entry);
    for (uint32_t i = 0; i < 255 && entry->name[i]; i++) MEM_W8(out + 0x58u + i, entry->name[i]);
    return 1;
}

static uint32_t h_IoDclose(CpuState *s) {
    DirHandle *dir = dir_find(A0);
    if (!dir) return 0x80010009u;
    free(dir->entries);
    memset(dir, 0, sizeof(*dir));
    return 0;
}

static uint32_t h_IoRemove(CpuState *s) {
    char path[256], hp[512];
    guest_cstr(A0, path, sizeof(path));
    if (is_disc_path(path)) return 0x8001001eu;
    struct _stat64 st;
    if (host_existing_path(path, hp, sizeof(hp), &st) != 0 || (st.st_mode & _S_IFDIR))
        return 0x80010002u;
    return _unlink(hp) == 0 ? 0 : 0x80010002u;
}

static uint32_t h_IoRmdir(CpuState *s) {
    char path[256], hp[512];
    guest_cstr(A0, path, sizeof(path));
    if (is_disc_path(path)) return 0x8001001eu;
    if (host_path(path, hp, sizeof(hp)) != 0) return 0x80010016u;
    if (_rmdir(hp) == 0) return 0;
    return errno == ENOTEMPTY || errno == EEXIST ? 0x8001005au : 0x80010002u;
}

/* sceIoDevctl, after PPSSPP sceIo.cpp: the UMD commands, and the memory stick ones for
 * mscmhc0:/ms0:/memstick: and fatms0:. The memory stick is always inserted and its FAT assigned,
 * and a registered insert/eject callback is notified with that state at once, as PPSSPP does.
 * a0=device, a1=cmd, a2=arg, a3=arglen, t0=out, t1=outlen. */
#define MS_FAKE_FREE_SPACE ((1024u + 512u) * 1024u * 1024u)
static uint32_t s_ms_callbacks[32], s_ms_fat_callbacks[32];
static int s_ms_ncb = 0, s_ms_nfatcb = 0;
static int cb_list_remove(uint32_t *list, int *n, uint32_t id) {
    for (int i = 0; i < *n; i++)
        if (list[i] == id) { memmove(&list[i], &list[i + 1], (size_t)(*n - i - 1) * sizeof(uint32_t)); (*n)--; return 1; }
    return 0;
}
static void ms_write_device_size(uint32_t argAddr) {
    uint32_t p = MEM_R32(argAddr);
    if (!sr_inrange(p)) return;
    uint32_t sectorSize = 0x200, sectorCount = (32 * 1024) / sectorSize;
    uint32_t maxClusters = (uint32_t)(((uint64_t)MS_FAKE_FREE_SPACE * 95 / 100) / (sectorSize * sectorCount));
    MEM_W32(p + 0, maxClusters);
    MEM_W32(p + 4, maxClusters);
    MEM_W32(p + 8, maxClusters);
    MEM_W32(p + 12, sectorSize);
    MEM_W32(p + 16, sectorCount);
}
static uint32_t h_IoDevctl(CpuState *s) {
    char name[32];
    guest_cstr(A0, name, sizeof name);
    uint32_t cmd = A1, argAddr = A2, argLen = A3, outPtr = stack_arg(s, 0), outLen = stack_arg(s, 1);
    if (getenv("SR_IOLOG")) fprintf(stderr, "sceIoDevctl(\"%s\", %08x, %08x, %u, %08x, %u)\n", name, cmd, argAddr, argLen, outPtr, outLen);
    const uint32_t BAD_PARAMS = 0x80220081u, ERR_EINVAL = 0x80010016u;
    switch (cmd) {   /* UMD */
    case 0x01F20001: if (sr_inrange(outPtr) && outLen >= 8) { MEM_W32(outPtr + 4, 0x10); return 0; } return BAD_PARAMS;
    case 0x01F20002: if (sr_inrange(outPtr) && outLen >= 4) { MEM_W32(outPtr, 0x10); return 0; } return BAD_PARAMS;
    case 0x01F100A3: case 0x01F100A4: case 0x01F300A7: case 0x01F300A8: case 0x01F300A9:
        return (sr_inrange(argAddr) && argLen >= 4) ? 0 : BAD_PARAMS;
    case 0x01F300A5: if (sr_inrange(argAddr) && argLen >= 4) { MEM_W32(outPtr, 1); return 0; } return BAD_PARAMS;
    case 0x01F100A6: case 0x01F100A8: case 0x01F100A9: return 0;
    }
    if (!strcmp(name, "mscmhc0:") || !strcmp(name, "ms0:") || !strcmp(name, "memstick:")) {
        switch (cmd) {
        case 0x02025801:   /* driver status: device inserted */
            if (sr_inrange(outPtr) && outLen >= 4) { MEM_W32(outPtr, 4); return 0; }
            return BAD_PARAMS;
        case 0x02015804:   /* register insert/eject callback; notified at once with "inserted" */
            if (sr_inrange(argAddr) && outPtr == 0 && argLen >= 4) {
                uint32_t cb = MEM_R32(argAddr);
                if (s_ms_ncb < 32 && sr_cb_notify(cb, 1)) { s_ms_callbacks[s_ms_ncb++] = cb; return 0; }
                return ERR_EINVAL;
            }
            return BAD_PARAMS;
        case 0x02015805:
            if (sr_inrange(argAddr) && argLen >= 4)
                return cb_list_remove(s_ms_callbacks, &s_ms_ncb, MEM_R32(argAddr)) ? 0 : ERR_EINVAL;
            return BAD_PARAMS;
        case 0x02025806:   /* inserted */
            if (sr_inrange(outPtr) && outLen >= 4) { MEM_W32(outPtr, 1); return 0; }
            return BAD_PARAMS;
        case 0x02425818:   /* capacity */
            if (sr_inrange(argAddr) && argLen >= 4) { ms_write_device_size(argAddr); return 0; }
            return BAD_PARAMS;
        case 0x02425824:   /* write protected: no */
            if (sr_inrange(outPtr) && outLen == 4) { MEM_W32(outPtr, 0); return 0; }
            return 0xFFFFFFFFu;
        }
    }
    if (!strcmp(name, "fatms0:")) {
        switch (cmd) {
        case 0x0240d81e: break;   /* invalidate the driver's file table cache: falls to UNSUP */
        case 0x02415821:   /* register insert/eject callback; notified at once with FAT assigned */
            if (sr_inrange(argAddr) && argLen >= 4) {
                uint32_t cb = MEM_R32(argAddr);
                if (s_ms_nfatcb < 32 && sr_cb_notify(cb, 1)) { s_ms_fat_callbacks[s_ms_nfatcb++] = cb; return 0; }
            }
            return ERR_EINVAL;
        case 0x02415822:
            if (sr_inrange(argAddr) && argLen >= 4)
                return cb_list_remove(s_ms_fat_callbacks, &s_ms_nfatcb, MEM_R32(argAddr)) ? 0 : ERR_EINVAL;
            break;
        case 0x02415823:   /* set FAT state */
            return (sr_inrange(argAddr) && argLen == 4) ? 0 : 0xFFFFFFFFu;
        case 0x02425823:   /* FAT enabled: 1 */
            if ((int32_t)(outPtr + outLen) < (int32_t)outPtr) return 0x800200d3u;
            if (!sr_inrange(outPtr)) return ERR_EINVAL;
            MEM_W32(outPtr, 1);
            return 0;
        case 0x02425824:
            if (sr_inrange(outPtr) && outLen == 4) { MEM_W32(outPtr, 0); return 0; }
            return 0xFFFFFFFFu;
        case 0x02425818:
            if (sr_inrange(argAddr) && argLen >= 4) { ms_write_device_size(argAddr); return 0; }
            return BAD_PARAMS;
        }
    }
    fprintf(stderr, "HLE: unimplemented sceIoDevctl(\"%s\", %08x, %08x, %u, %08x, %u)\n", name, cmd, argAddr, argLen, outPtr, outLen);
    return 0x80020325u;   /* SCE_KERNEL_ERROR_UNSUP */
}

/* sceIoChangeAsyncPriority(fd, priority): -1 means the caller's current priority. */
static uint32_t h_IoChangeAsyncPriority(CpuState *s) {
    int32_t fd = (int32_t)A0, pri = (int32_t)A1;
    if (pri != -1 && (pri < 0x08 || pri > 0x77)) return 0x80020193u;   /* ILLEGAL_PRIORITY */
    if (fd == -1) {
        s_async_default_priority = pri;
        return 0;
    }
    if (fd < 0 || fd >= 64 || !s_fds[fd].used) return SCE_KERNEL_ERROR_BADF;
    Fd *f = &s_fds[fd];
    f->async_priority = pri;
    if (f->async_thread)
        sched_set_priority(f->async_thread, pri == -1 ? sched_current_priority() : pri);
    return 0;
}

/* ---- audio / control / display / GE / SAS: functional stubs ----
 * These return success and neutral data so the boot reaches and runs its main loop without an
 * actual audio device or GPU. Real rendering (sceGe display lists) and audio are later work;
 * here the calls must not block forever and must hand back valid-shaped results. */

/* sceAudio: a small channel table. The *Blocking output calls must block until the buffer is
 * consumed by the (virtual) audio hardware; that block is what lets the rest of the game run,
 * so a no-op return makes the audio thread monopolise the CPU. Here it yields the thread for
 * the buffer's duration and forwards samples to the shared XAudio2 mixer. */
static int s_audio_ch[9], s_audio_fmt[9];   /* fmt: 0=stereo, 1=mono; [8] = Output2 SRC channel */
static uint32_t s_audio_len[9];
static uint32_t h_AudioChReserve(CpuState *s) {
    int ch = (int)A0;
    if (ch < 0) { for (int i = 0; i < 8; i++) if (!s_audio_ch[i]) { ch = i; break; } }
    if (ch < 0 || ch >= 8) return 0xFFFFFFFF;
    s_audio_ch[ch] = 1; s_audio_len[ch] = A1; s_audio_fmt[ch] = (int)A2 & 1;
    return (uint32_t)ch;
}
static uint32_t h_AudioChRelease(CpuState *s) { if (A0 < 8) s_audio_ch[A0] = 0; return 0; }
static uint32_t h_AudioSetChannelDataLen(CpuState *s) {
    if (A0 < 8 && A1 > 0 && A1 <= 65536) s_audio_len[A0] = A1;
    return 0;
}
/* Read a guest sample buffer, expand mono to stereo, hand to the backend, then block until
 * the host queue is back down to ~one buffer of lead (real sceAudio blocking semantics).
 * Pacing against the queue self-corrects: a late thread returns immediately and catches up,
 * an early one sleeps the difference. The old open-loop sleep of the buffer's duration ran
 * slightly slower than the device every iteration (sleep + decode time), so streams drifted
 * behind (voice lagging subtitles) and underran (crackle). */

static uint32_t audio_output(CpuState *s, uint32_t ch, uint32_t buf, int voll, int volr) {
    uint32_t n = ch < 9 ? s_audio_len[ch] : 1024;
    if (buf && n > 0 && n <= 65536) {
        static int16_t lr[65536 * 2];
        int mono = ch < 9 ? s_audio_fmt[ch] : 0;
        for (uint32_t i = 0; i < n; i++) {
            if (mono) { int16_t v = (int16_t)MEM_R16(buf + i * 2); lr[i*2] = v; lr[i*2+1] = v; }
            else { lr[i*2] = (int16_t)MEM_R16(buf + i * 4); lr[i*2+1] = (int16_t)MEM_R16(buf + i * 4 + 2); }
        }
        sr_audio_push((int)ch, lr, (int)n, voll, volr);
    }
    int q = sr_audio_queued((int)ch);
    if (q < 0 || n == 0) {                 /* no host audio: open-loop pacing as before */
        sched_delay_current(n ? (n * 1000000u / 44100u) : 1000u);
        return n;
    }
    while ((q = sr_audio_queued((int)ch)) > (int)n)
        sched_delay_current((uint32_t)(q - (int)n) * 1000000u / 44100u);
    return n;
}
static uint32_t h_AudioOutputBlocking(CpuState *s) {
    /* sceAudioOutputBlocking(ch, vol, buf) */
    return audio_output(s, A0, A2, (int)(A1 & 0xFFFF), (int)(A1 & 0xFFFF));
}
static uint32_t h_AudioOutputPannedBlocking(CpuState *s) {
    /* sceAudioOutputPannedBlocking(ch, leftvol, rightvol, buf) */
    return audio_output(s, A0, A3, (int)(A1 & 0xFFFF), (int)(A2 & 0xFFFF));
}
static uint32_t h_AudioRestLen(CpuState *s) { (void)s; return 0; }         /* never backed up */

/* sceAudioOutput2*: the SRC channel (PPSSPP sceAudio.cpp), stereo at 44.1 kHz here. */
#define AUDIO_SRC_CH 8
#define ERR_AUDIO_CHANNEL_NOT_RESERVED     0x80260008u
#define ERR_AUDIO_CHANNEL_ALREADY_RESERVED 0x80268002u
#define ERR_AUDIO_INVALID_VOLUME           0x8026000Bu
static uint32_t h_AudioOutput2Reserve(CpuState *s) {
    uint32_t n = A0 & 0x7FFFFFFFu;
    if (n < 17 || n > 4111) return 0x80000104u;   /* INVALID_SIZE */
    if (s_audio_ch[AUDIO_SRC_CH]) return ERR_AUDIO_CHANNEL_ALREADY_RESERVED;
    s_audio_ch[AUDIO_SRC_CH] = 1; s_audio_len[AUDIO_SRC_CH] = n; s_audio_fmt[AUDIO_SRC_CH] = 0;
    return 0;
}
static uint32_t h_AudioOutput2OutputBlocking(CpuState *s) {
    if (A0 > 0xFFFFF) return ERR_AUDIO_INVALID_VOLUME;
    if (!s_audio_ch[AUDIO_SRC_CH]) return ERR_AUDIO_CHANNEL_NOT_RESERVED;
    return audio_output(s, AUDIO_SRC_CH, A1, (int)A0, (int)A0);
}
static uint32_t h_AudioOutput2ChangeLength(CpuState *s) {
    if (!s_audio_ch[AUDIO_SRC_CH]) return ERR_AUDIO_CHANNEL_NOT_RESERVED;
    s_audio_len[AUDIO_SRC_CH] = A0;
    return 0;
}
static uint32_t h_AudioOutput2Release(CpuState *s) {
    (void)s;
    if (!s_audio_ch[AUDIO_SRC_CH]) return ERR_AUDIO_CHANNEL_NOT_RESERVED;
    s_audio_ch[AUDIO_SRC_CH] = 0;
    return 0;
}

/* KZL uses sceAudioInput for the headset-microphone path. Host capture is deliberately not
 * opened: complete each request with mono silence and preserve its asynchronous pacing. */
#define AUDIO_INPUT_MAX_SAMPLES (65536u - 64u)
static uint32_t s_audio_input_samples;
static uint32_t s_audio_input_rate;
static int s_audio_input_pending;

static uint32_t h_AudioInputInitEx(CpuState *s) {
    (void)s;
    s_audio_input_samples = 0;
    s_audio_input_rate = 0;
    s_audio_input_pending = 0;
    return 0;
}

static uint32_t h_AudioInput(CpuState *s) {
    uint32_t samples = A0;
    uint32_t rate = A1;
    uint32_t buffer = A2;
    uint32_t bytes;

    if (samples > AUDIO_INPUT_MAX_SAMPLES ||
        (rate != 11025 && rate != 22050 && rate != 44100))
        return 0xFFFFFFFFu;
    if (!samples) {
        s_audio_input_samples = 0;
        s_audio_input_rate = rate;
        s_audio_input_pending = 0;
        return 0;
    }
    bytes = samples * 2u;
    if (!buffer || buffer > UINT32_MAX - (bytes - 1u) ||
        !sr_inrange(buffer) || !sr_inrange(buffer + bytes - 1u))
        return 0xFFFFFFFFu;

    for (uint32_t i = 0; i < samples; i++)
        MEM_W16(buffer + i * 2u, 0);
    s_audio_input_samples = samples;
    s_audio_input_rate = rate;
    s_audio_input_pending = 1;
    return samples;
}

static uint32_t h_AudioWaitInputEnd(CpuState *s) {
    (void)s;
    if (s_audio_input_pending && s_audio_input_rate) {
        uint64_t usec =
            (uint64_t)s_audio_input_samples * 1000000u / s_audio_input_rate;
        sched_delay_current((uint32_t)(usec ? usec : 1));
    }
    s_audio_input_pending = 0;
    return 0;
}

/* sceImpose language/button (PPSSPP sceImpose.cpp): English, cross = enter, matching
 * sceUtilityGetSystemParamInt. SetLanguageMode stores both. */
static uint32_t s_impose_lang = 1, s_impose_btn = 1;
static uint32_t h_ImposeGetLanguageMode(CpuState *s) {
    if (sr_inrange(A0)) MEM_W32(A0, s_impose_lang);
    if (sr_inrange(A1)) MEM_W32(A1, s_impose_btn);
    return 0;
}
static uint32_t h_ImposeSetLanguageMode(CpuState *s) { s_impose_lang = A0; s_impose_btn = A1; return 0; }

/* sceCtrlSetSamplingCycle: returns the previous cycle; 0 means sample at vblank. */
static uint32_t s_ctrl_cycle = 0;
static uint32_t h_CtrlSetSamplingCycle(CpuState *s) {
    uint32_t c = A0;
    if ((c > 0 && c < 5555) || c > 20000) return 0x800001FEu;   /* INVALID_VALUE */
    uint32_t prev = s_ctrl_cycle;
    s_ctrl_cycle = c;
    return prev;
}

/* sceUmd callback (PPSSPP sceUmd.cpp): Activate notifies it with PRESENT|READABLE|READY and
 * Deactivate with PRESENT|READY. */
static uint32_t s_umd_cb = 0;
static uint32_t h_UmdRegisterUMDCallBack(CpuState *s) { s_umd_cb = A0; return 0; }
static uint32_t h_UmdUnRegisterUMDCallBack(CpuState *s) {
    if (A0 != s_umd_cb) return 0x80010016u;   /* ERRNO_INVALID_ARGUMENT */
    s_umd_cb = 0;
    return g_sdk_version > 0x3000000 ? 0 : A0;
}
static uint32_t h_UmdActivate(CpuState *s) {
    (void)s;
    if (s_umd_cb) sr_cb_notify(s_umd_cb, 0x02 | 0x20 | (g_sdk_version ? 0x10 : 0));
    return 0;
}
static uint32_t h_UmdDeactivate(CpuState *s) {
    (void)s;
    if (s_umd_cb) sr_cb_notify(s_umd_cb, 0x02 | 0x10);
    return 0;
}

/* SR_CALLCOUNT instrumentation: per-NID call tallies, dumped at the capture point. */
static struct { uint32_t nid; const char *nm; unsigned long n; } g_cc[512];
static int g_ncc = 0, g_callcount = 0;
static void sr_dump_calls(void) {
    if (!g_callcount) return;
    for (int a = 0; a < g_ncc; a++) for (int b = a + 1; b < g_ncc; b++)
        if (g_cc[b].n > g_cc[a].n) { __typeof__(g_cc[0]) t = g_cc[a]; g_cc[a] = g_cc[b]; g_cc[b] = t; }
    fprintf(stderr, "--- top HLE calls ---\n");
    for (int a = 0; a < g_ncc && a < 18; a++) fprintf(stderr, "  %-32s 0x%08x  %lu\n", g_cc[a].nm, g_cc[a].nid, g_cc[a].n);
}
/* PSP controller ring buffer, modelled on PPSSPP (Core/HLE/sceCtrl.cpp): one sample is latched
 * per VBLANK into a 64-entry ring; sceCtrlReadBufferPositive returns the samples accumulated
 * since the last read (real per-frame history), not a flat copy of the current state. Games do
 * edge detection across these samples, so a flat fill makes a button look permanently held and a
 * press is never seen -- which is why the title/attract loop never advanced on START. */
#define CTRL_RING 64
typedef struct { uint32_t btn; uint8_t lx, ly; } CtrlSample;
static CtrlSample s_ctrl_ring[CTRL_RING] = { [0 ... CTRL_RING-1] = { 0, 128, 128 } };
static int s_ctrl_w = 1, s_ctrl_r = 0;   /* start with one sample available */
static uint32_t s_present_count = 0;
#define CTRL_WAIT_OBJ 0xC471D000u

/* sceCtrl: sticks centred. To drive past the skippable intro movie and confirmation prompts
 * without a human, pulse START/CROSS/CIRCLE for a few frames on a periodic cadence (edge presses,
 * so the game sees press+release). Disable with SR_NOINPUT for a truly neutral pad. */
static uint32_t h_CtrlButtons(void) {
    /* Live keyboard (windowed mode) is OR'd with the auto-input pulse below -- in this headless
     * window environment no key is ever pressed, so without the pulse the intro movie never gets
     * its START and loops forever. SR_NOINPUT disables the pulse for a neutral pad. */
    uint32_t keys = gui_on() ? gui_buttons() : 0;
    /* SR_PADSCRIPT=<file>: v3 uses PPSSPP vblanks and runtime presentations so turbo-mode
     * scheduler timing cannot make runtime inputs expire before a screen appears. Legacy
     * three-column scripts and v2 continue to use runtime vblanks. */
    {
        static int scr_n = -1;
        static int scr_present_timing;
        static struct { uint32_t f, mask; uint32_t w; } scr[256];
        if (scr_n < 0) {
            scr_n = 0;
            const char *sp = getenv("SR_PADSCRIPT");
            if (sp) {
                FILE *fp = fopen(sp, "r");
                if (fp) {
                    char line[160];
                    while (scr_n < 256 && fgets(line, sizeof(line), fp)) {
                        uint32_t pf, rf, mask, pw, rw;
                        char extra;
                        char *p = line;
                        while (*p == ' ' || *p == '\t') p++;
                        if (!*p || *p == '\r' || *p == '\n') continue;
                        if (*p == '#') {
                            if (strncmp(
                                    p, "# psp-recomp pad script v3",
                                    sizeof("# psp-recomp pad script v3") - 1) == 0)
                                scr_present_timing = 1;
                            continue;
                        }
                        if (sscanf(p, "%u %u %x %u %u %c",
                                   &pf, &rf, &mask, &pw, &rw, &extra) == 5) {
                            scr[scr_n].f = rf;
                            scr[scr_n].mask = mask;
                            scr[scr_n].w = rw;
                            scr_n++;
                        } else if (sscanf(p, "%u %x %u %c", &rf, &mask, &rw, &extra) == 3) {
                            scr[scr_n].f = rf;
                            scr[scr_n].mask = mask;
                            scr[scr_n].w = rw;
                            scr_n++;
                        } else {
                            fprintf(stderr, "SR_PADSCRIPT: malformed line: %s", line);
                            fclose(fp);
                            _Exit(2);
                        }
                    }
                    fclose(fp);
                } else {
                    fprintf(stderr, "SR_PADSCRIPT: cannot open %s\n", sp);
                    _Exit(2);
                }
            }
        }
        if (scr_n > 0) {
            uint32_t frame =
                scr_present_timing ? s_present_count : s_vcount_fwd;
            for (int i = 0; i < scr_n; i++)
                if (frame >= scr[i].f && frame < scr[i].f + scr[i].w)
                    keys |= scr[i].mask;
            return keys;
        }
    }
    /* The auto-START pulse below only exists to advance the intro/attract in headless or no-input
     * runs. When a real controller is connected the player drives input themselves, so suppress the
     * pulse (otherwise a phantom START every few seconds would keep opening the pause menu). */
    if (getenv("SR_NOINPUT") || gui_pad_present()) return keys;
    /* Pulse START only, briefly, on a slow cadence to skip the (minutes-long) intro movie and the
     * "press start" prompt. Pressing CROSS/CIRCLE as well drove the menus into bad states (it
     * confirmed things the game was not ready for); START alone advances the intro without that.
     * SR_PAD=<hex> overrides the mask; SR_PADPERIOD/SR_PADWIDTH tune the cadence. */
    const char *m = getenv("SR_PAD");
    uint32_t mask = m ? (uint32_t)strtoul(m, NULL, 16) : 0x0008u;   /* START */
    const char *pe = getenv("SR_PADPERIOD"); int period = pe ? atoi(pe) : 240;
    const char *pw = getenv("SR_PADWIDTH");  int width  = pw ? atoi(pw) : 4;
    const char *ps = getenv("SR_PADSTART");  int startf = ps ? atoi(ps) : 0;  /* hold input until frame */
    if (period <= 0) period = 240;
    if ((int)s_vcount_fwd < startf) return keys;
    if ((int)(s_vcount_fwd % (uint32_t)period) < width) return keys | mask;
    return keys;
}
/* sceCtrlReadBuffer*: fill SceCtrlData[count]. Positive reports pressed buttons as set bits;
 * Negative reports them inverted (set = not pressed), so it must write ~buttons -- writing the
 * positive mask there makes the game see almost every button held and run wild (it jumped through
 * an uninitialised menu handler). */
/* Latch one controller sample per frame into the ring (called from sr_vblank_tick). */
void sr_ctrl_sample(void) {
    uint8_t lx = 128, ly = 128;
    if (gui_on()) gui_analog(&lx, &ly);
    s_ctrl_ring[s_ctrl_w].btn = h_CtrlButtons();
    s_ctrl_ring[s_ctrl_w].lx = lx;
    s_ctrl_ring[s_ctrl_w].ly = ly;
    s_ctrl_w = (s_ctrl_w + 1) % CTRL_RING;
    if (s_ctrl_w == s_ctrl_r) s_ctrl_r = (s_ctrl_r + 1) % CTRL_RING;  /* drop oldest on overflow */
    sched_wake(CTRL_WAIT_OBJ);
}

/* Fill SceCtrlData[n] from the ring. Returns the number of new samples since the last read (the
 * PSP semantics), giving the game genuine per-frame history for edge/latch detection. Negative
 * reports inverted buttons. peek does not consume or block. */
static uint32_t ctrl_fill_n(uint32_t buf, uint32_t nbufs, int negate, int peek) {
    if (nbufs == 0) nbufs = 1;
    if (nbufs > CTRL_RING) nbufs = CTRL_RING;
    int avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
    /* Blocking ReadBuffer waits for at least one fresh sample (delivered each VBLANK). */
    if (!peek) {
        int guard = 0;
        while (avail == 0 && sr_sched_on && guard++ < 4) {
            sched_block_on(CTRL_WAIT_OBJ);
            avail = (s_ctrl_w - s_ctrl_r + CTRL_RING) % CTRL_RING;
        }
    }
    if (avail < 1) avail = 1;                      /* always give at least the latest */
    if (avail > (int)nbufs) avail = (int)nbufs;
    int start = peek ? (s_ctrl_w - avail + CTRL_RING) % CTRL_RING
                     : (s_ctrl_w - avail + CTRL_RING) % CTRL_RING;
    for (int i = 0; i < avail; i++) {
        CtrlSample smp = s_ctrl_ring[(start + i) % CTRL_RING];
        uint32_t field = negate ? ~smp.btn : smp.btn;   /* negative mode inverts buttons only */
        uint32_t e = buf + (uint32_t)i * 16;
        MEM_W32(e + 0, s_vcount_fwd);  /* timestamp (monotonic) */
        MEM_W32(e + 4, field);
        MEM_W8(e + 8, smp.lx); MEM_W8(e + 9, smp.ly); MEM_W8(e + 10, 128); MEM_W8(e + 11, 128);
    }
    if (!peek) s_ctrl_r = s_ctrl_w;                /* consume */
    if (getenv("SR_INLOG")) {
        static unsigned long calls = 0;
        CtrlSample latest = s_ctrl_ring[(s_ctrl_w - 1 + CTRL_RING) % CTRL_RING];
        if ((++calls % 200) == 0 || (latest.btn & 0x8))
            fprintf(stderr, "ctrl_fill #%lu vc=%u avail=%d latest=0x%x lx=%u ly=%u buf=0x%08x neg=%d peek=%d\n",
                    calls, s_vcount_fwd, avail, latest.btn, latest.lx, latest.ly, buf, negate, peek);
    }
    return (uint32_t)avail;
}
static uint32_t ctrl_fill(uint32_t buf, uint32_t count, int negate) {
    return ctrl_fill_n(buf, count, negate, 0);
}
static uint32_t h_CtrlReadBuffer(CpuState *s) { return ctrl_fill(A0, A1, 0); }
static uint32_t h_CtrlPeekBuffer(CpuState *s) { return ctrl_fill_n(A0, A1, 0, 1); }

/* sceDisplay: remember the framebuffer; vblank waits block until the next delivered vblank. */
static int dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride);
static uint32_t s_framebuf = 0, s_vcount = 0;
static uint32_t s_last_flip_vcount = 0;   /* hang watchdog (see sr_vblank_tick) */

#define FRAME_CAPTURE_COUNT 50
typedef struct {
    uint32_t present;
    char name[64];
} FrameCaptureRequest;

static FrameCaptureRequest s_frame_capture[FRAME_CAPTURE_COUNT];
static int s_frame_capture_initialized;
static int s_frame_capture_next;
static char s_frame_capture_dir[768];

static void frame_capture_fail(const char *message, const char *detail) {
    fprintf(stderr, "golden-frame capture: %s%s%s\n",
            message, detail ? ": " : "", detail ? detail : "");
    fflush(stderr);
    _Exit(2);
}

static void frame_capture_init(void) {
    const char *manifest = getenv("PSPRECOMP_CAPTURE_MANIFEST");
    const char *output_dir;
    FILE *f;
    char line[256];
    int count = 0, line_number = 0, seen_header = 0;
    uint32_t previous_ppsspp_present = 0;

    if (s_frame_capture_initialized) return;
    s_frame_capture_initialized = 1;
    if (!manifest || !manifest[0]) return;
    output_dir = getenv("PSPRECOMP_CAPTURE_DIR");
    if (!output_dir || !output_dir[0])
        frame_capture_fail("PSPRECOMP_CAPTURE_DIR is required", NULL);
    if (strlen(output_dir) >= sizeof(s_frame_capture_dir))
        frame_capture_fail("capture directory path is too long", output_dir);
    strcpy(s_frame_capture_dir, output_dir);

    f = fopen(manifest, "rb");
    if (!f) frame_capture_fail("cannot open manifest", manifest);
    while (fgets(line, sizeof(line), f)) {
        uint32_t ppsspp_present, present;
        char name[64], scene[32], extra;
        char *p = line;
        int fields;
        line_number++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) continue;
        if (*p == '#') {
            if (strcmp(p, "# psp-recomp golden frames v2\n") == 0 ||
                strcmp(p, "# psp-recomp golden frames v2\r\n") == 0)
                seen_header = 1;
            continue;
        }
        fields = sscanf(p, "%u %u %63s %31s %c",
                        &ppsspp_present, &present, name, scene, &extra);
        if (fields != 4) {
            char detail[64];
            snprintf(detail, sizeof(detail), "malformed line %d", line_number);
            fclose(f);
            frame_capture_fail("invalid manifest", detail);
        }
        if (count >= FRAME_CAPTURE_COUNT || !ppsspp_present || !present ||
            (count && ppsspp_present <= previous_ppsspp_present) ||
            (count && present <= s_frame_capture[count - 1].present) ||
            strchr(name, '/') || strchr(name, '\\') ||
            strlen(name) < 5 || strcmp(name + strlen(name) - 4, ".ppm") != 0) {
            char detail[64];
            snprintf(detail, sizeof(detail), "invalid request on line %d", line_number);
            fclose(f);
            frame_capture_fail("invalid manifest", detail);
        }
        for (int i = 0; i < count; i++) {
            if (strcmp(name, s_frame_capture[i].name) == 0) {
                char detail[64];
                snprintf(detail, sizeof(detail), "duplicate name on line %d", line_number);
                fclose(f);
                frame_capture_fail("invalid manifest", detail);
            }
        }
        s_frame_capture[count].present = present;
        strcpy(s_frame_capture[count].name, name);
        previous_ppsspp_present = ppsspp_present;
        count++;
    }
    if (ferror(f)) {
        fclose(f);
        frame_capture_fail("cannot read manifest", manifest);
    }
    fclose(f);
    if (!seen_header || count != FRAME_CAPTURE_COUNT)
        frame_capture_fail("manifest must contain its v2 header and exactly 50 frames", manifest);
    fprintf(stderr, "golden-frame capture: loaded %d requests from %s\n", count, manifest);
}

static int frame_capture_path(char *path, size_t path_size) {
    FrameCaptureRequest *request;
    int n;

    frame_capture_init();
    if (!s_frame_capture_dir[0] || s_frame_capture_next >= FRAME_CAPTURE_COUNT) return 0;
    request = &s_frame_capture[s_frame_capture_next];
    if (s_present_count < request->present) return 0;
    if (s_present_count != request->present)
        frame_capture_fail("runtime skipped a requested presented frame", request->name);
    n = snprintf(path, path_size, "%s\\%s", s_frame_capture_dir, request->name);
    if (n < 0 || (size_t)n >= path_size)
        frame_capture_fail("capture output path is too long", request->name);
    return 1;
}

static void frame_capture_complete(
    uint32_t fbaddr, int fmt, uint32_t stride,
    const char *path, int captured_by_d3d12) {
    char complete_path[1024];
    int n;
    FILE *done;

    if (!captured_by_d3d12 && dump_fb_fmt(path, fbaddr, fmt, stride) != 0)
        frame_capture_fail("cannot write frame", path);
    fprintf(stderr, "golden-frame capture: present %u vblank %u%s -> %s\n",
            s_present_count, s_vcount,
            captured_by_d3d12 ? " d3d12" : "", path);
    s_frame_capture_next++;
    if (s_frame_capture_next != FRAME_CAPTURE_COUNT) return;

    n = snprintf(complete_path, sizeof(complete_path),
                 "%s\\capture.complete", s_frame_capture_dir);
    if (n < 0 || (size_t)n >= sizeof(complete_path))
        frame_capture_fail("completion path is too long", s_frame_capture_dir);
    done = fopen(complete_path, "wb");
    if (!done || fprintf(done, "%d\n", FRAME_CAPTURE_COUNT) < 0 || fclose(done) != 0)
        frame_capture_fail("cannot write completion marker", complete_path);
    fprintf(stderr, "golden-frame capture: completed %d frames\n", FRAME_CAPTURE_COUNT);
    sr_audio_shutdown();
    sr_trace_close();
    fflush(stderr);
    _Exit(0);
}

static uint32_t h_DisplaySetFrameBuf(CpuState *s) {
    char capture_path[1024];
    int capture_requested = 0;
    int captured_by_d3d12 = 0;

    s_framebuf = A0;
    s_last_flip_vcount = s_vcount;
    if (s_framebuf) {
        s_present_count++;
        capture_requested = frame_capture_path(
            capture_path, sizeof(capture_path));
        if (capture_requested && gui_on())
            captured_by_d3d12 = gui_capture_next(capture_path);
    }
    /* SR_GEWATCH: interleave presents with GELIST lines to expose draw-vs-present ordering. */
    {
        static int gw = -1, gwa = 0;
        if (gw < 0) { gw = getenv("SR_GEWATCH") ? 1 : 0;
                      const char *p = getenv("SR_GEWATCH_AFTER"); gwa = p ? atoi(p) : 0; }
        if (gw && s_vcount >= (uint32_t)gwa)
            fprintf(stderr, "PRESENT f=%u buf=0x%08x fmt=%u stride=%u\n", s_vcount, A0, A2, A1);
    }
    /* Interactive window: show this finished frame and sample the keyboard (A1=bufferwidth,
     * A2=pixelformat 0=5650/1=5551/2=4444/3=8888). */
    if (gui_on() && s_framebuf) gui_present(s_framebuf, (int)A2, A1);
    if (capture_requested)
        frame_capture_complete(
            s_framebuf, (int)A2, A1 ? A1 : 512,
            capture_path, captured_by_d3d12);
    /* SR_FBSNAP=<N>: dump the presented framebuffer every N frames into a rotating set of
     * PPMs so an unattended run can be observed after the fact (8 most recent kept). */
    {
        static int fs = -2; static uint32_t fs_last = 0;
        if (fs == -2) { const char *e = getenv("SR_FBSNAP"); fs = e ? atoi(e) : 0; }
        if (fs > 0 && s_vcount - fs_last >= (uint32_t)fs) {
            fs_last = s_vcount;
            char p[64];
            snprintf(p, sizeof p, "snap_%u.ppm", (s_vcount / (uint32_t)fs) % 8u);
            dump_fb_fmt(p, A0, (int)A2, A1 ? A1 : 512);
            fprintf(stderr, "FBSNAP f=%u -> %s\n", s_vcount, p);
        }
    }
    /* The buffer handed to SetFrameBuf is a freshly-completed frame. With SR_FBDUMP=<N>, once N
     * frames have elapsed, snapshot exactly this presented buffer (the right instant -- before the
     * game recycles it as the next back buffer) and stop. */
    const char *fd = getenv("SR_FBDUMP");
    if (fd && s_framebuf && s_vcount >= (uint32_t)atoi(fd)) {
        extern unsigned long g_ge_pixels;
        dump_fb_fmt("fb_present.ppm", s_framebuf, (int)A2, A1 ? A1 : 512);
        /* Also snapshot the whole 2MB eDRAM so any rendered region can be found regardless of which
         * buffer/stride/format the game settled on. */
        FILE *raw = fopen("edram.bin", "wb");
        if (raw) { for (uint32_t a = 0x04000000; a < 0x04200000; a += 4) { uint32_t w = MEM_R32(a); fwrite(&w, 4, 1, raw); } fclose(raw); }
        sr_trace_close();
        extern unsigned long g_tex_samples, g_tex_nonzero;
        fprintf(stderr, "presented frame %u: buf=0x%08x fmt=%u stride=%u ge_pixels=%lu tex_samples=%lu tex_nonzero=%lu\n",
                s_vcount, s_framebuf, A2, A1 ? A1 : 512, g_ge_pixels, g_tex_samples, g_tex_nonzero);
        { extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
          fprintf(stderr, "mpeg: ringPut=%lu getAvcAu=%lu avcDecode=%lu noData=%lu\n",
                  g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata); }
        sr_dump_calls();
        extern void sched_dump_threads(void); sched_dump_threads();
        _Exit(0);
    }
    return 0;
}
static uint32_t h_DisplayGetFrameBuf(CpuState *s) {
    if (A0) MEM_W32(A0, s_framebuf);
    return 0;
}

/* Dump a PSP framebuffer to a binary PPM. fmt: 0=5650, 1=5551, 2=4444, 3=8888. */
static int dump_fb_fmt(const char *path, uint32_t fbaddr, int fmt, uint32_t stride) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (!stride) stride = 512;
    if (fprintf(f, "P6\n480 272\n255\n") < 0) {
        fclose(f);
        return -1;
    }
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            unsigned char rgb[3];
            if (fmt == 3) {
                uint32_t p = MEM_R32(fbaddr + (uint32_t)(y * (int)stride + x) * 4);
                rgb[0] = p & 0xFF; rgb[1] = (p >> 8) & 0xFF; rgb[2] = (p >> 16) & 0xFF;
            } else {
                uint16_t p = MEM_R16(fbaddr + (uint32_t)(y * (int)stride + x) * 2);
                if (fmt == 1) {
                    rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
                    rgb[1] = (unsigned char)(((p >> 5) & 0x1F) * 255 / 31);
                    rgb[2] = (unsigned char)(((p >> 10) & 0x1F) * 255 / 31);
                } else if (fmt == 2) {
                    rgb[0] = (unsigned char)(((p) & 0xF) * 17);
                    rgb[1] = (unsigned char)(((p >> 4) & 0xF) * 17);
                    rgb[2] = (unsigned char)(((p >> 8) & 0xF) * 17);
                } else {
                    rgb[0] = (unsigned char)(((p) & 0x1F) * 255 / 31);
                    rgb[1] = (unsigned char)(((p >> 5) & 0x3F) * 255 / 63);
                    rgb[2] = (unsigned char)(((p >> 11) & 0x1F) * 255 / 31);
                }
            }
            if (fwrite(rgb, 1, 3, f) != 3) {
                fclose(f);
                return -1;
            }
        }
    if (fclose(f) != 0) return -1;
    fprintf(stderr, "dumped framebuffer 0x%08x fmt=%d stride=%u -> %s\n", fbaddr, fmt, stride, path);
    return 0;
}
static void dump_fb(const char *path, uint32_t fbaddr) { dump_fb_fmt(path, fbaddr, 3, 512); }

/* Block until the scheduler delivers the next vblank. The old behaviour (delay one tick) let the
 * render loop wake while worker threads were still runnable and redraw the same frame dozens of
 * times per vblank -- the loading screen burned ~50s/60 frames in the rasterizer that way. */
static uint32_t h_DisplayWaitVblank(CpuState *s) { (void)s; sched_wait_vblank(); return 0; }

/* Called once per delivered VBLANK by the scheduler. Counts frames and, with SR_FBDUMP=<n>,
 * snapshots the framebuffer the game has drawn (the display buffer, else the GE target, else
 * the VRAM candidates) and stops, so we can see the rendered frame. */
void sr_ctrl_sample(void);
void ge_set_frame(uint32_t frame);
void sr_vblank_tick(void) {
    s_vcount++; s_vcount_fwd = s_vcount;
    ge_set_frame(s_vcount);
    sr_ctrl_sample();   /* latch one controller sample per frame (PPSSPP ring semantics) */
    /* Hang watchdog: vblanks keep being delivered even when every game thread is blocked,
     * so a stretch with no sceDisplaySetFrameBuf means the render loop is stuck (e.g. an
     * infinite wait). Dump the thread table + mpeg counters so the hang is diagnosable. */
    if (s_last_flip_vcount && s_vcount - s_last_flip_vcount > 0 &&
        (s_vcount - s_last_flip_vcount) % 600 == 0) {
        extern unsigned long g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata;
        fprintf(stderr, "WATCHDOG: no frame presented for %u vblanks (~%us); "
                "mpeg put=%lu getavc=%lu avcdec=%lu nodata=%lu\n",
                s_vcount - s_last_flip_vcount, (s_vcount - s_last_flip_vcount) / 60,
                g_mpeg_put, g_mpeg_getavc, g_mpeg_avcdec, g_mpeg_nodata);
        { extern void sched_dump_threads(void); sched_dump_threads(); }
        fflush(stderr);
    }
    /* Frame capture happens at sceDisplaySetFrameBuf (the instant a finished frame is presented),
     * which is the correct moment -- snapshotting here at an arbitrary vblank catches a buffer the
     * game has already begun clearing for the next frame. */
}
static uint32_t h_DisplayGetVcount(CpuState *s) { (void)s; return s_vcount; }
/* The PSP scans hCountPerVblank=286 lines per frame (PPSSPP Core/HW/Display.cpp). The current
 * scanline is the intra-frame position [0,285]; the accumulated count is vcount*286 + that, and
 * it is what games poll to time intervals far finer than a frame (e.g. logo/intro durations).
 * Returning the frame counter here (286x too small) makes those waits never elapse. */
static uint32_t s_hcount_vc = 0xffffffff, s_hcount_intra = 0;
static uint32_t hcount_intra(void) {
    if (s_vcount != s_hcount_vc) { s_hcount_vc = s_vcount; s_hcount_intra = 0; }
    else if (s_hcount_intra < 285) s_hcount_intra++;
    return s_hcount_intra;
}
static uint32_t h_DisplayGetCurrentHcount(CpuState *s) { (void)s; return hcount_intra(); }
static uint32_t h_DisplayGetAccumulatedHcount(CpuState *s) { (void)s; return s_vcount * 286u + hcount_intra(); }

/* ---- sceSasCore: VAG/noise voices, ADSR, effect sends and preset reverb ---- */
#define SAS_VOICES 32
#define SAS_GRAIN_MAX 1024
#define SAS_ENVELOPE_MAX 0x40000000LL

#define SCE_SAS_ERROR_INVALID_VOICE           0x80420010u
#define SCE_SAS_ERROR_INVALID_NOISE_FREQ      0x80420011u
#define SCE_SAS_ERROR_INVALID_PITCH           0x80420012u
#define SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE 0x80420013u
#define SCE_SAS_ERROR_VOICE_PAUSED            0x80420016u
#define SCE_SAS_ERROR_INVALID_VOLUME          0x80420018u
#define SCE_SAS_ERROR_INVALID_ADSR_RATE       0x80420019u
#define SCE_SAS_ERROR_REV_INVALID_TYPE        0x80420020u
#define SCE_SAS_ERROR_REV_INVALID_FEEDBACK    0x80420021u
#define SCE_SAS_ERROR_REV_INVALID_DELAY       0x80420022u
#define SCE_SAS_ERROR_REV_INVALID_VOLUME      0x80420023u

enum {
    SAS_VOICE_OFF,
    SAS_VOICE_VAG,
    SAS_VOICE_NOISE,
};

enum {
    SAS_ENV_KEYON_STEP = -42,
    SAS_ENV_KEYON = -2,
    SAS_ENV_OFF = -1,
    SAS_ENV_ATTACK,
    SAS_ENV_DECAY,
    SAS_ENV_SUSTAIN,
    SAS_ENV_RELEASE,
};

typedef struct {
    int attack_rate, decay_rate, sustain_rate, release_rate;
    int attack_type, decay_type, sustain_type, release_type;
    int sustain_level;
    int state;
    int64_t height;
} SasEnvelope;

typedef struct {
    int playing, keyed, paused, type;
    uint32_t vag, vag_size;       /* VAG stream base and byte size */
    uint32_t pos;                 /* byte offset of the next 16-byte block */
    int loop_start;               /* block offset to loop to (-1 = none) */
    int hist1, hist2;             /* ADPCM filter state */
    int pitch;                    /* 0x1000 = native 44.1 kHz */
    int voll, volr, effl, effr;   /* signed dry/effect volumes, -0x1000..0x1000 */
    int16_t buf[28]; int bufn, bufi;
    uint32_t frac;                /* 12-bit fixed-point resample remainder */
    int noise_freq, noise_count;
    uint16_t noise_lfsr;
    int16_t noise_sample;
    SasEnvelope envelope;
} SasVoice;

typedef struct {
    int type, delay, feedback;
    int voll, volr;
    int dry, wet;
} SasEffect;

static SasVoice s_sasv[SAS_VOICES];
static int s_sas_grain = 256;
static SasEffect s_sas_effect;
static SrSasReverb s_sas_reverb;
static int32_t s_sas_mixl[SAS_GRAIN_MAX], s_sas_mixr[SAS_GRAIN_MAX];
static int32_t s_sas_sendl[SAS_GRAIN_MAX], s_sas_sendr[SAS_GRAIN_MAX];
static int16_t s_sas_reverb_in[SAS_GRAIN_MAX];
static int16_t s_sas_reverb_out[SAS_GRAIN_MAX * 2];

static const int vag_f0[5] = { 0, 60, 115,  98, 122 };
static const int vag_f1[5] = { 0,  0, -52, -55, -60 };

static int sas_voice_valid(uint32_t voice) {
    return (int32_t)voice >= 0 && voice < SAS_VOICES;
}

static void sas_envelope_init(SasEnvelope *env) {
    memset(env, 0, sizeof(*env));
    env->attack_type = 0;
    env->decay_type = 1;
    env->sustain_type = 1;
    env->release_type = 1;
    env->state = SAS_ENV_OFF;
}

static void sas_envelope_set_state(SasEnvelope *env, int state) {
    if (env->height > SAS_ENVELOPE_MAX) env->height = SAS_ENVELOPE_MAX;
    env->state = state;
}

static void sas_envelope_walk(SasEnvelope *env, int type, int rate) {
    int64_t delta;
    switch (type) {
    case 0:
        env->height += rate;
        break;
    case 1:
        env->height -= rate;
        break;
    case 2:
        env->height += env->height <= SAS_ENVELOPE_MAX * 3 / 4 ? rate : rate / 4;
        break;
    case 3:
        delta = env->height - SAS_ENVELOPE_MAX;
        delta += (-delta * rate) >> 32;
        env->height = delta + SAS_ENVELOPE_MAX - ((uint32_t)rate + 3u) / 4u;
        break;
    case 4:
        delta = env->height - SAS_ENVELOPE_MAX;
        delta += (-delta * rate) >> 32;
        env->height = delta + 0x4000 + SAS_ENVELOPE_MAX;
        break;
    case 5:
        env->height = rate;
        break;
    }
}

static void sas_envelope_step(SasEnvelope *env) {
    switch (env->state) {
    case SAS_ENV_ATTACK:
        sas_envelope_walk(env, env->attack_type, env->attack_rate);
        if (env->height >= SAS_ENVELOPE_MAX || env->height < 0)
            sas_envelope_set_state(env, SAS_ENV_DECAY);
        break;
    case SAS_ENV_DECAY:
        sas_envelope_walk(env, env->decay_type, env->decay_rate);
        if (env->height < env->sustain_level)
            sas_envelope_set_state(env, SAS_ENV_SUSTAIN);
        break;
    case SAS_ENV_SUSTAIN:
        sas_envelope_walk(env, env->sustain_type, env->sustain_rate);
        if (env->height <= 0) {
            env->height = 0;
            sas_envelope_set_state(env, SAS_ENV_RELEASE);
        }
        break;
    case SAS_ENV_RELEASE:
        sas_envelope_walk(env, env->release_type, env->release_rate);
        if (env->height <= 0) {
            env->height = 0;
            sas_envelope_set_state(env, SAS_ENV_OFF);
        }
        break;
    case SAS_ENV_KEYON:
        env->height = 0;
        sas_envelope_set_state(env, SAS_ENV_KEYON_STEP);
        break;
    case SAS_ENV_KEYON_STEP:
        env->height++;
        if (env->height >= 31) {
            env->height = 0;
            sas_envelope_set_state(env, SAS_ENV_ATTACK);
        }
        break;
    default:
        break;
    }
}

static uint32_t sas_envelope_height(const SasEnvelope *env) {
    int64_t height = env->height;
    if (height > SAS_ENVELOPE_MAX) height = SAS_ENVELOPE_MAX;
    return (uint32_t)height;
}

static int sas_simple_rate(uint32_t value) {
    uint32_t n = value & 0x7Fu;
    if (n == 0x7F) return 0;
    uint32_t rate = ((7u - (n & 3u)) << 26) >> (n >> 2);
    return rate ? (int)rate : 1;
}

static int sas_exponent_rate(uint32_t value) {
    uint32_t n = value & 0x7Fu;
    if (n == 0x7F) return 0;
    uint32_t rate = ((7u - (n & 3u)) << 24) >> (n >> 2);
    return rate ? (int)rate : 1;
}

static void sas_envelope_set_simple(SasEnvelope *env, uint32_t env1, uint32_t env2) {
    uint32_t decay = (env1 >> 4) & 0xFu;
    uint32_t sustain_type = (env2 >> 14) & 3u;
    uint32_t release = env2 & 0x1Fu;

    env->attack_rate = sas_simple_rate(env1 >> 8);
    env->attack_type = env1 & 0x8000u ? 2 : 0;
    env->decay_rate = decay ? (int)(0x80000000u >> decay) : 0x7FFFFFFF;
    env->decay_type = 3;
    env->sustain_rate = sustain_type == 3
        ? sas_exponent_rate(env2 >> 6)
        : sas_simple_rate(env2 >> 6);
    env->sustain_type = (int)sustain_type;
    if (release == 31) {
        env->release_rate = 0;
    } else if (!(env2 & 0x20u)) {
        env->release_rate = release == 30 ? 0x40000000 :
            release == 29 ? 1 : (int)(0x10000000u >> release);
    } else {
        env->release_rate = release ? (int)(0x80000000u >> release) : 0x7FFFFFFF;
    }
    env->release_type = env2 & 0x20u ? 3 : 1;
    env->sustain_level = (int)(((env1 & 0xFu) + 1u) << 26);
}

/* Decode the next 16-byte VAG block into v->buf. Returns 0 when the stream ends. */
static int sas_vag_block(SasVoice *v) {
    if (v->pos + 16 > v->vag_size) return 0;
    uint32_t a = v->vag + v->pos;
    int hdr = MEM_R8(a), flags = MEM_R8(a + 1);
    if (flags == 7) return 0;                            /* end marker block */
    int pred = (hdr >> 4) & 0xF, shift = hdr & 0xF;
    if (pred > 4) pred = 0;
    if (flags == 6) v->loop_start = (int)v->pos;         /* loop start block */
    for (int i = 0; i < 28; i++) {
        int byte = MEM_R8(a + 2 + (i >> 1));
        int nib = (i & 1) ? (byte >> 4) : (byte & 0xF);
        int samp = (int)((int16_t)((uint16_t)nib << 12)) >> shift;
        samp += (v->hist1 * vag_f0[pred] + v->hist2 * vag_f1[pred]) >> 6;
        if (samp > 32767) samp = 32767;
        if (samp < -32768) samp = -32768;
        v->buf[i] = (int16_t)samp;
        v->hist2 = v->hist1; v->hist1 = samp;
    }
    v->pos += 16;
    if (flags == 3 && v->loop_start >= 0) v->pos = (uint32_t)v->loop_start;  /* loop end */
    v->bufn = 28; v->bufi = 0;
    return 1;
}

static int sas_noise_sample(SasVoice *v) {
    int period = 0x1000 >> (v->noise_freq >> 2);
    if (period < 1) period = 1;
    if (++v->noise_count >= period) {
        uint16_t feedback = (uint16_t)((v->noise_lfsr ^ (v->noise_lfsr >> 1)) & 1u);
        v->noise_lfsr = (uint16_t)((v->noise_lfsr >> 1) | (feedback << 14));
        v->noise_sample = (int16_t)(v->noise_lfsr << 1);
        v->noise_count = 0;
    }
    return v->noise_sample;
}

static int sas_voice_sample(SasVoice *v, int *ended) {
    if (v->type == SAS_VOICE_NOISE) return sas_noise_sample(v);
    if (v->type != SAS_VOICE_VAG) return 0;
    if (v->bufi >= v->bufn && !sas_vag_block(v)) {
        *ended = 1;
        return 0;
    }
    return v->buf[v->bufi];
}

/* Mix one grain into out (s16 interleaved stereo in guest memory). */
static void sas_mix(uint32_t out, int add, int input_voll, int input_volr) {
    if (!out) return;
    int n = s_sas_grain > SAS_GRAIN_MAX ? SAS_GRAIN_MAX : s_sas_grain;
    for (int i = 0; i < n; i++) {
        s_sas_mixl[i] = s_sas_mixr[i] = 0;
        s_sas_sendl[i] = s_sas_sendr[i] = 0;
    }
    for (int vi = 0; vi < SAS_VOICES; vi++) {
        SasVoice *v = &s_sasv[vi];
        if (!v->playing || v->paused) continue;
        for (int i = 0; i < n; i++) {
            if (v->envelope.state == SAS_ENV_KEYON ||
                v->envelope.state == SAS_ENV_KEYON_STEP) {
                sas_envelope_step(&v->envelope);
                continue;
            }

            int ended = 0;
            int sample = sas_voice_sample(v, &ended);
            if (ended) {
                v->playing = v->keyed = 0;
                v->envelope.height = 0;
                v->envelope.state = SAS_ENV_OFF;
                break;
            }
            int envelope = (int)((sas_envelope_height(&v->envelope) + (1u << 14)) >> 15);
            sas_envelope_step(&v->envelope);
            sample = (int)(((int64_t)sample * envelope + (1 << 14)) >> 15);
            s_sas_mixl[i] += sample * v->voll >> 12;
            s_sas_mixr[i] += sample * v->volr >> 12;
            s_sas_sendl[i] += sample * v->effl >> 12;
            s_sas_sendr[i] += sample * v->effr >> 12;

            v->frac += (uint32_t)(v->pitch > 0 ? v->pitch : 0x1000);
            while (v->frac >= 0x1000) { v->frac -= 0x1000; v->bufi++; }
            if (v->envelope.state == SAS_ENV_OFF) {
                v->playing = v->keyed = 0;
                break;
            }
        }
    }

    if (s_sas_effect.wet) {
        int frames = (n + 1) / 2;
        for (int i = 0; i < frames; i++) {
            int sample = i * 2 < n ? i * 2 : n - 1;
            s_sas_reverb_in[i * 2] = (int16_t)(
                s_sas_sendl[sample] < -32768 ? -32768 :
                s_sas_sendl[sample] > 32767 ? 32767 : s_sas_sendl[sample]);
            s_sas_reverb_in[i * 2 + 1] = (int16_t)(
                s_sas_sendr[sample] < -32768 ? -32768 :
                s_sas_sendr[sample] > 32767 ? 32767 : s_sas_sendr[sample]);
        }
        sr_sas_reverb_process(
            &s_sas_reverb, s_sas_reverb_out, s_sas_reverb_in, (size_t)frames,
            s_sas_effect.voll << 3, s_sas_effect.volr << 3);
    }

    for (int i = 0; i < n; i++) {
        int l = s_sas_effect.dry ? s_sas_mixl[i] : 0;
        int r = s_sas_effect.dry ? s_sas_mixr[i] : 0;
        if (s_sas_effect.wet) {
            l += s_sas_reverb_out[i * 2];
            r += s_sas_reverb_out[i * 2 + 1];
        }
        if (add) {
            l += (int16_t)MEM_R16(out + (uint32_t)i * 4) * input_voll >> 12;
            r += (int16_t)MEM_R16(out + (uint32_t)i * 4 + 2) * input_volr >> 12;
        }
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        MEM_W16(out + (uint32_t)i * 4,     (uint16_t)(int16_t)l);
        MEM_W16(out + (uint32_t)i * 4 + 2, (uint16_t)(int16_t)r);
    }
}

static uint32_t h_SasInit(CpuState *s) {
    /* __sceSasInit(core, grain, maxVoices, outMode, sampleRate) */
    s_sas_grain = (int)A1 > 0 && (int)A1 <= SAS_GRAIN_MAX ? (int)A1 : 256;
    memset(s_sasv, 0, sizeof(s_sasv));
    for (int i = 0; i < SAS_VOICES; i++) {
        SasVoice *v = &s_sasv[i];
        v->pitch = 0x1000;
        v->voll = v->volr = v->effl = v->effr = 0x1000;
        v->loop_start = -1;
        v->noise_lfsr = (uint16_t)(0x4001u + (uint16_t)i * 0x101u);
        sas_envelope_init(&v->envelope);
    }
    memset(&s_sas_effect, 0, sizeof(s_sas_effect));
    s_sas_effect.type = -1;
    s_sas_effect.dry = 1;
    sr_sas_reverb_init(&s_sas_reverb);
    return 0;
}
static uint32_t h_SasSetVoice(CpuState *s) {
    /* __sceSasSetVoice(core, voice, vagAddr, size, loopmode) */
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    SasVoice *v = &s_sasv[A1];
    v->type = SAS_VOICE_VAG;
    v->vag = A2; v->vag_size = A3;
    v->pos = 0; v->loop_start = stack_arg(s, 0) ? 0 : -1;
    v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    return 0;
}
static uint32_t h_SasSetPitch(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    if (A2 > 0x4000u) return SCE_SAS_ERROR_INVALID_PITCH;
    s_sasv[A1].pitch = (int)A2;
    return 0;
}
static uint32_t h_SasSetVolume(CpuState *s) {
    /* __sceSasSetVolume(core, voice, l, r, effectL, effectR); volumes 0..0x1000 */
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    int64_t voll = (int32_t)A2;
    int64_t volr = (int32_t)A3;
    int64_t effl = (int32_t)stack_arg(s, 0);
    int64_t effr = (int32_t)stack_arg(s, 1);
    if (voll < -0x1000 || voll > 0x1000 ||
        volr < -0x1000 || volr > 0x1000 ||
        effl < -0x1000 || effl > 0x1000 ||
        effr < -0x1000 || effr > 0x1000)
        return SCE_SAS_ERROR_INVALID_VOLUME;
    SasVoice *v = &s_sasv[A1];
    v->voll = (int)voll;
    v->volr = (int)volr;
    v->effl = (int)effl;
    v->effr = (int)effr;
    return 0;
}
static uint32_t h_SasGetEndFlag(CpuState *s) {
    (void)s;
    uint32_t m = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (!s_sasv[i].playing) m |= 1u << i;
    return m;
}
static uint32_t h_SasSetKeyOn(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    SasVoice *v = &s_sasv[A1];
    if (v->paused || v->keyed) return SCE_SAS_ERROR_VOICE_PAUSED;
    v->pos = 0; v->hist1 = v->hist2 = 0; v->bufn = v->bufi = 0; v->frac = 0;
    v->noise_count = 0;
    v->playing = v->keyed = 1;
    v->paused = 0;
    v->envelope.state = SAS_ENV_KEYON;
    return 0;
}
static uint32_t h_SasSetKeyOff(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    SasVoice *v = &s_sasv[A1];
    if (v->paused || !v->keyed) return SCE_SAS_ERROR_VOICE_PAUSED;
    v->keyed = 0;
    v->envelope.state = SAS_ENV_RELEASE;
    return 0;
}
static uint32_t h_SasGetEnvelopeHeight(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    return sas_envelope_height(&s_sasv[A1].envelope);
}
static uint32_t h_SasSetADSR(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    int flag = (int)A2;
    int attack = (int)A3;
    int decay = (int)stack_arg(s, 0);
    int sustain = (int)stack_arg(s, 1);
    int release = (int)stack_arg(s, 2);
    int invalid = (attack < 0 ? 1 : 0) | (decay < 0 ? 2 : 0) |
        (sustain < 0 ? 4 : 0) | (release < 0 ? 8 : 0);
    if (invalid & flag) return SCE_SAS_ERROR_INVALID_ADSR_RATE;
    SasEnvelope *env = &s_sasv[A1].envelope;
    if (flag & 1) env->attack_rate = attack;
    if (flag & 2) env->decay_rate = decay;
    if (flag & 4) env->sustain_rate = sustain;
    if (flag & 8) env->release_rate = release;
    return 0;
}
static uint32_t h_SasSetADSRMode(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    int flag = (int)A2;
    uint32_t attack = A3 & ~0x80000000u;
    uint32_t decay = stack_arg(s, 0) & ~0x80000000u;
    uint32_t sustain = stack_arg(s, 1) & ~0x80000000u;
    uint32_t release = stack_arg(s, 2) & ~0x80000000u;
    int invalid = 0;
    if (attack > 5 || (attack & 1)) invalid |= 1;
    if (decay > 5 || !(decay & 1)) invalid |= 2;
    if (sustain > 5) invalid |= 4;
    if (release > 5 || !(release & 1)) invalid |= 8;
    if (invalid & flag) return SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE;
    SasEnvelope *env = &s_sasv[A1].envelope;
    if (flag & 1) env->attack_type = (int)attack;
    if (flag & 2) env->decay_type = (int)decay;
    if (flag & 4) env->sustain_type = (int)sustain;
    if (flag & 8) env->release_type = (int)release;
    return 0;
}
static uint32_t h_SasSetSimpleADSR(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    if (A3 & 0x2000u) return SCE_SAS_ERROR_INVALID_ADSR_CURVE_MODE;
    sas_envelope_set_simple(&s_sasv[A1].envelope, A2 & 0xFFFFu, A3 & 0xFFFFu);
    return 0;
}
static uint32_t h_SasSetSL(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    s_sasv[A1].envelope.sustain_level = (int)A2;
    return 0;
}
static uint32_t h_SasSetNoise(CpuState *s) {
    if (!sas_voice_valid(A1)) return SCE_SAS_ERROR_INVALID_VOICE;
    if ((int32_t)A2 < 0 || A2 >= 64) return SCE_SAS_ERROR_INVALID_NOISE_FREQ;
    SasVoice *v = &s_sasv[A1];
    v->type = SAS_VOICE_NOISE;
    v->noise_freq = (int)A2;
    return 0;
}
static uint32_t h_SasGetPauseFlag(CpuState *s) {
    (void)s;
    uint32_t paused = 0;
    for (int i = 0; i < SAS_VOICES; i++) if (s_sasv[i].paused) paused |= 1u << i;
    return paused;
}
static uint32_t h_SasSetPause(CpuState *s) {
    uint32_t voices = A1;
    for (int i = 0; i < SAS_VOICES; i++)
        if (voices & (1u << i)) s_sasv[i].paused = A2 != 0;
    return 0;
}
static uint32_t h_SasRevType(CpuState *s) {
    int type = (int32_t)A1;
    if (type < -1 || type > 8) return SCE_SAS_ERROR_REV_INVALID_TYPE;
    if (type != s_sas_effect.type) {
        s_sas_effect.type = type;
        sr_sas_reverb_set_preset(&s_sas_reverb, type);
    }
    return 0;
}
static uint32_t h_SasRevParam(CpuState *s) {
    if ((int32_t)A1 < 0 || A1 >= 128) return SCE_SAS_ERROR_REV_INVALID_DELAY;
    if ((int32_t)A2 < 0 || A2 >= 128) return SCE_SAS_ERROR_REV_INVALID_FEEDBACK;
    s_sas_effect.delay = (int)A1;
    s_sas_effect.feedback = (int)A2;
    return 0;
}
static uint32_t h_SasRevEVOL(CpuState *s) {
    if (A1 > 0x1000u || A2 > 0x1000u) return SCE_SAS_ERROR_REV_INVALID_VOLUME;
    s_sas_effect.voll = (int)A1;
    s_sas_effect.volr = (int)A2;
    return 0;
}
static uint32_t h_SasRevVON(CpuState *s) {
    s_sas_effect.dry = A1 != 0;
    s_sas_effect.wet = A2 != 0;
    return 0;
}
static uint32_t h_SasCore(CpuState *s) {
    sas_mix(A1, 0, 0, 0);
    return 0;
}
static uint32_t h_SasCoreWithMix(CpuState *s) {
    sas_mix(A1, 1, (int32_t)A2, (int32_t)A3);
    return 0;
}

/* ---- semaphores and event flags, backed by the scheduler's block/wake-on-object ---- */

typedef struct { int used; uint32_t uid; int count, maxc; uint32_t pattern; } Sync;
static Sync s_sync[128];
static Sync *sync_find(uint32_t uid) {
    for (int i = 0; i < 128; i++) if (s_sync[i].used && s_sync[i].uid == uid) return &s_sync[i];
    return NULL;
}
static Sync *sync_new(uint32_t type) {
    for (int i = 0; i < 128; i++) if (!s_sync[i].used) {
        memset(&s_sync[i], 0, sizeof(s_sync[i]));
        s_sync[i].used = 1;
        s_sync[i].uid = sr_alloc_uid_type(type);
        return &s_sync[i];
    }
    return NULL;
}

static uint32_t h_CreateSema(CpuState *s) {
    /* a0=name, a1=attr, a2=initCount, a3=maxCount. */
    Sync *m = sync_new(SR_TMID_SEMAPHORE); if (!m) return 0x80020000;
    m->count = (int)A2; m->maxc = (int)A3;
    if (getenv("SR_SEMALOG")) fprintf(stderr, "CreateSema uid=0x%x init=%d max=%d\n", m->uid, (int)A2, (int)A3);
    return m->uid;
}
static uint32_t h_DeleteSema(CpuState *s) {
    Sync *m = sync_find(A0);
    if (m) { sr_unregister_uid(m->uid); m->used = 0; }
    return 0;
}
static uint32_t h_WaitSema(CpuState *s) {
    /* a0=semaid, a1=signal, a2=timeout ptr (0=infinite, else *a2 = microseconds). */
    uint32_t uid = A0; int need = (int)A1; uint32_t toptr = A2;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (getenv("SR_SEMALOG")) fprintf(stderr, "WaitSema uid=0x%x need=%d count=%d timeout_ptr=0x%x\n", uid, need, m ? m->count : -999, toptr);
    while (m->count < need) {
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (sched_block_on_timeout(uid, usec)) return 0x800201A8;  /* WAIT_TIMEOUT */
        } else {
            sched_block_on(uid);
        }
        m = sync_find(uid); if (!m) return 0x80020000;
    }
    m->count -= need;
    return 0;
}
static uint32_t h_SignalSema(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->count += (int)A1;
    sched_wake(A0);
    sched_preempt();    /* a woken higher-priority waiter runs immediately */
    return 0;
}
static uint32_t h_PollSema(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    int need = (int)A1;
    if (m->count < need) return 0x80020001;   /* would block */
    m->count -= need;
    return 0;
}

static uint32_t h_CreateEventFlag(CpuState *s) {
    /* a0=name, a1=attr, a2=initPattern, a3=opt. */
    Sync *m = sync_new(SR_TMID_EVENT_FLAG); if (!m) return 0x80020000;
    m->pattern = A2;
    return m->uid;
}
static uint32_t h_DeleteEventFlag(CpuState *s) {
    Sync *m = sync_find(A0);
    if (m) { sr_unregister_uid(m->uid); m->used = 0; }
    return 0;
}
static uint32_t h_SetEventFlag(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->pattern |= A1; sched_wake(A0); sched_preempt(); return 0;
}
static uint32_t h_ClearEventFlag(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    m->pattern &= A1; return 0;
}
static int evf_match(Sync *m, uint32_t bits, uint32_t mode) {
    return (mode & 1) ? ((m->pattern & bits) != 0) : ((m->pattern & bits) == bits);
}
static uint32_t h_WaitEventFlag(CpuState *s) {
    /* a0=uid, a1=bits, a2=mode, a3=outBits, [sp+16]=timeout. */
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3, toptr = stack_arg(s, 0);
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    while (!evf_match(m, bits, mode)) {
        if (toptr) {
            uint32_t usec = MEM_R32(toptr);
            if (sched_block_on_timeout(uid, usec)) { if (outp) MEM_W32(outp, m->pattern); return 0x800201A8; }
        } else {
            sched_block_on(uid);
        }
        m = sync_find(uid); if (!m) return 0x80020000;
    }
    if (outp) MEM_W32(outp, m->pattern);
    if (mode & 0x20) m->pattern &= ~bits;       /* PSP_EVENT_WAITCLEAR */
    else if (mode & 0x10) m->pattern = 0;        /* PSP_EVENT_WAITCLEARALL */
    return 0;
}
static uint32_t h_PollEventFlag(CpuState *s) {
    uint32_t uid = A0, bits = A1, mode = A2, outp = A3;
    Sync *m = sync_find(uid); if (!m) return 0x80020000;
    if (!evf_match(m, bits, mode)) { if (outp) MEM_W32(outp, m->pattern); return 0x80020021; }
    if (outp) MEM_W32(outp, m->pattern);
    return 0;
}
/* sceKernelReferEventFlagStatus(uid, SceKernelEventFlagInfo *info): size(0), name[32](4),
 * attr(36), initPattern(40), currentPattern(44), numWaitThreads(48). Size stays as the caller
 * wrote it; we don't track init pattern or waiters separately. */
static uint32_t h_ReferEventFlagStatus(CpuState *s) {
    Sync *m = sync_find(A0); if (!m) return 0x80020000;
    uint32_t info = A1; if (!info) return 0x80020000;
    for (int i = 0; i < 32; i++) MEM_W8(info + 4 + (uint32_t)i, 0);
    MEM_W32(info + 36, 0x200);          /* PSP_EVENT_WAITMULTIPLE */
    MEM_W32(info + 40, m->pattern);
    MEM_W32(info + 44, m->pattern);
    MEM_W32(info + 48, 0);
    return 0;
}

static uint32_t h_WaitSemaCB(CpuState *s) { return with_callbacks(s, h_WaitSema); }
static uint32_t h_WaitEventFlagCB(CpuState *s) { return with_callbacks(s, h_WaitEventFlag); }
static uint32_t h_IoWaitAsyncCB(CpuState *s) { return with_callbacks(s, h_IoWaitAsync); }

void sr_hle_init(void) {
    if (s_hle_n) return;
    g_callcount = getenv("SR_CALLCOUNT") ? 1 : 0;
    /* NID audit 2026-06: every entry below verified against PPSSPP's HLE tables. A handler on
     * the wrong NID is worse than none -- handlers that fill out-params write through whatever
     * the registers happen to hold for the REAL function's signature (the VolatileMemUnlock
     * mixup sprayed two wild words per asset load and zeroed the model-slot counter). */
    sr_hle_register(0x7591c7db, "sceKernelSetCompiledSdkVersion", h_SetCompiledSdkVersion);
    sr_hle_register(0xf77d77cb, "sceKernelSetCompilerVersion", h_SetCompilerVersion);
    sr_kmem_register();   /* partition memory, free-size queries, FPLs */
    sr_hle_register(0x446d8de6, "sceKernelCreateThread", h_CreateThread);
    sr_hle_register(0xf475845d, "sceKernelStartThread", h_StartThread);
    sr_hle_register(0xaa73c935, "sceKernelExitThread", h_ExitThread);
    sr_hle_register(0x809ce29b, "sceKernelExitDeleteThread", h_ExitDeleteThread);
    sr_cb_register();     /* callbacks, sceKernelCheckCallback, scePower callbacks */
    sr_hle_register(0xceadeb47, "sceKernelDelayThread", h_DelayThread);
    sr_hle_register(0x94aa61ee, "sceKernelGetThreadCurrentPriority", h_GetThreadPriority);
    sr_hle_register(0x9ace131e, "sceKernelSleepThread", h_SleepThread);
    sr_hle_register(0xd59ead2f, "sceKernelWakeupThread", h_WakeupThread);
    sr_hle_register(0xfccfad26, "sceKernelCancelWakeupThread", h_CancelWakeupThread);
    sr_hle_register(0x278c0df5, "sceKernelWaitThreadEnd", h_WaitThreadEnd);
    sr_hle_register(0x840e8133, "sceKernelWaitThreadEndCB", h_WaitThreadEndCB);
    sr_hle_register(0x71bc9871, "sceKernelChangeThreadPriority", h_ChangeThreadPriority);
    sr_hle_register(0x383f7bcc, "sceKernelTerminateDeleteThread", h_TerminateDeleteThread);
    sr_hle_register(0x9fa03cd3, "sceKernelDeleteThread", h_DeleteThread);
    sr_hle_register(0xa66b0120, "sceKernelReferEventFlagStatus", h_ReferEventFlagStatus);
    sr_hle_register(0xffc36a14, "sceKernelReferThreadRunStatus", h_ReferThreadRunStatus);
    sr_hle_register(0x17c1684e, "sceKernelReferThreadStatus", h_ReferThreadStatus);
    sr_hle_register(0x3b183e26, "sceKernelGetThreadExitStatus", h_GetThreadExitStatus);
    sr_hle_register(0x94416130, "sceKernelGetThreadmanIdList", h_GetThreadmanIdList);
    sr_hle_register(0xba6b92e2, "sceKernelSysClock2USec", h_SysClock2USec);
    sr_hle_register(0xd8b73127, "sceKernelGetModuleIdByAddress", h_GetModuleId);
    sr_hle_register(0x092968f4, "sceKernelCpuSuspendIntr", h_CpuSuspendIntr);
    sr_hle_register(0x5f10d406, "sceKernelCpuResumeIntr", h_ok);
    /* Boot setup batch (return success / reference value). */
    sr_hle_register(0x4ac57943, "sceKernelRegisterExitCallback", h_ok);
    sr_hle_register(0xa5da2406, "sceUtilityGetSystemParamInt", h_GetSystemParamInt);
    sr_hle_register(0x34b78343, "sceUtilityGetSystemParamString", h_GetSystemParamString);
    sr_hle_register(0x36aa6e91, "sceImposeSetLanguageMode", h_ImposeSetLanguageMode);
    sr_hle_register(0x24fd7bcf, "sceImposeGetLanguageMode", h_ImposeGetLanguageMode);
    /* sceUtility dialogs (OSK / savedata / netconf): no dialog active -> status 0, calls ok. */
    sr_hle_register(0xf3f76017, "sceUtilityOskGetStatus", h_OskGetStatus);
    sr_hle_register(0x4b85c861, "sceUtilityOskUpdate", h_OskUpdate);
    sr_hle_register(0x3dfaeba9, "sceUtilityOskShutdownStart", h_OskShutdown);
    sr_hle_register(0x1579a159, "sceUtilityLoadNetModule", h_ok);
    /* 0xf6269b82 is OskInitStart, NOT GetSystemParamString -- the old string handler wrote
     * A2 bytes through A1, both garbage for this signature. */
    sr_hle_register(0xf6269b82, "sceUtilityOskInitStart", h_OskInitStart);
    sr_hle_register(0x50c4cd57, "sceUtilitySavedataInitStart", h_SavedataInitStart);
    sr_hle_register(0x9790b33c, "sceUtilitySavedataShutdownStart", h_DlgShutdown);
    sr_hle_register(0x8874dbe0, "sceUtilitySavedataGetStatus", h_DlgGetStatus);
    sr_hle_register(0xd4b95ffb, "sceUtilitySavedataUpdate", h_SavedataUpdate);
    sr_hle_register(0x64d50c56, "sceUtilityUnloadNetModule", h_ok);
    sr_net_register();    /* deterministic offline networking and stable identity */
    sr_hle_register(0x293b45b8, "sceKernelGetThreadId", h_GetThreadIdSched);
    sr_hle_register(0x82826f70, "sceKernelSleepThreadCB", h_SleepThreadCB);
    sr_hle_register(0x68da9e36, "sceKernelDelayThreadCB", h_DelayThreadCB);
    sr_hle_register(0x46ebb729, "sceUmdCheckMedium", h_UmdCheckMedium);
    sr_hle_register(0xc6183d47, "sceUmdActivate", h_UmdActivate);
    sr_hle_register(0xe83742ba, "sceUmdDeactivate", h_UmdDeactivate);
    sr_hle_register(0x87533940, "sceUmdReplaceProhibit", h_ok);
    sr_hle_register(0x8ef08fce, "sceUmdWaitDriveStat", h_ok);
    sr_hle_register(0x617f3fe6, "sceDmacMemcpy", h_DmacMemcpy);
    /* sceMpeg: faithful port (src/rt/mpeg.c). Drives the PSMF intro to completion. */
    sr_hle_register(0x682a619b, "sceMpegInit", h_MpegInit);
    sr_hle_register(0x874624d6, "sceMpegFinish", h_MpegFinish);
    sr_hle_register(0xc132e22f, "sceMpegQueryMemSize", h_MpegQueryMemSize);
    sr_hle_register(0xd8c5f121, "sceMpegCreate", h_MpegCreate);
    sr_hle_register(0x606a4649, "sceMpegDelete", h_MpegDelete);
    sr_hle_register(0x42560f23, "sceMpegRegistStream", h_MpegRegistStream);
    sr_hle_register(0x591a4aa2, "sceMpegUnRegistStream", h_MpegUnRegistStream);
    sr_hle_register(0x21ff80e4, "sceMpegQueryStreamOffset", h_MpegQueryStreamOffset);
    sr_hle_register(0x611e9e11, "sceMpegQueryStreamSize", h_MpegQueryStreamSize);
    sr_hle_register(0xd7a29f46, "sceMpegRingbufferQueryMemSize", h_MpegRingbufferQueryMemSize);
    sr_hle_register(0x37295ed8, "sceMpegRingbufferConstruct", h_MpegRingbufferConstruct);
    sr_hle_register(0x13407f13, "sceMpegRingbufferDestruct", h_ok);
    sr_hle_register(0xb240a59e, "sceMpegRingbufferPut", h_MpegRingbufferPut);
    sr_hle_register(0xb5f6dc87, "sceMpegRingbufferAvailableSize", h_MpegRingbufferAvailable);
    sr_hle_register(0xfe246728, "sceMpegGetAvcAu", h_MpegGetAvcAu);
    sr_hle_register(0xe1ce83a7, "sceMpegGetAtracAu", h_MpegGetAtracAu);
    sr_hle_register(0x0e3c2e9d, "sceMpegAvcDecode", h_MpegAvcDecode);
    sr_hle_register(0x800c44df, "sceMpegAtracDecode", h_MpegAtracDecode);
    sr_hle_register(0x740fccd1, "sceMpegAvcDecodeStop", h_MpegAvcDecodeStop);
    sr_hle_register(0x4571cc64, "sceMpegAvcDecodeFlush", h_ok);
    sr_hle_register(0xa11c7026, "sceMpegAvcDecodeMode", h_MpegAvcDecodeMode);
    sr_hle_register(0xa780cf7e, "sceMpegMallocAvcEsBuf", h_MpegMallocAvcEsBuf);
    sr_hle_register(0xceb870b1, "sceMpegFreeAvcEsBuf", h_MpegFreeAvcEsBuf);
    sr_hle_register(0x167afd9e, "sceMpegInitAu", h_MpegInitAu);
    sr_hle_register(0xf8dcb679, "sceMpegQueryAtracEsSize", h_MpegQueryAtracEsSize);
    /* sceAtrac3plus (RIFF parsing, real decode, looping and streamed refills). */
    sr_hle_register(0x7a20e7af, "sceAtracSetDataAndGetID", h_AtracSetDataAndGetID);
    sr_hle_register(0x61eb33f5, "sceAtracReleaseAtracID", h_AtracReleaseAtracID);
    sr_hle_register(0x6a8c3cd5, "sceAtracDecodeData", h_AtracDecodeData);
    sr_hle_register(0x9ae849a7, "sceAtracGetRemainFrame", h_AtracGetRemainFrame);
    sr_hle_register(0x5d268707, "sceAtracGetStreamDataInfo", h_AtracGetStreamDataInfo);
    sr_hle_register(0x7db31251, "sceAtracAddStreamData", h_AtracAddStreamData);
    sr_hle_register(0xe23e3a35, "sceAtracGetNextDecodePosition", h_AtracGetNextDecodePosition);
    sr_hle_register(0xa2bba8be, "sceAtracGetSoundSample", h_AtracGetSoundSample);
    sr_hle_register(0xfaa4f89b, "sceAtracGetLoopStatus", h_AtracGetLoopStatus);
    sr_hle_register(0x868120b5, "sceAtracSetLoopNum", h_AtracSetLoopNum);
    sr_hle_register(0xe88f759b, "sceAtracGetInternalErrorInfo", h_AtracGetInternalErrorInfo);
    sr_hle_register(0x2dd3e298, "sceAtracGetBufferInfoForResetting", h_AtracGetBufferInfoForResetting);
    sr_hle_register(0x644e5607, "sceAtracResetPlayPosition", h_AtracResetPlayPosition);
    sr_hle_register(0x707b7629, "sceMpegFlushAllStream", h_ok);
    /* sceLibFont (firmware fonts absent; valid handles + empty glyphs). */
    sr_hle_register(0x67f17ed7, "sceFontNewLib", h_FontNewLib);
    sr_hle_register(0xa834319d, "sceFontOpen", h_FontOpen);
    sr_hle_register(0x574b6fbc, "sceFontDoneLib", h_ok);
    sr_hle_register(0x0da7535e, "sceFontGetFontInfo", h_FontGetFontInfo);
    sr_hle_register(0xdcc80c2f, "sceFontGetCharInfo", h_FontGetCharInfo);
    sr_hle_register(0x980f4895, "sceFontGetCharGlyphImage", h_FontGetCharGlyphImage);
    sr_hle_register(0x099ef33c, "sceFontFindOptimumFont", h_FontFindOptimumFont);
    sr_hle_register(0x3aea8cb6, "sceFontClose", h_ok);
    sr_hle_register(0xd97f94d8, "sceDmacTryMemcpy", h_DmacMemcpy);
    sr_hle_register(0xaee7404d, "sceUmdRegisterUMDCallBack", h_UmdRegisterUMDCallBack);
    sr_hle_register(0xbd2bde07, "sceUmdUnRegisterUMDCallBack", h_UmdUnRegisterUMDCallBack);
    sr_hle_register(0x6af9b50a, "sceUmdCancelWaitDriveStat", h_ok);
    sr_hle_register(0x6b4a146c, "sceUmdGetDriveStat", h_UmdDriveStat);
    sr_hle_register(0x20628e6f, "sceUmdGetErrorStat", h_ok);
    sr_hle_register(0x56202973, "sceUmdWaitDriveStatWithTimer", h_ok);
    sr_hle_register(0xc629af26, "sceUtilityLoadAvModule", h_ok);
    sr_hle_register(0x977de386, "sceKernelLoadModule", h_LoadModule);
    sr_hle_register(0x50f0c1ec, "sceKernelStartModule", h_ok);
    sr_hle_register(0xf0a26395, "sceKernelGetModuleId", h_GetModuleId);
    sr_hle_register(0xd1ff982a, "sceKernelStopModule", h_ok);
    sr_hle_register(0x2e0911aa, "sceKernelUnloadModule", h_ok);
    sr_hle_register(0x8f2df740, "sceKernelStopUnloadSelfModuleWithStatus", h_ok);
    sr_hle_register(0xf919f628, "SysMemUserForUser_f919f628", h_F919F628);
    /* IoFileMgrForUser: file IO from the ISO. */
    sr_hle_register(0x109f50bc, "sceIoOpen", h_IoOpen);
    sr_hle_register(0x6a638d83, "sceIoRead", h_IoRead);
    sr_hle_register(0x42ec03ac, "sceIoWrite", h_IoWrite);
    sr_hle_register(0x779103a0, "sceIoRename", h_ok);
    sr_hle_register(0x68963324, "sceIoLseek32", h_IoLseek32);
    sr_hle_register(0x27eb27b8, "sceIoLseek", h_IoLseek);
    sr_hle_register(0x810c4bc3, "sceIoClose", h_IoClose);
    sr_hle_register(0xace946e8, "sceIoGetstat", h_IoGetstat);
    sr_hle_register(0xb29ddf9c, "sceIoDopen", h_IoDopen);
    sr_hle_register(0xe3eb004c, "sceIoDread", h_IoDread);
    sr_hle_register(0xeb092469, "sceIoDclose", h_IoDclose);
    sr_hle_register(0xf27a9c51, "sceIoRemove", h_IoRemove);
    sr_hle_register(0x1117c65f, "sceIoRmdir", h_IoRmdir);
    sr_hle_register(0x89aa9906, "sceIoOpenAsync", h_IoOpenAsync);
    sr_hle_register(0xa0b5a7c2, "sceIoReadAsync", h_IoReadAsync);
    sr_hle_register(0x71b19e77, "sceIoLseekAsync", h_IoLseekAsync);
    sr_hle_register(0xe23eec33, "sceIoWaitAsync", h_IoWaitAsync);
    sr_hle_register(0x35dbd746, "sceIoWaitAsyncCB", h_IoWaitAsyncCB);
    sr_hle_register(0x3251ea56, "sceIoPollAsync", h_IoPollAsync);
    sr_hle_register(0xff5940b6, "sceIoCloseAsync", h_IoCloseAsync);
    sr_hle_register(0x54f5fb11, "sceIoDevctl", h_IoDevctl);
    sr_hle_register(0xb293727f, "sceIoChangeAsyncPriority", h_IoChangeAsyncPriority);
    /* sceAudio */
    sr_hle_register(0x5ec81c55, "sceAudioChReserve", h_AudioChReserve);
    sr_hle_register(0x6fc46853, "sceAudioChRelease", h_AudioChRelease);
    sr_hle_register(0x136caf51, "sceAudioOutputBlocking", h_AudioOutputBlocking);
    sr_hle_register(0x13f592bc, "sceAudioOutputPannedBlocking", h_AudioOutputPannedBlocking);
    sr_hle_register(0xe2d56b2d, "sceAudioOutputPanned", h_AudioOutputPannedBlocking);
    sr_hle_register(0x95fd0c2d, "sceAudioChangeChannelConfig", h_ok);
    sr_hle_register(0xb011922f, "sceAudioGetChannelRestLength", h_AudioRestLen);
    sr_hle_register(0xb7e1d8e7, "sceAudioChangeChannelVolume", h_ok);
    sr_hle_register(0xcb2e439e, "sceAudioSetChannelDataLen", h_AudioSetChannelDataLen);
    sr_hle_register(0x01562ba3, "sceAudioOutput2Reserve", h_AudioOutput2Reserve);
    sr_hle_register(0x2d53f36e, "sceAudioOutput2OutputBlocking", h_AudioOutput2OutputBlocking);
    sr_hle_register(0x63f2889c, "sceAudioOutput2ChangeLength", h_AudioOutput2ChangeLength);
    sr_hle_register(0x43196845, "sceAudioOutput2Release", h_AudioOutput2Release);
    sr_hle_register(0x6d4bec68, "sceAudioInput", h_AudioInput);
    sr_hle_register(0x87b2e651, "sceAudioWaitInputEnd", h_AudioWaitInputEnd);
    sr_hle_register(0xe926d3fb, "sceAudioInputInitEx", h_AudioInputInitEx);
    /* sceCtrl */
    sr_hle_register(0x1f4011e6, "sceCtrlSetSamplingMode", h_ok);
    sr_hle_register(0xa7144800, "sceCtrlSetIdleCancelThreshold", h_ok);
    sr_hle_register(0x1f803938, "sceCtrlReadBufferPositive", h_CtrlReadBuffer);
    sr_hle_register(0x3a622550, "sceCtrlPeekBufferPositive", h_CtrlPeekBuffer);
    sr_hle_register(0x6a2774f3, "sceCtrlSetSamplingCycle", h_CtrlSetSamplingCycle);
    /* 0x687660fa is GetIdleCancelThreshold(int*,int*), NOT ReadBufferNegative -- the pad
     * handler used pointer a1 as a buffer count and wrote up to a ring of SceCtrlData
     * through a1's 4-byte int (and could block the caller on the input ring). */
    sr_hle_register(0x687660fa, "sceCtrlGetIdleCancelThreshold", h_CtrlGetIdleCancelThreshold);
    /* sceDisplay */
    sr_hle_register(0x0e20f177, "sceDisplaySetMode", h_ok);
    sr_hle_register(0x289d82fe, "sceDisplaySetFrameBuf", h_DisplaySetFrameBuf);
    sr_hle_register(0xeeda2e54, "sceDisplayGetFrameBuf", h_DisplayGetFrameBuf);
    sr_hle_register(0x984c27e7, "sceDisplayWaitVblankStart", h_DisplayWaitVblank);
    sr_hle_register(0x9c6eaad7, "sceDisplayGetVcount", h_DisplayGetVcount);
    sr_hle_register(0x773dd3a3, "sceDisplayGetCurrentHcount", h_DisplayGetCurrentHcount);
    sr_hle_register(0x210eab3a, "sceDisplayGetAccumulatedHcount", h_DisplayGetAccumulatedHcount);
    sr_hle_register(0x4d4e10ec, "sceDisplayIsVblank", h_ok);
    sr_hle_register(0xdba6c4c4, "sceDisplayGetFramePerSec", h_ok);
    /* sceGe_user: display lists and GE interrupts (gelist.c) */
    sr_ge_register();
    /* sceSasCore: real VAG voice mixing (see sas_mix above). */
    sr_hle_register(0x68a46b95, "__sceSasGetEndFlag", h_SasGetEndFlag);
    sr_hle_register(0xa3589d81, "__sceSasCore", h_SasCore);
    sr_hle_register(0x50a14dfc, "__sceSasCoreWithMix", h_SasCoreWithMix);
    sr_hle_register(0x76f01aca, "__sceSasSetKeyOn", h_SasSetKeyOn);
    sr_hle_register(0xa0cf2fa4, "__sceSasSetKeyOff", h_SasSetKeyOff);
    sr_hle_register(0x42778a9f, "__sceSasInit", h_SasInit);
    sr_hle_register(0x99944089, "__sceSasSetVoice", h_SasSetVoice);
    sr_hle_register(0xad84d37f, "__sceSasSetPitch", h_SasSetPitch);
    sr_hle_register(0x440ca7d8, "__sceSasSetVolume", h_SasSetVolume);
    sr_hle_register(0x74ae582a, "__sceSasGetEnvelopeHeight", h_SasGetEnvelopeHeight);
    sr_hle_register(0x019b25eb, "__sceSasSetADSR", h_SasSetADSR);
    sr_hle_register(0x9ec3676a, "__sceSasSetADSRmode", h_SasSetADSRMode);
    sr_hle_register(0xcbcd4f79, "__sceSasSetSimpleADSR", h_SasSetSimpleADSR);
    sr_hle_register(0x5f9529f6, "__sceSasSetSL", h_SasSetSL);
    sr_hle_register(0xb7660a23, "__sceSasSetNoise", h_SasSetNoise);
    sr_hle_register(0x2c8e6ab3, "__sceSasGetPauseFlag", h_SasGetPauseFlag);
    sr_hle_register(0x787d04d5, "__sceSasSetPause", h_SasSetPause);
    sr_hle_register(0x33d4ab37, "__sceSasRevType", h_SasRevType);
    sr_hle_register(0x267a6dd2, "__sceSasRevParam", h_SasRevParam);
    sr_hle_register(0xd5a229c9, "__sceSasRevEVOL", h_SasRevEVOL);
    sr_hle_register(0xf983b186, "__sceSasRevVON", h_SasRevVON);
    /* time / rtc / libc clock */
    sr_hle_register(0x369ed59d, "sceKernelGetSystemTimeLow", h_GetSystemTimeLow);
    sr_hle_register(0x82bc5777, "sceKernelGetSystemTimeWide", h_GetSystemTimeWide);
    sr_hle_register(0xdb738f35, "sceKernelGetSystemTime", h_GetSystemTime);
    sr_hle_register(0x3f7ad767, "sceRtcGetCurrentTick", h_RtcGetCurrentTick);
    sr_hle_register(0x27c4594c, "sceRtcGetTime_t", h_RtcGetTime_t);
    sr_hle_register(0xcf561893, "sceRtcGetWin32FileTime", h_RtcGetWin32FileTime);
    sr_hle_register(0xe7c27d1b, "sceRtcGetCurrentClockLocalTime", h_RtcGetCurrentClock);
    sr_hle_register(0x91e4f6a7, "sceKernelLibcClock", h_LibcClock);
    sr_hle_register(0x27cc57f0, "sceKernelLibcTime", h_LibcTime);
    sr_hle_register(0x71ec4271, "sceKernelLibcGettimeofday", h_LibcGettimeofday);
    /* cache / misc UtilsForUser: no-ops are fine without a real cache. */
    sr_hle_register(0x79d1c3fa, "sceKernelDcacheWritebackAll", h_ok);
    sr_hle_register(0x3ee30821, "sceKernelDcacheWritebackRange", h_ok);
    sr_hle_register(0x34b9fa9e, "sceKernelDcacheWritebackInvalidateRange", h_ok);
    sr_hle_register(0xb435dec5, "sceKernelDcacheWritebackInvalidateAll", h_ok);
    sr_hle_register(0x37fb5c42, "sceKernelGetGPI", h_ok);
    sr_hle_register(0x6ad345d7, "sceKernelSetGPO", h_ok);
    /* Stdio std handles. */
    sr_hle_register(0x13a5abef, "sceKernelPrintf", h_KernelPrintf);
    sr_hle_register(0x172d316e, "sceKernelStdin", h_StdFd);
    sr_hle_register(0xa6bab2e9, "sceKernelStdout", h_StdFd);
    sr_hle_register(0xf78ba90a, "sceKernelStderr", h_StdFd);
    /* scePower / sceSuspendForUser / LoadExecForUser: locks and registrations succeed. */
    sr_hle_register(0x3aee7261, "sceKernelPowerUnlock", h_ok);
    sr_hle_register(0xeadb1bd7, "sceKernelPowerLock", h_ok);
    sr_hle_register(0x090ccb3f, "sceKernelPowerTick", h_ok);
    sr_hle_register(0x3e0271d3, "sceKernelVolatileMemLock", h_VolatileMemLock);
    sr_hle_register(0xa14f40b2, "sceKernelVolatileMemTryLock", h_VolatileMemLock);
    /* Unlock takes only the type arg -- it must NOT run the Lock handler: writing the
     * out-params through leftover a1/a2 register garbage sprayed two wild 4-byte writes
     * per asset-load unlock (this zeroed the resource registry's model-slot counter,
     * which silently killed every .PMD model lookup, e.g. the hangar aircraft). */
    sr_hle_register(0xa569e425, "sceKernelVolatileMemUnlock", h_ok);
    sr_hle_register(0x05572a5f, "sceKernelExitGame", h_ok);   /* returning lets the app idle on */
    /* InterruptManager: sub-interrupt handlers (intr.c); the scheduler delivers vblank. */
    sr_intr_register();
    /* semaphores */
    sr_hle_register(0xd6da4ba1, "sceKernelCreateSema", h_CreateSema);
    sr_hle_register(0x28b6489c, "sceKernelDeleteSema", h_DeleteSema);
    sr_hle_register(0x4e3a1105, "sceKernelWaitSema", h_WaitSema);
    sr_hle_register(0x6d212bac, "sceKernelWaitSemaCB", h_WaitSemaCB);
    sr_hle_register(0x3f53e640, "sceKernelSignalSema", h_SignalSema);
    sr_hle_register(0x58b1f937, "sceKernelPollSema", h_PollSema);
    /* event flags */
    sr_hle_register(0x55c20a00, "sceKernelCreateEventFlag", h_CreateEventFlag);
    sr_hle_register(0xef9e4c70, "sceKernelDeleteEventFlag", h_DeleteEventFlag);
    sr_hle_register(0x1fb15a32, "sceKernelSetEventFlag", h_SetEventFlag);
    sr_hle_register(0x812346e4, "sceKernelClearEventFlag", h_ClearEventFlag);
    sr_hle_register(0x402fcf22, "sceKernelWaitEventFlag", h_WaitEventFlag);
    sr_hle_register(0x328c546a, "sceKernelWaitEventFlagCB", h_WaitEventFlagCB);
    sr_hle_register(0x30fd48f0, "sceKernelPollEventFlag", h_PollEventFlag);
}

/* ---- dispatch ---- */

void sr_syscall(CpuState *s, uint32_t nid) {
    sr_hle_init();
    sr_last_nid = nid;
    if (getenv("SR_NIDLOG")) {
        static FILE *nf = NULL; static unsigned long nc = 0;
        if (!nf) nf = fopen("nidseq_mine.txt", "w");
        if (nf) { fprintf(nf, "0x%08x %x\n", nid, sched_current_uid()); fflush(nf); nc++; }
    }
    HleEntry *e = hle_find(nid);
    if (!e) {
        /* Permissive mode (SR_PERMISSIVE): unimplemented NIDs log once and return 0 instead of
         * trapping the thread. This is for bring-up exploration -- it lets audio/video (sceMpeg,
         * sceAtrac, sceSas) and other not-yet-modelled subsystems no-op so the main render thread
         * can run. It deliberately returns fake success, so it is gated and not the default. */
        if (getenv("SR_PERMISSIVE")) {
            static uint32_t warned[1024]; static int nwarned = 0; int known = 0;
            for (int i = 0; i < nwarned; i++) if (warned[i] == nid) { known = 1; break; }
            if (!known && nwarned < 1024) { warned[nwarned++] = nid;
                const char *nm = sr_nid_name(nid);
                fprintf(stderr, "HLE: permissive no-op nid 0x%08x (%s) (thread uid 0x%x)\n",
                        nid, nm ? nm : "unknown", sched_current_uid()); }
            s->r[1] = 0xDEADBEEFu;
            for (int i = 4; i <= 15; i++) s->r[i] = 0xDEADBEEFu;
            s->r[24] = 0xDEADBEEFu; s->r[25] = 0xDEADBEEFu; s->hi = 0xDEADBEEFu; s->lo = 0xDEADBEEFu;
            s->r[2] = 0;
            return;
        }
        {
            const char *nm = sr_nid_name(nid);
            fprintf(stderr, "HLE: unimplemented nid 0x%08x (%s) (thread uid 0x%x)\n"
                            "     -> add a handler in src/rt/hle.c: sr_hle_register(0x%08xu, \"%s\", h_...);\n",
                    nid, nm ? nm : "unknown", sched_current_uid(), nid, nm ? nm : "sceUnknown");
        }
        sr_hit_hle = 1;
        /* Under the fiber scheduler, longjmp across fibers is invalid; stop the process cleanly
         * after flushing the trace. The plain driver (no scheduler) keeps the longjmp boundary. */
        if (sr_sched_on) { sr_trace_close(); fflush(stderr); _Exit(7); }
        longjmp(g_hle_jmp, 1);
    }
    if (getenv("SR_SYSLOG")) {
        /* Log the first time each (thread, nid) pair occurs, to see what each thread does. */
        static struct { uint32_t uid, nid; } seen[4096]; static int nseen = 0;
        uint32_t cur = sched_current_uid(); int known = 0;
        for (int i = 0; i < nseen; i++) if (seen[i].uid == cur && seen[i].nid == nid) { known = 1; break; }
        if (!known && nseen < 4096) { seen[nseen].uid = cur; seen[nseen].nid = nid; nseen++;
            fprintf(stderr, "thr 0x%x : %s (0x%08x)\n", cur, e->name, nid); }
    }
    if (g_callcount) {
        int j = 0; for (; j < g_ncc; j++) if (g_cc[j].nid == nid) break;
        if (j == g_ncc && g_ncc < 512) { g_cc[j].nid = nid; g_cc[j].nm = e->name; g_cc[j].n = 0; g_ncc++; }
        if (j < 512) g_cc[j].n++;
    }
    uint32_t ret = e->fn(s);
    /* Poison caller-saved temps exactly like PPSSPP SetDeadbeefRegs: r1, r4-r15, r24, r25,
     * hi, lo. The return value in v0 (and v1) is written afterward and survives. */
    s->r[1] = 0xDEADBEEFu;
    for (int i = 4; i <= 15; i++) s->r[i] = 0xDEADBEEFu;
    s->r[24] = 0xDEADBEEFu; s->r[25] = 0xDEADBEEFu;
    s->hi = 0xDEADBEEFu; s->lo = 0xDEADBEEFu;
    s->r[2] = ret;
}
