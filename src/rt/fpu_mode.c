#include "recomp.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define SR_MXCSR_DAZ (1u << 6)
#define SR_MXCSR_FTZ (1u << 15)
#endif

void sr_configure_fpu(void) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    unsigned int control = _mm_getcsr();
    _mm_setcsr(control | SR_MXCSR_DAZ | SR_MXCSR_FTZ);
#endif
}
