#ifndef GE_SDL3VK_FEEDBACK_REGION_H
#define GE_SDL3VK_FEEDBACK_REGION_H

#include <math.h>
#include <stdint.h>

typedef struct GeVkRegion {
    int32_t x0, y0, x1, y1;
} GeVkRegion;

typedef struct GeVkFeedbackRegion {
    GeVkRegion dirty;
    int valid;
} GeVkFeedbackRegion;

static inline int ge_vk_region_empty(GeVkRegion region) {
    return region.x0 >= region.x1 || region.y0 >= region.y1;
}

static inline int ge_vk_region_overlaps(GeVkRegion a, GeVkRegion b) {
    return !ge_vk_region_empty(a) && !ge_vk_region_empty(b) &&
           a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

static inline int ge_vk_region_contains(GeVkRegion outer, GeVkRegion inner) {
    return ge_vk_region_empty(inner) ||
           (!ge_vk_region_empty(outer) && outer.x0 <= inner.x0 &&
            outer.y0 <= inner.y0 && outer.x1 >= inner.x1 && outer.y1 >= inner.y1);
}

static inline float ge_vk_region_clamp(float value, int32_t low, int32_t high) {
    return value < (float)low ? (float)low :
           value > (float)high ? (float)high : value;
}

static inline GeVkRegion ge_vk_draw_damage(
    float min_x, float min_y, float max_x, float max_y,
    uint32_t scale, GeVkRegion scissor) {
    float x0 = min_x * (float)scale, y0 = min_y * (float)scale;
    float x1 = max_x * (float)scale, y1 = max_y * (float)scale;
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return scissor;
    GeVkRegion result = {
        (int32_t)ge_vk_region_clamp(x0 - 1.0f, scissor.x0, scissor.x1),
        (int32_t)ge_vk_region_clamp(y0 - 1.0f, scissor.y0, scissor.y1),
        (int32_t)ge_vk_region_clamp(x1 + 2.0f, scissor.x0, scissor.x1),
        (int32_t)ge_vk_region_clamp(y1 + 2.0f, scissor.y0, scissor.y1),
    };
    return result;
}

static inline void ge_vk_feedback_record(
    GeVkFeedbackRegion *snapshot, GeVkRegion damage) {
    if (!snapshot->valid || ge_vk_region_empty(damage)) return;
    if (ge_vk_region_empty(snapshot->dirty)) {
        snapshot->dirty = damage;
        return;
    }
    if (damage.x0 < snapshot->dirty.x0) snapshot->dirty.x0 = damage.x0;
    if (damage.y0 < snapshot->dirty.y0) snapshot->dirty.y0 = damage.y0;
    if (damage.x1 > snapshot->dirty.x1) snapshot->dirty.x1 = damage.x1;
    if (damage.y1 > snapshot->dirty.y1) snapshot->dirty.y1 = damage.y1;
}

static inline int ge_vk_feedback_copy_region(
    GeVkFeedbackRegion *snapshot, GeVkRegion whole, GeVkRegion sampled,
    GeVkRegion *copy) {
    if (!snapshot->valid) {
        *copy = whole;
        snapshot->valid = 1;
    } else {
        if (!ge_vk_region_overlaps(snapshot->dirty, sampled)) return 0;
        *copy = snapshot->dirty;
    }
    snapshot->dirty = (GeVkRegion){0, 0, 0, 0};
    return 1;
}

#endif
