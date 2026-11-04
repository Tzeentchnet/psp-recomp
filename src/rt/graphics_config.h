#ifndef PSP_RECOMP_GRAPHICS_CONFIG_H
#define PSP_RECOMP_GRAPHICS_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SR_GRAPHICS_SCALE_MIN 1u
#define SR_GRAPHICS_SCALE_MAX 8u
#define SR_TEXTURE_SCALE_MIN 1u
#define SR_TEXTURE_SCALE_MAX 5u

enum SrGraphicsRenderer {
    SR_GRAPHICS_RENDERER_AUTO,
    SR_GRAPHICS_RENDERER_D3D12,
    SR_GRAPHICS_RENDERER_SOFTWARE,
    SR_GRAPHICS_RENDERER_VULKAN,
};

enum SrGraphicsAntiAliasing {
    SR_GRAPHICS_ANTI_ALIASING_OFF,
    SR_GRAPHICS_ANTI_ALIASING_SMAA,
};

enum SrGraphicsQualityMode {
    SR_GRAPHICS_QUALITY_AUTO,
    SR_GRAPHICS_QUALITY_MANUAL,
};

enum SrGraphicsAutoProfile {
    SR_GRAPHICS_AUTO_PROFILE_MANUAL,
    SR_GRAPHICS_AUTO_PROFILE_REFERENCE,
    SR_GRAPHICS_AUTO_PROFILE_SAFE,
    SR_GRAPHICS_AUTO_PROFILE_BALANCED,
    SR_GRAPHICS_AUTO_PROFILE_HIGH,
};

typedef struct SrGraphicsConfig {
    int renderer;
    int quality_mode;
    uint32_t internal_resolution;
    uint32_t texture_scale;
    uint32_t d3d12_full_rate;
    int anti_aliasing;
} SrGraphicsConfig;

typedef struct SrGraphicsAutoCaps {
    const char *device_name;
    uint32_t drawable_width;
    uint32_t drawable_height;
    uint32_t max_image_dimension_2d;
    int hardware_renderer;
    int smaa_supported;
} SrGraphicsAutoCaps;

typedef struct SrGraphicsAutoSelection {
    int profile;
    uint32_t internal_resolution;
    uint32_t texture_scale;
    int anti_aliasing;
    char reason[192];
} SrGraphicsAutoSelection;

void sr_graphics_config_defaults(SrGraphicsConfig *config);
int sr_graphics_auto_select(
    const SrGraphicsAutoCaps *caps,
    SrGraphicsAutoSelection *selection);
int sr_graphics_config_resolve_auto(
    const SrGraphicsAutoCaps *caps,
    SrGraphicsAutoSelection *selection,
    char *error, size_t error_size);
int sr_graphics_config_parse(
    SrGraphicsConfig *config, const char *text,
    char *error, size_t error_size);
int sr_graphics_config_init(void);
int sr_graphics_config_first_run(void);
const SrGraphicsConfig *sr_graphics_config_get(void);
const char *sr_graphics_config_path(void);
const char *sr_graphics_renderer_name(int renderer);
const char *sr_graphics_quality_name(int quality_mode);
const char *sr_graphics_auto_profile_name(int profile);
const char *sr_graphics_anti_aliasing_name(int anti_aliasing);
int sr_graphics_config_save(
    const SrGraphicsConfig *config, char *error, size_t error_size);
int sr_graphics_effective_scale(
    uint32_t *scale, char *error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif
