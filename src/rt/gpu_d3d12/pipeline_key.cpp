#include "pipeline.h"

#include <string.h>

void ged3d12_pipeline_key_build(
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

uint64_t ged3d12_pipeline_key_hash(const GeD3D12PipelineKey *key) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(key);
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < sizeof(*key); i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}
