#include "pipeline.h"
#include "shader_state.h"

#include <string.h>

static void build_manual_key(
    const GeState *state, int sprite_semantics, GeD3D12PipelineKey *out) {
    memset(out, 0, sizeof(*out));
    out->source_blend = D3D12_BLEND_ONE;
    out->destination_blend = D3D12_BLEND_ZERO;
    out->blend_operation = D3D12_BLEND_OP_ADD;
    out->source_alpha_blend = D3D12_BLEND_ONE;
    out->destination_alpha_blend = D3D12_BLEND_ZERO;
    out->alpha_blend_operation = D3D12_BLEND_OP_ADD;
    out->color_write_mask = D3D12_COLOR_WRITE_ENABLE_ALL;
    out->depth_enable = 1;
    out->depth_write = 1;
    out->depth_function = D3D12_COMPARISON_FUNC_ALWAYS;
    out->render_target_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    out->depth_stencil_format = DXGI_FORMAT_D16_UNORM;
    out->cull_mode =
        !sprite_semantics && state->cull_enable && !state->clear
            ? D3D12_CULL_MODE_BACK
            : D3D12_CULL_MODE_NONE;
}

static uint8_t depth_function(uint32_t function) {
    static const uint8_t map[8] = {
        D3D12_COMPARISON_FUNC_NEVER,
        D3D12_COMPARISON_FUNC_ALWAYS,
        D3D12_COMPARISON_FUNC_EQUAL,
        D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_LESS,
        D3D12_COMPARISON_FUNC_LESS_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL,
    };
    return map[function & 7u];
}

static uint8_t clear_color_write_mask(const GeState *state) {
    uint8_t mask = 0;
    if (state->clear_mode & 0x100u) {
        if ((state->mask_rgb & 0x0000ffu) != 0x0000ffu)
            mask |= D3D12_COLOR_WRITE_ENABLE_RED;
        if ((state->mask_rgb & 0x00ff00u) != 0x00ff00u)
            mask |= D3D12_COLOR_WRITE_ENABLE_GREEN;
        if ((state->mask_rgb & 0xff0000u) != 0xff0000u)
            mask |= D3D12_COLOR_WRITE_ENABLE_BLUE;
    }
    if ((state->clear_mode & 0x200u) &&
        (uint32_t)state->mask_alpha != 0xffu) {
        mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    }
    return mask;
}

static uint8_t source_blend_factor(
    uint32_t factor, uint32_t fixed_color) {
    static const uint8_t map[6] = {
        D3D12_BLEND_DEST_COLOR,
        D3D12_BLEND_INV_DEST_COLOR,
        D3D12_BLEND_SRC_ALPHA,
        D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_ALPHA,
        D3D12_BLEND_INV_DEST_ALPHA,
    };
    if (factor <= 5u) return map[factor];
    return (fixed_color & 0xffffffu) == 0u
        ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
}

static uint8_t destination_blend_factor(
    uint32_t factor, uint32_t fixed_color) {
    static const uint8_t map[6] = {
        D3D12_BLEND_SRC_COLOR,
        D3D12_BLEND_INV_SRC_COLOR,
        D3D12_BLEND_SRC_ALPHA,
        D3D12_BLEND_INV_SRC_ALPHA,
        D3D12_BLEND_DEST_ALPHA,
        D3D12_BLEND_INV_DEST_ALPHA,
    };
    if (factor <= 5u) return map[factor];
    return (fixed_color & 0xffffffu) == 0u
        ? D3D12_BLEND_ZERO : D3D12_BLEND_ONE;
}

static uint8_t blend_operation(uint32_t equation) {
    static const uint8_t map[5] = {
        D3D12_BLEND_OP_ADD,
        D3D12_BLEND_OP_SUBTRACT,
        D3D12_BLEND_OP_REV_SUBTRACT,
        D3D12_BLEND_OP_MIN,
        D3D12_BLEND_OP_MAX,
    };
    return map[equation <= 4u ? equation : 0u];
}

void ged3d12_pipeline_key_build(
    const GeState *state, int sprite_semantics, GeD3D12PipelineKey *out) {
    build_manual_key(state, sprite_semantics, out);
    if (!ged3d12_shader_direct_compatible(state)) return;

    out->color_write_mask = state->clear
        ? clear_color_write_mask(state)
        : D3D12_COLOR_WRITE_ENABLE_RED |
          D3D12_COLOR_WRITE_ENABLE_GREEN |
          D3D12_COLOR_WRITE_ENABLE_BLUE;
    if (!state->clear &&
        ged3d12_shader_direct_stencil_write(state)) {
        out->color_write_mask |= D3D12_COLOR_WRITE_ENABLE_ALPHA;
    }
    if (state->clear) {
        out->depth_enable = (state->clear_mode & 0x400u) != 0;
        out->depth_write = out->depth_enable;
        out->depth_function = D3D12_COMPARISON_FUNC_ALWAYS;
    } else {
        out->depth_enable = state->ztest_enable != 0;
        out->depth_write =
            out->depth_enable && !state->zwrite_disable;
        out->depth_function = depth_function(state->ztest);
    }
    if (!state->clear && state->blend_enable) {
        uint32_t mode = state->blend_mode;
        uint32_t source_factor = mode & 0xfu;
        uint32_t destination_factor = (mode >> 4) & 0xfu;
        uint32_t equation = (mode >> 8) & 7u;
        bool replace =
            source_factor == 10u &&
            destination_factor == 10u &&
            equation == 0u &&
            (state->blend_fixa & 0xffffffu) == 0xffffffu &&
            (state->blend_fixb & 0xffffffu) == 0u;
        if (!replace) {
            out->blend_enable = 1;
            out->source_blend =
                source_blend_factor(
                    source_factor, state->blend_fixa);
            out->destination_blend =
                destination_blend_factor(
                    destination_factor, state->blend_fixb);
            out->blend_operation = blend_operation(equation);
        }
    }
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

void ged3d12_pipeline_uber_key(GeD3D12PipelineKey *out) {
    GeState state = {};
    build_manual_key(&state, 0, out);
}

bool ged3d12_pipeline_uber_compatible(
    const GeD3D12PipelineKey *key) {
    if (!key ||
        (key->cull_mode != D3D12_CULL_MODE_NONE &&
         key->cull_mode != D3D12_CULL_MODE_BACK)) {
        return false;
    }
    GeD3D12PipelineKey candidate = *key;
    candidate.cull_mode = D3D12_CULL_MODE_NONE;
    GeD3D12PipelineKey uber;
    ged3d12_pipeline_uber_key(&uber);
    return memcmp(&candidate, &uber, sizeof(uber)) == 0;
}
