#ifndef SR_SDL3VK_PRESENT_LAYOUT_H
#define SR_SDL3VK_PRESENT_LAYOUT_H

#include <stdint.h>

typedef struct SrVkPresentRect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} SrVkPresentRect;

typedef struct SrVkCaptureLayout {
    uint32_t width;
    uint32_t height;
    uint32_t source_step;
    uint32_t sample_offset;
} SrVkCaptureLayout;

typedef struct SrVkExtent {
    uint32_t width;
    uint32_t height;
} SrVkExtent;

int sr_vk_present_rect(
    uint32_t output_width, uint32_t output_height,
    SrVkPresentRect *rect);
int sr_vk_capture_layout(
    uint32_t source_width, uint32_t source_height,
    int internal_resolution, SrVkCaptureLayout *layout);
int sr_vk_internal_extent(
    uint32_t internal_resolution, SrVkExtent *extent);

#endif
