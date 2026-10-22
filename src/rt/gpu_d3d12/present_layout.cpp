#include "present.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define PSP_DISPLAY_WIDTH 480.0f
#define PSP_DISPLAY_HEIGHT 272.0f

void ged3d12_present_viewport(
    UINT output_width, UINT output_height, GeD3D12PresentViewport *out) {
    memset(out, 0, sizeof(*out));
    if (!output_width || !output_height) return;

    float width;
    float height;
    if ((uint64_t)output_width * 272u <=
        (uint64_t)output_height * 480u) {
        width = (float)output_width;
        height = width * (PSP_DISPLAY_HEIGHT / PSP_DISPLAY_WIDTH);
    } else {
        height = (float)output_height;
        width = height * (PSP_DISPLAY_WIDTH / PSP_DISPLAY_HEIGHT);
    }
    float x = ((float)output_width - width) * 0.5f;
    float y = ((float)output_height - height) * 0.5f;

    out->viewport.TopLeftX = x;
    out->viewport.TopLeftY = y;
    out->viewport.Width = width;
    out->viewport.Height = height;
    out->viewport.MinDepth = 0.0f;
    out->viewport.MaxDepth = 1.0f;
    out->scissor.left = (LONG)floorf(x);
    out->scissor.top = (LONG)floorf(y);
    out->scissor.right = (LONG)ceilf(x + width);
    out->scissor.bottom = (LONG)ceilf(y + height);
}

void ged3d12_present_capture_layout(
    UINT render_scale, bool internal_resolution,
    GeD3D12CaptureLayout *out) {
    memset(out, 0, sizeof(*out));
    if (!render_scale) return;
    if (internal_resolution) {
        out->width = 480u * render_scale;
        out->height = 272u * render_scale;
        out->source_step = 1;
        return;
    }
    out->width = 480u;
    out->height = 272u;
    out->source_step = render_scale;
    out->sample_offset = render_scale / 2u;
}

uint32_t ged3d12_present_capture_rgb(
    uint32_t rgb, uint32_t format) {
    uint32_t red = (rgb >> 16) & 0xffu;
    uint32_t green = (rgb >> 8) & 0xffu;
    uint32_t blue = rgb & 0xffu;
    switch (format & 3u) {
        case 0:
            red &= 0xf8u;
            green &= 0xfcu;
            blue &= 0xf8u;
            break;
        case 1:
            red &= 0xf8u;
            green &= 0xf8u;
            blue &= 0xf8u;
            break;
        case 2:
            red &= 0xf0u;
            green &= 0xf0u;
            blue &= 0xf0u;
            break;
        default:
            break;
    }
    return (red << 16) | (green << 8) | blue;
}
