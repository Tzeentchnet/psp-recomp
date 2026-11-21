#include "present_layout.h"

#include <string.h>

#define PSP_DISPLAY_WIDTH 480u
#define PSP_DISPLAY_HEIGHT 272u

int sr_vk_present_rect(
    uint32_t output_width, uint32_t output_height,
    SrVkPresentRect *rect) {
    if (!rect)
        return 0;
    memset(rect, 0, sizeof(*rect));
    if (!output_width || !output_height)
        return 0;

    if ((uint64_t)output_width * PSP_DISPLAY_HEIGHT <=
        (uint64_t)output_height * PSP_DISPLAY_WIDTH) {
        rect->width = output_width;
        rect->height = (uint32_t)(
            (uint64_t)output_width * PSP_DISPLAY_HEIGHT /
            PSP_DISPLAY_WIDTH);
    } else {
        rect->height = output_height;
        rect->width = (uint32_t)(
            (uint64_t)output_height * PSP_DISPLAY_WIDTH /
            PSP_DISPLAY_HEIGHT);
    }
    if (!rect->width)
        rect->width = 1;
    if (!rect->height)
        rect->height = 1;
    rect->x = (output_width - rect->width) / 2u;
    rect->y = (output_height - rect->height) / 2u;
    return 1;
}

int sr_vk_capture_layout(
    uint32_t source_width, uint32_t source_height,
    int internal_resolution, SrVkCaptureLayout *layout) {
    if (!layout)
        return 0;
    memset(layout, 0, sizeof(*layout));
    if (!source_width || !source_height ||
        source_width % PSP_DISPLAY_WIDTH != 0 ||
        source_height % PSP_DISPLAY_HEIGHT != 0) {
        return 0;
    }

    uint32_t width_scale = source_width / PSP_DISPLAY_WIDTH;
    uint32_t height_scale = source_height / PSP_DISPLAY_HEIGHT;
    if (!width_scale || width_scale != height_scale)
        return 0;
    if (internal_resolution) {
        layout->width = source_width;
        layout->height = source_height;
        layout->source_step = 1;
        return 1;
    }
    layout->width = PSP_DISPLAY_WIDTH;
    layout->height = PSP_DISPLAY_HEIGHT;
    layout->source_step = width_scale;
    layout->sample_offset = width_scale / 2u;
    return 1;
}

int sr_vk_internal_extent(
    uint32_t internal_resolution, SrVkExtent *extent) {
    if (!extent)
        return 0;
    memset(extent, 0, sizeof(*extent));
    if (!internal_resolution ||
        internal_resolution > UINT32_MAX / PSP_DISPLAY_WIDTH) {
        return 0;
    }
    extent->width = PSP_DISPLAY_WIDTH * internal_resolution;
    extent->height = PSP_DISPLAY_HEIGHT * internal_resolution;
    return 1;
}
