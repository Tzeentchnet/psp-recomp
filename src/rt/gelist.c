/* sceGe display lists, ported from PPSSPP GPU/GPUCommon.cpp (list manager) and
 * Core/HLE/sceGe.cpp (HLE calls and the GE interrupt handler), v1.20.4.
 *
 * Up to 64 display lists. A list runs from its pc until it reaches its stall address (the game
 * moves the stall forward with sceGeListUpdateStallAddr as it writes commands) or a SIGNAL/FINISH
 * followed by END:
 *   - SIGNAL + END: the signal behaviour in the SIGNAL word decides. HANDLER_SUSPEND/CONTINUE
 *     raise a GE interrupt that runs the sub-interrupt handler sceGeSetCallback registered
 *     (signal_func) with a0 = the signal token, and the list resumes once it returns. KZL's libgu
 *     uses this to set its "SceGuSignal" event flag. The JUMP/CALL/RET behaviours change the pc.
 *   - FINISH + END: the list is done. A GE interrupt runs finish_func, and the list then counts
 *     as completed, which releases sceGeListSync waiters.
 *   - END after anything else does nothing.
 * The list ends DrawSync waits when the queue drains.
 *
 * Differences from PPSSPP, all from the runtime not modelling GE timing: a list completes the
 * moment its commands have run (PPSSPP adds an estimated duration), and a GE interrupt is
 * delivered when the sceGe call that raised it returns (or, if raised inside another interrupt
 * handler, when that handler returns). PPSSPP schedules both a few microseconds later.
 * The saved context of PspGeListArgs is kept on the host, not written to guest memory.
 *
 * SR_GELOG=1 prints PPSSPP-style lines (list start/resume, enqueue, signals, interrupts).
 * SR_DLDUMP=<vblank> [SR_DLDUMP_COUNT=<n>, default 1] writes every GE command executed during
 * those frames to gedump_f<vblank>.txt: "<pc> <op>" per command, with a "# list ..." line each
 * time a list starts or resumes and "# end ..." when it stops.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "ge_shared.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

GeState *ge_state_ptr(void);

#define LIST_ID_MAGIC 0x35000000u
#define MAX_LISTS     64

enum { DL_NONE = 0, DL_QUEUED = 1, DL_RUNNING = 2, DL_COMPLETED = 3, DL_PAUSED = 4 };
enum { LIST_COMPLETED = 0, LIST_QUEUED = 1, LIST_DRAWING = 2, LIST_STALLING = 3, LIST_PAUSED = 4 };
enum { GPU_RUNNING = 0, GPU_DONE = 1, GPU_STALL = 2, GPU_INTERRUPT = 3, GPU_ERROR = 4 };
enum {
    SIG_NONE = 0x00, SIG_HANDLER_SUSPEND = 0x01, SIG_HANDLER_CONTINUE = 0x02, SIG_HANDLER_PAUSE = 0x03,
    SIG_SYNC = 0x08, SIG_JUMP = 0x10, SIG_CALL = 0x11, SIG_RET = 0x12, SIG_RJUMP = 0x13,
    SIG_RCALL = 0x14, SIG_OJUMP = 0x15, SIG_OCALL = 0x16,
};
enum { CMD_BOUNDINGBOX = 0x07, CMD_JUMP = 0x08, CMD_BJUMP = 0x09, CMD_CALL = 0x0A, CMD_RET = 0x0B,
       CMD_END = 0x0C, CMD_SIGNAL = 0x0E, CMD_FINISH = 0x0F };
enum { SUBINTR_SIGNAL = 0, SUBINTR_FINISH = 1 };

#define ERR_ALREADY         0x80000020u
#define ERR_BUSY            0x80000021u
#define ERR_OUT_OF_MEMORY   0x80000022u
#define ERR_PRIV_REQUIRED   0x80000023u
#define ERR_INVALID_ID      0x80000100u
#define ERR_INVALID_POINTER 0x80000103u
#define ERR_INVALID_SIZE    0x80000104u
#define ERR_INVALID_MODE    0x80000107u
#define ERR_INVALID_VALUE   0x800001FEu
#define ERR_ILLEGAL_CONTEXT 0x80020064u

/* Threads in sceGeListSync wait on GE_LIST_OBJ(id); threads in sceGeDrawSync on GE_DRAW_OBJ. */
#define GE_LIST_OBJ(id) (0x47450000u | (uint32_t)(id))   /* "GE" */
#define GE_DRAW_OBJ      0x4745FFFFu

typedef struct { uint32_t pc, offsetAddr, baseAddr; } StackEntry;

typedef struct {
    int id;
    uint32_t startpc, pc, stall;
    int state;
    int subIntrBase;
    uint32_t subIntrToken;
    StackEntry stack[32];
    int stackptr;
    int interrupted;
    int sync_pending;        /* PPSSPP waitUntilTicks == -1: ListSync must wait */
    uint32_t context;
    int signal;
    int interruptsEnabled;
    int pendingInterrupt;
    int started;
    uint32_t offsetAddr;
    int bboxResult;
    uint32_t stackAddr;
} DisplayList;

static DisplayList dls[MAX_LISTS];
static int s_queue[MAX_LISTS], s_qn = 0;      /* dlQueue, front at index 0 */
static DisplayList *s_current = NULL;
static int s_next_id = 0;
static int s_gpustate = GPU_DONE;
static int s_isbreak = 0;
static int s_draw_complete = 1;               /* PPSSPP drawCompleteTicks <= now */
static uint32_t s_cmdmem[256];
static GeState s_ctx[MAX_LISTS];              /* host copy of a list's saved context */

typedef struct { int listid; uint32_t pc, cmd; } GeIntr;
static GeIntr s_intr[256];
static int s_intr_n = 0;
static int s_intr_running = 0;

typedef struct { uint32_t signal_func, signal_arg, finish_func, finish_arg; } GeCallbackData;
static GeCallbackData s_cb_data[16];
static int s_cb_used[16];

static int s_log = -1;
static int gelog(void) { if (s_log < 0) s_log = getenv("SR_GELOG") ? 1 : 0; return s_log; }

/* ---- display-list dump (SR_DLDUMP) ---- */
static FILE *s_dump;
static uint32_t s_dump_frame = 0xFFFFFFFFu;
static FILE *dump_file(void) {
    static int first = -2, count = 1;
    if (first == -2) {
        const char *e = getenv("SR_DLDUMP");
        first = e ? atoi(e) : -1;
        const char *c = getenv("SR_DLDUMP_COUNT");
        if (c) count = atoi(c);
    }
    if (first < 0) return NULL;
    uint32_t f = ge_get_frame();
    if (f < (uint32_t)first || f >= (uint32_t)first + (uint32_t)count) {
        if (s_dump) { fclose(s_dump); s_dump = NULL; }
        return NULL;
    }
    if (f != s_dump_frame) {
        if (s_dump) fclose(s_dump);
        char path[64];
        snprintf(path, sizeof path, "gedump_f%u.txt", f);
        s_dump = fopen(path, "w");
        s_dump_frame = f;
        if (s_dump) fprintf(stderr, "GE: dumping frame %u display lists to %s\n", f, path);
    }
    return s_dump;
}

/* ---- dlQueue ---- */
static int q_empty(void) { return s_qn == 0; }
static int q_front(void) { return s_queue[0]; }
static void q_push_back(int id) { if (s_qn < MAX_LISTS) s_queue[s_qn++] = id; }
static void q_push_front(int id) {
    if (s_qn >= MAX_LISTS) return;
    memmove(&s_queue[1], &s_queue[0], (size_t)s_qn * sizeof(int));
    s_queue[0] = id; s_qn++;
}
static void q_remove(int id) {
    int j = 0;
    for (int i = 0; i < s_qn; i++) if (s_queue[i] != id) s_queue[j++] = s_queue[i];
    s_qn = j;
}
static void pop_dl_queue(void) {
    if (q_empty()) return;
    memmove(&s_queue[0], &s_queue[1], (size_t)(s_qn - 1) * sizeof(int));
    s_qn--;
    if (!q_empty()) {
        int running = s_current && s_current->state == DL_RUNNING;
        s_current = &dls[q_front()];
        if (running) s_current->state = DL_RUNNING;
    } else {
        s_current = NULL;
    }
}

static int valid_addr(uint32_t a) { return a != 0 && sr_inrange(a); }

/* gstate_c.getRelativeAddress on an unmasked value (signal jump/call targets). */
static uint32_t rel_full(uint32_t data) {
    GeState *g = ge_state_ptr();
    uint32_t base_ext = ((g->base & 0x000F0000u) << 8) | data;
    return (g->offset + base_ext) & 0x0FFFFFFFu;
}

static void ctx_save(DisplayList *l) { s_ctx[l->id] = *ge_state_ptr(); }
static void ctx_restore(DisplayList *l) { *ge_state_ptr() = s_ctx[l->id]; }

/* ---- GE interrupts (__GeTriggerInterrupt) ---- */
static void trigger_interrupt(DisplayList *l) {
    if (s_intr_n >= (int)(sizeof(s_intr) / sizeof(s_intr[0]))) {
        fprintf(stderr, "GE: interrupt queue full\n");
        return;
    }
    s_intr[s_intr_n].listid = l->id;
    s_intr[s_intr_n].pc = l->pc;
    s_intr[s_intr_n].cmd = MEM_R32(l->pc - 4) >> 24;
    s_intr_n++;
}
static void intr_pop(void) {
    if (!s_intr_n) return;
    memmove(&s_intr[0], &s_intr[1], (size_t)(s_intr_n - 1) * sizeof(GeIntr));
    s_intr_n--;
}

/* ---- END (GPUCommon::Execute_End) ---- */
static void execute_end(DisplayList *l, uint32_t op) {
    const uint32_t prev = MEM_R32(l->pc - 4);
    switch (prev >> 24) {
    case CMD_SIGNAL: {
        int behaviour = (int)((prev >> 16) & 0xFF);
        uint32_t signal = prev & 0xFFFF, enddata = op & 0xFFFF;
        int trigger = 1;
        l->subIntrToken = signal;
        switch (behaviour) {
        case SIG_HANDLER_SUSPEND:
            if (g_sdk_version <= 0x02000010) l->state = DL_PAUSED;
            l->signal = behaviour;
            break;
        case SIG_HANDLER_CONTINUE:
            l->signal = behaviour;
            break;
        case SIG_HANDLER_PAUSE:
        case SIG_SYNC:
            trigger = 0;
            l->signal = behaviour;
            break;
        case SIG_JUMP: case SIG_RJUMP: case SIG_OJUMP: {
            trigger = 0;
            l->signal = behaviour;
            uint32_t target = (((signal << 16) | enddata) & 0xFFFFFFFCu) - 4;
            if (behaviour == SIG_RJUMP) target += l->pc - 4;
            else if (behaviour == SIG_OJUMP) target = rel_full(target);
            if (!valid_addr(target)) { fprintf(stderr, "GE: signal jump to bad address %08x\n", target); s_gpustate = GPU_ERROR; }
            else l->pc = target;
            break;
        }
        case SIG_CALL: case SIG_RCALL: case SIG_OCALL: {
            trigger = 0;
            l->signal = behaviour;
            uint32_t target = (((signal << 16) | enddata) & 0xFFFFFFFCu) - 4;
            if (behaviour == SIG_RCALL) target += l->pc - 4;
            else if (behaviour == SIG_OCALL) target = rel_full(target);
            if (l->stackptr == 32) {
                fprintf(stderr, "GE: signal call: stack full\n");
            } else if (!valid_addr(target)) {
                fprintf(stderr, "GE: signal call to bad address %08x\n", target);
                s_gpustate = GPU_ERROR;
            } else {
                StackEntry *e = &l->stack[l->stackptr++];
                e->pc = l->pc;
                e->offsetAddr = ge_state_ptr()->offset;
                e->baseAddr = ge_state_ptr()->base;
                l->pc = target;
            }
            break;
        }
        case SIG_RET:
            trigger = 0;
            l->signal = behaviour;
            if (l->stackptr == 0) {
                fprintf(stderr, "GE: signal return: stack empty\n");
            } else {
                StackEntry *e = &l->stack[--l->stackptr];
                ge_state_ptr()->offset = e->offsetAddr;
                ge_state_ptr()->base = e->baseAddr;
                l->pc = e->pc;
            }
            break;
        default:
            fprintf(stderr, "GE: unknown signal behaviour %d (signal/end %04x %04x)\n", behaviour, signal, enddata);
            break;
        }
        if (gelog()) fprintf(stderr, "GE: signal behaviour=%d signal/end: %04x %04x list=%d\n",
                             behaviour, signal, enddata, l->id);
        if (l->interruptsEnabled && trigger) {
            trigger_interrupt(l);
            l->pendingInterrupt = 1;
            s_gpustate = GPU_INTERRUPT;
        }
        break;
    }
    case CMD_FINISH:
        switch (l->signal) {
        case SIG_HANDLER_PAUSE:
            l->state = DL_PAUSED;
            if (l->interruptsEnabled) {
                trigger_interrupt(l);
                l->pendingInterrupt = 1;
                s_gpustate = GPU_INTERRUPT;
            }
            break;
        case SIG_SYNC:
            l->signal = SIG_NONE;
            break;
        default:
            l->subIntrToken = prev & 0xFFFF;
            s_gpustate = GPU_DONE;
            ge_list_end();
            if (l->started && l->context) {
                ctx_restore(l);
                l->started = 0;
            }
            if (l->interruptsEnabled) {
                trigger_interrupt(l);
                l->pendingInterrupt = 1;
            } else {
                l->state = DL_COMPLETED;
                l->sync_pending = 0;
                sched_wake(GE_LIST_OBJ(l->id));
            }
            break;
        }
        break;
    default:
        break;   /* END without SIGNAL/FINISH: not finished, keep going */
    }
}

/* Runs the list's commands until it stalls, ends, raises an interrupt or errors. */
static void run_list(DisplayList *l) {
    while (s_gpustate == GPU_RUNNING) {
        if (l->pc == l->stall) { s_gpustate = GPU_STALL; break; }
        if (!sr_inrange(l->pc)) { s_gpustate = GPU_ERROR; break; }
        uint32_t op = MEM_R32(l->pc);
        uint32_t cmd = op >> 24;
        s_cmdmem[cmd] = op;
        if (s_dump) fprintf(s_dump, "%08x %08x\n", l->pc, op);
        switch (cmd) {
        case CMD_JUMP: case CMD_BJUMP: {
            if (cmd == CMD_BJUMP && l->bboxResult) break;
            uint32_t target = ge_rel(op & 0x00FFFFFCu);
            if (!valid_addr(target)) {
                fprintf(stderr, "GE: %s to bad address %08x\n", cmd == CMD_JUMP ? "JUMP" : "BJUMP", target);
                s_gpustate = GPU_ERROR;
            } else {
                l->pc = target - 4;
            }
            break;
        }
        case CMD_CALL: {
            uint32_t target = ge_rel(op & 0x00FFFFFCu);
            if (!valid_addr(target)) {
                fprintf(stderr, "GE: CALL to bad address %08x\n", target);
                s_gpustate = GPU_ERROR;
            } else if (l->stackptr == 32) {
                fprintf(stderr, "GE: CALL: stack full\n");
            } else {
                StackEntry *e = &l->stack[l->stackptr++];
                e->pc = l->pc + 4;
                e->offsetAddr = ge_state_ptr()->offset;   /* BASE is not saved for a CALL */
                l->pc = target - 4;
            }
            break;
        }
        case CMD_RET:
            if (l->stackptr > 0) {
                StackEntry *e = &l->stack[--l->stackptr];
                ge_state_ptr()->offset = e->offsetAddr;
                l->pc = (e->pc & 0x0FFFFFFFu) - 4;
            }
            break;
        case CMD_END:
            execute_end(l, op);
            break;
        case CMD_BOUNDINGBOX:
            /* No bounding-box test: count 0 resets the result, anything else counts as visible,
             * so BJUMP never skips geometry. */
            l->bboxResult = (op & 0xFFFF) != 0;
            ge_execute_op(op, l->pc);
            break;
        default:
            ge_execute_op(op, l->pc);
            break;
        }
        l->pc += 4;
    }
}

/* GPUCommon::ProcessDLQueue */
static void process_dl_queue(void) {
    while (!q_empty()) {
        int idx = q_front();
        DisplayList *l = &dls[idx];
        if (l->state == DL_PAUSED) return;
        if (l->state == DL_NONE) {
            fprintf(stderr, "GE: discarding display list with state NONE (pc=%08x)\n", l->pc);
            q_remove(idx);
            return;
        }
        if (gelog()) fprintf(stderr, "GE: %s DL execution at %08x - stall = %08x (list %d)\n",
                             l->pc == l->startpc ? "Starting" : "Resuming", l->pc, l->stall, l->id);
        s_current = l;
        if (!l->started) {
            if (l->context) ctx_save(l);
            ge_list_begin(l->startpc);
        }
        l->started = 1;
        ge_state_ptr()->offset = l->offsetAddr;
        if (!sr_inrange(l->pc)) { fprintf(stderr, "GE: DL pc %08x invalid\n", l->pc); return; }
        l->state = DL_RUNNING;
        l->interrupted = 0;
        s_gpustate = l->pc == l->stall ? GPU_STALL : GPU_RUNNING;
        FILE *df = dump_file();
        if (df) fprintf(df, "# list %d %s pc=%08x stall=%08x\n", l->id,
                        l->pc == l->startpc ? "start" : "resume", l->pc, l->stall);
        run_list(l);
        if (df) fprintf(df, "# end list %d pc=%08x state=%s\n", l->id, l->pc,
                        s_gpustate == GPU_STALL ? "stall" : s_gpustate == GPU_DONE ? "done" :
                        s_gpustate == GPU_INTERRUPT ? "interrupt" : "error");
        l->offsetAddr = ge_state_ptr()->offset;
        if (s_gpustate == GPU_STALL) return;
        if (s_gpustate != GPU_DONE && s_gpustate != GPU_ERROR) return;   /* interrupt pending */
        if (l->state != DL_QUEUED) q_remove(idx);
    }
    s_current = NULL;
    s_draw_complete = 1;
    /* __GeTriggerSync(GPU_SYNC_DRAW) + SyncEnd: release DrawSync waiters; if any woke, the
     * completed lists are free again. */
    if (sched_wake(GE_DRAW_OBJ))
        for (int i = 0; i < MAX_LISTS; i++)
            if (dls[i].state == DL_COMPLETED) dls[i].state = DL_NONE;
}

/* GPUCommon::InterruptEnd */
static void interrupt_end(int listid) {
    s_isbreak = 0;
    DisplayList *l = &dls[listid];
    l->pendingInterrupt = 0;
    if (l->state == DL_COMPLETED || l->state == DL_NONE) {
        if (l->started && l->context) ctx_restore(l);
        l->sync_pending = 0;
        sched_wake(GE_LIST_OBJ(listid));
        if (!q_empty()) {
            if (listid == q_front()) pop_dl_queue();
            else q_remove(listid);
        }
    }
}

/* GeIntrHandler::run + handleResult, one pending interrupt at a time. Interrupts raised while a
 * guest interrupt handler runs stay queued until the outermost one has returned. */
void sr_ge_run_interrupts(CpuState *s) {
    if (s_intr_running || sched_in_interrupt()) return;
    if (!s_intr_n) return;
    s_intr_running = 1;
    while (s_intr_n) {
        GeIntr in = s_intr[0];
        DisplayList *dl = &dls[in.listid];
        int subintr = -1;
        if (dl->subIntrBase >= 0) {
            switch (dl->signal) {
            case SIG_SYNC: case SIG_JUMP: case SIG_CALL: case SIG_RET:
                break;
            case SIG_HANDLER_PAUSE:
                if (in.cmd == CMD_FINISH) subintr = dl->subIntrBase | SUBINTR_SIGNAL;
                break;
            default:
                subintr = dl->subIntrBase | (in.cmd == CMD_SIGNAL ? SUBINTR_SIGNAL : SUBINTR_FINISH);
                break;
            }
        }
        /* The list counts as complete once its finish interrupt starts. */
        if (dl->signal != SIG_HANDLER_PAUSE && in.cmd == CMD_FINISH) dl->state = DL_COMPLETED;

        uint32_t addr = 0, arg = 0;
        if (subintr >= 0 && sr_intr_get(SR_INTR_GE, (uint32_t)subintr, &addr, &arg) && addr) {
            uint32_t a2 = g_sdk_version <= 0x02000010 ? 0 : in.pc + 4;
            if (gelog()) fprintf(stderr, "GE: entering interrupt handler %08x (list %d, %s, token %04x)\n",
                                 addr, in.listid, in.cmd == CMD_SIGNAL ? "signal" : "finish",
                                 dl->subIntrToken & 0xFFFF);
            sched_run_interrupt(s, addr, dl->subIntrToken & 0xFFFF, arg, a2);
            intr_pop();
            if (dl->signal == SIG_HANDLER_SUSPEND && g_sdk_version <= 0x02000010 &&
                dl->state != DL_NONE && dl->state != DL_COMPLETED)
                dl->state = DL_QUEUED;
        } else {
            if (dl->signal == SIG_HANDLER_SUSPEND && g_sdk_version <= 0x02000010 &&
                dl->state != DL_NONE && dl->state != DL_COMPLETED)
                dl->state = DL_QUEUED;
            intr_pop();
        }
        interrupt_end(in.listid);
        process_dl_queue();
    }
    s_intr_running = 0;
    sched_preempt();   /* return from interrupt: a woken higher-priority thread runs now */
}

/* GPUCommon::EnqueueList */
static uint32_t enqueue_list(uint32_t listpc, uint32_t stall, int subIntrBase, uint32_t args, int head, int *run) {
    *run = 0;
    if (((listpc | stall) & 3) != 0 || !sr_inrange(listpc)) {
        fprintf(stderr, "GE: sceGeListEnQueue: invalid address %08x\n", listpc);
        return ERR_INVALID_POINTER;
    }
    uint32_t a_size = 0, a_context = 0, a_numStacks = 0, a_stackAddr = 0;
    if (args && sr_inrange(args)) {
        a_size = MEM_R32(args);
        a_context = MEM_R32(args + 4);
        a_numStacks = MEM_R32(args + 8);
        a_stackAddr = MEM_R32(args + 12);
        if (a_size >= 16 && a_numStacks >= 256) return ERR_INVALID_SIZE;
    }
    uint32_t stackAddr = (args && sr_inrange(args) && a_size >= 16) ? a_stackAddr : 0;
    if (g_sdk_version > 0x01FFFFFF) {
        for (int i = 0; i < MAX_LISTS; i++) {
            if (dls[i].state != DL_NONE && dls[i].state != DL_COMPLETED) {
                if (dls[i].pc == listpc && !dls[i].pendingInterrupt) {
                    fprintf(stderr, "GE: sceGeListEnQueue: list address %08x already used\n", listpc);
                    return 0x80000021u;
                } else if (stackAddr != 0 && dls[i].stackAddr == stackAddr && !dls[i].pendingInterrupt) {
                    fprintf(stderr, "GE: sceGeListEnQueue: stack address %08x already used\n", stackAddr);
                    return 0x80000021u;
                }
            }
        }
    }
    int id = -1;
    for (int i = 0; i < MAX_LISTS; i++) {
        int possible = (i + s_next_id) % MAX_LISTS;
        DisplayList *p = &dls[possible];
        if (p->pendingInterrupt) continue;
        if (p->state == DL_NONE) { id = possible; break; }
        if (p->state == DL_COMPLETED && !p->sync_pending) id = possible;
    }
    if (id < 0) {
        fprintf(stderr, "GE: no display list id available\n");
        return ERR_OUT_OF_MEMORY;
    }
    s_next_id = id + 1;

    DisplayList *dl = &dls[id];
    memset(dl, 0, sizeof(*dl));
    dl->id = id;
    dl->startpc = listpc & 0x0FFFFFFFu;
    dl->pc = listpc & 0x0FFFFFFFu;
    dl->stall = stall & 0x0FFFFFFFu;
    dl->subIntrBase = subIntrBase < -1 ? -1 : subIntrBase;
    dl->signal = SIG_NONE;
    dl->sync_pending = 1;
    dl->interruptsEnabled = 1;
    dl->stackAddr = stackAddr;
    dl->context = (args && sr_inrange(args) && a_context && sr_inrange(a_context)) ? a_context : 0;

    if (head) {
        if (s_current) {
            if (s_current->state != DL_PAUSED) return ERR_INVALID_VALUE;
            s_current->state = DL_QUEUED;
            s_current->signal = SIG_NONE;
        }
        dl->state = DL_PAUSED;
        s_current = dl;
        q_push_front(id);
    } else if (s_current) {
        dl->state = DL_QUEUED;
        q_push_back(id);
    } else {
        dl->state = DL_RUNNING;
        s_current = dl;
        q_push_front(id);
        s_draw_complete = 0;
        *run = 1;
    }
    return (uint32_t)id;
}

/* ---- HLE ---- */

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])

static uint32_t enqueue(CpuState *s, int head) {
    int run;
    int cbid = (int)A2;
    uint32_t id = enqueue_list(A0, A1, cbid * 2, A3, head, &run);
    if ((int32_t)id >= 0) id ^= LIST_ID_MAGIC;
    if (gelog()) fprintf(stderr, "GE: %08x=sceGeListEnQueue%s(addr=%08x, stall=%08x, cbid=%08x, param=%08x)\n",
                         id, head ? "Head" : "", A0, A1, A2, A3);
    if (run) process_dl_queue();
    sr_ge_run_interrupts(s);
    return id;
}
static uint32_t h_GeListEnQueue(CpuState *s) { return enqueue(s, 0); }
static uint32_t h_GeListEnQueueHead(CpuState *s) { return enqueue(s, 1); }

static uint32_t h_GeListDeQueue(CpuState *s) {
    int listid = (int)(A0 ^ LIST_ID_MAGIC);
    if (listid < 0 || listid >= MAX_LISTS || dls[listid].state == DL_NONE) return ERR_INVALID_ID;
    DisplayList *dl = &dls[listid];
    if (dl->started) return ERR_BUSY;
    dl->state = DL_NONE;
    if (!q_empty() && listid == q_front()) pop_dl_queue();
    else q_remove(listid);
    dl->sync_pending = 0;
    sched_wake(GE_LIST_OBJ(listid));
    if (q_empty())
        for (int i = 0; i < MAX_LISTS; i++) dls[i].state = DL_NONE;   /* CheckDrawSync */
    sched_preempt();
    return 0;
}

static uint32_t h_GeListUpdateStallAddr(CpuState *s) {
    int listid = (int)(A0 ^ LIST_ID_MAGIC);
    if (listid < 0 || listid >= MAX_LISTS || dls[listid].state == DL_NONE) return ERR_INVALID_ID;
    DisplayList *dl = &dls[listid];
    if (dl->state == DL_COMPLETED) return ERR_ALREADY;
    dl->stall = A1 & 0x0FFFFFFFu;
    process_dl_queue();
    sr_ge_run_interrupts(s);
    return 0;
}

static uint32_t h_GeListSync(CpuState *s) {
    int listid = (int)(A0 ^ LIST_ID_MAGIC);
    uint32_t mode = A1;
    if (listid < 0 || listid >= MAX_LISTS) return ERR_INVALID_ID;
    if (mode > 1) return ERR_INVALID_MODE;
    DisplayList *dl = &dls[listid];
    if (mode == 1) {
        switch (dl->state) {
        case DL_QUEUED: return dl->interrupted ? LIST_PAUSED : LIST_QUEUED;
        case DL_RUNNING: return dl->pc == dl->stall ? LIST_STALLING : LIST_DRAWING;
        case DL_COMPLETED: return LIST_COMPLETED;
        case DL_PAUSED: return LIST_PAUSED;
        default: return ERR_INVALID_ID;
        }
    }
    if (sched_in_interrupt()) return ERR_ILLEGAL_CONTEXT;
    if (dl->sync_pending) sched_block_on(GE_LIST_OBJ(listid));
    return LIST_COMPLETED;
}

static uint32_t h_GeDrawSync(CpuState *s) {
    uint32_t mode = A0;
    if (mode > 1) return ERR_INVALID_MODE;
    if (mode == 0) {
        if (sched_in_interrupt()) return ERR_ILLEGAL_CONTEXT;
        if (!s_draw_complete) {
            sched_block_on(GE_DRAW_OBJ);
        } else {
            for (int i = 0; i < MAX_LISTS; i++)
                if (dls[i].state == DL_COMPLETED) dls[i].state = DL_NONE;
        }
        return 0;
    }
    DisplayList *top = NULL;
    for (int i = 0; i < s_qn; i++)
        if (dls[s_queue[i]].state != DL_COMPLETED) { top = &dls[s_queue[i]]; break; }
    if (!top || top->state == DL_COMPLETED) return LIST_COMPLETED;
    if (s_current && s_current->pc == s_current->stall) return LIST_STALLING;
    return LIST_DRAWING;
}

/* GPUCommon::Continue */
static uint32_t h_GeContinue(CpuState *s) {
    uint32_t ret = 0;
    int run = 0;
    if (s_current) {
        if (s_current->state == DL_PAUSED) {
            if (!s_isbreak) {
                s_current->state = DL_RUNNING;
                s_current->signal = SIG_NONE;
                s_draw_complete = 0;
            } else {
                s_current->state = DL_QUEUED;
                s_current->signal = SIG_NONE;
            }
            run = 1;
        } else if (s_current->state == DL_RUNNING) {
            ret = g_sdk_version >= 0x02000000 ? 0x80000020u : 0xFFFFFFFFu;
        } else {
            ret = g_sdk_version >= 0x02000000 ? 0x80000004u : 0xFFFFFFFFu;
        }
    }
    if (gelog()) fprintf(stderr, "GE: %08x=sceGeContinue()\n", ret);
    if (run) process_dl_queue();
    sr_ge_run_interrupts(s);
    if (!sched_in_interrupt()) sched_preempt();
    return ret;
}

/* GPUCommon::Break */
static uint32_t h_GeBreak(CpuState *s) {
    uint32_t mode = A0, unk = A1;
    if (mode > 1) return ERR_INVALID_MODE;
    if ((int32_t)unk < 0 || (int32_t)(unk + 16) < 0) return ERR_PRIV_REQUIRED;
    if (!s_current) return ERR_ALREADY;
    if (mode == 1) {
        s_qn = 0;
        for (int i = 0; i < MAX_LISTS; i++) { dls[i].state = DL_NONE; dls[i].signal = SIG_NONE; }
        s_next_id = 0;
        s_current = NULL;
        return 0;
    }
    if (s_current->state == DL_NONE || s_current->state == DL_COMPLETED)
        return g_sdk_version >= 0x02000000 ? 0x80000004u : 0xFFFFFFFFu;
    if (s_current->state == DL_PAUSED) {
        if (g_sdk_version > 0x02000010) {
            if (s_current->signal != SIG_HANDLER_PAUSE) return ERR_ALREADY;
            fprintf(stderr, "GE: sceGeBreak: can't break signal-pausing list\n");
        }
        return ERR_BUSY;
    }
    if (s_current->state == DL_QUEUED) {
        s_current->state = DL_PAUSED;
        return (uint32_t)s_current->id ^ LIST_ID_MAGIC;
    }
    if (s_current->signal == SIG_SYNC) s_current->pc += 8;
    s_current->interrupted = 1;
    s_current->state = DL_PAUSED;
    s_current->signal = SIG_HANDLER_SUSPEND;
    s_isbreak = 1;
    return (uint32_t)s_current->id ^ LIST_ID_MAGIC;
}

/* sceGeSetCallback registers signal_func/finish_func as GE sub-interrupts cbid*2 and cbid*2+1. */
static uint32_t h_GeSetCallback(CpuState *s) {
    int cbid = -1;
    for (int i = 0; i < 16; i++) if (!s_cb_used[i]) { cbid = i; break; }
    if (cbid < 0) return ERR_OUT_OF_MEMORY;
    s_cb_used[cbid] = 1;
    GeCallbackData *d = &s_cb_data[cbid];
    d->signal_func = MEM_R32(A0 + 0);
    d->signal_arg  = MEM_R32(A0 + 4);
    d->finish_func = MEM_R32(A0 + 8);
    d->finish_arg  = MEM_R32(A0 + 12);
    uint32_t base = (uint32_t)cbid * 2;
    if (d->finish_func) sr_intr_set(SR_INTR_GE, base | SUBINTR_FINISH, d->finish_func, d->finish_arg, 1);
    if (d->signal_func) sr_intr_set(SR_INTR_GE, base | SUBINTR_SIGNAL, d->signal_func, d->signal_arg, 1);
    if (gelog()) fprintf(stderr, "GE: %d=sceGeSetCallback(signal %08x/%08x, finish %08x/%08x)\n",
                         cbid, d->signal_func, d->signal_arg, d->finish_func, d->finish_arg);
    return (uint32_t)cbid;
}

static uint32_t h_GeUnsetCallback(CpuState *s) {
    uint32_t cbid = A0;
    if (cbid >= 16) return ERR_INVALID_ID;
    if (s_cb_used[cbid]) {
        sr_intr_release(SR_INTR_GE, cbid * 2 | SUBINTR_FINISH);
        sr_intr_release(SR_INTR_GE, cbid * 2 | SUBINTR_SIGNAL);
    }
    s_cb_used[cbid] = 0;
    return 0;
}

static uint32_t h_GeEdramGetAddr(CpuState *s) { (void)s; return 0x04000000; }
static uint32_t h_GeEdramGetSize(CpuState *s) { (void)s; return 0x00200000; }

static uint32_t h_GeGetCmd(CpuState *s) {
    int cmd = (int)A0;
    if (cmd < 0 || cmd >= 256) return 0x80000102u;   /* INVALID_INDEX */
    uint32_t v = s_cmdmem[cmd];
    switch (cmd) {
    case 0x2B: case 0x3B: case 0x3D: case 0x3F: case 0x41: v &= 0xFF000000u; break;   /* matrix data */
    case 0x2A: v &= 0xFF00007Fu; break;                                                /* bone number */
    case 0x3A: case 0x3C: case 0x3E: case 0x40: v &= 0xFF00000Fu; break;               /* matrix number */
    default: break;
    }
    return v;
}

void sr_ge_register(void) {
    sr_hle_register(0xe47e40e4, "sceGeEdramGetAddr", h_GeEdramGetAddr);
    sr_hle_register(0x1f6752ad, "sceGeEdramGetSize", h_GeEdramGetSize);
    sr_hle_register(0xab49e76a, "sceGeListEnQueue", h_GeListEnQueue);
    sr_hle_register(0x1c0d95a6, "sceGeListEnQueueHead", h_GeListEnQueueHead);
    sr_hle_register(0x5fb86ab0, "sceGeListDeQueue", h_GeListDeQueue);
    sr_hle_register(0xe0d68148, "sceGeListUpdateStallAddr", h_GeListUpdateStallAddr);
    sr_hle_register(0x03444eb4, "sceGeListSync", h_GeListSync);
    sr_hle_register(0xb287bd61, "sceGeDrawSync", h_GeDrawSync);
    sr_hle_register(0xb448ec0d, "sceGeBreak", h_GeBreak);
    sr_hle_register(0x4c06e472, "sceGeContinue", h_GeContinue);
    sr_hle_register(0xa4fc06a4, "sceGeSetCallback", h_GeSetCallback);
    sr_hle_register(0x05db22ce, "sceGeUnsetCallback", h_GeUnsetCallback);
    sr_hle_register(0xdc93cfef, "sceGeGetCmd", h_GeGetCmd);
}
