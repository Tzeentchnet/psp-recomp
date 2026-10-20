#include "shader_state.h"

#include "device.h"

#include <string.h>

static float color_channel(uint32_t color, uint32_t shift) {
    return (float)((color >> shift) & 0xffu) * (1.0f / 255.0f);
}

void ged3d12_shader_constants_build(
    const GeState *state, int perspective, int sprite_semantics,
    const GeD3D12TextureBinding *texture,
    GeD3D12ShaderConstants *out) {
    memset(out, 0, sizeof(*out));
    if (!state) return;

    uint32_t flags = 0;
    bool clear = state->clear != 0;
    bool textured =
        state->tex_enable && !clear && state->tex_addr != 0;
    if (clear) flags |= GE_D3D12_SHADER_CLEAR;
    if (perspective && !(clear && sprite_semantics))
        flags |= GE_D3D12_SHADER_DEPTH_RANGE;
    if (perspective && state->fog_enable && !clear)
        flags |= GE_D3D12_SHADER_FOG;
    if (textured) {
        flags |= GE_D3D12_SHADER_TEXTURE;
        if (state->tex_func & 0x100u)
            flags |= GE_D3D12_SHADER_TEXTURE_RGBA;
        if (state->tex_func & 0x10000u)
            flags |= GE_D3D12_SHADER_COLOR_DOUBLE;
        if (!(state->tex_filter & 1u) &&
            !((state->tex_filter >> 8) & 1u)) {
            flags |= GE_D3D12_SHADER_NEAREST;
        }
    }
    if (!clear && state->blend_enable) {
        uint32_t source_factor = state->blend_mode & 0xfu;
        if (source_factor == 6u)
            flags |= GE_D3D12_SHADER_DOUBLE_SOURCE_ALPHA;
        else if (source_factor == 7u)
            flags |= GE_D3D12_SHADER_DOUBLE_INVERSE_SOURCE_ALPHA;
    }
    if (!clear && state->color_test_enable)
        flags |= GE_D3D12_SHADER_COLOR_TEST;
    if (state->dither_enable)
        flags |= GE_D3D12_SHADER_DITHER;
    if (!clear && state->stencil_test_enable)
        flags |= GE_D3D12_SHADER_STENCIL;

    out->config[0] =
        (int32_t)((state->tex_func & 7u) | (flags << 8));
    out->config[1] =
        !clear && state->atest_enable ? (int32_t)state->atest : 1;
    out->config[2] = (int32_t)state->minz;
    out->config[3] = (int32_t)state->maxz;
    out->texture_environment[0] = color_channel(state->tex_env, 0);
    out->texture_environment[1] = color_channel(state->tex_env, 8);
    out->texture_environment[2] = color_channel(state->tex_env, 16);
    out->fog_color[0] = color_channel(state->fog_color, 0);
    out->fog_color[1] = color_channel(state->fog_color, 8);
    out->fog_color[2] = color_channel(state->fog_color, 16);
    out->texture_size[0] =
        state->tex_w > 0 ? (float)state->tex_w : 1.0f;
    out->texture_size[1] =
        state->tex_h > 0 ? (float)state->tex_h : 1.0f;
    if (texture &&
        texture->framebuffer.handle !=
            GE_D3D12_FRAMEBUFFER_HANDLE_NONE) {
        out->texture_size[2] = (float)texture->framebuffer.x;
        out->texture_size[3] = (float)texture->framebuffer.y;
    }

    out->color_test[0] =
        !clear && state->color_test_enable
            ? state->color_test & 3u : 1u;
    out->color_test[1] = state->color_ref & 0xffffffu;
    out->color_test[2] = state->color_test_mask & 0xffffffu;
    out->stencil_test[0] =
        !clear && state->stencil_test_enable
            ? state->stencil_test : 1u;
    out->stencil_test[1] = state->stencil_op;
    out->stencil_test[2] = (uint32_t)state->mask_alpha & 0xffu;
    out->stencil_test[3] = state->fbfmt & 3u;
    for (uint32_t row = 0; row < 4; row++) {
        uint32_t packed = 0;
        for (uint32_t column = 0; column < 4; column++)
            packed |=
                ((uint32_t)state->dith[row][column] & 0xfu) <<
                (column * 4u);
        out->dither[row] = packed;
    }
}

void ged3d12_shader_constants_bind(
    const GeD3D12ShaderConstants *constants) {
    ged3d12_command_list()->SetGraphicsRoot32BitConstants(
        GE_D3D12_ROOT_GE_STATE,
        sizeof(*constants) / sizeof(uint32_t), constants, 0);
}
