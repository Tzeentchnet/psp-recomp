#ifndef SR_GPU_SDL3VK_UPLOAD_SCALE_H
#define SR_GPU_SDL3VK_UPLOAD_SCALE_H

#include <stdint.h>

typedef struct SrVkUploadScalePlan {
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t physical_width;
    uint32_t physical_height;
    uint64_t logical_bytes;
    uint64_t host_bytes;
    uint64_t expanded_pixels;
    int use_gpu;
    int fallback;
} SrVkUploadScalePlan;

static inline int sr_vk_upload_scale_plan(
    uint32_t logical_width, uint32_t logical_height, uint32_t scale,
    int gpu_supported, int force_cpu, SrVkUploadScalePlan *out) {
    if (!logical_width || !logical_height || !scale || !out ||
        logical_width > UINT32_MAX / scale ||
        logical_height > UINT32_MAX / scale) {
        return 0;
    }

    SrVkUploadScalePlan plan = {0};
    plan.logical_width = logical_width;
    plan.logical_height = logical_height;
    plan.physical_width = logical_width * scale;
    plan.physical_height = logical_height * scale;

    uint64_t logical_pixels =
        (uint64_t)logical_width * (uint64_t)logical_height;
    plan.expanded_pixels =
        (uint64_t)plan.physical_width * (uint64_t)plan.physical_height;
    if (logical_pixels > UINT64_MAX / 4u ||
        plan.expanded_pixels > UINT64_MAX / 4u) {
        return 0;
    }

    plan.logical_bytes = logical_pixels * 4u;
    plan.use_gpu = scale > 1 && gpu_supported && !force_cpu;
    plan.fallback = scale > 1 && !plan.use_gpu;
    plan.host_bytes =
        (plan.use_gpu ? logical_pixels : plan.expanded_pixels) * 4u;
    *out = plan;
    return 1;
}

#endif
