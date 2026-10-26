#include "shader_state.h"

#include <string.h>

static int blend_factor_compatible(
    uint32_t factor, uint32_t fixed_color) {
    return factor <= 5u ||
           (factor == 10u &&
            ((fixed_color & 0xffffffu) == 0u ||
             (fixed_color & 0xffffffu) == 0xffffffu));
}

static int stencil_operation_keeps(uint32_t operation) {
    operation &= 7u;
    return operation == 0u || operation >= 6u;
}

static int direct_stencil_compatible(
    const GeState *state, int *writes_stencil) {
    uint32_t pass_operation;
    *writes_stencil = 0;
    if (!state->stencil_test_enable) return 1;
    if ((state->stencil_test & 7u) != 1u)
        return 0;
    if (!stencil_operation_keeps(state->stencil_op >> 8))
        return 0;

    pass_operation = (state->stencil_op >> 16) & 7u;
    if (stencil_operation_keeps(pass_operation))
        return 1;
    if ((uint32_t)state->mask_alpha != 0u)
        return 0;
    if (pass_operation == 1u || pass_operation == 2u ||
        ((state->fbfmt & 3u) <= 1u &&
         (pass_operation == 4u || pass_operation == 5u))) {
        *writes_stencil = 1;
        return 1;
    }
    return 0;
}

int ge_vk_shader_direct_compatible(const GeState *state) {
    int writes_stencil;
    uint32_t mode;
    uint32_t source_factor;
    uint32_t destination_factor;
    uint32_t equation;
    if (!state || state->clear)
        return 0;
    if (!direct_stencil_compatible(state, &writes_stencil) ||
        state->logic_op_enable ||
        state->mask_rgb != 0u) {
        return 0;
    }
    if (!state->blend_enable) return 1;

    mode = state->blend_mode;
    source_factor = mode & 0xfu;
    destination_factor = (mode >> 4) & 0xfu;
    equation = (mode >> 8) & 7u;
    if (writes_stencil &&
        (source_factor == 2u || source_factor == 3u ||
         destination_factor == 2u || destination_factor == 3u)) {
        return 0;
    }
    if (source_factor == 10u && destination_factor == 10u &&
        equation == 0u &&
        (state->blend_fixa & 0xffffffu) == 0xffffffu &&
        (state->blend_fixb & 0xffffffu) == 0u) {
        return 1;
    }
    return state->fbfmt == 3u &&
           !state->dither_enable &&
           equation <= 4u &&
           blend_factor_compatible(
               source_factor, state->blend_fixa) &&
           blend_factor_compatible(
               destination_factor, state->blend_fixb);
}

int ge_vk_shader_direct_stencil_write(const GeState *state) {
    int writes_stencil;
    return state &&
           direct_stencil_compatible(state, &writes_stencil) &&
           writes_stencil;
}

static uint32_t pack_dimensions(int width, int height) {
    uint32_t w = width > 0 ? (uint32_t)width : 1u;
    uint32_t h = height > 0 ? (uint32_t)height : 1u;
    if (w > 0xffffu) w = 0xffffu;
    if (h > 0xffffu) h = 0xffffu;
    return w | (h << 16);
}

void ge_vk_shader_state_build(
    const GeState *state, int perspective, int sprite_semantics,
    GeVkShaderState *out) {
    uint32_t flags = 0;
    uint32_t row;
    int clear;
    int textured;
    (void)sprite_semantics;
    memset(out, 0, sizeof(*out));
    if (!state) return;

    clear = state->clear != 0;
    textured = state->tex_enable && !clear && state->tex_addr != 0;
    if (clear) flags |= GE_VK_SHADER_CLEAR;
    if (perspective && !(clear && sprite_semantics))
        flags |= GE_VK_SHADER_DEPTH_RANGE;
    if (perspective && state->fog_enable && !clear)
        flags |= GE_VK_SHADER_FOG;
    if (textured) {
        flags |= GE_VK_SHADER_TEXTURE;
        if (state->tex_func & 0x100u)
            flags |= GE_VK_SHADER_TEXTURE_RGBA;
        if (state->tex_func & 0x10000u)
            flags |= GE_VK_SHADER_COLOR_DOUBLE;
        if (!(state->tex_filter & 1u) &&
            !((state->tex_filter >> 8) & 1u)) {
            flags |= GE_VK_SHADER_NEAREST;
        }
        if (state->tex_wrap & 1u)
            flags |= GE_VK_SHADER_CLAMP_U;
        if ((state->tex_wrap >> 8) & 1u)
            flags |= GE_VK_SHADER_CLAMP_V;
    }
    if (!clear && state->color_test_enable)
        flags |= GE_VK_SHADER_COLOR_TEST;
    if (state->dither_enable)
        flags |= GE_VK_SHADER_DITHER;
    if (!clear && state->stencil_test_enable)
        flags |= GE_VK_SHADER_STENCIL;
    if (ge_vk_shader_direct_compatible(state))
        flags |= GE_VK_SHADER_DIRECT;

    out->config[0] =
        (int32_t)((state->tex_func & 7u) | (flags << 8));
    out->config[1] =
        !clear && state->atest_enable ? (int32_t)state->atest : 1;
    out->config[2] = (int32_t)state->minz;
    out->config[3] = (int32_t)state->maxz;
    out->texture[0] = state->tex_env & 0xffffffu;
    out->texture[1] = state->fog_color & 0xffffffu;
    out->texture[2] =
        pack_dimensions(state->tex_w, state->tex_h);

    out->color_test[0] =
        !clear && state->color_test_enable
            ? state->color_test & 3u : 1u;
    out->color_test[1] = state->color_ref & 0xffffffu;
    out->color_test[2] = state->color_test_mask & 0xffffffu;
    out->color_test[3] = state->clut_fmt;
    if (state->tex_wrap & 1u)
        out->color_test[3] |= 1u << 30;
    if ((state->tex_wrap >> 8) & 1u)
        out->color_test[3] |= 1u << 31;

    out->stencil[0] =
        !clear && state->stencil_test_enable
            ? state->stencil_test : 1u;
    out->stencil[1] = state->stencil_op;
    out->stencil[2] = (uint32_t)state->mask_alpha & 0xffu;
    out->stencil[3] = state->fbfmt & 3u;
    for (row = 0; row < 4; row++) {
        uint32_t column;
        uint32_t packed = 0;
        for (column = 0; column < 4; column++)
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
}
