/*
 * GE-exact SIMD patch arithmetic adapted from PPSSPP
 * GPU/Common/SplineCommon.cpp, commit ce97cb120182b46e6a849e839982c3752cf5549b.
 *
 * Copyright (C) 2009-2026 PPSSPP project.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SR_GE_PATCH_MATH_H
#define SR_GE_PATCH_MATH_H

#include <math.h>
#include <stdint.h>
#include <string.h>

static inline float sr_patch_ge_lerp(float a, float b, int k) {
    if (k == 0) return a;
    if (k == 256) return b;
    uint32_t bits_a, bits_b;
    memcpy(&bits_a, &a, sizeof(bits_a));
    memcpy(&bits_b, &b, sizeof(bits_b));
    int exponent_a = (int)((bits_a >> 23) & 0xffu);
    int exponent_b = (int)((bits_b >> 23) & 0xffu);
    int exponent =
        exponent_a > exponent_b ? exponent_a : exponent_b;
    if (exponent == 0) return 0.0f;
    int fixed_a = 0;
    int fixed_b = 0;
    if (exponent_a != 0 && exponent - exponent_a < 16) {
        fixed_a = (int)(
            ((bits_a & 0x7fffffu) | 0x800000u) >>
            (exponent - exponent_a + 8));
        if ((bits_a & 0x80000000u) != 0) fixed_a = -fixed_a;
    }
    if (exponent_b != 0 && exponent - exponent_b < 16) {
        fixed_b = (int)(
            ((bits_b & 0x7fffffu) | 0x800000u) >>
            (exponent - exponent_b + 8));
        if ((bits_b & 0x80000000u) != 0) fixed_b = -fixed_b;
    }
    int result = fixed_a + (((fixed_b - fixed_a) * k) >> 8);
    if (exponent <= 15)
        return ldexpf((float)result, exponent - 127 - 15);
    uint32_t scale_bits = (uint32_t)(exponent - 15) << 23;
    float scale;
    memcpy(&scale, &scale_bits, sizeof(scale));
    return (float)result * scale;
}

static inline float sr_patch_spline_eval(
    const float values[4], const int alpha[6],
    float *before, float *after) {
    float left0 =
        sr_patch_ge_lerp(values[0], values[1], alpha[0]);
    float left1 =
        sr_patch_ge_lerp(values[1], values[2], alpha[1]);
    float left2 =
        sr_patch_ge_lerp(values[2], values[3], alpha[2]);
    float middle0 = sr_patch_ge_lerp(left0, left1, alpha[3]);
    float middle1 = sr_patch_ge_lerp(left1, left2, alpha[4]);
    if (before) *before = middle0;
    if (after) *after = middle1;
    return sr_patch_ge_lerp(middle0, middle1, alpha[5]);
}

static inline int sr_patch_color_lerp(int a, int b, int k) {
    return a + (((b - a) * k) >> 8);
}

static inline int sr_patch_spline_color(
    const int values[4], const int alpha[6]) {
    int left0 =
        sr_patch_color_lerp(values[0], values[1], alpha[0]);
    int left1 =
        sr_patch_color_lerp(values[1], values[2], alpha[1]);
    int left2 =
        sr_patch_color_lerp(values[2], values[3], alpha[2]);
    return sr_patch_color_lerp(
        sr_patch_color_lerp(left0, left1, alpha[3]),
        sr_patch_color_lerp(left1, left2, alpha[4]), alpha[5]);
}

static inline float sr_patch_truncate_float24(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bits &= 0xffffff00u;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static inline float sr_patch_ge_add(float a, float b) {
    uint32_t bits_a, bits_b;
    memcpy(&bits_a, &a, sizeof(bits_a));
    memcpy(&bits_b, &b, sizeof(bits_b));
    int exponent_a = (int)((bits_a >> 23) & 0xffu);
    int exponent_b = (int)((bits_b >> 23) & 0xffu);
    if (exponent_a == 0 || exponent_b == 0) {
        if (exponent_a == 0 && exponent_b == 0) return 0.0f;
        return exponent_a == 0
            ? sr_patch_truncate_float24(b)
            : sr_patch_truncate_float24(a);
    }
    int max_exponent =
        exponent_a > exponent_b ? exponent_a : exponent_b;
    int clear_a = 8 + max_exponent - exponent_a;
    int clear_b = 8 + max_exponent - exponent_b;
    bits_a = clear_a >= 24
        ? bits_a & 0x80000000u
        : bits_a & ~((1u << clear_a) - 1u);
    bits_b = clear_b >= 24
        ? bits_b & 0x80000000u
        : bits_b & ~((1u << clear_b) - 1u);
    memcpy(&a, &bits_a, sizeof(a));
    memcpy(&b, &bits_b, sizeof(b));
    return a + b;
}

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
#define SR_GE_PATCH_SIMD_SUPPORTED 1

#include <emmintrin.h>

typedef struct {
    __m128i k[6];
    __m128i k0[6];
    __m128i k256[6];
} SrPatchSplineAlpha4;

static inline __m128i sr_patch_mul16(__m128i a, __m128i b) {
    return _mm_madd_epi16(
        a, _mm_and_si128(b, _mm_set1_epi32(0x0000ffff)));
}

static inline __m128 sr_patch_select(
    __m128i mask, __m128 if_true, __m128 if_false) {
    return _mm_or_ps(
        _mm_and_ps(_mm_castsi128_ps(mask), if_true),
        _mm_andnot_ps(_mm_castsi128_ps(mask), if_false));
}

static inline __m128 sr_patch_ge_lerp4(
    __m128 a, __m128 b, __m128i k,
    __m128i k0, __m128i k256) {
    const __m128i exponent_mask = _mm_set1_epi32(0x7f800000);
    const __m128i absolute_mask = _mm_set1_epi32(0x7fffffff);
    __m128i bits_a = _mm_castps_si128(a);
    __m128i bits_b = _mm_castps_si128(b);
    __m128 absolute_a = _mm_castsi128_ps(
        _mm_and_si128(bits_a, absolute_mask));
    __m128 absolute_b = _mm_castsi128_ps(
        _mm_and_si128(bits_b, absolute_mask));
    __m128i exponent = _mm_and_si128(
        _mm_castps_si128(_mm_max_ps(absolute_a, absolute_b)),
        exponent_mask);
    __m128 down = _mm_castsi128_ps(_mm_sub_epi32(
        _mm_set1_epi32((int)(269u << 23)), exponent));
    __m128 up = _mm_castsi128_ps(_mm_sub_epi32(
        exponent, _mm_set1_epi32(15 << 23)));
    __m128i fixed_a = _mm_cvttps_epi32(_mm_mul_ps(a, down));
    __m128i fixed_b = _mm_cvttps_epi32(_mm_mul_ps(b, down));
    __m128i difference = _mm_sub_epi32(fixed_b, fixed_a);
    __m128i quotient = _mm_add_epi32(
        sr_patch_mul16(_mm_srai_epi32(difference, 8), k),
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_and_si128(difference, _mm_set1_epi32(255)), k),
            8));
    __m128 result = _mm_mul_ps(
        _mm_cvtepi32_ps(_mm_add_epi32(fixed_a, quotient)), up);
    result = sr_patch_select(k256, b, result);
    result = sr_patch_select(k0, a, result);

    __m128i small = _mm_cmplt_epi32(
        exponent, _mm_set1_epi32(16 << 23));
    int small_mask = _mm_movemask_ps(_mm_castsi128_ps(small));
    if (small_mask != 0) {
        float values_a[4], values_b[4], values_result[4];
        int values_k[4];
        _mm_storeu_ps(values_a, a);
        _mm_storeu_ps(values_b, b);
        _mm_storeu_ps(values_result, result);
        _mm_storeu_si128((__m128i *)values_k, k);
        for (int lane = 0; lane < 4; lane++) {
            if ((small_mask & (1 << lane)) != 0) {
                values_result[lane] = sr_patch_ge_lerp(
                    values_a[lane], values_b[lane], values_k[lane]);
            }
        }
        result = _mm_loadu_ps(values_result);
    }
    return result;
}

static inline __m128 sr_patch_ge_add4(__m128 a, __m128 b) {
    const __m128i exponent_mask = _mm_set1_epi32(0x7f800000);
    const __m128i absolute_mask = _mm_set1_epi32(0x7fffffff);
    __m128i bits_a = _mm_castps_si128(a);
    __m128i bits_b = _mm_castps_si128(b);
    __m128 zero = _mm_setzero_ps();
    __m128 value_a = sr_patch_select(
        _mm_cmpeq_epi32(
            _mm_and_si128(bits_a, exponent_mask),
            _mm_setzero_si128()),
        zero, a);
    __m128 value_b = sr_patch_select(
        _mm_cmpeq_epi32(
            _mm_and_si128(bits_b, exponent_mask),
            _mm_setzero_si128()),
        zero, b);
    __m128 absolute_a = _mm_castsi128_ps(
        _mm_and_si128(bits_a, absolute_mask));
    __m128 absolute_b = _mm_castsi128_ps(
        _mm_and_si128(bits_b, absolute_mask));
    __m128i exponent = _mm_and_si128(
        _mm_castps_si128(_mm_max_ps(absolute_a, absolute_b)),
        exponent_mask);
    __m128 down = _mm_castsi128_ps(_mm_sub_epi32(
        _mm_set1_epi32((int)(269u << 23)), exponent));
    __m128 up = _mm_castsi128_ps(_mm_sub_epi32(
        exponent, _mm_set1_epi32(15 << 23)));
    __m128i fixed_sum = _mm_add_epi32(
        _mm_cvttps_epi32(_mm_mul_ps(value_a, down)),
        _mm_cvttps_epi32(_mm_mul_ps(value_b, down)));
    __m128 result = _mm_mul_ps(_mm_cvtepi32_ps(fixed_sum), up);

    __m128i small = _mm_cmplt_epi32(
        exponent, _mm_set1_epi32(16 << 23));
    int small_mask = _mm_movemask_ps(_mm_castsi128_ps(small));
    if (small_mask != 0) {
        float values_a[4], values_b[4], values_result[4];
        _mm_storeu_ps(values_a, a);
        _mm_storeu_ps(values_b, b);
        _mm_storeu_ps(values_result, result);
        for (int lane = 0; lane < 4; lane++) {
            if ((small_mask & (1 << lane)) != 0) {
                values_result[lane] =
                    sr_patch_ge_add(values_a[lane], values_b[lane]);
            }
        }
        result = _mm_loadu_ps(values_result);
    }
    return result;
}

static inline void sr_patch_spline_alpha4_init(
    SrPatchSplineAlpha4 *out, const int alpha[6][4]) {
    for (int level = 0; level < 6; level++) {
        out->k[level] = _mm_loadu_si128(
            (const __m128i *)alpha[level]);
        out->k0[level] = _mm_cmpeq_epi32(
            out->k[level], _mm_setzero_si128());
        out->k256[level] = _mm_cmpeq_epi32(
            out->k[level], _mm_set1_epi32(256));
    }
}

static inline __m128 sr_patch_spline_eval4(
    const __m128 values[4], const SrPatchSplineAlpha4 *alpha,
    __m128 *before, __m128 *after) {
    __m128 left0 = sr_patch_ge_lerp4(
        values[0], values[1], alpha->k[0],
        alpha->k0[0], alpha->k256[0]);
    __m128 left1 = sr_patch_ge_lerp4(
        values[1], values[2], alpha->k[1],
        alpha->k0[1], alpha->k256[1]);
    __m128 left2 = sr_patch_ge_lerp4(
        values[2], values[3], alpha->k[2],
        alpha->k0[2], alpha->k256[2]);
    __m128 middle0 = sr_patch_ge_lerp4(
        left0, left1, alpha->k[3],
        alpha->k0[3], alpha->k256[3]);
    __m128 middle1 = sr_patch_ge_lerp4(
        left1, left2, alpha->k[4],
        alpha->k0[4], alpha->k256[4]);
    if (before) *before = middle0;
    if (after) *after = middle1;
    return sr_patch_ge_lerp4(
        middle0, middle1, alpha->k[5],
        alpha->k0[5], alpha->k256[5]);
}

static inline __m128i sr_patch_spline_color4(
    const __m128i values[4], const SrPatchSplineAlpha4 *alpha) {
    __m128i left0 = _mm_add_epi32(
        values[0],
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_sub_epi32(values[1], values[0]), alpha->k[0]),
            8));
    __m128i left1 = _mm_add_epi32(
        values[1],
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_sub_epi32(values[2], values[1]), alpha->k[1]),
            8));
    __m128i left2 = _mm_add_epi32(
        values[2],
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_sub_epi32(values[3], values[2]), alpha->k[2]),
            8));
    __m128i middle0 = _mm_add_epi32(
        left0,
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_sub_epi32(left1, left0), alpha->k[3]),
            8));
    __m128i middle1 = _mm_add_epi32(
        left1,
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_sub_epi32(left2, left1), alpha->k[4]),
            8));
    return _mm_add_epi32(
        middle0,
        _mm_srai_epi32(
            sr_patch_mul16(
                _mm_sub_epi32(middle1, middle0), alpha->k[5]),
            8));
}

#else
#define SR_GE_PATCH_SIMD_SUPPORTED 0
#endif

#endif
