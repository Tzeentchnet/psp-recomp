#include "recomp.h"

#include <stdio.h>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define TEST_MXCSR_DAZ (1u << 6)
#define TEST_MXCSR_FTZ (1u << 15)
#if defined(_MSC_VER)
#define TEST_NOINLINE __declspec(noinline)
#else
#define TEST_NOINLINE __attribute__((noinline))
#endif

static TEST_NOINLINE float multiply_runtime(float left, float right) {
    return left * right;
}
#endif

int main(void) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    unsigned int original = _mm_getcsr();
    unsigned int required = TEST_MXCSR_DAZ | TEST_MXCSR_FTZ;
    _mm_setcsr(original & ~required);
    sr_configure_fpu();
    unsigned int configured = _mm_getcsr();
    if ((configured & required) != required) {
        fprintf(
            stderr, "fpu_mode_test: MXCSR 0x%08x lacks FTZ/DAZ 0x%08x\n",
            configured, required);
        return 1;
    }
    volatile float smallest_normal = 0x1p-126f;
    volatile float smallest_subnormal = 0x1p-149f;
    volatile float half = 0.5f;
    volatile float two = 2.0f;
    float underflow_result = multiply_runtime(smallest_normal, half);
    float denormal_result = multiply_runtime(smallest_subnormal, two);
    if (underflow_result != 0.0f) {
        fputs("fpu_mode_test: underflow result was not flushed to zero\n", stderr);
        return 1;
    }
    if (denormal_result != 0.0f) {
        fputs("fpu_mode_test: denormal input was not treated as zero\n", stderr);
        return 1;
    }
    printf("FPU mode tests passed: MXCSR=0x%08x\n", configured);
#else
    puts("FPU mode tests skipped: non-x86 host");
#endif
    return 0;
}
