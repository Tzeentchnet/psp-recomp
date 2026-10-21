/* Cooperative thread scheduler for the recompiled game (Phase 6).
 *
 * PSP threads are priority-scheduled and a busy-waiting thread is preempted by its timeslice
 * so a sibling can run. The recompiled code is straight C, so to suspend a thread mid-call-
 * stack we run each guest thread on its own Windows fiber and switch between them. All threads
 * share one CpuState; on a switch its contents are saved into the outgoing thread's control
 * block and the incoming thread's are loaded, so the single register file follows whichever
 * thread is running. SR_YIELD (emitted by codegen at function entry and loop back-edges) burns
 * the timeslice and switches when it reaches zero, giving preemption without a real timer.
 *
 * This is a simplified model: highest-priority ready thread runs; equal priority round-robins;
 * sceKernelDelayThread blocks until enough yields have elapsed. It is enough to interleave the
 * boot threads the way the game's startup expects, not a cycle-accurate kernel.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <windows.h>

#ifndef SR_MODULE_START_PRIORITY
#define SR_MODULE_START_PRIORITY 32u
#endif
#ifndef SR_MODULE_START_STACK_SIZE
#define SR_MODULE_START_STACK_SIZE 0x400u
#endif
#ifndef SR_MODULE_START_ATTR
#define SR_MODULE_START_ATTR 0x80000000u
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int     sr_sched_on = 0;
int32_t sr_timeslice = 0;

#define TIMESLICE 20000          /* yields per thread run before preemption */
#define MAXTHREADS 128

enum { TH_DORMANT = 0, TH_READY, TH_RUNNING, TH_WAIT_DELAY, TH_WAIT_OBJ };
enum { PSP_THREAD_RUNNING = 1, PSP_THREAD_READY = 2, PSP_THREAD_WAITING = 4, PSP_THREAD_STOPPED = 16 };
enum { PSP_WAIT_NONE = 0, PSP_WAIT_SLEEP = 1, PSP_WAIT_DELAY = 2, PSP_WAIT_OBJECT = 3 };

typedef struct {
    uint32_t uid;
    int      state;
    int      priority;
    int      initial_priority;
    int32_t  exit_status;
    uint32_t entry, arglen, argp;
    void    *fiber;
    int      started;            /* fiber has begun running its body */
    uint64_t wake;               /* scheduler tick to wake at (TH_WAIT_DELAY) */
    uint32_t wait_obj;           /* object uid this thread waits on (TH_WAIT_OBJ) */
    int      wakeups;            /* pending sceKernelWakeupThread count (sleep/wakeup semantics) */
    int      sleeping;           /* 1 while blocked in sceKernelSleepThread[CB] */
    uint64_t vbl_seen;           /* s_vbl_count this thread last consumed (vblank latch) */
    char     name[32];
    uint32_t attr;
    uint32_t gp;
    uint32_t stack_start, stack_size;   /* block in the user partition (kmem.c); 0 = none */
    int      deleted;            /* slot free for reuse once its fiber is gone */
    int      delete_on_exit;     /* the module start thread: deleted when its entry returns */
    int      cb_wait;            /* in a ...CB wait: notified callbacks run on this thread */
    int      cb_woken;           /* readied only to run callbacks; the wait resumes afterwards */
    SrHleThreadFn hle_entry;      /* host HLE helper body, instead of a recompiled guest entry */
    uint32_t hle_arg;
    CpuState saved;              /* register file while not running */
} TCB;

/* PSP thread attributes that change the stack (PPSSPP sceKernelThread.h). */
#define THREAD_ATTR_NO_FILLSTACK 0x00100000u
#define THREAD_ATTR_CLEAR_STACK  0x00200000u
#define THREAD_ATTR_LOW_STACK    0x00400000u

static TCB      s_tcb[MAXTHREADS];
static int cb_wake_handled(TCB *t);
static int      s_ntcb = 0;
static int      s_cur = -1;      /* index of running thread, -1 = scheduler */
static void    *s_sched_fiber = NULL;
static CpuState *s_cpu = NULL;
static uint64_t s_tick = 0;
static uint32_t s_gp = 0;        /* module global pointer, inherited by created threads */

static void switch_to_scheduler(void);

static TCB *tcb_by_uid(uint32_t uid) {
    for (int i = 0; i < s_ntcb; i++) if (!s_tcb[i].deleted && s_tcb[i].uid == uid) return &s_tcb[i];
    return NULL;
}

static uint32_t resolve_thread_uid(uint32_t uid) {
    return uid ? uid : (s_cur >= 0 ? s_tcb[s_cur].uid : 0);
}

void sched_init(CpuState *cpu) {
    s_cpu = cpu;
    s_gp = cpu->r[28];           /* the driver seeded gp from the module's # init */
    s_sched_fiber = ConvertThreadToFiber(NULL);
    sr_sched_on = 1;
    sr_timeslice = TIMESLICE;
}

uint32_t sched_current_uid(void) { return s_cur >= 0 ? s_tcb[s_cur].uid : 0; }

/* Diagnostic: dump every thread's state, entry, saved PC, and what it waits on. Reveals a thread
 * blocked on a sema/event that is never signalled (a likely scene-transition gate). */
void sched_dump_threads(void) {
    static const char *st[] = { "DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ" };
    fprintf(stderr, "--- threads (%d) cur=%d tick=%llu ---\n", s_ntcb, s_cur, (unsigned long long)s_tick);
    for (int i = 0; i < s_ntcb; i++) {
        TCB *t = &s_tcb[i];
        fprintf(stderr, "  uid=0x%x entry=0x%08x pc=0x%08x %-10s pri=%d wait_obj=0x%x wake=%llu\n",
                t->uid, t->entry, t->saved.pc, st[t->state < 5 ? t->state : 0], t->priority,
                t->wait_obj, (unsigned long long)t->wake);
    }
}

/* Interrupt context. A guest interrupt handler (vblank, GE signal/finish) runs on the
 * dedicated interrupt stack with s_cur = -1, so SR_YIELD cannot switch threads and blocking
 * calls return at once; the interrupted register file is saved and restored around it. An
 * interrupt raised while one runs (e.g. sceGeContinue inside the vblank handler) is queued by
 * its source and delivered when the outer handler has returned, as PPSSPP does. */
static int s_intr_depth = 0;
int sched_in_interrupt(void) { return s_intr_depth > 0; }

void sched_run_interrupt(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
    CpuState save;
    memcpy(&save, s, sizeof(CpuState));
    int32_t save_slice = sr_timeslice;
    memset(s, 0, sizeof(CpuState));
    s->r[29] = sr_hle_stack_top(0);      /* dedicated interrupt stack in kernel memory */
    s->r[28] = s_gp ? s_gp : save.r[28];
    s->r[4] = a0;
    s->r[5] = a1;
    s->r[6] = a2;
    s->r[31] = 0;
    s->vfpuCtrl[0] = 0xe4; s->vfpuCtrl[1] = 0xe4;
    int save_cur = s_cur; s_cur = -1;
    s_intr_depth++;
    sr_timeslice = TIMESLICE;
    dispatch(s, fn);
    s_intr_depth--;
    s_cur = save_cur;
    sr_timeslice = save_slice;
    memcpy(s, &save, sizeof(CpuState));
}

/* Threads blocked in sceDisplayWaitVblankStart wait on this object; deliver_vblank readies
 * them, so the render loop draws exactly once per delivered vblank instead of spinning. */
#define VBLANK_WAIT_OBJ 0x56424c4bu   /* "VBLK" */
static uint64_t s_vbl_count = 0;     /* vblanks delivered so far (latch reference) */

/* The vblank is an interrupt: it can fire WHILE a thread runs (from the yield path, or while a
 * host call like a vsynced swapchain present blocks). A thread that then calls
 * sceDisplayWaitVblankStart must not sleep a whole extra period for the NEXT one -- that
 * hard-quantizes any frame whose work+present crosses the period to 30/20 fps. Latch it
 * instead: if a vblank was delivered since this thread last consumed one, return immediately
 * (consume the pending vblank); only block when none is pending. */
void sched_wait_vblank(void) {
    if (s_cur >= 0) {
        TCB *t = &s_tcb[s_cur];
        if (t->vbl_seen != s_vbl_count) { t->vbl_seen = s_vbl_count; return; }
        sched_block_on(VBLANK_WAIT_OBJ);
        t->vbl_seen = s_vbl_count;
        return;
    }
    sched_block_on(VBLANK_WAIT_OBJ);
}

/* ---- virtual time ------------------------------------------------------------------------
 * The scheduler keeps a microsecond clock (s_vtime_us) that all timed waits compare against.
 * With vblank pacing ON (default) it tracks the real QPC clock, so sceKernelDelayThread and
 * timed sema/event waits elapse in REAL time -- the same timebase the paced vblanks run on.
 * (They used to be counted in scheduler "ticks" -- one tick per yield -- an elastic unit
 * that passed in microseconds while threads were busy and was jumped over when idle. Game
 * speed then depended on incidental scheduling: menus pacing via DelayThread ran 2x, and
 * mission logic threads ran a random number of iterations per frame.)
 * With SR_NOVBPACE=1 (turbo) it advances 1/59.94 s per delivered vblank and jumps over idle
 * delay waits, so everything runs as fast as the host allows. */
static uint64_t s_vtime_us = 0;
static int s_pace_on = -1;
static int s_permanent_turbo;
static int s_fast_forwarding;
static uint32_t s_fast_forward_present;
static LARGE_INTEGER s_qfreq;
static LONGLONG s_qpc0;
static double s_vbl_next;            /* QPC counts: when the next vblank is due */
static uint64_t s_vbl_next_us = 0;   /* same deadline on the s_vtime_us scale (cheap compare) */

static void vbl_next_us_sync(void) {
    s_vbl_next_us = (uint64_t)((s_vbl_next - (double)s_qpc0) * 1000000.0 / (double)s_qfreq.QuadPart);
}

static void pace_setup(void) {
    if (s_pace_on >= 0) return;
    s_permanent_turbo = getenv("SR_NOVBPACE") ? 1 : 0;
    const char *fast_forward = getenv("SR_FAST_FORWARD_PRESENT");
    if (fast_forward && fast_forward[0]) {
        char *end = NULL;
        unsigned long parsed = strtoul(fast_forward, &end, 10);
        if (!end || *end || !parsed || parsed > UINT32_MAX) {
            fprintf(
                stderr,
                "SR_FAST_FORWARD_PRESENT: expected a positive presentation, got %s\n",
                fast_forward);
            _Exit(2);
        }
        s_fast_forward_present = (uint32_t)parsed;
    }
    s_fast_forwarding =
        !s_permanent_turbo && s_fast_forward_present &&
        sr_present_count() < s_fast_forward_present;
    s_pace_on = !s_permanent_turbo && !s_fast_forwarding;
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&s_qfreq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    s_qpc0 = c.QuadPart;
    s_vbl_next = (double)c.QuadPart;
    vbl_next_us_sync();
    if (s_fast_forwarding) {
        fprintf(
            stderr,
            "scheduler: fast-forwarding to presentation %u\n",
            s_fast_forward_present);
    }
}

static void pace_update_mode(void) {
    pace_setup();
    if (!s_fast_forwarding ||
        sr_present_count() < s_fast_forward_present) {
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    s_fast_forwarding = 0;
    s_pace_on = 1;
    s_qpc0 =
        now.QuadPart -
        (LONGLONG)((double)s_vtime_us * (double)s_qfreq.QuadPart / 1000000.0);
    s_vbl_next = (double)now.QuadPart;
    vbl_next_us_sync();
    fprintf(
        stderr,
        "scheduler: real-time pacing enabled at presentation %u\n",
        sr_present_count());
}

/* True when the scheduler paces vblanks to real time (the default). gui_present uses this to
 * skip its own legacy 60 Hz sleep: two independent pacers stack and push frames past the
 * vblank period (the second one then costs a whole extra frame). */
int sched_vbl_paced(void) {
    pace_update_mode();
    return s_pace_on > 0;
}

int sched_fast_forwarding(void) {
    if (s_pace_on < 0) {
        const char *fast_forward = getenv("SR_FAST_FORWARD_PRESENT");
        if (!fast_forward || !fast_forward[0]) return 0;
    }
    pace_update_mode();
    return s_fast_forwarding;
}

static uint64_t qpc_us(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (uint64_t)((double)(c.QuadPart - s_qpc0) * 1000000.0 / (double)s_qfreq.QuadPart);
}

/* Advance the virtual clock to "now" (real-time mode only; monotonic). */
static void vtime_refresh(void) {
    pace_setup();
    if (s_pace_on) {
        uint64_t t = qpc_us();
        if (t > s_vtime_us) s_vtime_us = t;
    }
}

/* Sleep (host) until the virtual clock reaches target_us. Real-time mode only. */
static void sleep_until_us(uint64_t target_us) {
    for (;;) {
        uint64_t now = qpc_us();
        if (now >= target_us) break;
        uint64_t d = target_us - now;
        if (d > 2000) Sleep((DWORD)((d - 1000) / 1000));
        else if (d > 300) Sleep(1);
        /* else spin out the remainder via the loop */
    }
    vtime_refresh();
}

/* Pace vblank delivery to the PSP's real ~59.94 Hz. Without this, vblanks fire whenever
 * the scheduler goes idle, so game speed becomes "however fast the GE renders": apps that
 * flip once per vblank were rescued by gui_present's 60 Hz sleep, but apps that wait 2
 * vblanks per flip (30 fps games, some menus) ran at double speed once the GPU rasterizer
 * made the GE fast. Pacing the vblank itself makes every wait ratio correct.
 * SR_NOVBPACE=1 disables (turbo / old behaviour). */
static void vblank_pace(void) {
    pace_update_mode();
    if (!s_pace_on) { s_vtime_us += 16667; return; }   /* turbo: virtual frame per vblank */
    double period = (double)s_qfreq.QuadPart / 59.94;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (s_vbl_next > (double)now.QuadPart) {
        double ms = (s_vbl_next - (double)now.QuadPart) * 1000.0 / (double)s_qfreq.QuadPart;
        if (ms > 2.0) Sleep((DWORD)(ms - 1.0));
        do { QueryPerformanceCounter(&now); } while ((double)now.QuadPart < s_vbl_next);
    }
    s_vbl_next += period;
    /* fell behind by more than a frame (slow scene, breakpoint): resync, don't fast-forward */
    if ((double)now.QuadPart > s_vbl_next) s_vbl_next = (double)now.QuadPart;
    vbl_next_us_sync();
    vtime_refresh();
}

/* Microseconds of virtual time until the next vblank is due (0 when overdue). */
static uint64_t vblank_due_us(void) {
    pace_update_mode();
    if (!s_pace_on) return 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if ((double)now.QuadPart >= s_vbl_next) return 0;
    return (uint64_t)((s_vbl_next - (double)now.QuadPart) * 1000000.0 / (double)s_qfreq.QuadPart);
}

/* One vblank: pace to real time, release sceDisplayWaitVblankStart waiters, run every enabled
 * vblank sub-interrupt handler (they usually wake the game thread), then any GE interrupt those
 * handlers raised (KZL's calls sceGeContinue). */
static void deliver_vblank(void) {
    vblank_pace();
    s_vbl_count++;
    static unsigned long long vb = 0;
    if (getenv("SR_VBLOG") && (++vb % 1) == 0) fprintf(stderr, "vblank #%llu\n", vb);
    sched_wake(VBLANK_WAIT_OBJ);
    if (getenv("SR_PCSAMPLE")) {
        static unsigned long n = 0;
        if ((n++ % 30) == 0) fprintf(stderr, "PCSAMPLE frame=%lu interrupted_pc=0x%08x ra=0x%08x\n",
                                     n, s_cpu->pc, s_cpu->r[31]);
    }
    sr_intr_run_vblank(s_cpu);
    sr_ge_run_interrupts(s_cpu);
    extern void sr_vblank_tick(void);
    sr_vblank_tick();
}

static void free_stack(TCB *t) {
    if (!t->stack_start) return;
    if (t->attr & THREAD_ATTR_CLEAR_STACK)
        for (uint32_t i = 0; i < t->stack_size; i++) MEM_W8(t->stack_start + i, 0);
    sr_ba_free(sr_kmem_user(), t->stack_start);
    t->stack_start = 0;
}

/* Frees the thread's stack and its slot. The fiber of a thread deleting itself is still running,
 * so the scheduler loop deletes it once the thread has switched away. */
static void delete_tcb(TCB *t, int is_current) {
    if (getenv("SR_THLOG")) fprintf(stderr, "delete thread uid=0x%x %s\n", t->uid, t->name);
    sr_unregister_uid(t->uid);
    free_stack(t);
    t->state = TH_DORMANT;
    t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
    t->deleted = 1;
    if (!is_current && t->fiber) { DeleteFiber(t->fiber); t->fiber = NULL; }
}

static void CALLBACK fiber_proc(void *param) {
    TCB *t = (TCB *)param;
    for (;;) {
        /* Registers, args and ra were set when the thread was started (start_reset). */
        if (t->hle_entry)
            t->hle_entry(t->hle_arg);
        else
            dispatch(s_cpu, t->entry);    /* runs until the thread returns or exits */
        /* Returned without calling sceKernelExitThread: treat as exit. */
        t->exit_status = (int32_t)s_cpu->r[2];
        t->state = TH_DORMANT;
        sched_wake(t->uid);           /* release threads in sceKernelWaitThreadEnd on this thread */
        if (t->delete_on_exit) delete_tcb(t, 1);
        SwitchToFiber(s_sched_fiber); /* back to scheduler; fiber may be reused if restarted */
    }
}

/* PPSSPP PSPThreadContext::reset: the register file every PSP thread starts with (F-048). */
static void context_reset(CpuState *c) {
    memset(c, 0, sizeof(*c));
    for (int i = 1; i < 32; i++) c->r[i] = 0xDEADBEEFu;
    for (int i = 0; i < 32; i++) c->fi[i] = 0x7f800001u;
    for (int i = 0; i < 128; i++) c->vi[i] = 0x7f800001u;
    c->vfpuCtrl[0] = 0xe4;          /* S prefix: neutral */
    c->vfpuCtrl[1] = 0xe4;          /* T prefix: neutral */
    c->vfpuCtrl[2] = 0;             /* D prefix */
    c->vfpuCtrl[3] = 0x3f;          /* CC */
    c->vfpuCtrl[7] = 0x7772ceab;    /* REV */
    c->vfpuCtrl[8] = 0x3f800001; c->vfpuCtrl[9] = 0x3f800002;
    c->vfpuCtrl[10] = 0x3f800004; c->vfpuCtrl[11] = 0x3f800008;
    c->vfpuCtrl[12] = c->vfpuCtrl[13] = c->vfpuCtrl[14] = c->vfpuCtrl[15] = 0x3f800000;
    c->fcr31 = 0x00000e00;
    c->hi = c->lo = 0xDEADBEEFu;
}

/* What sceKernelStartThread does to a thread (PPSSPP __KernelResetThread, FillStack and
 * __KernelStartThread): reset the registers, fill the stack with 0xFF, build the 256-byte k0
 * block at the top, copy the argument block below it, then reserve 64 bytes holding the
 * thread-return stub that ra points at. */
static void start_reset(TCB *t, uint32_t arglen, uint32_t argp) {
    CpuState *c = &t->saved;
    context_reset(c);
    c->pc = t->entry;
    c->r[28] = t->gp;
    if (!(t->attr & THREAD_ATTR_NO_FILLSTACK))
        for (uint32_t i = 0; i < t->stack_size; i++) MEM_W8(t->stack_start + i, 0xFF);
    uint32_t sp = t->stack_start + t->stack_size - 256;
    uint32_t k0 = sp;
    for (uint32_t i = 0; i < 0x100; i++) MEM_W8(k0 + i, 0);
    MEM_W32(k0 + 0xc0, t->uid);
    MEM_W32(k0 + 0xc8, t->stack_start);
    MEM_W32(k0 + 0xf8, 0xFFFFFFFFu);
    MEM_W32(k0 + 0xfc, 0xFFFFFFFFu);
    MEM_W32(t->stack_start, t->uid);
    c->r[26] = k0;
    if (argp && (int32_t)arglen > 0) {
        sp -= (arglen + 0xF) & ~0xFu;
        c->r[4] = arglen;
        c->r[5] = sp;
        if (sr_inrange(argp)) memmove(SR_HOST(sp), SR_HOST(argp), arglen);
    } else {
        c->r[4] = 0;
        c->r[5] = 0;
    }
    sp -= 64;
    MEM_W32(sp + 0, 0x03E00008u);   /* jr ra */
    MEM_W32(sp + 4, 0x0000000Cu);   /* syscall (PPSSPP: its thread-return HLE call) */
    MEM_W32(sp + 8, 0x1000FFFFu);   /* b -1 */
    MEM_W32(sp + 12, 0);            /* nop */
    c->r[29] = sp;
    c->r[30] = sp;
    c->r[31] = sp;
    t->exit_status = (int32_t)0x800201a4u;   /* NOT_DORMANT until the thread exits */
}

/* A free thread slot: a deleted one whose fiber is gone, or a new one. */
static TCB *tcb_slot(void) {
    for (int i = 0; i < s_ntcb; i++) if (s_tcb[i].deleted && !s_tcb[i].fiber) return &s_tcb[i];
    if (s_ntcb >= MAXTHREADS) return NULL;
    return &s_tcb[s_ntcb++];
}

/* sceKernelCreateThread. The stack comes from the user partition, from the top unless the
 * thread asks for a low stack, exactly as PPSSPP allocates it (PSPThread::AllocateStack). */
uint32_t sched_create_thread(const char *name, uint32_t entry, int priority, uint32_t stack_size,
                             uint32_t attr) {
    if (stack_size < 0x200) return 0x80020194u;                  /* ILLEGAL_STACK_SIZE */
    if (priority < 0x08 || priority > 0x77) return 0x80020193u;  /* ILLEGAL_PRIORITY */
    TCB *t = tcb_slot();
    if (!t) {
        fprintf(stderr, "sched_create_thread: MAXTHREADS(%d) exhausted (entry=0x%08x)\n", MAXTHREADS, entry);
        return 0x80020190u;                                      /* NO_MEMORY */
    }
    int fresh = (t == &s_tcb[s_ntcb - 1]) && !t->deleted;
    uint32_t uid = sr_alloc_uid_type(SR_TMID_THREAD);   /* PPSSPP creates the object before the stack */
    char tag[40];
    snprintf(tag, sizeof(tag), "stack/%s", name);
    uint32_t size = stack_size;
    uint32_t start = sr_ba_alloc(sr_kmem_user(), &size, !(attr & THREAD_ATTR_LOW_STACK), tag);
    if (start == SR_BA_FAIL) {
        fprintf(stderr, "sched_create_thread: no memory for a 0x%x stack (%s)\n", stack_size, name);
        sr_unregister_uid(uid);
        if (fresh) s_ntcb--;                                     /* give the new slot back */
        return 0x80020190u;                                      /* NO_MEMORY */
    }
    memset(t, 0, sizeof(*t));
    t->uid = uid;
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->attr = attr;
    t->gp = (s_cur >= 0 && s_cpu) ? s_cpu->r[28] : s_gp;
    t->state = TH_DORMANT;
    t->priority = priority;
    t->initial_priority = priority;
    t->exit_status = (int32_t)0x800201a4u;
    t->entry = entry;
    t->stack_start = start;
    t->stack_size = size;
    if (getenv("SR_THLOG")) fprintf(stderr, "create thread uid=0x%x %s entry=0x%08x pri=%d stack=%08x-%08x\n",
                                    uid, t->name, entry, priority, start, start + size);
    return uid;
}

void sched_set_thread_gp(uint32_t uid, uint32_t gp) {
    TCB *t = tcb_by_uid(uid);
    if (!t || t->state != TH_DORMANT) {
        fprintf(
            stderr,
            "sched_set_thread_gp: thread 0x%x is missing or already running\n",
            uid);
        abort();
    }
    t->gp = gp;
}

uint32_t sched_create_hle_thread(const char *name, int priority, uint32_t stack_size,
                                 SrHleThreadFn entry, uint32_t arg) {
    uint32_t uid = sched_create_thread(name, 0, priority, stack_size, 0);
    if ((int32_t)uid < 0) return uid;
    TCB *t = tcb_by_uid(uid);
    t->hle_entry = entry;
    t->hle_arg = arg;
    uint32_t result = sched_start_thread(uid, 0, 0);
    if ((int32_t)result < 0) {
        delete_tcb(t, 0);
        return result;
    }
    return uid;
}

int sched_delete_hle_thread(uint32_t uid) {
    TCB *t = tcb_by_uid(uid);
    if (!t) return (int)0x80020198u;
    if (!t->hle_entry || uid == sched_current_uid()) return (int)0x800201a4u;
    delete_tcb(t, 0);
    return 0;
}

/* sceKernelStartThread: 0, or a kernel error for an unknown or running thread. */
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp) {
    TCB *t = tcb_by_uid(uid);
    if (!t) return 0x80020198u;                                  /* UNKNOWN_THID */
    if (t->state != TH_DORMANT) return 0x800201a4u;              /* NOT_DORMANT */
    if (getenv("SR_THLOG")) fprintf(stderr, "start thread uid=0x%x entry=0x%08x pri=%d arglen=%u%s\n",
                                    t->uid, t->entry, t->priority, arglen, t->started ? " (restart)" : "");
    /* PSP semantics: starting a DORMANT thread that ran before restarts it from its entry.
     * The old fiber is parked wherever the thread last gave up the CPU -- for a thread that
     * exited via sceKernelExitThread that is deep inside the exit syscall, so resuming it
     * would fall through past the exit into garbage (this stranded the BGM streamer thread
     * and with it the mission scene-switch fade). Throw the old fiber away so the scheduler
     * re-enters the body fresh. */
    if (t->started) {
        if (t->fiber) { DeleteFiber(t->fiber); t->fiber = NULL; }
        t->started = 0;
        t->wakeups = 0; t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
    }
    start_reset(t, arglen, argp);
    t->arglen = arglen; t->argp = argp;
    t->state = TH_READY;
    return 0;
}

/* sceKernelDeleteThread: only a dormant thread other than the caller. */
int sched_delete_thread(uint32_t uid) {
    if (uid == 0 || uid == sched_current_uid()) return (int)0x800201a4u;   /* NOT_DORMANT */
    TCB *t = tcb_by_uid(uid);
    if (!t) return (int)0x80020198u;                                      /* UNKNOWN_THID */
    if (t->state != TH_DORMANT) return (int)0x800201a4u;
    delete_tcb(t, 0);
    return 0;
}

/* sceKernelExitDeleteThread: exit, free the stack, and give the slot back. */
void sched_exit_delete_current(int32_t status) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    t->exit_status = status;
    t->state = TH_DORMANT;
    sched_wake(t->uid);          /* release threads in sceKernelWaitThreadEnd on this thread */
    delete_tcb(t, 1);
    switch_to_scheduler();
}

/* Pick the highest-priority runnable thread (lowest priority number). Wakes delayed threads
 * whose deadline has passed. Returns an index or -1 if nothing is runnable. */
static int pick_next(void) {
    for (int i = 0; i < s_ntcb; i++)
        if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
            s_vtime_us >= s_tcb[i].wake)
            s_tcb[i].state = TH_READY;   /* delay expired, or a timed wait timed out */
    int best = -1;
    for (int i = 0; i < s_ntcb; i++) {
        if (s_tcb[i].state != TH_READY) continue;
        if (best < 0 || s_tcb[i].priority < s_tcb[best].priority) best = i;
    }
    return best;
}

/* Save the running thread's registers, return to the scheduler, which selects and resumes the
 * next thread. Called from a thread fiber. */
static void switch_to_scheduler(void) {
    SwitchToFiber(s_sched_fiber);
}

void sr_yield(CpuState *s) {
    sr_timeslice = TIMESLICE;
    if (s_cur < 0) return;                 /* not in a thread */
    s_tick++;
    vtime_refresh();                       /* so a busy thread sees siblings' delays expire */
    /* The PSP vblank is an INTERRUPT: it fires even while threads compute. Only delivering it
     * from the scheduler's idle loop deadlocks any code that polls vblank-derived state
     * (sceDisplayGetVcount spin-pacing, two threads ping-ponging on each other) -- the vcount
     * can never advance because nobody ever blocks. Deliver an overdue vblank from the yield
     * path too; deliver_vblank saves/restores the live register file and runs the handler with
     * s_cur = -1, so it is interrupt-safe here. */
    if (!s_pace_on || s_vtime_us >= s_vbl_next_us)
        deliver_vblank();
    TCB *t = &s_tcb[s_cur];
    /* Only switch if someone else could run; otherwise keep going (avoids pointless churn). */
    int other = 0;
    for (int i = 0; i < s_ntcb; i++)
        if (i != s_cur && (s_tcb[i].state == TH_READY ||
            ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
             s_vtime_us >= s_tcb[i].wake))) { other = 1; break; }
    if (!other) {
        /* Diagnostic: if one thread spins with nobody else runnable for a long time, it is a
         * deadlock (a producer thread exited or is blocked). Dump the thread table with each
         * thread's saved PC and wait object; repeat occasionally so later phases are visible. */
        static unsigned long long spun = 0;
        static int dumps = 0;
        if (++spun % 50000000ull == 2001 && dumps < 6) {
            dumps++;
            static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
            int ready = 0, delay = 0, dormant = 0, run = 0;
            for (int i = 0; i < s_ntcb; i++) {
                if (s_tcb[i].state == TH_READY) ready++;
                else if (s_tcb[i].state == TH_WAIT_DELAY) delay++;
                else if (s_tcb[i].state == TH_DORMANT) dormant++;
                else run++;
            }
            fprintf(stderr, "sched: spin on uid 0x%x at pc=0x%08x ra=0x%08x; threads=%d ready=%d delay=%d dormant=%d running=%d\n",
                    t->uid, s->pc, s->r[31], s_ntcb, ready, delay, dormant, run);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %-10s prio %d pc=0x%08x ra=0x%08x wait_obj=0x%x wakeups=%d\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state < 5 ? s_tcb[i].state : 0],
                        s_tcb[i].priority, s_tcb[i].saved.pc, s_tcb[i].saved.r[31],
                        s_tcb[i].wait_obj, s_tcb[i].wakeups);
        }
        return;
    }
    memcpy(&t->saved, s, sizeof(CpuState));
    if (t->state == TH_RUNNING) t->state = TH_READY;
    switch_to_scheduler();
    /* resumed later: our registers were restored into *s by the scheduler before SwitchToFiber */
}

/* PSP scheduling is strict-priority preemptive: the moment a higher-priority thread becomes
 * ready (e.g. sceKernelStartThread starts one), it runs instead of the current thread. Without
 * this, a low-priority boot thread that starts a high-priority worker and busy-waits on its
 * output would never let the worker run. Call after any op that readies a thread. */
void sched_preempt(void) {
    if (s_cur < 0) return;
    TCB *cur = &s_tcb[s_cur];
    int best = -1;
    for (int i = 0; i < s_ntcb; i++) {
        if (i == s_cur) continue;
        if (s_tcb[i].state == TH_READY && (best < 0 || s_tcb[i].priority < s_tcb[best].priority))
            best = i;
    }
    if (best >= 0 && s_tcb[best].priority < cur->priority) {   /* strictly higher priority ready */
        memcpy(&cur->saved, s_cpu, sizeof(CpuState));
        cur->state = TH_READY;
        switch_to_scheduler();
    }
}

void sched_delay_current(uint32_t usec) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (usec > 2000000u && getenv("SR_DELAYLOG"))   /* > 2s: catch a bogus huge delay */
        fprintf(stderr, "BIG DELAY uid=0x%x entry=0x%08x usec=%u (%.1fs)\n", t->uid, t->entry, usec, usec / 1e6);
    /* Real microseconds of virtual time; any positive delay yields at least once. */
    vtime_refresh();
    uint64_t deadline = s_vtime_us + (usec ? usec : 1);
    do {
        memcpy(&t->saved, s_cpu, sizeof(CpuState));
        t->state = TH_WAIT_DELAY;
        t->wake = deadline;
        switch_to_scheduler();
    } while (cb_wake_handled(t) && (vtime_refresh(), s_vtime_us < deadline));
}

/* Callbacks during a ...CB wait. A thread waiting with callbacks enabled runs its notified
 * callbacks when the wait starts, and is woken (cb_woken) to run any notified during the wait.
 * sched_block_on/_timeout then return without the object being signalled, so their callers
 * re-check their condition (they all loop); delays and sleeps resume waiting themselves. */
int sr_cb_run_pending(CpuState *s, uint32_t thread_uid);
int sr_cb_pending(uint32_t thread_uid);

void sched_cb_wait(int on) {
    if (s_cur < 0) return;
    s_tcb[s_cur].cb_wait = on;
    if (on) sr_cb_run_pending(s_cpu, s_tcb[s_cur].uid);
}

void sched_cb_notify_thread(uint32_t uid) {
    TCB *t = tcb_by_uid(uid);
    if (!t || !t->cb_wait) return;
    if (t->state == TH_WAIT_OBJ || t->state == TH_WAIT_DELAY) {
        t->cb_woken = 1;
        t->state = TH_READY;
    }
}

/* After a wait returns: run callbacks if the thread was woken for them. 1 if it was. */
static int cb_wake_handled(TCB *t) {
    if (!t->cb_woken) return 0;
    t->cb_woken = 0;
    sr_cb_run_pending(s_cpu, t->uid);
    return 1;
}

void sched_block_on(uint32_t obj) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    if (t->cb_wait && sr_cb_run_pending(s_cpu, t->uid)) return;
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wake = (uint64_t)-1;     /* infinite: only sched_wake releases it */
    switch_to_scheduler();
    cb_wake_handled(t);
}

/* Block on obj, but also wake after usec of virtual time (a timed sema/event wait). Returns 1
 * if it timed out (the deadline passed), 0 if it was woken by a signal. */
int sched_block_on_timeout(uint32_t obj, uint32_t usec) {
    if (s_cur < 0) return 1;
    TCB *t = &s_tcb[s_cur];
    vtime_refresh();
    uint64_t deadline = s_vtime_us + (usec ? usec : 1);
    if (t->cb_wait && sr_cb_run_pending(s_cpu, t->uid)) return 0;
    memcpy(&t->saved, s_cpu, sizeof(CpuState));
    t->state = TH_WAIT_OBJ;
    t->wait_obj = obj;
    t->wake = deadline;
    switch_to_scheduler();
    if (cb_wake_handled(t)) return 0;
    vtime_refresh();
    return s_vtime_us >= deadline;   /* resumed: timed out if the deadline has passed */
}

int sched_wake(uint32_t obj) {
    int n = 0;
    for (int i = 0; i < s_ntcb; i++)
        if (s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == obj) {
            s_tcb[i].state = TH_READY;
            n++;
        }
    return n;
}

/* sceKernelSleepThread[CB]: PSP wakeup-count semantics. If a wakeup is already pending, consume it
 * and return without blocking; otherwise block until sceKernelWakeupThread targets this thread.
 * This is distinct from sceKernelDelayThread (a timed sleep) -- conflating the two left the main
 * thread sleeping ~forever on a poisoned (0xDEADBEEF) delay argument. */
void sched_thread_sleep(void) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    for (;;) {
        if (t->wakeups > 0) { t->wakeups--; return; }   /* pending wakeup: don't block */
        t->sleeping = 1;
        memcpy(&t->saved, s_cpu, sizeof(CpuState));
        t->state = TH_WAIT_OBJ;
        t->wait_obj = t->uid;   /* sleep marker: woken only by sched_thread_wakeup(uid) */
        t->wake = (uint64_t)-1;
        switch_to_scheduler();
        if (!cb_wake_handled(t)) return;
    }
}

/* sceKernelWakeupThread(uid): wake a sleeping thread, or bank a pending wakeup if it is not
 * currently asleep (so a wakeup issued before the sleep is not lost). */
void sched_thread_wakeup(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    for (int i = 0; i < s_ntcb; i++) if (s_tcb[i].uid == uid) {
        if (s_tcb[i].sleeping && s_tcb[i].state == TH_WAIT_OBJ && s_tcb[i].wait_obj == uid) {
            s_tcb[i].sleeping = 0;
            s_tcb[i].state = TH_READY;
        } else {
            s_tcb[i].wakeups++;
        }
        return;
    }
}

/* sceKernelCancelWakeupThread(uid): returns the number of pending wakeups and clears them.
 * Passing uid 0 targets the current thread; ACX does this once per frame before sleeping. */
int sched_thread_cancel_wakeup(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return -1;
    int old = t->wakeups;
    t->wakeups = 0;
    return old;
}

static uint32_t psp_thread_status(const TCB *t) {
    switch (t->state) {
        case TH_RUNNING: return PSP_THREAD_RUNNING;
        case TH_READY: return PSP_THREAD_READY;
        case TH_WAIT_DELAY:
        case TH_WAIT_OBJ: return PSP_THREAD_WAITING;
        case TH_DORMANT:
        default: return PSP_THREAD_STOPPED;
    }
}

static uint32_t psp_wait_type(const TCB *t) {
    if (t->state == TH_WAIT_DELAY) return PSP_WAIT_DELAY;
    if (t->state == TH_WAIT_OBJ && t->sleeping && t->wait_obj == t->uid) return PSP_WAIT_SLEEP;
    if (t->state == TH_WAIT_OBJ) return PSP_WAIT_OBJECT;
    return PSP_WAIT_NONE;
}

int sched_thread_run_status(uint32_t uid, SrThreadRunStatus *out) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->size = 0x2c;
    out->status = psp_thread_status(t);
    out->currentPriority = (uint32_t)t->priority;
    out->waitType = psp_wait_type(t);
    out->waitId = (t->state == TH_WAIT_DELAY) ? uid : t->wait_obj;
    out->wakeupCount = (uint32_t)t->wakeups;
    out->runClocksLow = (uint32_t)s_tick;
    out->runClocksHigh = (uint32_t)(s_tick >> 32);
    return 0;
}

int sched_thread_status(uint32_t uid, SrThreadStatus *out) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t || !out) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->name, t->name, sizeof(out->name));
    out->attr = t->attr;
    out->status = psp_thread_status(t);
    out->entry = t->entry;
    out->stack = t->stack_start;
    out->stackSize = t->stack_size;
    out->gpReg = t->gp;
    out->initialPriority = t->initial_priority;
    out->currentPriority = t->priority;
    out->waitType = psp_wait_type(t);
    out->waitId = (t->state == TH_WAIT_DELAY) ? uid : t->wait_obj;
    out->wakeupCount = t->wakeups;
    out->exitStatus = t->exit_status;
    out->runClocksLow = (uint32_t)s_tick;
    out->runClocksHigh = (uint32_t)(s_tick >> 32);
    return 0;
}

int32_t sched_thread_exit_status(uint32_t uid) {
    uid = resolve_thread_uid(uid);
    TCB *t = tcb_by_uid(uid);
    if (!t) return (int32_t)0x80020198u;   /* UNKNOWN_THID */
    if (t->state != TH_DORMANT) return (int32_t)0x800201a4u;
    return t->exit_status;
}

uint32_t sched_thread_id_list(uint32_t type, uint32_t *out, uint32_t capacity) {
    uint32_t total = 0;
    for (int i = 0; i < s_ntcb; i++) {
        TCB *t = &s_tcb[i];
        if (t->deleted) continue;
        int include = type == 64 ? (t->state == TH_WAIT_OBJ && t->sleeping && t->wait_obj == t->uid) :
                      type == 65 ? t->state == TH_WAIT_DELAY :
                      type == 66 ? 0 :
                      type == 67 ? t->state == TH_DORMANT : 0;
        if (include) {
            if (out && total < capacity) out[total] = t->uid;
            total++;
        }
    }
    return total;
}

void sched_exit_current(int32_t status) {
    if (s_cur < 0) return;
    TCB *t = &s_tcb[s_cur];
    uint32_t uid = t->uid;
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x EXIT (entry 0x%08x)\n", uid, s_tcb[s_cur].entry);
    t->exit_status = status;
    t->state = TH_DORMANT;
    sched_wake(uid);             /* release threads in sceKernelWaitThreadEnd on this thread */
    switch_to_scheduler();
}

int sched_current_priority(void) { return s_cur >= 0 ? s_tcb[s_cur].priority : 32; }

/* sceKernelChangeThreadPriority: uid 0 = current thread. */
void sched_set_priority(uint32_t uid, int priority) {
    if (uid == 0 && s_cur >= 0) uid = s_tcb[s_cur].uid;
    TCB *t = tcb_by_uid(uid);
    if (t) t->priority = priority;
}

/* sceKernelTerminateDeleteThread: force another thread dormant so a later start restarts it from
 * its entry (the restart path resets registers/fiber). Terminating yourself exits. The old h_ok
 * stub left the "killed" thread running. */
void sched_terminate_thread(uint32_t uid) {
    if (s_cur >= 0 && s_tcb[s_cur].uid == uid) { sched_exit_current((int32_t)0x800201acu); return; }
    TCB *t = tcb_by_uid(uid);
    if (!t || t->state == TH_DORMANT) return;
    if (getenv("SR_SYSLOG")) fprintf(stderr, "thr 0x%x TERMINATED (entry 0x%08x)\n", uid, t->entry);
    t->exit_status = (int32_t)0x800201acu;
    t->state = TH_DORMANT;
    t->sleeping = 0; t->wait_obj = 0; t->wake = 0;
    sched_wake(uid);             /* release threads in sceKernelWaitThreadEnd on this thread */
}

int sched_is_dormant(uint32_t uid) {
    for (int i = 0; i < s_ntcb; i++) if (s_tcb[i].uid == uid) return s_tcb[i].state == TH_DORMANT;
    return 1;   /* unknown thread: treat as ended */
}

/* The scheduler loop. Runs on the main (converted) fiber. Creates the entry thread, then keeps
 * resuming the highest-priority ready thread until none remain runnable. */
void sched_run(uint32_t entry, uint32_t arglen, uint32_t argp, int create_start_thread) {
    /* A differential run adopts the oracle's module_start thread from its seeded k0 block.
     * Standalone boot creates the same 0x400-byte high stack through start_reset(). */
    TCB *t0 = tcb_slot();
    memset(t0, 0, sizeof(*t0));
    snprintf(t0->name, sizeof(t0->name), "module_start");
    t0->priority = SR_MODULE_START_PRIORITY;
    t0->initial_priority = SR_MODULE_START_PRIORITY;
    t0->exit_status = (int32_t)0x800201a4u;
    t0->attr = SR_MODULE_START_ATTR;
    t0->gp = s_gp;
    t0->entry = entry;
    t0->delete_on_exit = 1;
    uint32_t k0 = s_cpu->r[26];
    uint32_t base = sr_inrange(k0) ? MEM_R32(k0 + 0xc8) : 0;
    uint32_t seed_uid = sr_inrange(k0) ? MEM_R32(k0 + 0xc0) : 0;
    if (!create_start_thread) {
        if (!base || base >= k0 || k0 + 0x100 - base > 0x100000 || seed_uid < 0x100) {
            fprintf(stderr, "sched_run: invalid seeded module_start stack\n");
            abort();
        }
        uint32_t stack_size = k0 + 0x100 - base;
        if (sr_ba_alloc_at(sr_kmem_user(), base, stack_size, "stack/module_start") != base) {
            fprintf(stderr, "sched_run: seeded module_start stack is unavailable\n");
            abort();
        }
        t0->uid = seed_uid;
        sr_register_uid_type(t0->uid, SR_TMID_THREAD);
        t0->stack_start = base;
        t0->stack_size = stack_size;
        memcpy(&t0->saved, s_cpu, sizeof(CpuState));
    } else {
        uint32_t size = SR_MODULE_START_STACK_SIZE;
#ifdef SR_MODULE_START_UID
        t0->uid = SR_MODULE_START_UID;
        sr_register_uid_type(t0->uid, SR_TMID_THREAD);
#else
        t0->uid = sr_alloc_uid_type(SR_TMID_THREAD);
#endif
        uint32_t start = sr_ba_alloc(sr_kmem_user(), &size, 1, "stack/module_start");
        if (start == SR_BA_FAIL) {
            fprintf(stderr, "sched_run: no memory for module_start stack\n");
            abort();
        }
        t0->stack_start = start;
        t0->stack_size = size;
        start_reset(t0, arglen, argp);
        t0->saved.r[31] = 0x08000030u;
        fprintf(stderr,
                "standalone module_start: uid=0x%x stack=%08x-%08x k0=%08x sp=%08x "
                "a0=%u a1=%08x ra=%08x\n",
                t0->uid, t0->stack_start, t0->stack_start + t0->stack_size,
                t0->saved.r[26], t0->saved.r[29], t0->saved.r[4], t0->saved.r[5],
                t0->saved.r[31]);
    }
    if (getenv("SR_THLOG")) fprintf(stderr, "entry thread uid=0x%x stack=%08x-%08x\n",
                                    t0->uid, t0->stack_start, t0->stack_start + t0->stack_size);
    t0->arglen = t0->saved.r[4]; t0->argp = t0->saved.r[5];
    t0->state = TH_READY;

    static const char *stn[] = {"DORMANT", "READY", "RUNNING", "WAIT_DELAY", "WAIT_OBJ"};
    unsigned long long iters = 0;
    for (;;) {
        if (getenv("SCHED_DUMP") && (++iters % 400000) == 0) {
            fprintf(stderr, "--- sched dump (tick=%llu) ---\n", (unsigned long long)s_tick);
            for (int i = 0; i < s_ntcb; i++)
                fprintf(stderr, "  uid 0x%x entry 0x%08x %s prio %d wait_obj 0x%x\n",
                        s_tcb[i].uid, s_tcb[i].entry, stn[s_tcb[i].state], s_tcb[i].priority, s_tcb[i].wait_obj);
        }
        int idx = pick_next();
        if (idx < 0) {
            /* No thread is ready. If a timed wait expires before the next vblank is due,
             * sleep precisely to it (sub-frame delays keep their real duration); otherwise
             * deliver a VBLANK interrupt (real-time paced) -- the per-frame heartbeat that
             * drives the render loop. Vblank delivery can't starve: it runs whenever due. */
            uint64_t soonest = (uint64_t)-1;
            for (int i = 0; i < s_ntcb; i++)
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake < soonest) soonest = s_tcb[i].wake;
            vtime_refresh();
            if (s_pace_on && soonest != (uint64_t)-1 && soonest > s_vtime_us &&
                soonest - s_vtime_us < vblank_due_us())
                sleep_until_us(soonest);
            else
                deliver_vblank();
            idx = pick_next();
        }
        if (idx < 0) {
            /* Still nothing. Stop only when nothing is even waiting on a deadline. */
            uint64_t soonest = (uint64_t)-1;
            for (int i = 0; i < s_ntcb; i++)
                if ((s_tcb[i].state == TH_WAIT_DELAY || s_tcb[i].state == TH_WAIT_OBJ) &&
                    s_tcb[i].wake < soonest) soonest = s_tcb[i].wake;
            if (soonest == (uint64_t)-1) {
                if (getenv("SCHED_DUMP")) sched_dump_threads();
                break;   /* truly nothing runnable (all infinite waits) */
            }
            if (!s_pace_on) {                     /* turbo: jump the clock over the wait */
                if (soonest > s_vtime_us) s_vtime_us = soonest;
                idx = pick_next();
                if (idx < 0) break;
            } else {
                continue;   /* paced: keep delivering vblanks; real time reaches the deadline */
            }
        }
        TCB *t = &s_tcb[idx];
        s_cur = idx;
        t->state = TH_RUNNING;
        memcpy(s_cpu, &t->saved, sizeof(CpuState));   /* load this thread's registers */
        sr_timeslice = TIMESLICE;          /* a fresh slice for this run (the counter is global) */
        if (!t->started) {
            t->started = 1;
            /* Guest calls become native C calls, so a deep guest call chain needs a deep host
             * stack. Reserve a large fiber stack (committed on demand) to match. */
            t->fiber = CreateFiberEx((SIZE_T)1 << 18, (SIZE_T)64 << 20, 0, fiber_proc, t);
        }
        SwitchToFiber(t->fiber);           /* run until it yields/blocks/exits */
        if (t->deleted && t->fiber) { DeleteFiber(t->fiber); t->fiber = NULL; }   /* deleted itself */
        s_cur = -1;
        s_tick++;
    }
}
