#include "shader_state.h"

#include "device.h"

#include <stdlib.h>
#include <string.h>

static float color_channel(uint32_t color, uint32_t shift) {
    return (float)((color >> shift) & 0xffu) * (1.0f / 255.0f);
}

static uint32_t clamp_scissor_start(int value, uint32_t limit) {
    if (value <= 0) return 0;
    return (uint32_t)value < limit ? (uint32_t)value : limit;
}

static uint32_t clamp_scissor_end(int value, uint32_t limit) {
    if (value < 0) return 0;
    return (uint32_t)value < limit ? (uint32_t)value + 1u : limit;
}

static bool psp_sampler_enabled(void) {
    const char *setting = getenv("SR_GPU_PSP_SAMPLER");
    return !setting || !setting[0] || strcmp(setting, "0") != 0;
}

static bool blend_factor_compatible(
    uint32_t factor, uint32_t fixed_color) {
    return factor <= 5u ||
           (factor == 10u &&
            (fixed_color & 0xffffffu) == 0u) ||
           (factor == 10u &&
            (fixed_color & 0xffffffu) == 0xffffffu);
}

bool ged3d12_shader_direct_compatible(const GeState *state) {
    static int disabled = -1;
    if (disabled < 0)
        disabled = getenv("SR_D3D12_DISABLE_DIRECT") ? 1 : 0;
    if (disabled) return false;
    if (!state) return false;
    if (state->clear) return false;
    if (state->stencil_test_enable || state->logic_op_enable ||
        state->mask_rgb != 0u) {
        return false;
    }
    if (!state->blend_enable) return true;
    uint32_t mode = state->blend_mode;
    uint32_t source_factor = mode & 0xfu;
    uint32_t destination_factor = (mode >> 4) & 0xfu;
    uint32_t equation = (mode >> 8) & 7u;
    if (source_factor == 10u && destination_factor == 10u &&
        equation == 0u &&
        (state->blend_fixa & 0xffffffu) == 0xffffffu &&
        (state->blend_fixb & 0xffffffu) == 0u) {
        return true;
    }
    return state->fbfmt == 3u &&
           !state->dither_enable &&
           equation <= 4u &&
           blend_factor_compatible(
               source_factor, state->blend_fixa) &&
           blend_factor_compatible(
               destination_factor, state->blend_fixb);
}

D3D12_SHADING_RATE ged3d12_shader_shading_rate(
    uint32_t scale, D3D12_VARIABLE_SHADING_RATE_TIER tier,
    bool additional_rates) {
    if (tier == D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED ||
        scale < 2u) {
        return D3D12_SHADING_RATE_1X1;
    }
    if (scale >= 4u && additional_rates)
        return D3D12_SHADING_RATE_4X4;
    return D3D12_SHADING_RATE_2X2;
}

void ged3d12_shader_constants_build(
    const GeState *state, int perspective, int sprite_semantics,
    int triangle_semantics,
    const GeD3D12TextureBinding *texture,
    GeD3D12ShaderConstants *out) {
    memset(out, 0, sizeof(*out));
    if (!state) return;
    uint32_t render_scale = ged3d12_framebuffer_scale();
    if (!render_scale) render_scale = 1u;

    uint32_t flags = 0;
    bool clear = state->clear != 0;
    bool textured =
        state->tex_enable && !clear && state->tex_addr != 0;
    bool framebuffer_texture =
        texture &&
        texture->framebuffer.handle !=
            GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
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
        if (psp_sampler_enabled() &&
            (!texture ||
             !(texture->flags & GE_D3D12_TEXTURE_UPSCALED)) &&
            (!texture || !texture->sampler.mip_enabled)) {
            flags |= GE_D3D12_SHADER_PSP_SAMPLER;
        }
        if (texture &&
            (texture->flags &
             GE_D3D12_TEXTURE_FRAMEBUFFER_CLUT) != 0) {
            flags |= GE_D3D12_SHADER_FRAMEBUFFER_CLUT;
        }
        if (framebuffer_texture) {
            flags |= GE_D3D12_SHADER_FRAMEBUFFER;
        }
    }
    if (!clear && state->color_test_enable)
        flags |= GE_D3D12_SHADER_COLOR_TEST;
    if (state->dither_enable)
        flags |= GE_D3D12_SHADER_DITHER;
    if (!clear && state->stencil_test_enable)
        flags |= GE_D3D12_SHADER_STENCIL;
    if (!clear && triangle_semantics && state->cull_enable)
        flags |= GE_D3D12_SHADER_CULL_BACK;
    if (ged3d12_shader_direct_compatible(state))
        flags |= GE_D3D12_SHADER_DIRECT;

    out->config[0] =
        (int32_t)((state->tex_func & 7u) | (flags << 8));
    out->config[1] =
        !clear && state->atest_enable ? (int32_t)state->atest : 1;
    out->config[2] = (int32_t)state->minz;
    out->config[3] = (int32_t)state->maxz;
    out->texture_environment[0] = color_channel(state->tex_env, 0);
    out->texture_environment[1] = color_channel(state->tex_env, 8);
    out->texture_environment[2] = color_channel(state->tex_env, 16);
    out->texture_environment[3] = (float)render_scale;
    out->fog_color[0] = color_channel(state->fog_color, 0);
    out->fog_color[1] = color_channel(state->fog_color, 8);
    out->fog_color[2] = color_channel(state->fog_color, 16);
    out->texture_size[0] =
        state->tex_w > 0 ? (float)state->tex_w : 1.0f;
    out->texture_size[1] =
        state->tex_h > 0 ? (float)state->tex_h : 1.0f;
    if (framebuffer_texture) {
        out->texture_size[2] = (float)texture->framebuffer.x;
        out->texture_size[3] = (float)texture->framebuffer.y;
    }

    out->color_test[0] =
        !clear && state->color_test_enable
            ? state->color_test & 3u : 1u;
    out->color_test[1] = state->color_ref & 0xffffffu;
    out->color_test[2] = state->color_test_mask & 0xffffffu;
    out->color_test[3] =
        state->clut_fmt |
        (texture && texture->sampler.clamp_u ? 1u << 30 : 0u) |
        (texture && texture->sampler.clamp_v ? 1u << 31 : 0u);
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
    out->blend[0] =
        !clear && state->blend_enable &&
        ((state->blend_mode >> 8) & 7u) <= 5u;
    out->blend[1] = state->blend_mode;
    out->blend[2] = state->blend_fixa & 0xffffffu;
    out->blend[3] = state->blend_fixb & 0xffffffu;
    out->fragment[0] =
        (state->mask_rgb & 0xffffffu) |
        (((uint32_t)state->mask_alpha & 0xffu) << 24);
    out->fragment[1] =
        (!clear && state->logic_op_enable ? 1u : 0u) |
        ((state->logic_op & 0xfu) << 8);
    out->fragment[2] = state->clear_mode;
    out->fragment[3] =
        (state->ztest_enable ? 1u : 0u) |
        ((state->ztest & 7u) << 8) |
        ((!state->zwrite_disable ? 1u : 0u) << 16);
    uint32_t scissor_left = clamp_scissor_start(
        state->scis_x1, GE_D3D12_FRAMEBUFFER_WIDTH);
    uint32_t scissor_top = clamp_scissor_start(
        state->scis_y1, GE_D3D12_FRAMEBUFFER_HEIGHT);
    uint32_t scissor_right = clamp_scissor_end(
        state->scis_x2, GE_D3D12_FRAMEBUFFER_WIDTH);
    uint32_t scissor_bottom = clamp_scissor_end(
        state->scis_y2, GE_D3D12_FRAMEBUFFER_HEIGHT);
    out->scissor[0] = (int32_t)(scissor_left * render_scale);
    out->scissor[1] = (int32_t)(scissor_top * render_scale);
    out->scissor[2] =
        (int32_t)(scissor_right < scissor_left
            ? scissor_left * render_scale
            : scissor_right * render_scale);
    out->scissor[3] =
        (int32_t)(scissor_bottom < scissor_top
            ? scissor_top * render_scale
            : scissor_bottom * render_scale);
}

bool ged3d12_shader_constants_cache_update(
    GeD3D12ShaderConstantCache *cache,
    const GeD3D12ShaderConstants *constants) {
    if (!cache || !constants) return false;
    if (cache->valid &&
        memcmp(&cache->value, constants, sizeof(*constants)) == 0) {
        return false;
    }
    cache->value = *constants;
    cache->valid = 1;
    return true;
}

bool ged3d12_shader_constants_bind(
    GeD3D12ShaderConstantCache *cache,
    const GeD3D12ShaderConstants *constants) {
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    bool updated = ged3d12_shader_constants_cache_update(cache, constants);
    if (updated) {
        command_list->SetGraphicsRoot32BitConstants(
            GE_D3D12_ROOT_GE_STATE,
            sizeof(*constants) / sizeof(uint32_t), constants, 0);
    }
    D3D12_RECT scissor = {
        constants->scissor[0], constants->scissor[1],
        constants->scissor[2], constants->scissor[3],
    };
    command_list->RSSetScissorRects(1, &scissor);
    return updated;
}
