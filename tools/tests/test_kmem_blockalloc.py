#!/usr/bin/env python3
"""Differential test of src/rt/kmem.c's BlockAllocator port against PPSSPP's own
Core/Util/BlockAllocator.cpp.

Builds one program from both: PPSSPP's file (with its save-state code stripped and its logging
captured) and kmem.c (with the rest of the runtime stubbed out). Both allocators then get the same
random sequence of Alloc, AllocAligned, AllocAt, Free and free-size queries, and every return value,
every rounded size and the full block list (start, size, taken, tag) must match after each step.

    python tools/tests/test_kmem_blockalloc.py --ppsspp=<ppsspp checkout> [--cxx=g++] [--ops=N] [--keep]

PPSSPP is found from --ppsspp, else $PPSSPP_SRC, else a `ppsspp` folder next to this repo or its
superproject. Needs a C/C++ compiler (g++ or clang++) on PATH. Exits non-zero on any mismatch.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
RT = os.path.join(REPO, 'src', 'rt')

STUB_HEADERS = {
    'Common/CommonTypes.h': r"""
#pragma once
#include <cstdint>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
""",
    'Common/Log.h': r"""
#pragma once
#include "Common/CommonTypes.h"
namespace Log { enum { sceKernel, HLE }; }
void ref_log(const char *fmt, ...);
void ref_quiet(const char *fmt, ...);
#define DEBUG_LOG(cat, ...) ref_log(__VA_ARGS__)
#define ERROR_LOG(cat, ...) ref_quiet(__VA_ARGS__)
#define VERBOSE_LOG(cat, ...) ref_quiet(__VA_ARGS__)
#define WARN_LOG_REPORT(cat, ...) ref_quiet(__VA_ARGS__)
#define ERROR_LOG_REPORT(cat, ...) ref_quiet(__VA_ARGS__)
#define _assert_(x) ((void)0)
""",
    'Common/StringUtils.h': r"""
#pragma once
#include <cstring>
template <size_t N> inline void truncate_cpy(char (&dst)[N], const char *src) {
    strncpy(dst, src, N - 1); dst[N - 1] = 0;
}
""",
    'Core/Debugger/MemBlockInfo.h': r"""
#pragma once
enum class MemBlockFlags { ALLOC, SUB_ALLOC, FREE, SUB_FREE };
inline void NotifyMemInfo(MemBlockFlags, unsigned, unsigned, const char *) {}
""",
    'Core/Reporting.h': '#pragma once\n',
    'Common/Serialize/Serializer.h': '#pragma once\n',
    'Common/Serialize/SerializeFuncs.h': '#pragma once\n',
}

# PPSSPP side: a C interface over the class, plus a capture of ListBlocks' DEBUG_LOG lines.
REF_GLUE = r"""
#include <cstdarg>
#include <cstdio>
#include <string>
#include "Core/Util/BlockAllocator.h"
static std::string g_cap;
static bool g_capturing = false;
void ref_log(const char *fmt, ...) {
    if (!g_capturing) return;
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    g_cap += buf; g_cap += '\n';
}
void ref_quiet(const char *, ...) {}
static BlockAllocator *g_ref;
extern "C" {
void ref_init(unsigned grain, unsigned start, unsigned size) { delete g_ref; g_ref = new BlockAllocator((int)grain); g_ref->Init(start, size, false); }
unsigned ref_alloc(unsigned *size, int top, const char *tag) { u32 s = *size; u32 r = g_ref->Alloc(s, top != 0, tag); *size = s; return r; }
unsigned ref_alloc_aligned(unsigned *size, unsigned sg, unsigned g, int top, const char *tag) { u32 s = *size; u32 r = g_ref->AllocAligned(s, sg, g, top != 0, tag); *size = s; return r; }
unsigned ref_alloc_at(unsigned pos, unsigned size, const char *tag) { return g_ref->AllocAt(pos, size, tag); }
int ref_free(unsigned pos) { return g_ref->Free(pos) ? 1 : 0; }
unsigned ref_largest(void) { return g_ref->GetLargestFreeBlockSize(); }
unsigned ref_total(void) { return g_ref->GetTotalFreeBytes(); }
unsigned ref_block_start(unsigned a) { return g_ref->GetBlockStartFromAddress(a); }
unsigned ref_block_size(unsigned a) { return g_ref->GetBlockSizeFromAddress(a); }
const char *ref_dump(void) { g_cap.clear(); g_capturing = true; g_ref->ListBlocks(); g_capturing = false; return g_cap.c_str(); }
}
"""

# Port side: kmem.c compiled as-is with the rest of the runtime stubbed.
MINE_GLUE = r"""
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "recomp.h"
uint8_t *g_mem;
void sr_oor(uint32_t a, uint32_t v, int store) { (void)a; (void)v; (void)store; }
uint32_t sr_alloc_uid(void) { return 0x100; }
uint32_t sr_alloc_uid_type(uint32_t type) { (void)type; return sr_alloc_uid(); }
void sr_register_uid_type(uint32_t uid, uint32_t type) { (void)uid; (void)type; }
void sr_unregister_uid(uint32_t uid) { (void)uid; }
uint32_t sr_uid_list(uint32_t type, uint32_t *out, uint32_t capacity) {
    (void)type; (void)out; (void)capacity; return 0;
}
uint32_t sr_loaded_start(void) { return 0; }
uint32_t sr_loaded_end(void) { return 0; }
void sr_hle_register(uint32_t nid, const char *name, HleFn fn) { (void)nid; (void)name; (void)fn; }
int sched_wake(uint32_t o) { (void)o; return 0; }
void sched_preempt(void) {}
void sched_block_on(uint32_t o) { (void)o; }
int sched_block_on_timeout(uint32_t o, uint32_t u) { (void)o; (void)u; return 1; }
#include "kmem.c"
static SrBlockAlloc g_mine;
void mine_init(unsigned grain, unsigned start, unsigned size) {
    sr_ba_shutdown(&g_mine); g_mine.grain = grain; g_mine.name = "test"; sr_ba_init(&g_mine, start, size);
}
unsigned mine_alloc(unsigned *size, int top, const char *tag) { return sr_ba_alloc(&g_mine, size, top, tag); }
unsigned mine_alloc_aligned(unsigned *size, unsigned sg, unsigned g, int top, const char *tag) { return sr_ba_alloc_aligned(&g_mine, size, sg, g, top, tag); }
unsigned mine_alloc_at(unsigned pos, unsigned size, const char *tag) { return sr_ba_alloc_at(&g_mine, pos, size, tag); }
int mine_free(unsigned pos) { return sr_ba_free(&g_mine, pos); }
unsigned mine_largest(void) { return sr_ba_largest_free(&g_mine); }
unsigned mine_total(void) { return sr_ba_total_free(&g_mine); }
unsigned mine_block_start(unsigned a) { return sr_ba_block_start(&g_mine, a); }
unsigned mine_block_size(unsigned a) { return sr_ba_block_size(&g_mine, a); }
const char *mine_dump(void) {
    static char buf[1 << 20]; size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "-----------\n");
    for (const SrBlock *b = g_mine.bottom; b; b = b->next)
        n += (size_t)snprintf(buf + n, sizeof(buf) - n, "Block: %08x - %08x size %08x taken=%i tag=%s\n",
                              b->start, b->start + b->size, b->size, b->taken ? 1 : 0, b->tag);
    snprintf(buf + n, sizeof(buf) - n, "-----------\n");
    return buf;
}
"""

DRIVER = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
void ref_init(unsigned, unsigned, unsigned); void mine_init(unsigned, unsigned, unsigned);
unsigned ref_alloc(unsigned *, int, const char *); unsigned mine_alloc(unsigned *, int, const char *);
unsigned ref_alloc_aligned(unsigned *, unsigned, unsigned, int, const char *);
unsigned mine_alloc_aligned(unsigned *, unsigned, unsigned, int, const char *);
unsigned ref_alloc_at(unsigned, unsigned, const char *); unsigned mine_alloc_at(unsigned, unsigned, const char *);
int ref_free(unsigned); int mine_free(unsigned);
unsigned ref_largest(void); unsigned mine_largest(void);
unsigned ref_total(void); unsigned mine_total(void);
unsigned ref_block_start(unsigned); unsigned mine_block_start(unsigned);
unsigned ref_block_size(unsigned); unsigned mine_block_size(unsigned);
const char *ref_dump(void); const char *mine_dump(void);
#ifdef __cplusplus
}
#endif

static uint64_t rs;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)(rs >> 16); }
static uint32_t pick(uint32_t n) { return n ? rnd() % n : 0; }

static unsigned live[4096]; static int nlive;
static long fails;

#define CHECK(what, a, b) do { if ((a) != (b)) { \
    fprintf(stderr, "seed %u op %d: %s mismatch: ppsspp=%08x port=%08x\n", seed, op, what, (unsigned)(a), (unsigned)(b)); \
    fails++; return; } } while (0)

static unsigned rand_size(unsigned range) {
    switch (pick(8)) {
        case 0: return 0;
        case 1: return range + 1 + pick(0x1000);
        case 2: return 1 + pick(range);
        case 3: return 1 + pick(0x100);
        default: return 1 + pick(0x40000);
    }
}

static void run(unsigned seed, int ops, unsigned grain, unsigned start, unsigned size) {
    rs = 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x100000001B3ull); rnd();
    ref_init(grain, start, size); mine_init(grain, start, size);
    nlive = 0;
    char tag[32];
    for (int op = 0; op < ops; op++) {
        snprintf(tag, sizeof(tag), "t%d", op);
        unsigned k = pick(100), a, b;
        if (k < 30) {                                   /* Alloc */
            unsigned s1 = rand_size(size), s2 = s1; int top = (int)pick(2);
            a = ref_alloc(&s1, top, tag); b = mine_alloc(&s2, top, tag);
            CHECK("Alloc", a, b); CHECK("Alloc size", s1, s2);
            if (a != 0xFFFFFFFFu && nlive < 4096) live[nlive++] = a;
        } else if (k < 45) {                            /* AllocAligned */
            unsigned s1 = rand_size(size), s2 = s1; int top = (int)pick(2);
            unsigned sg = 1u << pick(13), g = 1u << pick(21);
            a = ref_alloc_aligned(&s1, sg, g, top, tag); b = mine_alloc_aligned(&s2, sg, g, top, tag);
            CHECK("AllocAligned", a, b); CHECK("AllocAligned size", s1, s2);
            if (a != 0xFFFFFFFFu && nlive < 4096) live[nlive++] = a;
        } else if (k < 60) {                            /* AllocAt */
            unsigned pos = pick(4) == 0 ? start - 0x1000 + pick(size + 0x2000) : start + pick(size);
            if (pick(3) == 0) pos &= ~0xFFu;
            unsigned s = pick(6) == 0 ? rand_size(size) : 1 + pick(0x20000);
            a = ref_alloc_at(pos, s, tag); b = mine_alloc_at(pos, s, tag);
            CHECK("AllocAt", a, b);
            if (a != 0xFFFFFFFFu && nlive < 4096) live[nlive++] = a;
        } else if (k < 88) {                            /* Free: a live block, inside one, or anywhere */
            unsigned pos;
            int i = -1;
            if (nlive && pick(5)) { i = (int)pick((uint32_t)nlive); pos = live[i]; if (pick(4) == 0) pos += pick(0x100); }
            else pos = start - 0x100 + pick(size + 0x200);
            a = (unsigned)ref_free(pos); b = (unsigned)mine_free(pos);
            CHECK("Free", a, b);
            if (i >= 0) live[i] = live[--nlive];
        } else {                                        /* queries */
            unsigned q = start - 0x100 + pick(size + 0x200);
            CHECK("Largest", ref_largest(), mine_largest());
            CHECK("Total", ref_total(), mine_total());
            CHECK("BlockStart", ref_block_start(q), mine_block_start(q));
            CHECK("BlockSize", ref_block_size(q), mine_block_size(q));
        }
        const char *rd = ref_dump(), *md = mine_dump();
        if (strcmp(rd, md) != 0) {
            fprintf(stderr, "seed %u op %d: block lists differ\n--- ppsspp\n%s--- port\n%s", seed, op, rd, md);
            fails++;
            return;
        }
    }
}

int main(int argc, char **argv) {
    int ops = argc > 1 ? atoi(argv[1]) : 4000;
    int seeds = argc > 2 ? atoi(argv[2]) : 40;
    long total = 0;
    for (int s = 0; s < seeds; s++) {
        /* PPSSPP's user partition (grain 256) and its default 16-byte grain. */
        run((unsigned)s, ops, 256, 0x08800000u, 0x01800000u); total += ops;
        run((unsigned)s + 1000, ops, 16, 0x08000000u, 0x00400000u); total += ops;
    }
    printf("%ld operations over %d seeds, %ld mismatches\n", total, seeds * 2, fails);
    return fails ? 1 : 0;
}
"""


def find_ppsspp(arg):
    cands = [arg, os.environ.get('PPSSPP_SRC'),
             os.path.join(os.path.dirname(REPO), 'ppsspp'),
             os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(REPO))), 'ppsspp')]
    for c in cands:
        if c and os.path.isfile(os.path.join(c, 'Core', 'Util', 'BlockAllocator.cpp')):
            return c
    return None


def strip_dostate(src):
    """Removes the save-state functions (they need PPSSPP's serializer)."""
    for sig in ('void BlockAllocator::DoState(PointerWrap &p)', 'void BlockAllocator::Block::DoState(PointerWrap &p)'):
        i = src.index(sig)
        j = src.index('{', i)
        depth = 0
        for k in range(j, len(src)):
            if src[k] == '{':
                depth += 1
            elif src[k] == '}':
                depth -= 1
                if depth == 0:
                    src = src[:i] + src[k + 1:]
                    break
    return src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ppsspp')
    ap.add_argument('--cxx', default='g++')
    ap.add_argument('--ops', type=int, default=4000)
    ap.add_argument('--seeds', type=int, default=40)
    ap.add_argument('--keep', action='store_true')
    args = ap.parse_args()

    pp = find_ppsspp(args.ppsspp)
    if not pp:
        sys.exit('PPSSPP source not found: pass --ppsspp=<checkout> or set PPSSPP_SRC')
    work = tempfile.mkdtemp(prefix='kmem_test_')
    try:
        for rel, text in STUB_HEADERS.items():
            path = os.path.join(work, 'inc', rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'w') as f:
                f.write(text)
        os.makedirs(os.path.join(work, 'inc', 'Core', 'Util'), exist_ok=True)
        shutil.copy(os.path.join(pp, 'Core', 'Util', 'BlockAllocator.h'),
                    os.path.join(work, 'inc', 'Core', 'Util', 'BlockAllocator.h'))
        with open(os.path.join(pp, 'Core', 'Util', 'BlockAllocator.cpp'), encoding='utf-8') as f:
            ref_src = strip_dostate(f.read())
        ref_src = re.sub(r'#include "Common/Serialize/[^"]+"\n', '', ref_src)
        files = {'ppsspp_blockalloc.cpp': ref_src, 'ref_glue.cpp': REF_GLUE,
                 'mine_glue.c': MINE_GLUE, 'driver.c': DRIVER}
        for name, text in files.items():
            with open(os.path.join(work, name), 'w', encoding='utf-8') as f:
                f.write(text)
        exe = os.path.join(work, 'kmem_test.exe')
        cc = args.cxx.replace('++', 'cc') if args.cxx.endswith('g++') else args.cxx.replace('clang++', 'clang')
        objs = []
        for name, comp, extra in (('ppsspp_blockalloc.cpp', args.cxx, ['-std=c++17']),
                                  ('ref_glue.cpp', args.cxx, ['-std=c++17']),
                                  ('mine_glue.c', cc, ['-std=gnu11', '-Wno-unused-function']),
                                  ('driver.c', cc, ['-std=gnu11'])):
            obj = os.path.join(work, name + '.o')
            cmd = [comp, '-O1', '-c', os.path.join(work, name), '-o', obj,
                   '-I', os.path.join(work, 'inc'), '-I', RT] + extra
            subprocess.run(cmd, check=True)
            objs.append(obj)
        subprocess.run([args.cxx] + objs + ['-o', exe], check=True)
        r = subprocess.run([exe, str(args.ops), str(args.seeds)])
        sys.exit(r.returncode)
    finally:
        if args.keep:
            print('kept', work)
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
