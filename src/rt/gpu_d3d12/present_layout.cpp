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
