#include "recomp.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define SR_MXCSR_DAZ (1u << 6)
#define SR_MXCSR_FTZ (1u << 15)
#elif defined(__aarch64__) || defined(_M_ARM64)
#define SR_FPCR_FZ (1ull << 24)
#endif

void sr_configure_fpu(void) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    unsigned int control = _mm_getcsr();
    _mm_setcsr(control | SR_MXCSR_DAZ | SR_MXCSR_FTZ);
#elif defined(__aarch64__) && (defined(__clang__) || defined(__GNUC__))
    uint64_t control;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    control |= SR_FPCR_FZ;
    __asm__ volatile("msr fpcr, %0" : : "r"(control));
#endif
}
