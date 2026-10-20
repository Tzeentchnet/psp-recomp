#include "pipeline.h"

#include <string.h>

static uint8_t map_blend_factor(
    uint32_t factor, int source_side, uint32_t fixed, int *uses_constant) {
    switch (factor & 0xf) {
    case 0:
        return source_side ? D3D12_BLEND_DEST_COLOR : D3D12_BLEND_SRC_COLOR;
    case 1:
        return source_side ? D3D12_BLEND_INV_DEST_COLOR : D3D12_BLEND_INV_SRC_COLOR;
    case 2:
        return D3D12_BLEND_SRC_ALPHA;
    case 3:
        return D3D12_BLEND_INV_SRC_ALPHA;
    case 4:
        return D3D12_BLEND_DEST_ALPHA;
    case 5:
        return D3D12_BLEND_INV_DEST_ALPHA;
    case 6:
        return source_side ? D3D12_BLEND_ONE : D3D12_BLEND_SRC_ALPHA;
    case 7:
        return source_side ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
    case 8:
        return D3D12_BLEND_DEST_ALPHA;
    case 9:
        return D3D12_BLEND_INV_DEST_ALPHA;
    default:
        if ((fixed & 0xffffffu) == 0) return D3D12_BLEND_ZERO;
        if ((fixed & 0xffffffu) == 0xffffffu) return D3D12_BLEND_ONE;
        *uses_constant = 1;
        return D3D12_BLEND_BLEND_FACTOR;
    }
}

static uint8_t map_blend_operation(uint32_t operation) {
    switch (operation & 7) {
    case 1:
        return D3D12_BLEND_OP_SUBTRACT;
    case 2:
        return D3D12_BLEND_OP_REV_SUBTRACT;
    case 3:
        return D3D12_BLEND_OP_MIN;
    case 4:
        return D3D12_BLEND_OP_MAX;
    case 5:
        return D3D12_BLEND_OP_SUBTRACT;
    default:
        return D3D12_BLEND_OP_ADD;
    }
}

static uint8_t color_write_mask(const GeState *state) {
    if (state->clear) {
        uint8_t mask = 0;
        if (state->clear_mode & 0x100)
            mask |= D3D12_COLOR_WRITE_ENABLE_RED |
                    D3D12_COLOR_WRITE_ENABLE_GREEN |
                    D3D12_COLOR_WRITE_ENABLE_BLUE;
        if (state->clear_mode & 0x200)
            mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
        return mask;
    }

    uint32_t red = state->mask_rgb & 0xff;
    uint32_t green = (state->mask_rgb >> 8) & 0xff;
    uint32_t blue = (state->mask_rgb >> 16) & 0xff;
    uint32_t alpha = (uint32_t)state->mask_alpha & 0xff;
    return (red >= 0x80 ? 0 : D3D12_COLOR_WRITE_ENABLE_RED) |
           (green >= 0x80 ? 0 : D3D12_COLOR_WRITE_ENABLE_GREEN) |
           (blue >= 0x80 ? 0 : D3D12_COLOR_WRITE_ENABLE_BLUE) |
           (alpha >= 0x80 ? 0 : D3D12_COLOR_WRITE_ENABLE_ALPHA);
}

void ged3d12_pipeline_key_build(
    const GeState *state, int sprite_semantics, GeD3D12PipelineKey *out) {
    static const uint8_t depth_functions[8] = {
        D3D12_COMPARISON_FUNC_NEVER,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_COMPARISON_FUNC_EQUAL,
        D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL,
    };
    memset(out, 0, sizeof(*out));
    out->source_alpha_blend = D3D12_BLEND_ONE;
    out->destination_alpha_blend = D3D12_BLEND_ONE;
    out->alpha_blend_operation = D3D12_BLEND_OP_ADD;
    out->color_write_mask = color_write_mask(state);
    out->render_target_format = DXGI_FORMAT_B8G8R8A8_UNORM;

    if (!state->clear && state->blend_enable) {
        int source_constant = 0;
        int destination_constant = 0;
        out->blend_enable = 1;
        out->source_blend = map_blend_factor(
            state->blend_mode, 1, state->blend_fixa, &source_constant);
        out->destination_blend = map_blend_factor(
            state->blend_mode >> 4, 0, state->blend_fixb, &destination_constant);
        if (source_constant && destination_constant &&
            (state->blend_fixa & 0xffffffu) !=
                (state->blend_fixb & 0xffffffu)) {
            uint32_t fixed = state->blend_fixb;
            uint32_t luminance =
                (fixed & 0xff) + ((fixed >> 8) & 0xff) + ((fixed >> 16) & 0xff);
            out->destination_blend =
                luminance >= 384 ? D3D12_BLEND_ONE : D3D12_BLEND_ZERO;
        }
        out->blend_operation = map_blend_operation(state->blend_mode >> 8);
    } else {
        out->source_blend = D3D12_BLEND_ONE;
        out->destination_blend = D3D12_BLEND_ZERO;
        out->blend_operation = D3D12_BLEND_OP_ADD;
    }

    if (state->clear) {
        out->depth_enable = (state->clear_mode & 0x400) != 0;
        out->depth_write = out->depth_enable;
        out->depth_function = D3D12_COMPARISON_FUNC_ALWAYS;
    } else {
        out->depth_enable = state->ztest_enable != 0;
        out->depth_write =
            out->depth_enable && !state->zwrite_disable;
        out->depth_function = out->depth_enable
            ? depth_functions[state->ztest & 7]
            : (uint8_t)D3D12_COMPARISON_FUNC_ALWAYS;
    }
    out->depth_stencil_format =
        out->depth_enable ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_UNKNOWN;
    out->cull_mode =
        !sprite_semantics && state->cull_enable && !state->clear
            ? D3D12_CULL_MODE_BACK
            : D3D12_CULL_MODE_NONE;
}

uint64_t ged3d12_pipeline_key_hash(const GeD3D12PipelineKey *key) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(key);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof(*key); i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
