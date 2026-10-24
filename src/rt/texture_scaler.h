#ifndef PSP_RECOMP_TEXTURE_SCALER_H
#define PSP_RECOMP_TEXTURE_SCALER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int sr_texture_scale_rgba(
    const uint32_t *source, uint32_t width, uint32_t height,
    uint32_t factor, uint32_t *destination);

#ifdef __cplusplus
}
#endif

#endif
