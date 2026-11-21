#ifndef PSP_RECOMP_FIBER_ARM64_H
#define PSP_RECOMP_FIBER_ARM64_H

#define SR_ARM64_CONTEXT_X19 0
#define SR_ARM64_CONTEXT_X20 8
#define SR_ARM64_CONTEXT_X21 16
#define SR_ARM64_CONTEXT_X22 24
#define SR_ARM64_CONTEXT_X23 32
#define SR_ARM64_CONTEXT_X24 40
#define SR_ARM64_CONTEXT_X25 48
#define SR_ARM64_CONTEXT_X26 56
#define SR_ARM64_CONTEXT_X27 64
#define SR_ARM64_CONTEXT_X28 72
#define SR_ARM64_CONTEXT_X29 80
#define SR_ARM64_CONTEXT_X30 88
#define SR_ARM64_CONTEXT_SP 96
#define SR_ARM64_CONTEXT_FPCR 104
#define SR_ARM64_CONTEXT_FPSR 112
#define SR_ARM64_CONTEXT_D8 128
#define SR_ARM64_CONTEXT_D9 136
#define SR_ARM64_CONTEXT_D10 144
#define SR_ARM64_CONTEXT_D11 152
#define SR_ARM64_CONTEXT_D12 160
#define SR_ARM64_CONTEXT_D13 168
#define SR_ARM64_CONTEXT_D14 176
#define SR_ARM64_CONTEXT_D15 184
#define SR_ARM64_CONTEXT_SIZE 192

#ifndef __ASSEMBLER__

#include <stddef.h>
#include <stdint.h>

typedef struct SrArm64FiberContext {
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t x30;
    uint64_t sp;
    uint64_t fpcr;
    uint64_t fpsr;
    uint64_t reserved;
    uint64_t d8;
    uint64_t d9;
    uint64_t d10;
    uint64_t d11;
    uint64_t d12;
    uint64_t d13;
    uint64_t d14;
    uint64_t d15;
} SrArm64FiberContext;

_Static_assert(
    offsetof(SrArm64FiberContext, x19) == SR_ARM64_CONTEXT_X19,
    "arm64 fiber x19 offset");
_Static_assert(
    offsetof(SrArm64FiberContext, x30) == SR_ARM64_CONTEXT_X30,
    "arm64 fiber x30 offset");
_Static_assert(
    offsetof(SrArm64FiberContext, sp) == SR_ARM64_CONTEXT_SP,
    "arm64 fiber sp offset");
_Static_assert(
    offsetof(SrArm64FiberContext, fpcr) == SR_ARM64_CONTEXT_FPCR,
    "arm64 fiber fpcr offset");
_Static_assert(
    offsetof(SrArm64FiberContext, fpsr) == SR_ARM64_CONTEXT_FPSR,
    "arm64 fiber fpsr offset");
_Static_assert(
    offsetof(SrArm64FiberContext, d8) == SR_ARM64_CONTEXT_D8,
    "arm64 fiber d8 offset");
_Static_assert(
    offsetof(SrArm64FiberContext, d15) == SR_ARM64_CONTEXT_D15,
    "arm64 fiber d15 offset");
_Static_assert(
    sizeof(SrArm64FiberContext) == SR_ARM64_CONTEXT_SIZE,
    "arm64 fiber context size");

void sr_fiber_swap_context(
    SrArm64FiberContext *source,
    const SrArm64FiberContext *target);

#endif

#endif
