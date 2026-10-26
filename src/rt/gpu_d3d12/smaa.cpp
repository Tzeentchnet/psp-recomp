#include "smaa.h"

#include "device.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../third_party/smaa/Textures/AreaTex.h"
#include "../../../third_party/smaa/Textures/SearchTex.h"

#define RELEASE_COM(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static const uint8_t k_smaa_edge_vs_dxil[] =
#include "shaders/smaa_edge_vs.inc"
;
static const uint8_t k_smaa_edge_ps_dxil[] =
#include "shaders/smaa_edge_ps.inc"
;
static const uint8_t k_smaa_blend_vs_dxil[] =
#include "shaders/smaa_blend_vs.inc"
;
static const uint8_t k_smaa_blend_ps_dxil[] =
#include "shaders/smaa_blend_ps.inc"
;
static const uint8_t k_smaa_neighborhood_vs_dxil[] =
#include "shaders/smaa_neighborhood_vs.inc"
;
static const uint8_t k_smaa_neighborhood_ps_dxil[] =
#include "shaders/smaa_neighborhood_ps.inc"
;
static_assert(sizeof(areaTexBytes) == AREATEX_SIZE, "invalid SMAA area texture");
static_assert(
    sizeof(searchTexBytes) == SEARCHTEX_SIZE, "invalid SMAA search texture");

static ID3D12RootSignature *s_root_signature;
static ID3D12PipelineState *s_edge_pipeline;
static ID3D12PipelineState *s_blend_pipeline;
static ID3D12PipelineState *s_neighborhood_pipeline;
static ID3D12Resource *s_edges_texture;
static ID3D12Resource *s_blend_texture;
static ID3D12Resource *s_output_texture;
static ID3D12Resource *s_area_texture;
static ID3D12Resource *s_search_texture;
static GeD3D12CpuDescriptor s_edges_rtv;
static GeD3D12CpuDescriptor s_blend_rtv;
static GeD3D12CpuDescriptor s_output_rtv;
static bool s_edges_rtv_allocated;
static bool s_blend_rtv_allocated;
static bool s_output_rtv_allocated;
static D3D12_RESOURCE_STATES s_edges_state;
static D3D12_RESOURCE_STATES s_blend_state;
static D3D12_RESOURCE_STATES s_output_state;
static UINT s_width;
static UINT s_height;
static UINT s_srv_stride;

static bool check_hr(HRESULT hr, const char *operation) {
    if (SUCCEEDED(hr)) return true;
    fprintf(
        stderr, "ged3d12: SMAA %s failed (HRESULT 0x%08lx)\n",
        operation, (unsigned long)hr);
    return false;
}

static void transition(
    ID3D12GraphicsCommandList *command_list,
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1, &barrier);
}

static bool create_root_signature(void) {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 3;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable.NumDescriptorRanges = 1;
    parameters[0].DescriptorTable.pDescriptorRanges = &range;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants.ShaderRegister = 0;
    parameters[1].Constants.Num32BitValues = 4;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (UINT i = 0; i < 2; i++) {
        samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[i].BorderColor =
            D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters =
        (UINT)(sizeof(parameters) / sizeof(parameters[0]));
    desc.pParameters = parameters;
    desc.NumStaticSamplers =
        (UINT)(sizeof(samplers) / sizeof(samplers[0]));
    desc.pStaticSamplers = samplers;
    desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ID3DBlob *serialized = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT serialize_hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (FAILED(serialize_hr)) {
        if (errors && errors->GetBufferPointer()) {
            fprintf(
                stderr, "ged3d12: serialize SMAA root signature failed: %s\n",
                (const char *)errors->GetBufferPointer());
        }
        RELEASE_COM(errors);
        RELEASE_COM(serialized);
        return check_hr(serialize_hr, "serialize root signature");
    }
    RELEASE_COM(errors);
    HRESULT create_hr = ged3d12_device()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&s_root_signature));
    RELEASE_COM(serialized);
    return check_hr(create_hr, "create root signature");
}

static bool create_pipeline(
    const uint8_t *vertex_shader, size_t vertex_shader_size,
    const uint8_t *pixel_shader, size_t pixel_shader_size,
    DXGI_FORMAT format, ID3D12PipelineState **pipeline,
    const char *operation) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = s_root_signature;
    desc.VS = {vertex_shader, vertex_shader_size};
    desc.PS = {pixel_shader, pixel_shader_size};
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    desc.RasterizerState.DepthBiasClamp =
        D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask =
        D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType =
        D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    return check_hr(
        ged3d12_device()->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(pipeline)),
        operation);
}

static bool create_render_target(
    DXGI_FORMAT format, ID3D12Resource **resource,
    GeD3D12CpuDescriptor *rtv, bool *rtv_allocated,
    D3D12_RESOURCE_STATES *state, const char *operation) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = s_width;
    desc.Height = s_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear = {};
    clear.Format = format;
    if (!check_hr(
            ged3d12_device()->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                IID_PPV_ARGS(resource)),
            operation)) {
        return false;
    }
    if (!ged3d12_rtv_alloc(rtv)) {
        fprintf(stderr, "ged3d12: SMAA RTV allocation failed\n");
        return false;
    }
    *rtv_allocated = true;
    D3D12_RENDER_TARGET_VIEW_DESC view = {};
    view.Format = format;
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    ged3d12_device()->CreateRenderTargetView(
        *resource, &view, rtv->cpu);
    *state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
}

static bool create_lookup_texture(
    UINT width, UINT height, UINT source_pitch, DXGI_FORMAT format,
    const uint8_t *data, ID3D12Resource **texture,
    const char *operation) {
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap.CreationNodeMask = 1;
    default_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture_desc = {};
    texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.DepthOrArraySize = 1;
    texture_desc.MipLevels = 1;
    texture_desc.Format = format;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (!check_hr(
            ged3d12_device()->CreateCommittedResource(
                &default_heap, D3D12_HEAP_FLAG_NONE, &texture_desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(texture)),
            operation)) {
        return false;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 row_size = 0;
    UINT64 upload_size = 0;
    ged3d12_device()->GetCopyableFootprints(
        &texture_desc, 0, 1, 0, &footprint, &rows,
        &row_size, &upload_size);
    if (rows != height || row_size < source_pitch ||
        footprint.Footprint.RowPitch < source_pitch) {
        fprintf(stderr, "ged3d12: SMAA lookup footprint is invalid\n");
        return false;
    }

    ID3D12Resource *upload = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *command_list = nullptr;
    bool ok = false;
    do {
        D3D12_HEAP_PROPERTIES upload_heap = {};
        upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        upload_heap.CreationNodeMask = 1;
        upload_heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC upload_desc = {};
        upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        upload_desc.Width = upload_size;
        upload_desc.Height = 1;
        upload_desc.DepthOrArraySize = 1;
        upload_desc.MipLevels = 1;
        upload_desc.SampleDesc.Count = 1;
        upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!check_hr(
                ged3d12_device()->CreateCommittedResource(
                    &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&upload)),
                "create lookup upload buffer")) {
            break;
        }
        uint8_t *mapped = nullptr;
        D3D12_RANGE read_range = {0, 0};
        if (!check_hr(
                upload->Map(
                    0, &read_range,
                    reinterpret_cast<void **>(&mapped)),
                "map lookup upload buffer")) {
            break;
        }
        memset(mapped, 0, (size_t)upload_size);
        for (UINT row = 0; row < height; row++) {
            memcpy(
                mapped + footprint.Offset +
                    (UINT64)row * footprint.Footprint.RowPitch,
                data + (size_t)row * source_pitch,
                source_pitch);
        }
        D3D12_RANGE written_range = {0, (SIZE_T)upload_size};
        upload->Unmap(0, &written_range);

        if (!check_hr(
                ged3d12_device()->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_DIRECT,
                    IID_PPV_ARGS(&allocator)),
                "create lookup command allocator") ||
            !check_hr(
                ged3d12_device()->CreateCommandList(
                    0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                    allocator, nullptr,
                    IID_PPV_ARGS(&command_list)),
                "create lookup command list")) {
            break;
        }
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = upload;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = *texture;
        destination.Type =
            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        command_list->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
        transition(
            command_list, *texture,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (!check_hr(
                command_list->Close(),
                "close lookup command list")) {
            break;
        }
        ID3D12CommandList *lists[] = {command_list};
        ged3d12_graphics_queue()->ExecuteCommandLists(1, lists);
        if (!ged3d12_graphics_wait_idle()) break;
        ok = true;
    } while (false);
    RELEASE_COM(command_list);
    RELEASE_COM(allocator);
    RELEASE_COM(upload);
    return ok;
}

static bool create_srv_table(
    ID3D12Resource *const resources[3],
    const DXGI_FORMAT formats[3],
    GeD3D12DescriptorSlice *table) {
    if (!ged3d12_srv_alloc(3, table)) return false;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = table->cpu;
    for (UINT i = 0; i < 3; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Format = formats[i];
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        ged3d12_device()->CreateShaderResourceView(
            resources[i], &view, handle);
        handle.ptr += s_srv_stride;
    }
    return true;
}

static bool record_pass(
    ID3D12Resource *target,
    D3D12_CPU_DESCRIPTOR_HANDLE rtv,
    D3D12_RESOURCE_STATES *state,
    ID3D12PipelineState *pipeline,
    ID3D12Resource *const resources[3],
    const DXGI_FORMAT formats[3]) {
    GeD3D12DescriptorSlice table = {};
    if (!create_srv_table(resources, formats, &table)) return false;
    ID3D12GraphicsCommandList *command_list =
        ged3d12_command_list();
    transition(
        command_list, target, *state,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    *state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    command_list->ClearRenderTargetView(
        rtv, clear, 0, nullptr);
    command_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    command_list->SetGraphicsRootDescriptorTable(0, table.gpu);
    command_list->SetPipelineState(pipeline);
    command_list->DrawInstanced(3, 1, 0, 0);
    transition(
        command_list, target,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    *state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

bool ged3d12_smaa_init(UINT width, UINT height) {
    if (!width || !height || s_root_signature || s_edges_texture ||
        s_blend_texture || s_output_texture || s_area_texture ||
        s_search_texture) {
        fprintf(stderr, "ged3d12: invalid SMAA initialization state\n");
        return false;
    }
    s_width = width;
    s_height = height;
    s_srv_stride =
        ged3d12_device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!create_root_signature() ||
        !create_pipeline(
            k_smaa_edge_vs_dxil, sizeof(k_smaa_edge_vs_dxil),
            k_smaa_edge_ps_dxil, sizeof(k_smaa_edge_ps_dxil),
            DXGI_FORMAT_R8G8B8A8_UNORM, &s_edge_pipeline,
            "create edge pipeline") ||
        !create_pipeline(
            k_smaa_blend_vs_dxil, sizeof(k_smaa_blend_vs_dxil),
            k_smaa_blend_ps_dxil, sizeof(k_smaa_blend_ps_dxil),
            DXGI_FORMAT_R8G8B8A8_UNORM, &s_blend_pipeline,
            "create blend pipeline") ||
        !create_pipeline(
            k_smaa_neighborhood_vs_dxil,
            sizeof(k_smaa_neighborhood_vs_dxil),
            k_smaa_neighborhood_ps_dxil,
            sizeof(k_smaa_neighborhood_ps_dxil),
            DXGI_FORMAT_B8G8R8A8_UNORM,
            &s_neighborhood_pipeline,
            "create neighborhood pipeline") ||
        !create_render_target(
            DXGI_FORMAT_R8G8B8A8_UNORM, &s_edges_texture,
            &s_edges_rtv, &s_edges_rtv_allocated,
            &s_edges_state, "create edges texture") ||
        !create_render_target(
            DXGI_FORMAT_R8G8B8A8_UNORM, &s_blend_texture,
            &s_blend_rtv, &s_blend_rtv_allocated,
            &s_blend_state, "create blend texture") ||
        !create_render_target(
            DXGI_FORMAT_B8G8R8A8_UNORM, &s_output_texture,
            &s_output_rtv, &s_output_rtv_allocated,
            &s_output_state, "create output texture") ||
        !create_lookup_texture(
            AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH,
            DXGI_FORMAT_R8G8_UNORM, areaTexBytes,
            &s_area_texture, "create area texture") ||
        !create_lookup_texture(
            SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH,
            DXGI_FORMAT_R8_UNORM, searchTexBytes,
            &s_search_texture, "create search texture")) {
        ged3d12_smaa_shutdown();
        return false;
    }
    fprintf(
        stderr, "ged3d12: SMAA 1x High enabled at %ux%u\n",
        s_width, s_height);
    return true;
}

void ged3d12_smaa_shutdown(void) {
    if (s_output_rtv_allocated) {
        ged3d12_rtv_free(s_output_rtv.index);
        s_output_rtv_allocated = false;
    }
    if (s_blend_rtv_allocated) {
        ged3d12_rtv_free(s_blend_rtv.index);
        s_blend_rtv_allocated = false;
    }
    if (s_edges_rtv_allocated) {
        ged3d12_rtv_free(s_edges_rtv.index);
        s_edges_rtv_allocated = false;
    }
    RELEASE_COM(s_search_texture);
    RELEASE_COM(s_area_texture);
    RELEASE_COM(s_output_texture);
    RELEASE_COM(s_blend_texture);
    RELEASE_COM(s_edges_texture);
    RELEASE_COM(s_neighborhood_pipeline);
    RELEASE_COM(s_blend_pipeline);
    RELEASE_COM(s_edge_pipeline);
    RELEASE_COM(s_root_signature);
    s_edges_state = D3D12_RESOURCE_STATE_COMMON;
    s_blend_state = D3D12_RESOURCE_STATE_COMMON;
    s_output_state = D3D12_RESOURCE_STATE_COMMON;
    s_width = 0;
    s_height = 0;
    s_srv_stride = 0;
}

bool ged3d12_smaa_record(ID3D12Resource *input) {
    if (!input || !s_width || !s_height || !s_root_signature ||
        !s_edge_pipeline || !s_blend_pipeline ||
        !s_neighborhood_pipeline || !s_edges_texture ||
        !s_blend_texture || !s_output_texture ||
        !s_area_texture || !s_search_texture) {
        fprintf(stderr, "ged3d12: invalid SMAA draw state\n");
        return false;
    }
    ID3D12GraphicsCommandList *command_list =
        ged3d12_command_list();
    ged3d12_set_coarse_shading(false);
    D3D12_VIEWPORT viewport = {};
    viewport.Width = (float)s_width;
    viewport.Height = (float)s_height;
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor = {
        0, 0, (LONG)s_width, (LONG)s_height
    };
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &scissor);
    command_list->SetGraphicsRootSignature(s_root_signature);
    const float metrics[4] = {
        1.0f / (float)s_width,
        1.0f / (float)s_height,
        (float)s_width,
        (float)s_height,
    };
    command_list->SetGraphicsRoot32BitConstants(
        1, 4, metrics, 0);
    command_list->IASetPrimitiveTopology(
        D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12Resource *edge_resources[3] = {
        input, nullptr, nullptr
    };
    const DXGI_FORMAT edge_formats[3] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    if (!record_pass(
            s_edges_texture, s_edges_rtv.cpu, &s_edges_state,
            s_edge_pipeline, edge_resources, edge_formats)) {
        return false;
    }

    ID3D12Resource *blend_resources[3] = {
        s_edges_texture, s_area_texture, s_search_texture
    };
    const DXGI_FORMAT blend_formats[3] = {
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8_UNORM,
        DXGI_FORMAT_R8_UNORM,
    };
    if (!record_pass(
            s_blend_texture, s_blend_rtv.cpu, &s_blend_state,
            s_blend_pipeline, blend_resources, blend_formats)) {
        return false;
    }

    ID3D12Resource *neighborhood_resources[3] = {
        input, s_blend_texture, nullptr
    };
    const DXGI_FORMAT neighborhood_formats[3] = {
        DXGI_FORMAT_B8G8R8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    return record_pass(
        s_output_texture, s_output_rtv.cpu, &s_output_state,
        s_neighborhood_pipeline,
        neighborhood_resources, neighborhood_formats);
}

ID3D12Resource *ged3d12_smaa_output(void) {
    return s_output_texture;
}
