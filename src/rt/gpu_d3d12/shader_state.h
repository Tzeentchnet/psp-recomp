/* Canonical per-draw GE constants consumed by the D3D12 pixel shader. */
#ifndef GE_D3D12_SHADER_STATE_H
#define GE_D3D12_SHADER_STATE_H

#include "../ge_shared.h"
#include "texture.h"

#include <d3d12.h>
#include <stdint.h>

#define GE_D3D12_SHADER_CONSTANT_DWORDS 40u

enum GeD3D12ShaderFlag : uint32_t {
    GE_D3D12_SHADER_TEXTURE = 1u,
    GE_D3D12_SHADER_TEXTURE_RGBA = 2u,
    GE_D3D12_SHADER_COLOR_DOUBLE = 4u,
    GE_D3D12_SHADER_FOG = 8u,
    GE_D3D12_SHADER_DEPTH_RANGE = 16u,
    GE_D3D12_SHADER_CLEAR = 32u,
    GE_D3D12_SHADER_NEAREST = 64u,
    GE_D3D12_SHADER_DOUBLE_SOURCE_ALPHA = 128u,
    GE_D3D12_SHADER_DOUBLE_INVERSE_SOURCE_ALPHA = 256u,
    GE_D3D12_SHADER_COLOR_TEST = 512u,
    GE_D3D12_SHADER_DITHER = 1024u,
    GE_D3D12_SHADER_STENCIL = 2048u,
};

struct GeD3D12ShaderConstants {
    int32_t config[4];
    float texture_environment[4];
    float fog_color[4];
    float texture_size[4];
    uint32_t color_test[4];
    uint32_t stencil_test[4];
    uint32_t dither[4];
    uint32_t blend[4];
    uint32_t fragment[4];
    int32_t scissor[4];
};

void ged3d12_shader_constants_build(
    const GeState *state, int perspective, int sprite_semantics,
    const GeD3D12TextureBinding *texture,
    GeD3D12ShaderConstants *out);
void ged3d12_shader_constants_bind(
    const GeD3D12ShaderConstants *constants);

static_assert(
    sizeof(GeD3D12ShaderConstants) ==
        GE_D3D12_SHADER_CONSTANT_DWORDS * sizeof(uint32_t),
    "D3D12 shader constants ABI changed");

#endif
