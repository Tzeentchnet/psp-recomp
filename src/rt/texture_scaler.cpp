#include "texture_scaler.h"

#include "../../third_party/xbrz/xbrz.h"

#include <limits.h>
#include <string.h>

int sr_texture_scale_rgba(
    const uint32_t *source, uint32_t width, uint32_t height,
    uint32_t factor, uint32_t *destination) {
    if (!source || !destination || !width || !height ||
        width > INT_MAX || height > INT_MAX ||
        factor < 1u || factor > 5u) {
        return 0;
    }
    if (factor == 1u) {
        memcpy(
            destination, source,
            (size_t)width * (size_t)height * sizeof(uint32_t));
        return 1;
    }
    xbrz::scale(
        factor, source, destination, (int)width, (int)height,
        xbrz::ColorFormat::ARGB);
    return 1;
}
