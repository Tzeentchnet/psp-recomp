/*
 * Scalar/SIMD equivalence coverage adapted from PPSSPP
 * unittest/TestSplineTessellation.cpp, commit
 * ce97cb120182b46e6a849e839982c3752cf5549b.
 *
 * Copyright (C) 2009-2026 PPSSPP project.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "ge_patch_math.h"

#include <stdio.h>
#include <stdlib.h>

static uint32_t s_random_state = 0x89abcdefu;

static uint32_t next_random(void) {
    s_random_state =
        s_random_state * 1664525u + 1013904223u;
    return s_random_state;
}

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float float_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float random_finite_float(void) {
    uint32_t value = next_random();
    uint32_t sign = value & 0x80000000u;
    switch (value & 15u) {
        case 0:
            return float_from_bits(sign);
        case 1:
            return float_from_bits(sign | (next_random() & 0x7fffffu));
        case 2:
            return float_from_bits(
                sign | (1u << 23) | (next_random() & 0x7fffffu));
        default: {
            uint32_t exponent = 1u + next_random() % 254u;
            return float_from_bits(
                sign | (exponent << 23) |
                (next_random() & 0x7fffffu));
        }
    }
}

static void fail_float(
    const char *test, int iteration, int lane,
    float actual, float expected) {
    fprintf(
        stderr,
        "%s failed at iteration %d lane %d: "
        "got %a (%08x), expected %a (%08x)\n",
        test, iteration, lane,
        actual, float_bits(actual), expected, float_bits(expected));
    exit(1);
}

static void require_float_bits(
    const char *test, int iteration, int lane,
    float actual, float expected) {
    if (float_bits(actual) != float_bits(expected))
        fail_float(test, iteration, lane, actual, expected);
}

static void fail_int(
    const char *test, int iteration, int lane,
    int actual, int expected) {
    fprintf(
        stderr,
        "%s failed at iteration %d lane %d: "
        "got %d, expected %d\n",
        test, iteration, lane, actual, expected);
    exit(1);
}

#if SR_GE_PATCH_SIMD_SUPPORTED

static __m128i load_alpha_values(
    const int values[4], int match) {
    __m128i loaded = _mm_loadu_si128((const __m128i *)values);
    return _mm_cmpeq_epi32(loaded, _mm_set1_epi32(match));
}

static void test_endpoint_bit_patterns(void) {
    const uint32_t bits_a[4] = {
        0x7f800000u, 0xff800000u, 0x7fc12345u, 0x80000001u,
    };
    const uint32_t bits_b[4] = {
        0xffc54321u, 0x00000001u, 0x7f7fffffu, 0x80000000u,
    };
    float values_a[4], values_b[4], actual[4];
    int alpha[4] = {0, 0, 0, 0};
    for (int lane = 0; lane < 4; lane++) {
        values_a[lane] = float_from_bits(bits_a[lane]);
        values_b[lane] = float_from_bits(bits_b[lane]);
    }
    __m128 a = _mm_loadu_ps(values_a);
    __m128 b = _mm_loadu_ps(values_b);
    __m128i k = _mm_loadu_si128((const __m128i *)alpha);
    _mm_storeu_ps(
        actual,
        sr_patch_ge_lerp4(
            a, b, k,
            load_alpha_values(alpha, 0),
            load_alpha_values(alpha, 256)));
    for (int lane = 0; lane < 4; lane++) {
        require_float_bits(
            "lerp k=0 raw bit preservation", 0, lane,
            actual[lane], values_a[lane]);
        alpha[lane] = 256;
    }
    k = _mm_loadu_si128((const __m128i *)alpha);
    _mm_storeu_ps(
        actual,
        sr_patch_ge_lerp4(
            a, b, k,
            load_alpha_values(alpha, 0),
            load_alpha_values(alpha, 256)));
    for (int lane = 0; lane < 4; lane++) {
        require_float_bits(
            "lerp k=256 raw bit preservation", 0, lane,
            actual[lane], values_b[lane]);
    }
}

static void test_ge_lerp4(void) {
    for (int iteration = 0; iteration < 100000; iteration++) {
        float values_a[4], values_b[4], actual[4];
        int alpha[4];
        for (int lane = 0; lane < 4; lane++) {
            values_a[lane] = random_finite_float();
            values_b[lane] = random_finite_float();
            uint32_t selector = next_random() & 15u;
            alpha[lane] = selector == 0
                ? 0 : selector == 1
                    ? 256 : (int)(next_random() % 257u);
        }
        __m128i k = _mm_loadu_si128((const __m128i *)alpha);
        __m128 result = sr_patch_ge_lerp4(
            _mm_loadu_ps(values_a), _mm_loadu_ps(values_b), k,
            load_alpha_values(alpha, 0),
            load_alpha_values(alpha, 256));
        _mm_storeu_ps(actual, result);
        for (int lane = 0; lane < 4; lane++) {
            require_float_bits(
                "GE lerp4", iteration, lane, actual[lane],
                sr_patch_ge_lerp(
                    values_a[lane], values_b[lane], alpha[lane]));
        }
    }
}

static void test_ge_add4(void) {
    for (int iteration = 0; iteration < 100000; iteration++) {
        float values_a[4], values_b[4], actual[4];
        for (int lane = 0; lane < 4; lane++) {
            values_a[lane] = random_finite_float();
            values_b[lane] = random_finite_float();
        }
        _mm_storeu_ps(
            actual,
            sr_patch_ge_add4(
                _mm_loadu_ps(values_a),
                _mm_loadu_ps(values_b)));
        for (int lane = 0; lane < 4; lane++) {
            require_float_bits(
                "GE add4", iteration, lane, actual[lane],
                sr_patch_ge_add(values_a[lane], values_b[lane]));
        }
    }
}

static void test_spline_eval4(void) {
    for (int iteration = 0; iteration < 30000; iteration++) {
        float values[4][4];
        int alpha[6][4];
        for (int point = 0; point < 4; point++) {
            for (int lane = 0; lane < 4; lane++)
                values[point][lane] = random_finite_float();
        }
        for (int level = 0; level < 6; level++) {
            for (int lane = 0; lane < 4; lane++)
                alpha[level][lane] = (int)(next_random() % 257u);
        }
        __m128 packed_values[4];
        for (int point = 0; point < 4; point++)
            packed_values[point] = _mm_loadu_ps(values[point]);
        SrPatchSplineAlpha4 packed_alpha;
        sr_patch_spline_alpha4_init(&packed_alpha, alpha);
        __m128 before4, after4;
        __m128 result4 = sr_patch_spline_eval4(
            packed_values, &packed_alpha, &before4, &after4);
        float actual[4], actual_before[4], actual_after[4];
        _mm_storeu_ps(actual, result4);
        _mm_storeu_ps(actual_before, before4);
        _mm_storeu_ps(actual_after, after4);
        for (int lane = 0; lane < 4; lane++) {
            float scalar_values[4];
            int scalar_alpha[6];
            for (int point = 0; point < 4; point++)
                scalar_values[point] = values[point][lane];
            for (int level = 0; level < 6; level++)
                scalar_alpha[level] = alpha[level][lane];
            float expected_before, expected_after;
            float expected = sr_patch_spline_eval(
                scalar_values, scalar_alpha,
                &expected_before, &expected_after);
            require_float_bits(
                "spline eval4", iteration, lane,
                actual[lane], expected);
            require_float_bits(
                "spline eval4 before", iteration, lane,
                actual_before[lane], expected_before);
            require_float_bits(
                "spline eval4 after", iteration, lane,
                actual_after[lane], expected_after);
        }
    }
}

static void test_spline_color4(void) {
    for (int iteration = 0; iteration < 30000; iteration++) {
        int values[4][4];
        int alpha[6][4];
        for (int point = 0; point < 4; point++) {
            for (int lane = 0; lane < 4; lane++) {
                values[point][lane] =
                    ((int)(next_random() & 255u) << 7) | 0x7f;
            }
        }
        for (int level = 0; level < 6; level++) {
            for (int lane = 0; lane < 4; lane++)
                alpha[level][lane] = (int)(next_random() % 257u);
        }
        __m128i packed_values[4];
        for (int point = 0; point < 4; point++) {
            packed_values[point] = _mm_loadu_si128(
                (const __m128i *)values[point]);
        }
        SrPatchSplineAlpha4 packed_alpha;
        sr_patch_spline_alpha4_init(&packed_alpha, alpha);
        int actual[4];
        _mm_storeu_si128(
            (__m128i *)actual,
            sr_patch_spline_color4(packed_values, &packed_alpha));
        for (int lane = 0; lane < 4; lane++) {
            int scalar_values[4];
            int scalar_alpha[6];
            for (int point = 0; point < 4; point++)
                scalar_values[point] = values[point][lane];
            for (int level = 0; level < 6; level++)
                scalar_alpha[level] = alpha[level][lane];
            int expected =
                sr_patch_spline_color(scalar_values, scalar_alpha);
            if (actual[lane] != expected) {
                fail_int(
                    "spline color4", iteration, lane,
                    actual[lane], expected);
            }
        }
    }
}

static void test_segment_gather(void) {
    const int segments[4] = {0, 0, 1, 2};
    float columns[6];
    int alpha[6][4];
    for (int iteration = 0; iteration < 10000; iteration++) {
        for (int column = 0; column < 6; column++)
            columns[column] = random_finite_float();
        for (int level = 0; level < 6; level++) {
            for (int lane = 0; lane < 4; lane++)
                alpha[level][lane] = (int)(next_random() % 257u);
        }
        __m128 values[4];
        for (int point = 0; point < 4; point++) {
            values[point] = _mm_setr_ps(
                columns[segments[0] + point],
                columns[segments[1] + point],
                columns[segments[2] + point],
                columns[segments[3] + point]);
        }
        SrPatchSplineAlpha4 packed_alpha;
        sr_patch_spline_alpha4_init(&packed_alpha, alpha);
        float actual[4];
        _mm_storeu_ps(
            actual,
            sr_patch_spline_eval4(
                values, &packed_alpha, NULL, NULL));
        for (int lane = 0; lane < 4; lane++) {
            float scalar_values[4];
            int scalar_alpha[6];
            for (int point = 0; point < 4; point++) {
                scalar_values[point] =
                    columns[segments[lane] + point];
            }
            for (int level = 0; level < 6; level++)
                scalar_alpha[level] = alpha[level][lane];
            require_float_bits(
                "cross-segment gather", iteration, lane,
                actual[lane],
                sr_patch_spline_eval(
                    scalar_values, scalar_alpha, NULL, NULL));
        }
    }
}

#endif

int main(void) {
#if SR_GE_PATCH_SIMD_SUPPORTED
    unsigned int previous_control = _mm_getcsr();
    _mm_setcsr(previous_control | (1u << 6) | (1u << 15));
    test_endpoint_bit_patterns();
    test_ge_lerp4();
    test_ge_add4();
    test_spline_eval4();
    test_spline_color4();
    test_segment_gather();
    _mm_setcsr(previous_control);
    puts("ge_patch_math_test: PASS");
#else
    puts("ge_patch_math_test: PASS (scalar-only target)");
#endif
    return 0;
}
