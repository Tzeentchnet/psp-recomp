#ifndef GE_SDL3VK_SHADER_STATE_H
#define GE_SDL3VK_SHADER_STATE_H

#include "../ge_shared.h"

#include <stdint.h>

enum GeVkShaderFlag {
    GE_VK_SHADER_TEXTURE = 1u,
    GE_VK_SHADER_TEXTURE_RGBA = 2u,
    GE_VK_SHADER_COLOR_DOUBLE = 4u,
    GE_VK_SHADER_FOG = 8u,
    GE_VK_SHADER_DEPTH_RANGE = 16u,
    GE_VK_SHADER_CLEAR = 32u,
    GE_VK_SHADER_NEAREST = 64u,
    GE_VK_SHADER_COLOR_TEST = 512u,
    GE_VK_SHADER_DITHER = 1024u,
    GE_VK_SHADER_STENCIL = 2048u,
    GE_VK_SHADER_DIRECT = 8192u,
    GE_VK_SHADER_PSP_SAMPLER = 32768u,
    GE_VK_SHADER_FRAMEBUFFER = 65536u,
    GE_VK_SHADER_CLAMP_U = 131072u,
    GE_VK_SHADER_CLAMP_V = 262144u,
};

typedef struct GeVkShaderState {
    int32_t config[4];
    uint32_t texture[4];
    uint32_t color_test[4];
    uint32_t stencil[4];
    uint32_t dither[4];
    uint32_t blend[4];
    uint32_t fragment[4];
} GeVkShaderState;

int ge_vk_shader_direct_compatible(const GeState *state);
int ge_vk_shader_direct_stencil_write(const GeState *state);
void ge_vk_shader_state_build(
    const GeState *state, int perspective, int sprite_semantics,
    GeVkShaderState *out);

#endif
