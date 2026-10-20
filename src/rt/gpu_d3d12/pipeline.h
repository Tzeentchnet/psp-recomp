/* D3D12 GE pipeline-state key and synchronous PSO cache. */
#ifndef GE_D3D12_PIPELINE_H
#define GE_D3D12_PIPELINE_H

#include "../ge_shared.h"

#include <d3d12.h>
#include <stdint.h>

struct GeD3D12PipelineKey {
    uint8_t blend_enable;
    uint8_t source_blend;
    uint8_t destination_blend;
    uint8_t blend_operation;
    uint8_t source_alpha_blend;
    uint8_t destination_alpha_blend;
    uint8_t alpha_blend_operation;
    uint8_t color_write_mask;
    uint8_t depth_enable;
    uint8_t depth_write;
    uint8_t depth_function;
    uint8_t cull_mode;
    uint16_t render_target_format;
    uint16_t depth_stencil_format;
};

struct GeD3D12PipelineStats {
    uint64_t lookups;
    uint64_t hits;
    uint64_t misses;
    uint32_t entries;
    uint32_t failures;
};

void ged3d12_pipeline_key_build(
    const GeState *state, int sprite_semantics, GeD3D12PipelineKey *out);
uint64_t ged3d12_pipeline_key_hash(const GeD3D12PipelineKey *key);

bool ged3d12_pipeline_cache_init(void);
void ged3d12_pipeline_cache_shutdown(void);
bool ged3d12_pipeline_bind(const GeD3D12PipelineKey *key);
GeD3D12PipelineStats ged3d12_pipeline_stats(void);

static_assert(sizeof(GeD3D12PipelineKey) == 16, "D3D12 PSO key ABI changed");

#endif
