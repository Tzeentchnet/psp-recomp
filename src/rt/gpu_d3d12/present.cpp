#include "present.h"

#include "device.h"
#include "framebuffer.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSP_WIDTH 480u
#define PSP_HEIGHT 272u
#define RELEASE_COM(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static const uint8_t k_present_vs_dxil[] =
#include "shaders/present_vs.inc"
;
static const uint8_t k_present_ps_dxil[] =
#include "shaders/present_ps.inc"
;

static ID3D12Resource *s_display_texture;
static ID3D12Resource *s_capture_readback;
static ID3D12RootSignature *s_present_root_signature;
static ID3D12PipelineState *s_present_pipeline;
static D3D12_RESOURCE_STATES s_display_state;
static UINT s_render_scale;
static UINT s_display_width;
static UINT s_display_height;
static UINT s_capture_row_pitch;
static UINT64 s_capture_bytes;

static bool check_hr(HRESULT hr, const char *operation) {
    if (SUCCEEDED(hr)) return true;
    fprintf(stderr, "ged3d12: %s failed (HRESULT 0x%08lx)\n",
            operation, (unsigned long)hr);
    return false;
}

static void transition(
    ID3D12GraphicsCommandList *command_list,
    ID3D12Resource *resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    command_list->ResourceBarrier(1, &barrier);
}

static bool create_display_texture(void) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    s_render_scale = ged3d12_framebuffer_scale();
    s_display_width = PSP_WIDTH * s_render_scale;
    s_display_height = PSP_HEIGHT * s_render_scale;
    desc.Width = s_display_width;
    desc.Height = s_display_height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&s_display_texture)), "create display texture")) {
        return false;
    }
    s_display_state = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

static bool create_capture_readback(void) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    s_capture_row_pitch =
        (s_display_width * 4u +
         D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
        ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    s_capture_bytes =
        (UINT64)s_capture_row_pitch * s_display_height;
    desc.Width = s_capture_bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return check_hr(ged3d12_device()->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&s_capture_readback)), "create present capture readback");
}

static bool create_present_root_signature(void) {
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 1;
    desc.pParameters = &parameter;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
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
            fprintf(stderr, "ged3d12: serialize present root signature failed: %s\n",
                    (const char *)errors->GetBufferPointer());
        }
        RELEASE_COM(errors);
        RELEASE_COM(serialized);
        return check_hr(serialize_hr, "serialize present root signature");
    }
    RELEASE_COM(errors);
    HRESULT create_hr = ged3d12_device()->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&s_present_root_signature));
    RELEASE_COM(serialized);
    return check_hr(create_hr, "create present root signature");
}

static bool create_present_pipeline(void) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = s_present_root_signature;
    desc.VS = {k_present_vs_dxil, sizeof(k_present_vs_dxil)};
    desc.PS = {k_present_ps_dxil, sizeof(k_present_ps_dxil)};
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
    desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    desc.RasterizerState.SlopeScaledDepthBias =
        D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    return check_hr(ged3d12_device()->CreateGraphicsPipelineState(
        &desc, IID_PPV_ARGS(&s_present_pipeline)), "create present pipeline");
}

bool ged3d12_present_init(void) {
    if (s_display_texture || s_capture_readback ||
        s_present_root_signature || s_present_pipeline) {
        fprintf(stderr, "ged3d12: present renderer already initialized\n");
        return false;
    }
    if (!create_display_texture() ||
        !create_capture_readback() ||
        !create_present_root_signature() ||
        !create_present_pipeline()) {
        ged3d12_present_shutdown();
        return false;
    }
    return true;
}

void ged3d12_present_shutdown(void) {
    RELEASE_COM(s_present_pipeline);
    RELEASE_COM(s_present_root_signature);
    RELEASE_COM(s_capture_readback);
    RELEASE_COM(s_display_texture);
    s_display_state = D3D12_RESOURCE_STATE_COMMON;
    s_render_scale = 0;
    s_display_width = 0;
    s_display_height = 0;
    s_capture_row_pitch = 0;
    s_capture_bytes = 0;
}

bool ged3d12_present_record(
    const D3D12_TEXTURE_COPY_LOCATION *source,
    const D3D12_BOX *source_box,
    ID3D12Resource *backbuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_rtv,
    UINT output_width,
    UINT output_height) {
    if (!source || !backbuffer || !output_width || !output_height ||
        !s_display_texture || !s_present_root_signature || !s_present_pipeline) {
        fprintf(stderr, "ged3d12: invalid present draw state\n");
        return false;
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    if (s_display_state != D3D12_RESOURCE_STATE_COPY_DEST) {
        transition(command_list, s_display_texture, s_display_state,
                   D3D12_RESOURCE_STATE_COPY_DEST);
    }
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = s_display_texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command_list->CopyTextureRegion(
        &destination, 0, 0, 0, source, source_box);
    transition(command_list, s_display_texture,
               D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s_display_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    GeD3D12DescriptorSlice display_srv = {};
    if (!ged3d12_srv_alloc(1, &display_srv)) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    ged3d12_device()->CreateShaderResourceView(
        s_display_texture, &srv, display_srv.cpu);

    transition(command_list, backbuffer,
               D3D12_RESOURCE_STATE_PRESENT,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    command_list->ClearRenderTargetView(
        backbuffer_rtv, clear_color, 0, nullptr);
    command_list->OMSetRenderTargets(
        1, &backbuffer_rtv, FALSE, nullptr);
    ged3d12_set_coarse_shading(false);

    GeD3D12PresentViewport layout = {};
    ged3d12_present_viewport(output_width, output_height, &layout);
    command_list->RSSetViewports(1, &layout.viewport);
    command_list->RSSetScissorRects(1, &layout.scissor);
    command_list->SetGraphicsRootSignature(s_present_root_signature);
    command_list->SetGraphicsRootDescriptorTable(0, display_srv.gpu);
    command_list->SetPipelineState(s_present_pipeline);
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    command_list->DrawInstanced(3, 1, 0, 0);
    transition(command_list, backbuffer,
               D3D12_RESOURCE_STATE_RENDER_TARGET,
               D3D12_RESOURCE_STATE_PRESENT);
    return true;
}

bool ged3d12_present_capture_record(void) {
    if (!s_display_texture || !s_capture_readback) {
        fprintf(stderr, "ged3d12: present capture is not initialized\n");
        return false;
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    transition(command_list, s_display_texture, s_display_state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = s_display_texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = s_capture_readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width = s_display_width;
    destination.PlacedFootprint.Footprint.Height = s_display_height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = s_capture_row_pitch;
    command_list->CopyTextureRegion(
        &destination, 0, 0, 0, &source, nullptr);

    transition(command_list, s_display_texture,
               D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    s_display_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

bool ged3d12_present_capture_write(const char *path) {
    if (!path || !path[0] || !s_capture_readback) return false;
    const char *internal_setting =
        getenv("SR_D3D12_CAPTURE_INTERNAL");
    const bool internal_resolution =
        internal_setting && internal_setting[0] &&
        strcmp(internal_setting, "0") != 0;
    GeD3D12CaptureLayout layout = {};
    ged3d12_present_capture_layout(
        s_render_scale, internal_resolution, &layout);
    if (!layout.width || !layout.height || !layout.source_step)
        return false;
    D3D12_RANGE read_range = {0, (SIZE_T)s_capture_bytes};
    uint8_t *data = nullptr;
    if (!check_hr(s_capture_readback->Map(
            0, &read_range, reinterpret_cast<void **>(&data)),
            "map present capture readback")) {
        return false;
    }

    FILE *file = fopen(path, "wb");
    bool failed = !file ||
        fprintf(
            file, "P6\n%u %u\n255\n",
            layout.width, layout.height) < 0;
    for (UINT y = 0; y < layout.height && !failed; y++) {
        const UINT source_y =
            y * layout.source_step + layout.sample_offset;
        const uint8_t *row =
            data + (UINT64)source_y * s_capture_row_pitch;
        for (UINT x = 0; x < layout.width; x++) {
            const UINT source_x =
                x * layout.source_step + layout.sample_offset;
            const uint8_t rgb[3] = {
                row[source_x * 4 + 2],
                row[source_x * 4 + 1],
                row[source_x * 4],
            };
            if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) {
                failed = true;
                break;
            }
        }
    }
    if (file && fclose(file) != 0) failed = true;
    D3D12_RANGE written_range = {0, 0};
    s_capture_readback->Unmap(0, &written_range);
    if (failed) {
        fprintf(stderr, "ged3d12: cannot write present capture %s\n", path);
        return false;
    }
    if (internal_resolution) {
        fprintf(
            stderr,
            "ged3d12: internal-resolution capture %ux%u\n",
            layout.width, layout.height);
    }
    return true;
}
