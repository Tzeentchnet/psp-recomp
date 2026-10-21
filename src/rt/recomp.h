/* Runtime the recompiled C links against (ARCHITECTURE.md sections 4, 5, 7).
 *
 * The codegen emits one C function per guest function with signature void f_<hexaddr>(CpuState*).
 * Those functions read and write this CpuState and access guest memory through the macros
 * below. Computed transfers go through dispatch(). When tracing is enabled (a trace file is
 * set), each translated instruction reports itself so the output can be diffed against the
 * PPSSPP reference trace (tools/TRACE_FORMAT.md).
 */

#ifndef PSP_RECOMP_RT_H
#define PSP_RECOMP_RT_H

#include <stdint.h>

typedef struct CpuState {
    uint32_t r[32];     /* r[0] reads 0; the codegen never emits a write to r[0]. */
    uint32_t hi, lo;
    uint32_t pc;        /* kept accurate at call sites / interpreter boundaries (section 7) */
    union {
        float f[32];
        uint32_t fi[32];
    };
    uint32_t fcr31;
    uint32_t fpcond;    /* FP compare result, separate from fcr31 to match PPSSPP */
    union {
        float v[128];   /* VFPU register file; physical order matches PPSSPP's v[128] */
        uint32_t vi[128];
    };
    uint32_t vfpuCtrl[16];  /* VFPU control: prefixes (S/T/D), cc, etc. */
} CpuState;

/* Guest memory: a single host region. g_mem points at guest 0x08000000, and the underlying
 * allocation also extends 0x04000000 bytes *below* g_mem so the same arena covers VRAM/eDRAM
 * (0x04000000..0x041fffff). SR_HOST maps a VRAM address to g_mem minus an offset that still
 * lands inside the allocation. Keeping SR_RAM_BASE at 0x08000000 means the recompiled code's
 * MEM_* macros are unchanged (no rebuild of the big object). */
extern uint8_t *g_mem;
extern int g_sr_gpu_vram_read_watch;
extern int g_sr_gpu_vram_write_watch;
void sr_gpu_vram_read(uint32_t addr, uint32_t bytes);
void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
void sr_gpu_vram_prepare_write(uint32_t addr, uint32_t bytes);
#define SR_RAM_BASE 0x08000000u
#define SR_PHYS(a)  ((a) & 0x1FFFFFFFu)
/* Signed offset so addresses below SR_RAM_BASE (VRAM/eDRAM at 0x04000000) map below g_mem,
 * into the part of the arena reserved for them, instead of wrapping to a huge positive offset. */
#define SR_HOST(a)  (g_mem + (int32_t)(SR_PHYS(a) - SR_RAM_BASE))

/* The arena covers guest physical [0x04000000, 0x0c000000): VRAM/eDRAM at 0x04000000 and user
 * RAM at 0x08000000. A guest pointer outside that window is wild (would fault on real hardware
 * too). During bring-up some not-yet-faithful subsystems compute such pointers; rather than
 * segfault the host, out-of-range reads return 0 and writes are dropped. Valid accesses are
 * unaffected, so differential/bit-exact checks still hold. */
static inline int sr_inrange(uint32_t a) {
    return (uint32_t)(SR_PHYS(a) - 0x04000000u) < 0x08000000u;
}
extern void sr_oor(uint32_t a, uint32_t v, int store);   /* records out-of-range access (diag) */
static inline void sr_prepare_read(uint32_t a, uint32_t bytes) {
    if ((uint32_t)(SR_PHYS(a) - 0x04000000u) < 0x00200000u &&
        g_sr_gpu_vram_read_watch) {
        sr_gpu_vram_read(a, bytes);
    }
}
static inline int sr_is_watched_vram(uint32_t a) {
    return (uint32_t)(SR_PHYS(a) - 0x04000000u) < 0x00200000u &&
           g_sr_gpu_vram_write_watch;
}
static inline uint8_t  sr_r8 (uint32_t a) { if (sr_inrange(a)) { sr_prepare_read(a,1); return *(uint8_t  *)SR_HOST(a); } sr_oor(a,0,0); return 0u; }
static inline uint16_t sr_r16(uint32_t a) { if (sr_inrange(a)) { sr_prepare_read(a,2); return *(uint16_t *)SR_HOST(a); } sr_oor(a,0,0); return 0u; }
static inline uint32_t sr_r32(uint32_t a) { if (sr_inrange(a)) { sr_prepare_read(a,4); return *(uint32_t *)SR_HOST(a); } sr_oor(a,0,0); return 0u; }
static inline void sr_w8 (uint32_t a, uint8_t  v) { if (sr_inrange(a)) { int w=sr_is_watched_vram(a); if(w)sr_gpu_vram_prepare_write(a,1); *(uint8_t  *)SR_HOST(a)=v; if(w)sr_gpu_vram_dirty(a,1); } else sr_oor(a,v,1); }
static inline void sr_w16(uint32_t a, uint16_t v) { if (sr_inrange(a)) { int w=sr_is_watched_vram(a); if(w)sr_gpu_vram_prepare_write(a,2); *(uint16_t *)SR_HOST(a)=v; if(w)sr_gpu_vram_dirty(a,2); } else sr_oor(a,v,1); }
static inline void sr_w32(uint32_t a, uint32_t v) { if (sr_inrange(a)) { int w=sr_is_watched_vram(a); if(w)sr_gpu_vram_prepare_write(a,4); *(uint32_t *)SR_HOST(a)=v; if(w)sr_gpu_vram_dirty(a,4); } else sr_oor(a,v,1); }

#define MEM_R8(a)   sr_r8(a)
#define MEM_R16(a)  sr_r16(a)
#define MEM_R32(a)  sr_r32(a)
#define MEM_W8(a,v)  sr_w8((a), (uint8_t)(v))
#define MEM_W16(a,v) sr_w16((a), (uint16_t)(v))
#define MEM_W32(a,v) sr_w32((a), (uint32_t)(v))

void  sr_mem_init(void);
void  sr_configure_fpu(void);
void  sr_load_segment(uint32_t vaddr, const void *data, uint32_t len);
uint32_t sr_loaded_start(void); /* lowest guest address the loader wrote (module start) */
uint32_t sr_loaded_end(void);   /* highest guest address the loader wrote (module end incl. BSS) */
void  sr_note_loaded(uint32_t vaddr, uint32_t memsz);   /* extend the loaded range (e.g. BSS) */

/* Unaligned word access (MIPS LWL/LWR/SWL/SWR), little-endian, matching PPSSPP's
 * interpreter. The load forms take the current rt and the effective address and return the
 * merged register value; the store forms read-modify-write the aligned word at addr&~3. */
uint32_t sr_lwl(uint32_t rtv, uint32_t addr);
uint32_t sr_lwr(uint32_t rtv, uint32_t addr);
void     sr_swl(uint32_t addr, uint32_t rtv);
void     sr_swr(uint32_t addr, uint32_t rtv);

/* VFPU source/destination prefix application (ARCHITECTURE section 6.4), ported from
 * PPSSPP. sr_vread reads n lanes from physical indices idx[], then applies a source
 * prefix (swizzle/abs/negate/constant). sr_vwrite applies the destination prefix
 * (saturate) and write mask, then stores. The prefix value comes from s->vfpuCtrl. */
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix);
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t dprefix);

/* VFPU transcendentals, exact ports of PPSSPP's table-based kernels (the PSP hardware does
 * not compute these with IEEE math). The lookup tables are loaded once from assets/vfpu/
 * (override the directory with PSP_VFPU_TABLES). */
float sr_vfpu_rcp(float x);
float sr_vfpu_rsqrt(float x);
float sr_vfpu_sqrt(float x);
float sr_vfpu_sin(float x);
float sr_vfpu_cos(float x);
float sr_vfpu_exp2(float x);
float sr_vfpu_asin(float x);   /* vfpu_asin.c: result in units of pi/2 */

/* Single-instruction VFPU interpreter (src/rt/vfpu_interp.c). Returns SR_VFPU_COMPUTE for a
 * value-producing op (compare v[]/f[] to the reference trace), SR_VFPU_STATE for a prefix/control op
 * (nothing to compare), or SR_VFPU_OTHER for ops not handled here (e.g. VFPU load/store). */
#define SR_VFPU_OTHER   0
#define SR_VFPU_COMPUTE 1
#define SR_VFPU_STATE   2
int sr_vfpu_interp(CpuState *s, uint32_t op);

/* Dispatch: guest address -> native recompiled function (section 7). Codegen emits a sorted,
 * immutable table so large targets do not pay for a fixed-capacity runtime hash. Computed
 * jumps/calls go through here; unknown targets would fall to the interpreter once linked. */
typedef void (*RecompFn)(CpuState *);
typedef struct SrDispatchEntry {
    uint32_t address;
    RecompFn function;
} SrDispatchEntry;
typedef enum SrInterpStopReason {
    SR_INTERP_RETURNED = 0,
    SR_INTERP_SYSCALL,
    SR_INTERP_UNIMPLEMENTED,
    SR_INTERP_MEMORY_FAULT,
    SR_INTERP_STEP_LIMIT,
    SR_INTERP_BREAK
} SrInterpStopReason;
typedef struct SrInterpStop {
    SrInterpStopReason reason;
    uint32_t pc;
    uint32_t op;
    uint32_t memory_address;
    uint64_t steps;
} SrInterpStop;
void     sr_set_dispatch_table(const SrDispatchEntry *entries, uint32_t count);
RecompFn sr_lookup(uint32_t addr);
int      sr_interpret_fallback(
             CpuState *s, uint32_t target, uint32_t return_pc,
             uint64_t max_steps, SrInterpStop *stop);
void     dispatch(CpuState *s, uint32_t target);

/* Tracing. When a trace file is open, the generated code reports each instruction. sr_begin
 * snapshots the register file and records pc/op; sr_end diffs and emits the line, with an
 * optional store (addr/size) read back from guest memory. Emit order follows PPSSPP: a branch
 * reports before its delay slot. */
int  sr_trace_open(const char *path, const char *target, uint32_t start_pc);
void sr_trace_close(void);
void sr_begin(const CpuState *s, uint32_t pc, uint32_t op);
void sr_end(const CpuState *s, uint32_t mem_addr, int mem_size);

/* HLE boundary marker used by the bring-up driver: a call to an unresolved import stop the
 * traced run so the comparison ends exactly where the reference trace reaches its first syscall. */
void sr_hle_call(CpuState *s, uint32_t nid);
extern int sr_hit_hle;

/* HLE syscall dispatch. The recompiled import stub at <stub> calls sr_syscall with the NID
 * resolved from the PRX import table. It dispatches to the registered handler (which reads
 * arguments from $a0-$a3 and returns the $v0 value), then poisons the caller-saved temp
 * registers to 0xDEADBEEF exactly as PPSSPP's kernel does, and writes the return to $v0. A
 * NID with no handler logs and stops at the HLE boundary (longjmp) so bring-up can see which
 * import to implement next. sr_last_nid records the most recent dispatched NID. */
/* Kernel object UID allocator, shared by every object type (threads, memory blocks, callbacks,
 * modules, ...) exactly like PPSSPP's single KernelObjectPool. PPSSPP hands out uid = index +
 * handleOffset(0x100) starting at index initialNextID(0x10), i.e. 0x110, 0x111, ... and games
 * index their own per-object tables by (uid - 0x100), so the base and a single shared counter
 * matter. */
uint32_t sr_alloc_uid(void);
void     sr_set_next_uid(uint32_t next);
uint32_t sr_get_next_uid(void);
void     sr_set_boot_uid_gap(uint32_t count);
void     sr_boot_uid_delay_checkpoint(void);

/* Kernel memory (src/rt/kmem.c): PPSSPP's BlockAllocator and the partitions built on it.
 * Allocation returns SR_BA_FAIL when no free block fits; *size is rounded up to the grain. */
#define SR_BA_FAIL 0xFFFFFFFFu
typedef struct SrBlockAlloc SrBlockAlloc;
void     sr_ba_init(SrBlockAlloc *a, uint32_t start, uint32_t size);
void     sr_ba_shutdown(SrBlockAlloc *a);
uint32_t sr_ba_alloc(SrBlockAlloc *a, uint32_t *size, int from_top, const char *tag);
uint32_t sr_ba_alloc_aligned(SrBlockAlloc *a, uint32_t *size, uint32_t size_grain, uint32_t grain,
                             int from_top, const char *tag);
uint32_t sr_ba_alloc_at(SrBlockAlloc *a, uint32_t position, uint32_t size, const char *tag);
int      sr_ba_free(SrBlockAlloc *a, uint32_t position);
uint32_t sr_ba_largest_free(const SrBlockAlloc *a);
uint32_t sr_ba_total_free(const SrBlockAlloc *a);
uint32_t sr_ba_block_start(const SrBlockAlloc *a, uint32_t addr);
uint32_t sr_ba_block_size(const SrBlockAlloc *a, uint32_t addr);
void     sr_ba_list(const SrBlockAlloc *a);
void          sr_kmem_init(void);       /* partitions, usersystemlib, module block (idempotent) */
SrBlockAlloc *sr_kmem_user(void);       /* user partition 0x08800000..0x0A000000 */
SrBlockAlloc *sr_kmem_kernel(void);     /* kernel partition 0x08000000..0x08400000 */
uint32_t      sr_kmem_user_end(void);
uint32_t      sr_hle_stack_top(int which);   /* 0 = interrupt, 1 = callback stack (kernel memory) */
void          sr_kmem_register(void);   /* SysMemUserForUser partition calls and FPLs */

/* GE command interpreter (src/rt/ge.c): GE state and rasterising into VRAM. gelist.c owns the
 * display lists and their control flow and feeds every other command to ge_execute_op. */
void     ge_list_begin(uint32_t list_addr);
void     ge_execute_op(uint32_t op, uint32_t pc);
void     ge_list_end(void);
uint32_t ge_rel(uint32_t data);         /* (offset + BASE-extended data) & 0x0FFFFFFF */
uint32_t ge_framebuffer(void);
uint32_t ge_get_frame(void);           /* vblank count the GE last saw (sr_vblank_tick) */
/* sceGe display lists (src/rt/gelist.c, PPSSPP GPUCommon port). */
void     sr_ge_register(void);          /* sceGe_user HLE handlers */
void     sr_ge_run_interrupts(CpuState *s);  /* deliver pending GE interrupts (outside interrupts) */
void     sr_ge_set_next_list_id(uint32_t next);
uint32_t sr_ge_get_next_list_id(void);
/* Sub-interrupt handlers (src/rt/intr.c, PPSSPP sceKernelInterrupt port). */
#define SR_INTR_GE     25
#define SR_INTR_VBLANK 30
void     sr_intr_register(void);        /* InterruptManager HLE handlers */
int      sr_intr_get(uint32_t intr, uint32_t sub, uint32_t *addr, uint32_t *arg);   /* 1 if registered */
void     sr_intr_set(uint32_t intr, uint32_t sub, uint32_t addr, uint32_t arg, int enable);
void     sr_intr_release(uint32_t intr, uint32_t sub);
void     sr_intr_run_vblank(CpuState *s);   /* every enabled vblank sub-handler, in order */
extern uint32_t g_sdk_version;          /* sceKernelSetCompiledSdkVersion (hle.c) */

/* Interactive window front-end (src/rt/gui.c). gui_init selects SR_GPU and opens the window; gui_present is
 * called from sceDisplaySetFrameBuf to show a frame, pump messages, and sample the keyboard;
 * gui_buttons returns the live PSP pad state; gui_on reports whether the window is active. */
int      gui_init(const char *title);
int      gui_on(void);
uint32_t gui_buttons(void);
void     gui_analog(uint8_t *lx, uint8_t *ly);   /* live left-stick (0..255, 128=centre) */
int      gui_pad_present(void);                  /* 1 when a game controller is connected */
int      gui_message(
             const char *text, int yes_no, int allow_cancel, int default_no);
void     gui_present(uint32_t fbaddr, int fmt, uint32_t stride);
int gui_capture_next(const char *path);
int sr_kzl_patch_offline_menu(uint8_t *data, uint32_t size);
void sr_kzl_killzone_com_disabled(CpuState *s);
void sr_kzl_downloads_unavailable(CpuState *s);
void sr_kzl_twinstick_apply_aim(CpuState *s);
uint32_t sr_present_count(void);
uint32_t sr_atrac_remaining_frames(
    uint32_t buffered, uint32_t block_align, int streamed);

typedef uint32_t (*HleFn)(CpuState *s);
void     sr_hle_register(uint32_t nid, const char *name, HleFn fn);
void     sr_hle_init(void);     /* registers all built-in handlers (idempotent) */
void     sr_net_register(void);  /* offline networking HLE (src/rt/net.c) */
void     sr_syscall(CpuState *s, uint32_t nid);
extern uint32_t sr_last_nid;
enum {
    SR_TMID_THREAD = 1,
    SR_TMID_SEMAPHORE = 2,
    SR_TMID_EVENT_FLAG = 3,
    SR_TMID_FPL = 6,
    SR_TMID_MODULE = 7,
    SR_TMID_CALLBACK = 8,
};
uint32_t sr_alloc_uid(void);
uint32_t sr_alloc_uid_type(uint32_t type);
void     sr_register_uid_type(uint32_t uid, uint32_t type);
void     sr_unregister_uid(uint32_t uid);
uint32_t sr_uid_list(uint32_t type, uint32_t *out, uint32_t capacity);

/* Cooperative scheduler with preemptive yield points (src/rt/sched.c). The recompiled code
 * calls SR_YIELD at function entry and loop back-edges; when the scheduler is active and the
 * thread's timeslice runs out, it switches to another ready thread (Windows fibers). When the
 * scheduler is off (per-function differential, plain driver) SR_YIELD is a cheap no-op that
 * changes nothing, so it does not affect the existing trace verification. */
extern int     sr_sched_on;
extern int32_t sr_timeslice;
void sr_yield(CpuState *s);
#define SR_YIELD(s) do { if (sr_sched_on && --sr_timeslice <= 0) sr_yield(s); } while (0)

/* Scheduler API used by the thread HLE handlers and the driver. */
typedef void (*SrHleThreadFn)(uint32_t arg);
void     sched_init(CpuState *cpu);                 /* CpuState the running thread reads/writes */
uint32_t sched_create_thread(const char *name, uint32_t entry, int priority, uint32_t stack_size,
                             uint32_t attr);                /* uid, or a kernel error (bit 31) */
uint32_t sched_create_hle_thread(const char *name, int priority, uint32_t stack_size,
                                 SrHleThreadFn entry, uint32_t arg);
void     sched_set_thread_gp(uint32_t uid, uint32_t gp);
int      sched_delete_hle_thread(uint32_t uid);
int      sched_delete_thread(uint32_t uid);         /* sceKernelDeleteThread: 0 or a kernel error */
void     sched_exit_delete_current(int32_t status); /* sceKernelExitDeleteThread */
uint32_t sched_start_thread(uint32_t uid, uint32_t arglen, uint32_t argp);  /* 0 or a kernel error */
void     sched_exit_current(int32_t status);        /* current thread is done */
void     sched_delay_current(uint32_t usec);        /* block current thread for usec */
void     sched_preempt(void);                       /* yield now if a higher-priority thread is ready */
void     sched_block_on(uint32_t obj);              /* block current thread until sched_wake(obj) */
void     sched_wait_vblank(void);                   /* block current thread until the next delivered vblank */
int      sched_vbl_paced(void);
int      sched_fast_forwarding(void);
int      sched_block_on_timeout(uint32_t obj, uint32_t usec);  /* returns 1 if timed out */
int      sched_wake(uint32_t obj);                  /* ready all threads blocked on obj; returns how many */
void     sched_run_interrupt(CpuState *s, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2);
int      sched_in_interrupt(void);                  /* 1 while a guest interrupt handler runs */
void     sched_cb_wait(int on);                     /* current thread enters/leaves a ...CB wait */
void     sched_cb_notify_thread(uint32_t uid);      /* a callback of uid was notified */
/* Kernel callbacks (src/rt/callback.c). */
void     sr_cb_register(void);                      /* callback and scePower callback HLE */
int      sr_cb_notify(uint32_t uid, uint32_t arg);  /* 1 if uid is a callback */
int      sr_cb_run_pending(CpuState *s, uint32_t thread_uid);
void     sched_thread_sleep(void);                  /* sceKernelSleepThread[CB] (wakeup-count) */
void     sched_thread_wakeup(uint32_t uid);         /* sceKernelWakeupThread (banks if not asleep) */
void     sched_set_priority(uint32_t uid, int priority);   /* sceKernelChangeThreadPriority */
void     sched_terminate_thread(uint32_t uid);      /* sceKernelTerminate[Delete]Thread */
int      sched_thread_cancel_wakeup(uint32_t uid);  /* sceKernelCancelWakeupThread; uid 0=current */
typedef struct SrThreadStatus {
    char name[32];
    uint32_t attr;
    uint32_t status;
    uint32_t entry;
    uint32_t stack;
    uint32_t stackSize;
    uint32_t gpReg;
    int32_t initialPriority;
    int32_t currentPriority;
    uint32_t waitType;
    uint32_t waitId;
    int32_t wakeupCount;
    int32_t exitStatus;
    uint32_t runClocksLow;
    uint32_t runClocksHigh;
    uint32_t intrPreemptCount;
    uint32_t threadPreemptCount;
    uint32_t releaseCount;
} SrThreadStatus;
typedef struct SrThreadRunStatus {
    uint32_t size;
    uint32_t status;
    uint32_t currentPriority;
    uint32_t waitType;
    uint32_t waitId;
    uint32_t wakeupCount;
    uint32_t runClocksLow;
    uint32_t runClocksHigh;
    uint32_t intrPreemptCount;
    uint32_t threadPreemptCount;
    uint32_t releaseCount;
} SrThreadRunStatus;
int      sched_thread_status(uint32_t uid, SrThreadStatus *out);
int      sched_thread_run_status(uint32_t uid, SrThreadRunStatus *out);
int32_t  sched_thread_exit_status(uint32_t uid);
uint32_t sched_thread_id_list(uint32_t type, uint32_t *out, uint32_t capacity);
int      sched_current_priority(void);
int      sched_is_dormant(uint32_t uid);
uint32_t sched_current_uid(void);
void     sched_run(uint32_t entry, uint32_t arglen, uint32_t argp, int create_start_thread);

/* Runtime-loaded user PRXs (src/rt/module.c). */
uint32_t sr_module_load_image(
             const uint8_t *data, uint32_t size, const char *path);
uint32_t sr_module_start(
             uint32_t uid, uint32_t arg_size, uint32_t arg_address,
             uint32_t status_address, uint32_t option_address);
uint32_t sr_module_unload(uint32_t uid);
int      sr_module_find_export(
             uint32_t nid, uint32_t *address, const char **library);
int      sr_module_syscall_nid(uint32_t code, uint32_t *nid);
int      sr_module_patch_import(
             uint32_t stub, uint32_t nid, const char *library);

/* A function the static recompiler could not translate (an unimplemented instruction). The
 * body traps loudly with the guest address and reason rather than silently doing nothing, so
 * reaching it during a run is unmistakable. Every such function is listed in STUBS.md. */
void sr_unimplemented(uint32_t pc, const char *reason);

#include <setjmp.h>
extern jmp_buf g_hle_jmp;  /* sr_hle_call longjmps here so the run stops at the HLE boundary */

#endif
