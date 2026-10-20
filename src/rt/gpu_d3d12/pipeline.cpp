#include "pipeline.h"

#include "device.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PIPELINE_CACHE_CAPACITY 512u

struct PipelineEntry {
    GeD3D12PipelineKey key;
    uint64_t hash;
    ID3D12PipelineState *pipeline;
    int used;
};

static PipelineEntry s_entries[PIPELINE_CACHE_CAPACITY];
static GeD3D12PipelineStats s_stats;
static int s_ready;
static int s_log;

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static ID3D12PipelineState *create_pipeline(const GeD3D12PipelineKey *key) {
    static const D3D12_INPUT_ELEMENT_DESC input_elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 28,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = ged3d12_root_signature();
    desc.VS = ged3d12_vertex_shader();
    desc.PS = ged3d12_pixel_shader();

    D3D12_RENDER_TARGET_BLEND_DESC *blend =
        &desc.BlendState.RenderTarget[0];
    blend->BlendEnable = key->blend_enable;
    blend->LogicOpEnable = FALSE;
    blend->SrcBlend = (D3D12_BLEND)key->source_blend;
    blend->DestBlend = (D3D12_BLEND)key->destination_blend;
    blend->BlendOp = (D3D12_BLEND_OP)key->blend_operation;
    blend->SrcBlendAlpha = (D3D12_BLEND)key->source_alpha_blend;
    blend->DestBlendAlpha = (D3D12_BLEND)key->destination_alpha_blend;
    blend->BlendOpAlpha = (D3D12_BLEND_OP)key->alpha_blend_operation;
    blend->LogicOp = D3D12_LOGIC_OP_NOOP;
    blend->RenderTargetWriteMask = key->color_write_mask;

    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = (D3D12_CULL_MODE)key->cull_mode;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable = TRUE;

    desc.DepthStencilState.DepthEnable = key->depth_enable;
    desc.DepthStencilState.DepthWriteMask = key->depth_write
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc =
        (D3D12_COMPARISON_FUNC)key->depth_function;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;

    desc.InputLayout.pInputElementDescs = input_elements;
    desc.InputLayout.NumElements =
        (UINT)(sizeof(input_elements) / sizeof(input_elements[0]));
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = (DXGI_FORMAT)key->render_target_format;
    desc.DSVFormat = (DXGI_FORMAT)key->depth_stencil_format;
    desc.SampleDesc.Count = 1;

    ID3D12PipelineState *pipeline = nullptr;
    HRESULT hr = ged3d12_device()->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(hr)) {
        fprintf(stderr,
                "ged3d12: create GE graphics pipeline failed "
                "(hash=%016llx, HRESULT 0x%08lx)\n",
                (unsigned long long)ged3d12_pipeline_key_hash(key),
                (unsigned long)hr);
        return nullptr;
    }
    return pipeline;
}

bool ged3d12_pipeline_cache_init(void) {
    if (s_ready) {
        fprintf(stderr, "ged3d12: pipeline cache already initialized\n");
        return false;
    }
    if (!ged3d12_device() || !ged3d12_root_signature()) {
        fprintf(stderr, "ged3d12: pipeline cache requires an initialized device\n");
        return false;
    }
    memset(s_entries, 0, sizeof(s_entries));
    memset(&s_stats, 0, sizeof(s_stats));
    s_log = env_enabled("SR_D3D12_PSO_LOG");
    s_ready = 1;
    return true;
}

void ged3d12_pipeline_cache_shutdown(void) {
    if (s_log && s_ready) {
        fprintf(stderr,
                "ged3d12: PSO cache entries=%u lookups=%llu hits=%llu "
                "misses=%llu failures=%u\n",
                s_stats.entries,
                (unsigned long long)s_stats.lookups,
                (unsigned long long)s_stats.hits,
                (unsigned long long)s_stats.misses,
                s_stats.failures);
    }
    for (UINT i = 0; i < PIPELINE_CACHE_CAPACITY; i++) {
        if (s_entries[i].pipeline) s_entries[i].pipeline->Release();
    }
    memset(s_entries, 0, sizeof(s_entries));
    memset(&s_stats, 0, sizeof(s_stats));
    s_ready = 0;
    s_log = 0;
}

bool ged3d12_pipeline_bind(const GeD3D12PipelineKey *key) {
    if (!s_ready || !key) {
        fprintf(stderr, "ged3d12: invalid pipeline cache lookup\n");
        return false;
    }
    s_stats.lookups++;
    uint64_t hash = ged3d12_pipeline_key_hash(key);
    UINT first_free = UINT_MAX;
    for (UINT probe = 0; probe < PIPELINE_CACHE_CAPACITY; probe++) {
        UINT index = (UINT)(hash + probe) & (PIPELINE_CACHE_CAPACITY - 1);
        PipelineEntry *entry = &s_entries[index];
        if (!entry->used) {
            first_free = index;
            break;
        }
        if (entry->hash == hash &&
            memcmp(&entry->key, key, sizeof(*key)) == 0) {
            s_stats.hits++;
            ged3d12_command_list()->SetPipelineState(entry->pipeline);
            return true;
        }
    }
    s_stats.misses++;
    if (first_free == UINT_MAX) {
        fprintf(stderr, "ged3d12: GE pipeline cache exhausted (%u entries)\n",
                PIPELINE_CACHE_CAPACITY);
        s_stats.failures++;
        return false;
    }

    ID3D12PipelineState *pipeline = create_pipeline(key);
    if (!pipeline) {
        s_stats.failures++;
        return false;
    }
    PipelineEntry *entry = &s_entries[first_free];
    entry->key = *key;
    entry->hash = hash;
    entry->pipeline = pipeline;
    entry->used = 1;
    s_stats.entries++;
    if (s_log) {
        fprintf(stderr,
                "ged3d12: created synchronous GE PSO %u hash=%016llx\n",
                s_stats.entries, (unsigned long long)hash);
    }
    ged3d12_command_list()->SetPipelineState(pipeline);
    return true;
}

GeD3D12PipelineStats ged3d12_pipeline_stats(void) {
    return s_stats;
}
