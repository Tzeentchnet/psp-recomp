#ifndef PSP_RECOMP_GRAPHICS_CONFIG_H
#define PSP_RECOMP_GRAPHICS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_GRAPHICS_SCALE_MIN 1u
#define SR_GRAPHICS_SCALE_MAX 8u

enum SrGraphicsRenderer {
    SR_GRAPHICS_RENDERER_AUTO,
    SR_GRAPHICS_RENDERER_D3D12,
    SR_GRAPHICS_RENDERER_SOFTWARE,
    SR_GRAPHICS_RENDERER_VULKAN,
};

typedef struct SrGraphicsConfig {
    int renderer;
    uint32_t internal_resolution;
} SrGraphicsConfig;

void sr_graphics_config_defaults(SrGraphicsConfig *config);
int sr_graphics_config_parse(
    SrGraphicsConfig *config, const char *text,
    char *error, size_t error_size);
int sr_graphics_config_init(void);
int sr_graphics_config_first_run(void);
const SrGraphicsConfig *sr_graphics_config_get(void);
const char *sr_graphics_config_path(void);
const char *sr_graphics_renderer_name(int renderer);
int sr_graphics_config_save(
    const SrGraphicsConfig *config, char *error, size_t error_size);
int sr_graphics_effective_scale(
    uint32_t *scale, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
