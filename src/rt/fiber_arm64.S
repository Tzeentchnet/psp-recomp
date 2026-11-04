#if defined(__APPLE__) && defined(__aarch64__)

#include "fiber_arm64.h"

.text
.p2align 2
.globl _sr_fiber_swap_context
_sr_fiber_swap_context:
    stp x19, x20, [x0, #SR_ARM64_CONTEXT_X19]
    stp x21, x22, [x0, #SR_ARM64_CONTEXT_X21]
    stp x23, x24, [x0, #SR_ARM64_CONTEXT_X23]
    stp x25, x26, [x0, #SR_ARM64_CONTEXT_X25]
    stp x27, x28, [x0, #SR_ARM64_CONTEXT_X27]
    stp x29, x30, [x0, #SR_ARM64_CONTEXT_X29]

    mov x9, sp
    str x9, [x0, #SR_ARM64_CONTEXT_SP]
    mrs x9, fpcr
    str x9, [x0, #SR_ARM64_CONTEXT_FPCR]
    mrs x9, fpsr
    str x9, [x0, #SR_ARM64_CONTEXT_FPSR]

    stp d8, d9, [x0, #SR_ARM64_CONTEXT_D8]
    stp d10, d11, [x0, #SR_ARM64_CONTEXT_D10]
    stp d12, d13, [x0, #SR_ARM64_CONTEXT_D12]
    stp d14, d15, [x0, #SR_ARM64_CONTEXT_D14]

    ldp d8, d9, [x1, #SR_ARM64_CONTEXT_D8]
    ldp d10, d11, [x1, #SR_ARM64_CONTEXT_D10]
    ldp d12, d13, [x1, #SR_ARM64_CONTEXT_D12]
    ldp d14, d15, [x1, #SR_ARM64_CONTEXT_D14]

    ldr x9, [x1, #SR_ARM64_CONTEXT_FPCR]
    msr fpcr, x9
    ldr x9, [x1, #SR_ARM64_CONTEXT_FPSR]
    msr fpsr, x9

    ldp x19, x20, [x1, #SR_ARM64_CONTEXT_X19]
    ldp x21, x22, [x1, #SR_ARM64_CONTEXT_X21]
    ldp x23, x24, [x1, #SR_ARM64_CONTEXT_X23]
    ldp x25, x26, [x1, #SR_ARM64_CONTEXT_X25]
    ldp x27, x28, [x1, #SR_ARM64_CONTEXT_X27]
    ldr x9, [x1, #SR_ARM64_CONTEXT_SP]
    ldp x29, x30, [x1, #SR_ARM64_CONTEXT_X29]
    mov sp, x9
    ret

.subsections_via_symbols

#endif
