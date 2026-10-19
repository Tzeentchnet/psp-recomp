/* Kernel memory: PPSSPP's BlockAllocator (Core/Util/BlockAllocator.cpp) ported to C, and the kernel
 * calls built on it -- partition memory, the free-size queries, FPLs and thread stacks.
 *
 * The PSP kernel hands out memory first fit at a 256-byte grain, from the bottom or the top of a
 * partition or at a fixed address, frees it again, and fails when no free block is big enough.
 * Games depend on all of it: KZL sizes its main heap by creating FPLs of decreasing size until one
 * succeeds, so an allocator that never fails runs the heap into the thread stacks.
 *
 * The layout follows PPSSPP (sceKernelMemory.cpp, __KernelMemoryInit): kernel 0x08000000-0x08400000,
 * volatile 0x08400000-0x08800000, user 0x08800000-0x0A000000. The first 16 KB of user memory are
 * taken by "usersystemlib", and the loaded module occupies its load range. Thread stacks come from
 * the user partition too (from the top unless the thread asks for a low stack), so creating and
 * deleting threads moves the free blocks exactly as on PPSSPP. The kernel partition holds only the
 * runtime's own stacks for guest callbacks (sr_hle_stack_top).
 *
 * The allocator is a literal port: the same block list, the same split and merge order, and the
 * same rounding, so every address and every failure matches PPSSPP for the same call sequence.
 * SR_MEMLOG=1 logs allocations, frees and failures (with the block list on failure).
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- BlockAllocator ---- */

typedef struct SrBlock {
    uint32_t start, size;
    int taken;
    char tag[32];
    struct SrBlock *prev, *next;
} SrBlock;

struct SrBlockAlloc {
    SrBlock *bottom, *top;
    uint32_t range_start, range_size;
    uint32_t grain;
    const char *name;
};

static int memlog(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_MEMLOG") ? 1 : 0;
    return on;
}

static SrBlock *blk_new(uint32_t start, uint32_t size, int taken, SrBlock *prev, SrBlock *next) {
    SrBlock *b = (SrBlock *)calloc(1, sizeof(SrBlock));
    if (!b) { fprintf(stderr, "kmem: out of host memory\n"); abort(); }
    b->start = start; b->size = size; b->taken = taken; b->prev = prev; b->next = next;
    strcpy(b->tag, "(untitled)");
    return b;
}

static void blk_set_allocated(SrBlockAlloc *a, SrBlock *b, const char *tag) {
    snprintf(b->tag, sizeof(b->tag), "%s", tag ? tag : "---");
    if (memlog()) fprintf(stderr, "kmem %s: alloc %08x-%08x size %08x %s\n",
                          a->name, b->start, b->start + b->size, b->size, b->tag);
}

void sr_ba_shutdown(SrBlockAlloc *a) {
    while (a->bottom) {
        SrBlock *next = a->bottom->next;
        free(a->bottom);
        a->bottom = next;
    }
    a->top = NULL;
}

void sr_ba_init(SrBlockAlloc *a, uint32_t start, uint32_t size) {
    sr_ba_shutdown(a);
    a->range_start = start;
    a->range_size = size;
    a->top = a->bottom = blk_new(start, size, 0, NULL, NULL);   /* one free block covering everything */
}

void sr_ba_list(const SrBlockAlloc *a) {
    fprintf(stderr, "kmem %s: -----------\n", a->name);
    for (const SrBlock *b = a->bottom; b; b = b->next)
        fprintf(stderr, "kmem %s: Block: %08x - %08x size %08x taken=%d tag=%s\n",
                a->name, b->start, b->start + b->size, b->size, b->taken, b->tag);
    fprintf(stderr, "kmem %s: -----------\n", a->name);
}

static SrBlock *insert_free_before(SrBlockAlloc *a, SrBlock *b, uint32_t size) {
    SrBlock *ins = blk_new(b->start, size, 0, b->prev, b);
    b->prev = ins;
    if (!ins->prev) a->bottom = ins;
    else ins->prev->next = ins;
    b->start += size;
    b->size -= size;
    return ins;
}

static SrBlock *insert_free_after(SrBlockAlloc *a, SrBlock *b, uint32_t size) {
    SrBlock *ins = blk_new(b->start + b->size - size, size, 0, b, b->next);
    b->next = ins;
    if (!ins->next) a->top = ins;
    else ins->next->prev = ins;
    b->size -= size;
    return ins;
}

static void merge_free_blocks(SrBlockAlloc *a, SrBlock *from) {
    SrBlock *prev = from->prev;
    while (prev && !prev->taken) {
        prev->size += from->size;
        if (!from->next) a->top = prev;
        else from->next->prev = prev;
        prev->next = from->next;
        free(from);
        from = prev;
        prev = from->prev;
    }
    if (!prev) a->bottom = from;
    else prev->next = from;

    SrBlock *next = from->next;
    while (next && !next->taken) {
        from->size += next->size;
        from->next = next->next;
        free(next);
        next = from->next;
    }
    if (!next) a->top = from;
    else next->prev = from;
}

static SrBlock *block_from_addr(const SrBlockAlloc *a, uint32_t addr) {
    for (SrBlock *b = a->bottom; b; b = b->next)
        if (b->start <= addr && b->start + b->size > addr) return b;
    return NULL;
}

static uint32_t alloc_failed(const SrBlockAlloc *a, uint32_t size) {
    if (memlog()) {
        sr_ba_list(a);
        fprintf(stderr, "kmem %s: (%08x-%08x) failed to allocate %u (%08x) bytes of contiguous memory\n",
                a->name, a->range_start, a->range_start + a->range_size, size, size);
    }
    return SR_BA_FAIL;
}

/* *size is rounded up to sizeGrain and returned to the caller, as in PPSSPP. */
uint32_t sr_ba_alloc_aligned(SrBlockAlloc *a, uint32_t *size, uint32_t size_grain, uint32_t grain,
                             int from_top, const char *tag) {
    if (*size == 0 || *size > a->range_size) {
        if (memlog()) fprintf(stderr, "kmem %s: clearly bogus size %08x - failing allocation\n", a->name, *size);
        return SR_BA_FAIL;
    }
    if (grain < a->grain) grain = a->grain;
    if (size_grain < a->grain) size_grain = a->grain;
    *size = (*size + size_grain - 1) & ~(size_grain - 1);
    const uint32_t sz = *size;

    if (!from_top) {
        for (SrBlock *b = a->bottom; b; b = b->next) {
            uint32_t offset = b->start % grain;
            if (offset != 0) offset = grain - offset;
            uint32_t needed = offset + sz;
            if (!b->taken && b->size >= needed) {
                if (b->size != needed) insert_free_after(a, b, b->size - needed);
                if (offset >= a->grain) insert_free_before(a, b, offset);
                b->taken = 1;
                blk_set_allocated(a, b, tag);
                return b->start;
            }
        }
    } else {
        for (SrBlock *b = a->top; b; b = b->prev) {
            uint32_t offset = (b->start + b->size - sz) % grain;
            uint32_t needed = offset + sz;
            if (!b->taken && b->size >= needed) {
                if (b->size != needed) insert_free_before(a, b, b->size - needed);
                if (offset >= a->grain) insert_free_after(a, b, offset);
                b->taken = 1;
                blk_set_allocated(a, b, tag);
                return b->start;
            }
        }
    }
    return alloc_failed(a, sz);
}

uint32_t sr_ba_alloc(SrBlockAlloc *a, uint32_t *size, int from_top, const char *tag) {
    return sr_ba_alloc_aligned(a, size, a->grain, a->grain, from_top, tag);
}

/* Allocates [position, position+size) rounded out to the grain. Returns position (not the
 * rounded-down block start) on success, as PPSSPP does. */
uint32_t sr_ba_alloc_at(SrBlockAlloc *a, uint32_t position, uint32_t size, const char *tag) {
    if (size > a->range_size) {
        if (memlog()) fprintf(stderr, "kmem %s: clearly bogus size %08x - failing allocation\n", a->name, size);
        return SR_BA_FAIL;
    }
    uint32_t aligned_pos = position, aligned_size = size;
    if (position & (a->grain - 1)) {
        aligned_pos &= ~(a->grain - 1);
        aligned_size += position - aligned_pos;
    }
    aligned_size = (aligned_size + a->grain - 1) & ~(a->grain - 1);

    SrBlock *b = block_from_addr(a, aligned_pos);
    if (!b) {
        if (memlog()) fprintf(stderr, "kmem %s: AllocAt failed, %08x outside the partition\n", a->name, position);
        return alloc_failed(a, aligned_size);
    }
    if (b->taken) {
        if (memlog()) fprintf(stderr, "kmem %s: AllocAt failed, block taken at %08x\n", a->name, position);
        return SR_BA_FAIL;
    }
    if (b->start + b->size < aligned_pos + aligned_size) {
        if (memlog()) fprintf(stderr, "kmem %s: AllocAt failed, not enough contiguous space at %08x\n", a->name, position);
        return SR_BA_FAIL;
    }
    if (b->start == aligned_pos) {
        if (b->size != aligned_size) insert_free_after(a, b, b->size - aligned_size);
    } else {
        insert_free_before(a, b, aligned_pos - b->start);
        if (b->size > aligned_size) insert_free_after(a, b, b->size - aligned_size);
    }
    b->taken = 1;
    blk_set_allocated(a, b, tag);
    return position;
}

int sr_ba_free(SrBlockAlloc *a, uint32_t position) {
    SrBlock *b = block_from_addr(a, position);
    if (b && b->taken) {
        if (memlog()) fprintf(stderr, "kmem %s: free  %08x-%08x size %08x %s\n",
                              a->name, b->start, b->start + b->size, b->size, b->tag);
        b->taken = 0;
        merge_free_blocks(a, b);
        return 1;
    }
    if (memlog()) fprintf(stderr, "kmem %s: invalid free %08x\n", a->name, position);
    return 0;
}

uint32_t sr_ba_largest_free(const SrBlockAlloc *a) {
    uint32_t m = 0;
    for (const SrBlock *b = a->bottom; b; b = b->next)
        if (!b->taken && b->size > m) m = b->size;
    return m;
}

uint32_t sr_ba_total_free(const SrBlockAlloc *a) {
    uint32_t sum = 0;
    for (const SrBlock *b = a->bottom; b; b = b->next)
        if (!b->taken) sum += b->size;
    return sum;
}

uint32_t sr_ba_block_start(const SrBlockAlloc *a, uint32_t addr) {
    const SrBlock *b = block_from_addr(a, addr);
    return b ? b->start : SR_BA_FAIL;
}

uint32_t sr_ba_block_size(const SrBlockAlloc *a, uint32_t addr) {
    const SrBlock *b = block_from_addr(a, addr);
    return b ? b->size : SR_BA_FAIL;
}

/* ---- partitions ---- */

#define KERNEL_BASE   0x08000000u
#define VOLATILE_BASE 0x08400000u
#define USER_BASE     0x08800000u
#define USER_END      0x0A000000u   /* 32 MB PSP; SR_PARTITION_TOP (hex) overrides */

static SrBlockAlloc s_kernel   = { NULL, NULL, 0, 0, 256, "kernel" };
static SrBlockAlloc s_volatile = { NULL, NULL, 0, 0, 256, "volatile" };
static SrBlockAlloc s_user     = { NULL, NULL, 0, 0, 256, "user" };
static uint32_t s_user_end = USER_END;

void sr_kmem_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    const char *et = getenv("SR_PARTITION_TOP");
    if (et) s_user_end = (uint32_t)strtoul(et, NULL, 16);
    sr_ba_init(&s_kernel, KERNEL_BASE, VOLATILE_BASE - KERNEL_BASE);
    sr_ba_init(&s_volatile, VOLATILE_BASE, USER_BASE - VOLATILE_BASE);
    sr_ba_init(&s_user, USER_BASE, s_user_end - USER_BASE);
    /* The kernel's own code at the bottom of user memory (Kernel_Library helpers). */
    sr_ba_alloc_at(&s_user, USER_BASE, 0x4000, "usersystemlib");
    /* The module occupies its whole load range, BSS included (PPSSPP: "ELF/<name>", AllocAt). */
    uint32_t lo = sr_loaded_start(), hi = sr_loaded_end();
    if (hi > lo && lo >= USER_BASE && hi <= s_user_end)
        sr_ba_alloc_at(&s_user, lo, hi - lo, "ELF/module");
}

SrBlockAlloc *sr_kmem_user(void)     { sr_kmem_init(); return &s_user; }
SrBlockAlloc *sr_kmem_kernel(void)   { sr_kmem_init(); return &s_kernel; }
uint32_t      sr_kmem_user_end(void) { sr_kmem_init(); return s_user_end; }

/* PPSSPP's BlockAllocatorFromID for a user-mode caller: kernel partitions are refused. */
static SrBlockAlloc *partition_from_id(int id) {
    sr_kmem_init();
    switch (id) {
        case 2: case 6: return &s_user;
        case 5:         return &s_volatile;
        default:        return NULL;
    }
}

static SrBlockAlloc *partition_from_addr(uint32_t addr) {
    sr_kmem_init();
    addr &= 0x3FFFFFFFu;
    if (addr >= KERNEL_BASE && addr < VOLATILE_BASE) return &s_kernel;
    if (addr >= VOLATILE_BASE && addr < USER_BASE) return &s_volatile;
    if (addr >= USER_BASE && addr < s_user_end) return &s_user;
    return NULL;
}

/* Stacks for guest code the runtime calls outside any thread (the vblank interrupt handler, GE
 * and mpeg callbacks). They used to sit at fixed addresses in user memory, inside what the game
 * now gets as its heap. 0 = interrupt, 1 = callback; returns the stack top. */
uint32_t sr_hle_stack_top(int which) {
    static uint32_t top[2];
    static const char *tag[2] = { "hle/interrupt-stack", "hle/callback-stack" };
    which = which ? 1 : 0;
    if (!top[which]) {
        uint32_t size = 0x10000;
        uint32_t base = sr_ba_alloc(sr_kmem_kernel(), &size, 1, tag[which]);
        if (base == SR_BA_FAIL) { fprintf(stderr, "kmem: no kernel memory for %s\n", tag[which]); abort(); }
        top[which] = base + size;
    }
    return top[which];
}

/* ---- HLE helpers ---- */

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])
#define A3 (s->r[7])
#define T0 (s->r[8])

#define ERR_ERROR                  0x80020001u
#define ERR_UNKNOWN_UID            0x800200cbu
#define ERR_ILLEGAL_PERM           0x800200d1u
#define ERR_ILLEGAL_ARGUMENT       0x800200d2u
#define ERR_ILLEGAL_ADDR           0x800200d3u
#define ERR_ILLEGAL_PARTITION      0x800200d6u
#define ERR_ILLEGAL_MEMBLOCKTYPE   0x800200d8u
#define ERR_MEMBLOCK_ALLOC_FAILED  0x800200d9u
#define ERR_ILLEGAL_ALIGNMENT_SIZE 0x800200e4u
#define ERR_NO_MEMORY              0x80020190u
#define ERR_ILLEGAL_ATTR           0x80020191u
#define ERR_UNKNOWN_FPLID          0x8002019du
#define ERR_WAIT_TIMEOUT           0x800201a8u
#define ERR_WAIT_CANCEL            0x800201a9u
#define ERR_WAIT_DELETE            0x800201b5u
#define ERR_ILLEGAL_MEMBLOCK       0x800201b6u
#define ERR_ILLEGAL_MEMSIZE        0x800201b7u

/* Copies a guest C string (at most 31 chars, as the kernel's object names). */
static void guest_name(uint32_t addr, char out[32]) {
    int i = 0;
    for (; i < 31; i++) {
        char c = (char)MEM_R8(addr + (uint32_t)i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
}

/* ---- SysMemUserForUser: partition memory ---- */

enum { SMEM_Low = 0, SMEM_High = 1, SMEM_Addr = 2, SMEM_LowAligned = 3, SMEM_HighAligned = 4 };

typedef struct { int used; uint32_t uid, addr; SrBlockAlloc *alloc; char name[32]; } PartBlock;
#define PART_CAP 1024
static PartBlock s_part[PART_CAP];

static PartBlock *part_find(uint32_t uid) {
    for (int i = 0; i < PART_CAP; i++) if (s_part[i].used && s_part[i].uid == uid) return &s_part[i];
    return NULL;
}

/* sceKernelAllocPartitionMemory(partition, name, type, size, addr). The UID is only taken once
 * the allocation succeeded, so failed probes do not shift the game's later handles. */
static uint32_t h_AllocPartitionMemory(CpuState *s) {
    int partition = (int)A0, type = (int)A2;
    uint32_t name = A1, size = A3, addr = T0;
    if (type < SMEM_Low || type > SMEM_HighAligned) return ERR_ILLEGAL_MEMBLOCKTYPE;
    if (type == SMEM_LowAligned || type == SMEM_HighAligned)
        if ((addr & (addr - 1)) != 0 || addr == 0) return ERR_ILLEGAL_ALIGNMENT_SIZE;
    if (partition < 1 || partition > 9 || partition == 7) return ERR_ILLEGAL_ARGUMENT;
    SrBlockAlloc *a = partition_from_id(partition);
    if (!a) return ERR_ILLEGAL_PARTITION;
    if (!name) return ERR_ERROR;
    if (size == 0) return ERR_MEMBLOCK_ALLOC_FAILED;

    PartBlock *pb = NULL;
    for (int i = 0; i < PART_CAP; i++) if (!s_part[i].used) { pb = &s_part[i]; break; }
    if (!pb) { fprintf(stderr, "kmem: partition block table full\n"); return ERR_MEMBLOCK_ALLOC_FAILED; }
    char nm[32];
    guest_name(name, nm);
    uint32_t got;
    if (type == SMEM_Addr)
        got = sr_ba_alloc_at(a, addr & ~0xFFu, size, nm);
    else if (type == SMEM_LowAligned || type == SMEM_HighAligned)
        got = sr_ba_alloc_aligned(a, &size, 0x100, addr, type == SMEM_HighAligned, nm);
    else
        got = sr_ba_alloc(a, &size, type == SMEM_High, nm);
    if (got == SR_BA_FAIL) return ERR_MEMBLOCK_ALLOC_FAILED;

    pb->used = 1;
    pb->uid = sr_alloc_uid();
    pb->addr = got;
    pb->alloc = a;
    memcpy(pb->name, nm, sizeof(nm));
    if (memlog()) fprintf(stderr, "kmem: partition block uid=0x%x %s at %08x\n", pb->uid, pb->name, got);
    return pb->uid;
}

static uint32_t h_FreePartitionMemory(CpuState *s) {
    PartBlock *pb = part_find(A0);
    if (!pb) return ERR_UNKNOWN_UID;
    sr_ba_free(pb->alloc, pb->addr);
    pb->used = 0;
    return 0;
}

static uint32_t h_GetBlockHeadAddr(CpuState *s) {
    PartBlock *pb = part_find(A0);
    return pb ? pb->addr : 0;
}

static uint32_t h_MaxFreeMemSize(CpuState *s)   { (void)s; return sr_ba_largest_free(sr_kmem_user()); }
static uint32_t h_TotalFreeMemSize(CpuState *s) { (void)s; return sr_ba_total_free(sr_kmem_user()); }

/* ---- ThreadManForUser: fixed-length pools (FPL) ---- */

#define FPL_ATTR_PRIORITY 0x0100u
#define FPL_ATTR_HIGHMEM  0x4000u
#define FPL_ATTR_KNOWN    (FPL_ATTR_PRIORITY | FPL_ATTR_HIGHMEM)

typedef struct {
    int used;
    uint32_t uid;
    char name[32];
    uint32_t attr;
    int32_t blocksize, num_blocks;
    uint32_t address, aligned_size;
    uint8_t *blocks;          /* 1 = taken */
    int next_block;           /* round-robin search start, as PPSSPP's FPL::nextBlock */
    int num_waiting;
    uint32_t cancel_gen;      /* bumped by sceKernelCancelFpl; waiters compare it */
} Fpl;
#define FPL_CAP 256
static Fpl s_fpl[FPL_CAP];

static Fpl *fpl_find(uint32_t uid) {
    for (int i = 0; i < FPL_CAP; i++) if (s_fpl[i].used && s_fpl[i].uid == uid) return &s_fpl[i];
    return NULL;
}

static int fpl_alloc_block(Fpl *f) {
    for (int i = 0; i < f->num_blocks; i++) {
        int b = f->next_block++ % f->num_blocks;
        if (!f->blocks[b]) { f->blocks[b] = 1; return b; }
    }
    return -1;
}

/* sceKernelCreateFpl(name, partition, attr, blockSize, numBlocks, opt) */
static uint32_t h_CreateFpl(CpuState *s) {
    uint32_t name = A0, mpid = A1, attr = A2, block_size = A3, num_blocks = T0, opt = s->r[9];
    if (!name) return ERR_NO_MEMORY;
    if (mpid < 1 || mpid > 9 || mpid == 7) return ERR_ILLEGAL_ARGUMENT;
    SrBlockAlloc *a = partition_from_id((int)mpid);
    if (!a) return ERR_ILLEGAL_PERM;
    if (((attr & ~FPL_ATTR_KNOWN) & ~0xFFu) != 0) return ERR_ILLEGAL_ATTR;

    /* Size limits measured on a PSP (PPSSPP's formula). */
    int illegal = block_size == 0 || num_blocks == 0;
    if (!illegal && (uint64_t)block_size > (0x100000000ULL / (uint64_t)num_blocks) - 4ULL) illegal = 1;
    if (!illegal && (uint64_t)num_blocks >= 0x100000000ULL / (((uint64_t)block_size + 3ULL) & ~3ULL)) illegal = 1;
    if (illegal) return ERR_ILLEGAL_MEMSIZE;

    int alignment = 4;
    if (opt && sr_inrange(opt)) {
        uint32_t optsize = MEM_R32(opt);
        if (optsize >= 4) alignment = (int)MEM_R32(opt + 4);
        if ((alignment & (alignment - 1)) != 0) return ERR_ILLEGAL_ARGUMENT;
    }
    if (alignment < 4) alignment = 4;

    int aligned_size = ((int)block_size + alignment - 1) & ~(alignment - 1);
    uint32_t total = (uint32_t)aligned_size * num_blocks;
    char nm[32], tag[40];
    guest_name(name, nm);
    snprintf(tag, sizeof(tag), "FPL/%s", nm);

    Fpl *f = NULL;
    for (int i = 0; i < FPL_CAP; i++) if (!s_fpl[i].used) { f = &s_fpl[i]; break; }
    if (!f) { fprintf(stderr, "kmem: FPL table full\n"); return ERR_NO_MEMORY; }
    uint32_t address = sr_ba_alloc(a, &total, (attr & FPL_ATTR_HIGHMEM) != 0, tag);
    if (address == SR_BA_FAIL) return ERR_NO_MEMORY;

    memset(f, 0, sizeof(*f));
    f->used = 1;
    f->uid = sr_alloc_uid_type(SR_TMID_FPL);
    memcpy(f->name, nm, sizeof(nm));
    f->attr = attr;
    f->blocksize = (int32_t)block_size;
    f->num_blocks = (int32_t)num_blocks;
    f->address = address;
    f->aligned_size = (uint32_t)aligned_size;
    f->blocks = (uint8_t *)calloc(num_blocks, 1);
    if (!f->blocks) { fprintf(stderr, "kmem: out of host memory\n"); abort(); }
    if (memlog()) fprintf(stderr, "kmem: FPL uid=0x%x %s %d x 0x%x at %08x\n",
                          f->uid, f->name, f->num_blocks, f->aligned_size, f->address);
    return f->uid;
}

static uint32_t h_DeleteFpl(CpuState *s) {
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    uint32_t uid = f->uid;
    SrBlockAlloc *a = partition_from_addr(f->address);
    if (a) sr_ba_free(a, f->address);
    free(f->blocks);
    sr_unregister_uid(uid);
    memset(f, 0, sizeof(*f));
    sched_wake(uid);          /* waiters find the pool gone and return WAIT_DELETE */
    sched_preempt();
    return 0;
}

/* Hardware timing quirks for short timeouts (PPSSPP __KernelSetFplTimeout). */
static uint32_t fpl_timeout_us(uint32_t micro) {
    if ((int)micro <= 5) return 20;
    if (micro == 7) return 25;
    if (micro <= 215) return 250;
    return micro;
}

/* Shared by sceKernelAllocateFpl[CB]: take a block or wait for one. A woken waiter retries,
 * so with PSP_FPL_ATTR_PRIORITY the highest-priority waiter (which the scheduler runs first)
 * gets the freed block; FIFO order among equal priorities is not modelled. */
static uint32_t fpl_allocate(CpuState *s, uint32_t uid, uint32_t out, uint32_t toptr) {
    Fpl *f = fpl_find(uid);
    if (!f) return ERR_UNKNOWN_FPLID;
    uint32_t gen = f->cancel_gen;
    for (;;) {
        int b = fpl_alloc_block(f);
        if (b >= 0) {
            MEM_W32(out, f->address + f->aligned_size * (uint32_t)b);
            return 0;
        }
        f->num_waiting++;
        int timed_out = 0;
        if (toptr) timed_out = sched_block_on_timeout(uid, fpl_timeout_us(MEM_R32(toptr)));
        else sched_block_on(uid);
        f = fpl_find(uid);
        if (!f) return ERR_WAIT_DELETE;
        f->num_waiting--;
        if (f->cancel_gen != gen) return ERR_WAIT_CANCEL;
        if (timed_out) { MEM_W32(toptr, 0); return ERR_WAIT_TIMEOUT; }
    }
    (void)s;
}

static uint32_t h_AllocateFpl(CpuState *s) { return fpl_allocate(s, A0, A1, A2); }

static uint32_t h_TryAllocateFpl(CpuState *s) {
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    int b = fpl_alloc_block(f);
    if (b < 0) return ERR_NO_MEMORY;
    MEM_W32(A1, f->address + f->aligned_size * (uint32_t)b);
    return 0;
}

static uint32_t h_FreeFpl(CpuState *s) {
    uint32_t block_ptr = A1;
    if (block_ptr > sr_kmem_user_end()) return ERR_ILLEGAL_ADDR;
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    int b = (int)((block_ptr - f->address) / f->aligned_size);
    if (b < 0 || b >= f->num_blocks) return ERR_ILLEGAL_MEMBLOCK;
    if (!f->blocks[b]) return ERR_ILLEGAL_MEMBLOCK;   /* already free */
    f->blocks[b] = 0;
    if (f->num_waiting) { sched_wake(f->uid); sched_preempt(); }
    return 0;
}

static uint32_t h_CancelFpl(CpuState *s) {
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    if (A1 && sr_inrange(A1)) MEM_W32(A1, (uint32_t)f->num_waiting);
    f->cancel_gen++;
    sched_wake(f->uid);
    sched_preempt();
    return 0;
}

/* SceKernelFplInfo: size(0) name[32](4) attr(36) blockSize(40) numBlocks(44) freeBlocks(48)
 * numWaitThreads(52). Written only when the caller set a nonzero size. */
static uint32_t h_ReferFplStatus(CpuState *s) {
    Fpl *f = fpl_find(A0);
    if (!f) return ERR_UNKNOWN_FPLID;
    uint32_t st = A1;
    if (st && sr_inrange(st) && MEM_R32(st) != 0) {
        int free_blocks = 0;
        for (int i = 0; i < f->num_blocks; i++) if (!f->blocks[i]) free_blocks++;
        MEM_W32(st + 0, 56);
        for (uint32_t i = 0; i < 32; i++) MEM_W8(st + 4 + i, (uint8_t)f->name[i]);
        MEM_W32(st + 36, f->attr);
        MEM_W32(st + 40, (uint32_t)f->blocksize);
        MEM_W32(st + 44, (uint32_t)f->num_blocks);
        MEM_W32(st + 48, (uint32_t)free_blocks);
        MEM_W32(st + 52, (uint32_t)f->num_waiting);
    }
    return 0;
}

void sr_kmem_register(void) {
    sr_hle_register(0x237dbd4f, "sceKernelAllocPartitionMemory", h_AllocPartitionMemory);
    sr_hle_register(0xb6d61d02, "sceKernelFreePartitionMemory", h_FreePartitionMemory);
    sr_hle_register(0x9d9a5ba1, "sceKernelGetBlockHeadAddr", h_GetBlockHeadAddr);
    sr_hle_register(0xa291f107, "sceKernelMaxFreeMemSize", h_MaxFreeMemSize);
    sr_hle_register(0xf919f628, "sceKernelTotalFreeMemSize", h_TotalFreeMemSize);
    sr_hle_register(0xc07bb470, "sceKernelCreateFpl", h_CreateFpl);
    sr_hle_register(0xed1410e0, "sceKernelDeleteFpl", h_DeleteFpl);
    sr_hle_register(0xd979e9bf, "sceKernelAllocateFpl", h_AllocateFpl);
    sr_hle_register(0xe7282cb6, "sceKernelAllocateFplCB", h_AllocateFpl);
    sr_hle_register(0x623ae665, "sceKernelTryAllocateFpl", h_TryAllocateFpl);
    sr_hle_register(0xf6414a71, "sceKernelFreeFpl", h_FreeFpl);
    sr_hle_register(0xa8aa591f, "sceKernelCancelFpl", h_CancelFpl);
    sr_hle_register(0xd8199e4c, "sceKernelReferFplStatus", h_ReferFplStatus);
}
