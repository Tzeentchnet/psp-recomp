/* D3D12 device and presentation foundation.
 *
 * GE primitives continue through the software rasterizer until native render-target and
 * presentation stages submit the captured batches. Its finished 480x272 framebuffer is uploaded
 * through the same per-frame ring that later draw data will use. */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <SDL3/SDL.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device.h"
#include "framebuffer.h"
#include "pipeline.h"
#include "present.h"
#include "shader_state.h"
#include "texture.h"
#include "vertex.h"
#include "../input_sdl.h"
extern "C" {
#include "../recomp.h"
}

#define PSP_W 480
#define PSP_H 272
#define FRAME_COUNT 3u
#define SRV_DESCRIPTOR_COUNT 4095u
#define SAMPLER_DESCRIPTOR_COUNT 768u
#define RTV_DESCRIPTOR_COUNT 256u
#define DSV_DESCRIPTOR_COUNT 64u
#define UPLOAD_BYTES_PER_FRAME (32ull * 1024ull * 1024ull)
#define UPLOAD_BYTES (FRAME_COUNT * UPLOAD_BYTES_PER_FRAME)
#define UPLOAD_OVERFLOW_SLAB_COUNT 16u

#define RELEASE_COM(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static const uint8_t k_psp_vs_dxil[] =
#include "shaders/psp_vs.inc"
;
static const uint8_t k_psp_ps_dxil[] =
#include "shaders/psp_ps.inc"
;

struct UploadSlab {
    ID3D12Resource *resource;
    uint8_t *map;
    UINT64 cursor;
    UINT64 size;
};

struct FrameContext {
    ID3D12CommandAllocator *allocator;
    UINT64 fence_value;
    UINT64 upload_begin;
    UINT64 upload_cursor;
    UINT64 upload_end;
    UINT srv_begin;
    UINT srv_cursor;
    UINT srv_end;
    UINT sampler_begin;
    UINT sampler_cursor;
    UINT sampler_end;
    UploadSlab upload_overflow[UPLOAD_OVERFLOW_SLAB_COUNT];
    UINT upload_overflow_count;
};

static SDL_Window *s_window;
static SDL_Gamepad *s_pad;
static HWND s_hwnd;
static int s_sdl_initialized;

static ID3D12Device *s_device;
static ID3D12CommandQueue *s_graphics_queue;
static ID3D12CommandQueue *s_copy_queue;
static ID3D12Fence *s_graphics_fence;
static ID3D12Fence *s_copy_fence;
static HANDLE s_graphics_event;
static HANDLE s_copy_event;
static UINT64 s_next_graphics_fence = 1;
static UINT64 s_next_copy_fence = 1;

static IDXGISwapChain3 *s_swapchain;
static ID3D12Resource *s_backbuffers[FRAME_COUNT];
static UINT s_backbuffer_width;
static UINT s_backbuffer_height;
static ID3D12DescriptorHeap *s_srv_heap;
static ID3D12DescriptorHeap *s_sampler_heap;
static ID3D12DescriptorHeap *s_rtv_heap;
static ID3D12DescriptorHeap *s_dsv_heap;
static ID3D12RootSignature *s_root_signature;
static UINT s_srv_stride;
static UINT s_sampler_stride;
static UINT s_rtv_stride;
static UINT s_dsv_stride;
static uint8_t s_rtv_used[RTV_DESCRIPTOR_COUNT];
static uint8_t s_dsv_used[DSV_DESCRIPTOR_COUNT];

static FrameContext s_frames[FRAME_COUNT];
static ID3D12GraphicsCommandList *s_command_list;
static UINT s_active_frame = UINT32_MAX;

static ID3D12Resource *s_upload;
static uint8_t *s_upload_map;

static uint32_t s_buttons;
static uint8_t s_lx = 128, s_ly = 128;
static int s_pad_present;
static char s_capture_path[1024];
static int s_vertex_log;
static int s_gpu_log;
static int s_framebuffer_safe;
static int s_destination_safe;
static UINT s_state_dump_index;
static UINT64 s_gpu_log_flushes;
static UINT64 s_gpu_log_draws;
static UINT64 s_vertex_frames;
static UINT64 s_vertex_total;
static UINT64 s_vertex_batches;
static UINT64 s_constant_updates;
static UINT64 s_constant_reuses;
static UINT64 s_vertex_primitives[GE_D3D12_PRIMITIVE_COUNT];
static UINT s_vertex_max;

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static const char *primitive_name(uint8_t primitive) {
    switch (primitive) {
    case GE_D3D12_PRIMITIVE_TRIANGLE: return "triangle";
    case GE_D3D12_PRIMITIVE_SPRITE: return "sprite";
    case GE_D3D12_PRIMITIVE_LINE: return "line";
    case GE_D3D12_PRIMITIVE_POINT: return "point";
    default: return "unknown";
    }
}

static void pix_begin_event(const char *label) {
    s_command_list->BeginEvent(
        1, label, (UINT)(strlen(label) + 1));
}

static void pix_end_event(void) {
    s_command_list->EndEvent();
}

static int check_hr(HRESULT hr, const char *operation) {
    if (SUCCEEDED(hr)) return 1;
    fprintf(stderr, "ged3d12: %s failed (HRESULT 0x%08lx)\n",
            operation, (unsigned long)hr);
    return 0;
}

static UINT64 align_up(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static int wait_for_fence(ID3D12Fence *fence, UINT64 value, HANDLE event_handle,
                          const char *operation) {
    if (!value || fence->GetCompletedValue() >= value) return 1;
    if (!check_hr(fence->SetEventOnCompletion(value, event_handle), operation)) return 0;
    DWORD wait = WaitForSingleObject(event_handle, 30000);
    if (wait != WAIT_OBJECT_0) {
        fprintf(stderr, "ged3d12: %s timed out or failed (wait=%lu, error=%lu)\n",
                operation, (unsigned long)wait, (unsigned long)GetLastError());
        return 0;
    }
    return 1;
}

static int wait_queue_idle(ID3D12CommandQueue *queue, ID3D12Fence *fence,
                           UINT64 *next_value, HANDLE event_handle, const char *operation) {
    if (!queue || !fence || !event_handle) return 1;
    UINT64 value = (*next_value)++;
    if (!check_hr(queue->Signal(fence, value), operation)) return 0;
    return wait_for_fence(fence, value, event_handle, operation);
}

static void release_upload_overflow(FrameContext *frame) {
    for (UINT i = 0; i < frame->upload_overflow_count; i++) {
        UploadSlab *slab = &frame->upload_overflow[i];
        if (slab->resource && slab->map) {
            D3D12_RANGE written = {0, 0};
            slab->resource->Unmap(0, &written);
        }
        slab->map = nullptr;
        RELEASE_COM(slab->resource);
        slab->cursor = 0;
        slab->size = 0;
    }
    frame->upload_overflow_count = 0;
}

static void release_device(void) {
    if (s_swapchain) s_swapchain->SetFullscreenState(FALSE, nullptr);
    wait_queue_idle(s_graphics_queue, s_graphics_fence, &s_next_graphics_fence,
                    s_graphics_event, "wait for graphics queue");
    wait_queue_idle(s_copy_queue, s_copy_fence, &s_next_copy_fence,
                    s_copy_event, "wait for copy queue");
    ged3d12_pipeline_cache_shutdown();
    ged3d12_texture_cache_shutdown();
    ged3d12_framebuffer_cache_shutdown();
    ged3d12_present_shutdown();
    ged3d12_vertex_capture_shutdown();

    if (s_upload && s_upload_map) {
        D3D12_RANGE written = {0, 0};
        s_upload->Unmap(0, &written);
    }
    s_upload_map = nullptr;
    RELEASE_COM(s_upload);

    for (UINT i = 0; i < FRAME_COUNT; i++) {
        release_upload_overflow(&s_frames[i]);
        RELEASE_COM(s_backbuffers[i]);
        RELEASE_COM(s_frames[i].allocator);
        memset(&s_frames[i], 0, sizeof(s_frames[i]));
    }
    RELEASE_COM(s_command_list);
    RELEASE_COM(s_swapchain);
    RELEASE_COM(s_root_signature);
    RELEASE_COM(s_srv_heap);
    RELEASE_COM(s_sampler_heap);
    RELEASE_COM(s_rtv_heap);
    RELEASE_COM(s_dsv_heap);
    RELEASE_COM(s_graphics_fence);
    RELEASE_COM(s_copy_fence);
    RELEASE_COM(s_graphics_queue);
    RELEASE_COM(s_copy_queue);
    RELEASE_COM(s_device);

    if (s_graphics_event) CloseHandle(s_graphics_event);
    if (s_copy_event) CloseHandle(s_copy_event);
    s_graphics_event = nullptr;
    s_copy_event = nullptr;
    s_next_graphics_fence = 1;
    s_next_copy_fence = 1;
    s_active_frame = UINT32_MAX;
    s_capture_path[0] = '\0';
    s_backbuffer_width = 0;
    s_backbuffer_height = 0;
    s_srv_stride = 0;
    s_sampler_stride = 0;
    s_rtv_stride = 0;
    s_dsv_stride = 0;
    memset(s_rtv_used, 0, sizeof(s_rtv_used));
    memset(s_dsv_used, 0, sizeof(s_dsv_used));
}

void ged3d12_window_shutdown(void) {
    if (s_pad) SDL_CloseGamepad(s_pad);
    s_pad = nullptr;
    if (s_window) SDL_DestroyWindow(s_window);
    s_window = nullptr;
    s_hwnd = nullptr;
    if (s_sdl_initialized) SDL_Quit();
    s_sdl_initialized = 0;
    s_buttons = 0;
    s_lx = s_ly = 128;
    s_pad_present = 0;
}

int ged3d12_window_init(const char *title) {
    if (s_window) {
        fprintf(stderr, "ged3d12: window already initialized\n");
        return 0;
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "ged3d12: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    s_sdl_initialized = 1;
    s_window = SDL_CreateWindow(
        title ? title : "PSP Recomp", PSP_W * 2, PSP_H * 2,
        SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        fprintf(stderr, "ged3d12: SDL_CreateWindow failed: %s\n", SDL_GetError());
        ged3d12_window_shutdown();
        return 0;
    }
    SDL_PropertiesID properties = SDL_GetWindowProperties(s_window);
    s_hwnd = (HWND)SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!s_hwnd) {
        fprintf(stderr, "ged3d12: SDL window has no Win32 HWND: %s\n", SDL_GetError());
        ged3d12_window_shutdown();
        return 0;
    }

    if (SDL_HasGamepad()) {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        if (ids && count > 0) s_pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    return 1;
}

static int enable_debug_layer(UINT *factory_flags) {
    if (!env_enabled("SR_D3D12_DEBUG")) return 1;

    ID3D12Debug *debug = nullptr;
    if (!check_hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)),
                  "D3D12GetDebugInterface")) {
        return 0;
    }
    debug->EnableDebugLayer();
    if (env_enabled("SR_D3D12_GPU_VALIDATION")) {
        ID3D12Debug1 *debug1 = nullptr;
        if (!check_hr(debug->QueryInterface(IID_PPV_ARGS(&debug1)),
                      "query ID3D12Debug1")) {
            RELEASE_COM(debug);
            return 0;
        }
        debug1->SetEnableGPUBasedValidation(TRUE);
        RELEASE_COM(debug1);
    }
    RELEASE_COM(debug);
    *factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    return 1;
}

static int choose_adapter(IDXGIFactory4 *factory, DXGI_ADAPTER_DESC1 *selected_desc) {
    if (env_enabled("SR_D3D12_WARP")) {
        IDXGIAdapter *warp = nullptr;
        if (!check_hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)), "EnumWarpAdapter"))
            return 0;
        HRESULT hr = D3D12CreateDevice(
            warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s_device));
        IDXGIAdapter1 *warp1 = nullptr;
        if (SUCCEEDED(warp->QueryInterface(IID_PPV_ARGS(&warp1)))) {
            warp1->GetDesc1(selected_desc);
            RELEASE_COM(warp1);
        }
        RELEASE_COM(warp);
        return check_hr(hr, "D3D12CreateDevice (WARP)");
    }

    IDXGIFactory6 *factory6 = nullptr;
    factory->QueryInterface(IID_PPV_ARGS(&factory6));
    for (UINT index = 0; ; index++) {
        IDXGIAdapter1 *adapter = nullptr;
        HRESULT enum_hr = factory6
            ? factory6->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))
            : factory->EnumAdapters1(index, &adapter);
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr)) {
            RELEASE_COM(factory6);
            return check_hr(enum_hr, "enumerate DXGI adapters");
        }

        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            HRESULT device_hr = D3D12CreateDevice(
                adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&s_device));
            if (SUCCEEDED(device_hr)) {
                *selected_desc = desc;
                RELEASE_COM(adapter);
                RELEASE_COM(factory6);
                return 1;
            }
        }
        RELEASE_COM(adapter);
    }
    RELEASE_COM(factory6);
    fprintf(stderr, "ged3d12: no hardware adapter supports D3D12 feature level 11_0\n");
    return 0;
}

static int create_queues_and_fences(void) {
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (!check_hr(s_device->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&s_graphics_queue)),
            "create graphics queue")) {
        return 0;
    }
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    if (!check_hr(s_device->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&s_copy_queue)),
            "create copy queue")) {
        return 0;
    }
    if (!check_hr(s_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_graphics_fence)),
            "create graphics fence") ||
        !check_hr(s_device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_copy_fence)),
            "create copy fence")) {
        return 0;
    }
    s_graphics_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_copy_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_graphics_event || !s_copy_event) {
        fprintf(stderr, "ged3d12: CreateEventW failed (error %lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }
    return 1;
}

static int create_descriptor_heaps(void) {
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = SRV_DESCRIPTOR_COUNT;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check_hr(s_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&s_srv_heap)), "create CBV/SRV/UAV heap")) {
        return 0;
    }
    desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    desc.NumDescriptors = SAMPLER_DESCRIPTOR_COUNT;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (!check_hr(s_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&s_sampler_heap)), "create sampler heap")) {
        return 0;
    }
    desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = RTV_DESCRIPTOR_COUNT;
    if (!check_hr(s_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&s_rtv_heap)), "create RTV heap")) {
        return 0;
    }
    desc = {};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    desc.NumDescriptors = DSV_DESCRIPTOR_COUNT;
    if (!check_hr(s_device->CreateDescriptorHeap(
            &desc, IID_PPV_ARGS(&s_dsv_heap)), "create DSV heap")) {
        return 0;
    }
    s_srv_stride = s_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    s_sampler_stride = s_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    s_rtv_stride = s_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_dsv_stride = s_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT i = 0; i < FRAME_COUNT; i++) s_rtv_used[i] = 1;
    return 1;
}

static int create_root_signature(void) {
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2; /* t0 texture, t1 CLUT */
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 1; /* s0 per-draw sampler */
    ranges[1].BaseShaderRegister = GE_D3D12_SAMPLER_DYNAMIC;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 2; /* t2 destination color, t3 depth */
    ranges[2].BaseShaderRegister = 2;
    ranges[2].RegisterSpace = 0;
    ranges[2].OffsetInDescriptorsFromTableStart =
        D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER parameters[5] = {};
    parameters[GE_D3D12_ROOT_GE_STATE].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[GE_D3D12_ROOT_GE_STATE].Constants.ShaderRegister = 0;
    parameters[GE_D3D12_ROOT_GE_STATE].Constants.RegisterSpace = 0;
    parameters[GE_D3D12_ROOT_GE_STATE].Constants.Num32BitValues =
        GE_D3D12_SHADER_CONSTANT_DWORDS;
    parameters[GE_D3D12_ROOT_GE_STATE].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[GE_D3D12_ROOT_TEXTURES].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[GE_D3D12_ROOT_TEXTURES].DescriptorTable.NumDescriptorRanges = 1;
    parameters[GE_D3D12_ROOT_TEXTURES].DescriptorTable.pDescriptorRanges = &ranges[0];
    parameters[GE_D3D12_ROOT_TEXTURES].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[GE_D3D12_ROOT_SAMPLER].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[GE_D3D12_ROOT_SAMPLER].DescriptorTable.NumDescriptorRanges = 1;
    parameters[GE_D3D12_ROOT_SAMPLER].DescriptorTable.pDescriptorRanges = &ranges[1];
    parameters[GE_D3D12_ROOT_SAMPLER].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[GE_D3D12_ROOT_DESTINATION].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[GE_D3D12_ROOT_DESTINATION]
        .DescriptorTable.NumDescriptorRanges = 1;
    parameters[GE_D3D12_ROOT_DESTINATION]
        .DescriptorTable.pDescriptorRanges = &ranges[2];
    parameters[GE_D3D12_ROOT_DESTINATION].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[GE_D3D12_ROOT_VERTICES].ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[GE_D3D12_ROOT_VERTICES].Descriptor.ShaderRegister = 0;
    parameters[GE_D3D12_ROOT_VERTICES].Descriptor.RegisterSpace = 1;
    parameters[GE_D3D12_ROOT_VERTICES].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_STATIC_SAMPLER_DESC static_samplers[2] = {};
    for (UINT i = 0; i < 2; i++) {
        static_samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        static_samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        static_samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        static_samplers[i].MipLODBias = 0.0f;
        static_samplers[i].MaxAnisotropy = 1;
        static_samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        static_samplers[i].BorderColor =
            D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
        static_samplers[i].MinLOD = 0.0f;
        static_samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        static_samplers[i].ShaderRegister =
            i == 0 ? GE_D3D12_SAMPLER_NEAREST : GE_D3D12_SAMPLER_LINEAR;
        static_samplers[i].RegisterSpace = 0;
        static_samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    static_samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    static_samplers[1].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = (UINT)(sizeof(parameters) / sizeof(parameters[0]));
    desc.pParameters = parameters;
    desc.NumStaticSamplers =
        (UINT)(sizeof(static_samplers) / sizeof(static_samplers[0]));
    desc.pStaticSamplers = static_samplers;
    desc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ID3DBlob *serialized = nullptr;
    ID3DBlob *errors = nullptr;
    HRESULT serialize_hr = D3D12SerializeRootSignature(
        &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (errors && errors->GetBufferSize()) {
        fwrite(errors->GetBufferPointer(), 1, errors->GetBufferSize(), stderr);
        fputc('\n', stderr);
    }
    RELEASE_COM(errors);
    if (!check_hr(serialize_hr, "serialize GE root signature")) {
        RELEASE_COM(serialized);
        return 0;
    }
    HRESULT create_hr = s_device->CreateRootSignature(
        0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
        IID_PPV_ARGS(&s_root_signature));
    RELEASE_COM(serialized);
    return check_hr(create_hr, "create GE root signature");
}

static int create_frame_resources(void) {
    const UINT descriptors_per_frame = SRV_DESCRIPTOR_COUNT / FRAME_COUNT;
    const UINT samplers_per_frame = SAMPLER_DESCRIPTOR_COUNT / FRAME_COUNT;
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        if (!check_hr(s_device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s_frames[i].allocator)),
                "create frame command allocator")) {
            return 0;
        }
        s_frames[i].upload_begin = i * UPLOAD_BYTES_PER_FRAME;
        s_frames[i].upload_cursor = s_frames[i].upload_begin;
        s_frames[i].upload_end = s_frames[i].upload_begin + UPLOAD_BYTES_PER_FRAME;
        s_frames[i].srv_begin = i * descriptors_per_frame;
        s_frames[i].srv_cursor = s_frames[i].srv_begin;
        s_frames[i].srv_end = s_frames[i].srv_begin + descriptors_per_frame;
        s_frames[i].sampler_begin = i * samplers_per_frame;
        s_frames[i].sampler_cursor = s_frames[i].sampler_begin;
        s_frames[i].sampler_end = s_frames[i].sampler_begin + samplers_per_frame;
    }
    if (!check_hr(s_device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_frames[0].allocator, nullptr,
            IID_PPV_ARGS(&s_command_list)), "create graphics command list") ||
        !check_hr(s_command_list->Close(), "close initial graphics command list")) {
        return 0;
    }

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC resource = {};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = UPLOAD_BYTES;
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!check_hr(s_device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&s_upload)), "create upload ring")) {
        return 0;
    }
    D3D12_RANGE read_range = {0, 0};
    if (!check_hr(s_upload->Map(
            0, &read_range, reinterpret_cast<void **>(&s_upload_map)),
            "map upload ring")) {
        return 0;
    }
    return 1;
}

static int acquire_backbuffers(void) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        s_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        if (!check_hr(s_swapchain->GetBuffer(
                i, IID_PPV_ARGS(&s_backbuffers[i])), "get swapchain buffer")) {
            return 0;
        }
        s_device->CreateRenderTargetView(s_backbuffers[i], nullptr, rtv);
        rtv.ptr += s_rtv_stride;
    }
    D3D12_RESOURCE_DESC desc = s_backbuffers[0]->GetDesc();
    s_backbuffer_width = (UINT)desc.Width;
    s_backbuffer_height = desc.Height;
    return 1;
}

static int create_swapchain(IDXGIFactory4 *factory) {
    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(
            s_window, &pixel_width, &pixel_height) ||
        pixel_width <= 0 || pixel_height <= 0) {
        fprintf(stderr, "ged3d12: could not query SDL window pixel size: %s\n",
                SDL_GetError());
        return 0;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = (UINT)pixel_width;
    desc.Height = (UINT)pixel_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = FRAME_COUNT;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    IDXGISwapChain1 *swapchain1 = nullptr;
    if (!check_hr(factory->CreateSwapChainForHwnd(
            s_graphics_queue, s_hwnd, &desc, nullptr, nullptr, &swapchain1),
            "create HWND flip-model swapchain")) {
        return 0;
    }
    HRESULT query_hr = swapchain1->QueryInterface(IID_PPV_ARGS(&s_swapchain));
    RELEASE_COM(swapchain1);
    if (!check_hr(query_hr, "query IDXGISwapChain3") ||
        !check_hr(factory->MakeWindowAssociation(
            s_hwnd, DXGI_MWA_NO_ALT_ENTER), "disable DXGI Alt+Enter")) {
        return 0;
    }

    return acquire_backbuffers();
}

static int resize_swapchain(void) {
    int pixel_width = 0;
    int pixel_height = 0;
    if (!SDL_GetWindowSizeInPixels(
            s_window, &pixel_width, &pixel_height)) {
        fprintf(stderr, "ged3d12: could not query SDL window pixel size: %s\n",
                SDL_GetError());
        return 0;
    }
    if (pixel_width <= 0 || pixel_height <= 0 ||
        ((UINT)pixel_width == s_backbuffer_width &&
         (UINT)pixel_height == s_backbuffer_height)) {
        return 1;
    }
    if (!wait_queue_idle(
            s_graphics_queue, s_graphics_fence, &s_next_graphics_fence,
            s_graphics_event, "wait for swapchain resize")) {
        return 0;
    }
    for (UINT i = 0; i < FRAME_COUNT; i++) RELEASE_COM(s_backbuffers[i]);
    if (!check_hr(s_swapchain->ResizeBuffers(
            FRAME_COUNT, (UINT)pixel_width, (UINT)pixel_height,
            DXGI_FORMAT_B8G8R8A8_UNORM, 0), "resize swapchain")) {
        return 0;
    }
    if (!acquire_backbuffers()) return 0;
    fprintf(stderr, "ged3d12: resized swapchain to %ux%u\n",
            s_backbuffer_width, s_backbuffer_height);
    return 1;
}

static const char *feature_level_name(D3D_FEATURE_LEVEL level) {
    switch (level) {
#ifdef D3D_FEATURE_LEVEL_12_2
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
#endif
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    default: return "11_0";
    }
}

static int bind_default_root_state(void) {
    GeD3D12DescriptorSlice srvs = {};
    GeD3D12DescriptorSlice destination = {};
    GeD3D12DescriptorSlice sampler = {};
    if (!ged3d12_srv_alloc(2, &srvs) ||
        !ged3d12_srv_alloc(2, &destination) ||
        !ged3d12_sampler_alloc(1, &sampler)) {
        return 0;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC texture = {};
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    texture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    texture.Texture2D.MipLevels = 1;
    s_device->CreateShaderResourceView(nullptr, &texture, srvs.cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC clut = {};
    clut.Format = DXGI_FORMAT_R32_UINT;
    clut.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    clut.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    clut.Buffer.NumElements = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE clut_cpu = srvs.cpu;
    clut_cpu.ptr += s_srv_stride;
    s_device->CreateShaderResourceView(nullptr, &clut, clut_cpu);

    D3D12_SHADER_RESOURCE_VIEW_DESC destination_color = {};
    destination_color.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    destination_color.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    destination_color.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    destination_color.Texture2D.MipLevels = 1;
    s_device->CreateShaderResourceView(
        nullptr, &destination_color, destination.cpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC destination_depth = {};
    destination_depth.Format = DXGI_FORMAT_R16_UNORM;
    destination_depth.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    destination_depth.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    destination_depth.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE destination_depth_cpu = destination.cpu;
    destination_depth_cpu.ptr += s_srv_stride;
    s_device->CreateShaderResourceView(
        nullptr, &destination_depth, destination_depth_cpu);

    D3D12_SAMPLER_DESC sampler_desc = {};
    sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler_desc.MaxAnisotropy = 1;
    sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
    s_device->CreateSampler(&sampler_desc, sampler.cpu);

    UINT constants[GE_D3D12_SHADER_CONSTANT_DWORDS] = {};
    s_command_list->SetGraphicsRoot32BitConstants(
        GE_D3D12_ROOT_GE_STATE,
        GE_D3D12_SHADER_CONSTANT_DWORDS, constants, 0);
    s_command_list->SetGraphicsRootDescriptorTable(
        GE_D3D12_ROOT_TEXTURES, srvs.gpu);
    s_command_list->SetGraphicsRootDescriptorTable(
        GE_D3D12_ROOT_SAMPLER, sampler.gpu);
    s_command_list->SetGraphicsRootDescriptorTable(
        GE_D3D12_ROOT_DESTINATION, destination.gpu);
    return 1;
}

static int device_init(void) {
    if (!s_window || !s_hwnd) {
        fprintf(stderr, "ged3d12: SDL3 window must be initialized first\n");
        return 0;
    }
    if (s_device) {
        fprintf(stderr, "ged3d12: device already initialized\n");
        return 0;
    }

    UINT factory_flags = 0;
    if (!enable_debug_layer(&factory_flags)) return 0;
    IDXGIFactory4 *factory = nullptr;
    if (!check_hr(CreateDXGIFactory2(
            factory_flags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2")) {
        return 0;
    }

    DXGI_ADAPTER_DESC1 adapter_desc = {};
    int ok = choose_adapter(factory, &adapter_desc) &&
             create_queues_and_fences() &&
             create_descriptor_heaps() &&
             create_root_signature() &&
             create_frame_resources() &&
             create_swapchain(factory) &&
             ged3d12_present_init();
    RELEASE_COM(factory);
    if (!ok) {
        release_device();
        return 0;
    }
    if (!ged3d12_vertex_capture_init()) {
        release_device();
        return 0;
    }
    if (!ged3d12_framebuffer_cache_init()) {
        release_device();
        return 0;
    }
    if (!ged3d12_texture_cache_init()) {
        release_device();
        return 0;
    }
    if (!ged3d12_pipeline_cache_init()) {
        release_device();
        return 0;
    }
    s_vertex_log = env_enabled("SR_D3D12_VERTEX_LOG");
    s_gpu_log = env_enabled("SR_GPU_LOG");
    s_framebuffer_safe = env_enabled("SR_D3D12_FRAMEBUFFER_SAFE");
    s_destination_safe = env_enabled("SR_D3D12_DESTINATION_SAFE");
    s_state_dump_index = 0;
    s_gpu_log_flushes = 0;
    s_gpu_log_draws = 0;
    s_vertex_frames = 0;
    s_vertex_total = 0;
    s_vertex_batches = 0;
    s_constant_updates = 0;
    s_constant_reuses = 0;
    s_vertex_max = 0;
    memset(s_vertex_primitives, 0, sizeof(s_vertex_primitives));

    D3D_FEATURE_LEVEL requested_levels[] = {
#ifdef D3D_FEATURE_LEVEL_12_2
        D3D_FEATURE_LEVEL_12_2,
#endif
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_0,
    };
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels = {};
    levels.NumFeatureLevels = (UINT)(sizeof(requested_levels) / sizeof(requested_levels[0]));
    levels.pFeatureLevelsRequested = requested_levels;
    levels.MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    s_device->CheckFeatureSupport(
        D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels));

    char adapter_name[256] = {};
    WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1,
                        adapter_name, sizeof(adapter_name), nullptr, nullptr);
    fprintf(stderr,
            "ged3d12: using %s (feature level %s, %u buffers, "
            "descriptors=%u srv/%u sampler/%u rtv/%u dsv, "
            "root=b0[40]+t0-t3+s0+s1/s2+space1:t0, upload=%llu MiB, "
            "gamepad=%s)\n",
            adapter_name[0] ? adapter_name : "<unnamed adapter>",
            feature_level_name(levels.MaxSupportedFeatureLevel),
            FRAME_COUNT, SRV_DESCRIPTOR_COUNT, SAMPLER_DESCRIPTOR_COUNT,
            RTV_DESCRIPTOR_COUNT, DSV_DESCRIPTOR_COUNT,
            (unsigned long long)(UPLOAD_BYTES / (1024 * 1024)),
            s_pad ? "yes" : "no");
    return 1;
}

static int begin_frame(UINT *frame_index) {
    UINT index = s_swapchain->GetCurrentBackBufferIndex();
    FrameContext *frame = &s_frames[index];
    if (!wait_for_fence(s_graphics_fence, frame->fence_value, s_graphics_event,
                        "wait for frame fence")) {
        return 0;
    }
    release_upload_overflow(frame);
    frame->upload_cursor = frame->upload_begin;
    frame->srv_cursor = frame->srv_begin;
    frame->sampler_cursor = frame->sampler_begin;
    if (!check_hr(frame->allocator->Reset(), "reset frame command allocator") ||
        !check_hr(s_command_list->Reset(frame->allocator, nullptr),
                  "reset graphics command list")) {
        return 0;
    }
    ID3D12DescriptorHeap *heaps[] = {s_srv_heap, s_sampler_heap};
    s_command_list->SetDescriptorHeaps(
        (UINT)(sizeof(heaps) / sizeof(heaps[0])), heaps);
    s_command_list->SetGraphicsRootSignature(s_root_signature);
    s_active_frame = index;
    if (!bind_default_root_state()) {
        s_active_frame = UINT32_MAX;
        return 0;
    }
    *frame_index = index;
    return 1;
}

bool ged3d12_upload_alloc(UINT64 size, UINT64 alignment, GeD3D12UploadSlice *out) {
    if (!out || s_active_frame >= FRAME_COUNT || !size ||
        !alignment || (alignment & (alignment - 1)) != 0) {
        return false;
    }
    FrameContext *frame = &s_frames[s_active_frame];
    UINT64 offset = align_up(frame->upload_cursor, alignment);
    if (offset <= frame->upload_end && size <= frame->upload_end - offset) {
        frame->upload_cursor = offset + size;
        out->resource = s_upload;
        out->cpu = s_upload_map + offset;
        out->gpu = s_upload->GetGPUVirtualAddress() + offset;
        out->offset = offset;
        out->size = size;
        return true;
    }

    UploadSlab *slab = nullptr;
    if (frame->upload_overflow_count) {
        UploadSlab *candidate =
            &frame->upload_overflow[frame->upload_overflow_count - 1];
        offset = align_up(candidate->cursor, alignment);
        if (offset <= candidate->size && size <= candidate->size - offset)
            slab = candidate;
    }
    if (!slab) {
        if (frame->upload_overflow_count >= UPLOAD_OVERFLOW_SLAB_COUNT ||
            size > UINT64_MAX - (alignment - 1)) {
            fprintf(stderr,
                    "ged3d12: frame upload overflow exhausted "
                    "(%llu bytes requested)\n",
                    (unsigned long long)size);
            return false;
        }
        UINT64 slab_size = align_up(size, alignment);
        if (slab_size < UPLOAD_BYTES_PER_FRAME)
            slab_size = UPLOAD_BYTES_PER_FRAME;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap.CreationNodeMask = 1;
        heap.VisibleNodeMask = 1;
        D3D12_RESOURCE_DESC resource = {};
        resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resource.Width = slab_size;
        resource.Height = 1;
        resource.DepthOrArraySize = 1;
        resource.MipLevels = 1;
        resource.SampleDesc.Count = 1;
        resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        UploadSlab created = {};
        if (!check_hr(s_device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &resource,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&created.resource)),
                "create frame upload overflow")) {
            return false;
        }
        D3D12_RANGE read_range = {0, 0};
        if (!check_hr(created.resource->Map(
                0, &read_range, reinterpret_cast<void **>(&created.map)),
                "map frame upload overflow")) {
            RELEASE_COM(created.resource);
            return false;
        }
        created.size = slab_size;
        slab = &frame->upload_overflow[frame->upload_overflow_count++];
        *slab = created;
        offset = 0;
    }

    slab->cursor = offset + size;
    out->resource = slab->resource;
    out->cpu = slab->map + offset;
    out->gpu = slab->resource->GetGPUVirtualAddress() + offset;
    out->offset = offset;
    out->size = size;
    return true;
}

bool ged3d12_srv_alloc(UINT count, GeD3D12DescriptorSlice *out) {
    if (!out || s_active_frame >= FRAME_COUNT || !count) return false;
    FrameContext *frame = &s_frames[s_active_frame];
    if (count > frame->srv_end - frame->srv_cursor) {
        fprintf(stderr, "ged3d12: frame CBV/SRV/UAV descriptor ring exhausted "
                        "(%u descriptors requested)\n", count);
        return false;
    }
    UINT index = frame->srv_cursor;
    frame->srv_cursor += count;
    out->cpu = s_srv_heap->GetCPUDescriptorHandleForHeapStart();
    out->cpu.ptr += (SIZE_T)index * s_srv_stride;
    out->gpu = s_srv_heap->GetGPUDescriptorHandleForHeapStart();
    out->gpu.ptr += (UINT64)index * s_srv_stride;
    out->count = count;
    return true;
}

bool ged3d12_sampler_alloc(UINT count, GeD3D12DescriptorSlice *out) {
    if (!out || s_active_frame >= FRAME_COUNT || !count) return false;
    FrameContext *frame = &s_frames[s_active_frame];
    if (count > frame->sampler_end - frame->sampler_cursor) {
        fprintf(stderr, "ged3d12: frame sampler descriptor ring exhausted "
                        "(%u descriptors requested)\n", count);
        return false;
    }
    UINT index = frame->sampler_cursor;
    frame->sampler_cursor += count;
    out->cpu = s_sampler_heap->GetCPUDescriptorHandleForHeapStart();
    out->cpu.ptr += (SIZE_T)index * s_sampler_stride;
    out->gpu = s_sampler_heap->GetGPUDescriptorHandleForHeapStart();
    out->gpu.ptr += (UINT64)index * s_sampler_stride;
    out->count = count;
    return true;
}

static bool cpu_descriptor_alloc(ID3D12DescriptorHeap *heap, UINT stride,
                                 uint8_t *used, UINT count, UINT first,
                                 const char *name, GeD3D12CpuDescriptor *out) {
    if (!out || !heap) return false;
    for (UINT index = first; index < count; index++) {
        if (!used[index]) {
            used[index] = 1;
            out->cpu = heap->GetCPUDescriptorHandleForHeapStart();
            out->cpu.ptr += (SIZE_T)index * stride;
            out->index = index;
            return true;
        }
    }
    fprintf(stderr, "ged3d12: %s descriptor pool exhausted\n", name);
    return false;
}

static bool cpu_descriptor_free(uint8_t *used, UINT count, UINT first,
                                UINT index, const char *name) {
    if (index < first || index >= count || !used[index]) {
        fprintf(stderr, "ged3d12: invalid %s descriptor free (index %u)\n",
                name, index);
        return false;
    }
    used[index] = 0;
    return true;
}

bool ged3d12_rtv_alloc(GeD3D12CpuDescriptor *out) {
    return cpu_descriptor_alloc(
        s_rtv_heap, s_rtv_stride, s_rtv_used, RTV_DESCRIPTOR_COUNT,
        FRAME_COUNT, "RTV", out);
}

bool ged3d12_dsv_alloc(GeD3D12CpuDescriptor *out) {
    return cpu_descriptor_alloc(
        s_dsv_heap, s_dsv_stride, s_dsv_used, DSV_DESCRIPTOR_COUNT,
        0, "DSV", out);
}

bool ged3d12_rtv_free(UINT index) {
    return cpu_descriptor_free(
        s_rtv_used, RTV_DESCRIPTOR_COUNT, FRAME_COUNT, index, "RTV");
}

bool ged3d12_dsv_free(UINT index) {
    return cpu_descriptor_free(
        s_dsv_used, DSV_DESCRIPTOR_COUNT, 0, index, "DSV");
}

static void dump_ge_state_hotkey(void) {
    char path[128];
    snprintf(
        path, sizeof(path), "ge_state_f%05u_p%lu_%03u.txt",
        ge_get_frame(), (unsigned long)GetCurrentProcessId(),
        s_state_dump_index++);
    if (ge_debug_dump(path))
        fprintf(stderr, "ged3d12: F10 GE state dump -> %s\n", path);
    else
        fprintf(stderr, "ged3d12: F10 GE state dump failed\n");
}

static void poll_input(int *quit) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            *quit = 1;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.key == SDLK_ESCAPE) *quit = 1;
            if (event.key.key == SDLK_F10 && !event.key.repeat)
                dump_ge_state_hotkey();
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!s_pad) s_pad = SDL_OpenGamepad(event.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (s_pad && SDL_GetGamepadID(s_pad) == event.gdevice.which) {
                SDL_CloseGamepad(s_pad);
                s_pad = nullptr;
            }
            break;
        default:
            break;
        }
    }

    const bool *keys = SDL_GetKeyboardState(nullptr);
    s_pad_present = s_pad != nullptr;
    s_buttons = sr_input_buttons(
        sr_input_sdl_key_down, (void *)keys,
        s_pad ? sr_input_sdl_pad_down : nullptr, s_pad);
    int key_x = 0;
    int key_y = 0;
    sr_input_keyboard_axis(
        sr_input_sdl_key_down, (void *)keys, &key_x, &key_y);
    int pad_x =
        s_pad ? SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX) : 0;
    int pad_y =
        s_pad ? SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY) : 0;
    sr_input_analog(
        key_x || key_y ? key_x : pad_x,
        key_x || key_y ? key_y : pad_y,
        &s_lx, &s_ly);
}

static bool same_list(
    const GeD3D12VertexBatch *left, const GeD3D12VertexBatch *right) {
    return left->list_id == right->list_id &&
           left->list_addr == right->list_addr;
}

static bool same_draw(
    const GeD3D12VertexBatch *left, const GeD3D12VertexBatch *right) {
    return same_list(left, right) &&
           left->draw_serial == right->draw_serial;
}

static void log_draw_batches(
    const GeD3D12VertexFrame *frame, UINT first) {
    const GeD3D12VertexBatch *batch = &frame->batches[first];
    UINT end = first + 1;
    uint32_t vertices = batch->count;
    while (end < frame->batch_count &&
           same_draw(batch, &frame->batches[end])) {
        vertices += frame->batches[end].count;
        end++;
    }
    fprintf(
        stderr,
        "GED3D12 draw present=%llu list=%u@0x%08x draw=%u "
        "pc=0x%08x op=0x%08x batches=%u vertices=%u primitive=%s "
        "framebuffer=%u/%u texture=%u\n",
        (unsigned long long)(s_vertex_frames + 1),
        batch->list_id, batch->list_addr, batch->draw_serial,
        batch->draw_pc, batch->draw_op, end - first, vertices,
        primitive_name(batch->primitive),
        batch->framebuffer.color_handle, batch->framebuffer.depth_handle,
        batch->texture.handle);
    s_gpu_log_draws++;
}

static bool record_vertex_batches(const GeD3D12VertexFrame *frame) {
    const GeD3D12VertexBatch *open_list = nullptr;
    const GeD3D12VertexBatch *open_draw = nullptr;
    GeD3D12ShaderConstantCache constant_cache = {};
    bool ok = true;
    if (frame->vertex_count) {
        s_command_list->SetGraphicsRootShaderResourceView(
            GE_D3D12_ROOT_VERTICES, frame->buffer.BufferLocation);
        s_command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    for (UINT i = 0; i < frame->batch_count; i++) {
        const GeD3D12VertexBatch *batch = &frame->batches[i];
        if (!open_list || !same_list(open_list, batch)) {
            if (open_draw) {
                pix_end_event();
                open_draw = nullptr;
            }
            if (open_list) pix_end_event();
            char label[96];
            snprintf(
                label, sizeof(label), "GE list %u @ 0x%08X",
                batch->list_id, batch->list_addr);
            pix_begin_event(label);
            open_list = batch;
        }
        if (!open_draw || !same_draw(open_draw, batch)) {
            if (open_draw) pix_end_event();
            char label[128];
            snprintf(
                label, sizeof(label),
                "GE draw %u pc=0x%08X op=0x%08X",
                batch->draw_serial, batch->draw_pc, batch->draw_op);
            pix_begin_event(label);
            open_draw = batch;
            if (s_gpu_log) log_draw_batches(frame, i);
        }
        if (!ged3d12_pipeline_bind(&batch->pipeline) ||
            !ged3d12_framebuffer_bind(&batch->framebuffer) ||
            (s_destination_safe &&
             !ged3d12_framebuffer_bind_destination(
                 &batch->framebuffer)) ||
            !ged3d12_texture_bind(&batch->texture)) {
            ok = false;
            break;
        }
        if (ged3d12_shader_constants_bind(
                &constant_cache, &batch->shader)) {
            s_constant_updates++;
        } else {
            s_constant_reuses++;
        }
    }
    if (open_draw) pix_end_event();
    if (open_list) pix_end_event();
    return ok;
}

static void convert_framebuffer(uint8_t *destination, UINT row_pitch,
                                uint32_t fbaddr, int format, uint32_t stride) {
    for (int y = 0; y < PSP_H; y++) {
        uint32_t *row = reinterpret_cast<uint32_t *>(destination + (UINT64)y * row_pitch);
        for (int x = 0; x < PSP_W; x++) {
            uint32_t index = (uint32_t)y * stride + (uint32_t)x;
            int red, green, blue;
            if (format == 3) {
                uint32_t pixel = sr_r32(fbaddr + index * 4);
                red = pixel & 0xff;
                green = (pixel >> 8) & 0xff;
                blue = (pixel >> 16) & 0xff;
            } else {
                uint16_t pixel = sr_r16(fbaddr + index * 2);
                if (format == 1) {
                    red = (pixel & 0x1f) << 3;
                    green = ((pixel >> 5) & 0x1f) << 3;
                    blue = ((pixel >> 10) & 0x1f) << 3;
                } else if (format == 2) {
                    red = (pixel & 0xf) << 4;
                    green = ((pixel >> 4) & 0xf) << 4;
                    blue = ((pixel >> 8) & 0xf) << 4;
                } else {
                    red = (pixel & 0x1f) << 3;
                    green = ((pixel >> 5) & 0x3f) << 2;
                    blue = ((pixel >> 11) & 0x1f) << 3;
                }
            }
            row[x] = 0xff000000u | ((uint32_t)red << 16) |
                     ((uint32_t)green << 8) | (uint32_t)blue;
        }
    }
}

static int device_present(uint32_t fbaddr, int format, uint32_t stride) {
    int quit = 0;
    poll_input(&quit);
    if (quit) return 0;
    if (!resize_swapchain()) return -2;

    UINT frame_index;
    if (!begin_frame(&frame_index)) return -2;

    GeD3D12VertexFrame vertices = {};
    if (!ged3d12_vertex_upload_pending(&vertices)) return -2;
    if (!ged3d12_framebuffer_prepare_pending()) return -2;
    if (!ged3d12_texture_prepare_pending()) return -2;
    if (!record_vertex_batches(&vertices)) return -2;
    s_vertex_frames++;
    s_vertex_total += vertices.vertex_count;
    s_vertex_batches += vertices.batch_count;
    if (vertices.vertex_count > s_vertex_max) s_vertex_max = vertices.vertex_count;
    for (UINT i = 0; i < GE_D3D12_PRIMITIVE_COUNT; i++)
        s_vertex_primitives[i] += vertices.primitive_count[i];
    if (s_vertex_log && (s_vertex_frames <= 3 || s_vertex_frames % 120 == 0)) {
        fprintf(stderr,
                "ged3d12: vertex present=%llu vertices=%u batches=%u hash=%016llx "
                "max=%u constants=%llu/%llu\n",
                (unsigned long long)s_vertex_frames,
                vertices.vertex_count, vertices.batch_count,
                (unsigned long long)vertices.hash, s_vertex_max,
                (unsigned long long)s_constant_updates,
                (unsigned long long)s_constant_reuses);
    }
    if (s_gpu_log) {
        fprintf(
            stderr,
            "GED3D12 present=%llu frame=%u vertices=%u batches=%u "
            "hash=%016llx target=0x%08x/%u fmt=%d\n",
            (unsigned long long)s_vertex_frames, ge_get_frame(),
            vertices.vertex_count, vertices.batch_count,
            (unsigned long long)vertices.hash, fbaddr, stride, format);
    }

    D3D12_TEXTURE_COPY_LOCATION source = {};
    D3D12_BOX source_box = {};
    const D3D12_BOX *source_box_ptr = nullptr;
    bool cached_framebuffer = ged3d12_framebuffer_present_source(
        fbaddr, (uint32_t)format, stride, &source, &source_box);
    if (cached_framebuffer) {
        source_box_ptr = &source_box;
    } else {
        const UINT row_pitch =
            (PSP_W * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
        const UINT64 upload_size = (UINT64)row_pitch * PSP_H;
        GeD3D12UploadSlice upload = {};
        if (!ged3d12_upload_alloc(
                upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                &upload)) {
            return -2;
        }
        memset(upload.cpu, 0, (size_t)upload_size);
        convert_framebuffer(static_cast<uint8_t *>(upload.cpu), row_pitch,
                            fbaddr, format, stride);
        source.pResource = upload.resource;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = upload.offset;
        source.PlacedFootprint.Footprint.Format =
            DXGI_FORMAT_B8G8R8A8_UNORM;
        source.PlacedFootprint.Footprint.Width = PSP_W;
        source.PlacedFootprint.Footprint.Height = PSP_H;
        source.PlacedFootprint.Footprint.Depth = 1;
        source.PlacedFootprint.Footprint.RowPitch = row_pitch;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        s_rtv_heap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)frame_index * s_rtv_stride;
    if (!ged3d12_present_record(
            &source, source_box_ptr, s_backbuffers[frame_index], rtv,
            s_backbuffer_width, s_backbuffer_height)) {
        return -2;
    }
    const bool capture_requested = s_capture_path[0] != '\0';
    if (capture_requested && !ged3d12_present_capture_record()) return -2;
    if (!check_hr(s_command_list->Close(), "close graphics command list"))
        return -2;

    ID3D12CommandList *lists[] = {s_command_list};
    s_graphics_queue->ExecuteCommandLists(1, lists);
    HRESULT present_hr = s_swapchain->Present(0, 0);

    UINT64 fence_value = s_next_graphics_fence++;
    HRESULT signal_hr = s_graphics_queue->Signal(s_graphics_fence, fence_value);
    s_frames[frame_index].fence_value = fence_value;
    s_active_frame = UINT32_MAX;
    if (!check_hr(signal_hr, "signal frame fence")) return -2;
    if (FAILED(present_hr)) {
        check_hr(present_hr, "present swapchain");
        HRESULT removed = s_device->GetDeviceRemovedReason();
        if (FAILED(removed)) check_hr(removed, "D3D12 device removed reason");
        return -2;
    }
    if (capture_requested) {
        if (!wait_for_fence(s_graphics_fence, fence_value, s_graphics_event,
                            "wait for present capture") ||
            !ged3d12_present_capture_write(s_capture_path)) {
            return -2;
        }
        fprintf(stderr, "ged3d12: captured presented frame -> %s\n", s_capture_path);
        s_capture_path[0] = '\0';
    }
    if (s_framebuffer_safe && cached_framebuffer &&
        !ged3d12_framebuffer_safe_readback(
            fbaddr, (uint32_t)format, stride)) {
        fprintf(stderr, "ged3d12: safe framebuffer readback failed\n");
        return -2;
    }
    return 1;
}

static void device_shutdown(void) {
    if (s_vertex_log) {
        fprintf(stderr,
                "ged3d12: vertex capture presents=%llu vertices=%llu batches=%llu "
                "max=%u constants=%llu/%llu tri=%llu sprite=%llu "
                "line=%llu point=%llu\n",
                (unsigned long long)s_vertex_frames,
                (unsigned long long)s_vertex_total,
                (unsigned long long)s_vertex_batches,
                s_vertex_max,
                (unsigned long long)s_constant_updates,
                (unsigned long long)s_constant_reuses,
                (unsigned long long)s_vertex_primitives[GE_D3D12_PRIMITIVE_TRIANGLE],
                (unsigned long long)s_vertex_primitives[GE_D3D12_PRIMITIVE_SPRITE],
                (unsigned long long)s_vertex_primitives[GE_D3D12_PRIMITIVE_LINE],
                (unsigned long long)s_vertex_primitives[GE_D3D12_PRIMITIVE_POINT]);
    }
    if (s_gpu_log) {
        fprintf(
            stderr,
            "GED3D12 summary presents=%llu flushes=%llu draws=%llu "
            "batches=%llu vertices=%llu constants=%llu/%llu\n",
            (unsigned long long)s_vertex_frames,
            (unsigned long long)s_gpu_log_flushes,
            (unsigned long long)s_gpu_log_draws,
            (unsigned long long)s_vertex_batches,
            (unsigned long long)s_vertex_total,
            (unsigned long long)s_constant_updates,
            (unsigned long long)s_constant_reuses);
    }
    release_device();
    ged3d12_window_shutdown();
}

static void device_flush(const char *reason) {
    if (s_gpu_log) {
        uint32_t vertices = 0;
        uint32_t batches = 0;
        GeDebugContext context = ge_debug_context();
        ged3d12_vertex_pending_counts(&vertices, &batches);
        fprintf(
            stderr,
            "GED3D12 flush=%llu frame=%u reason=%s list=%u@0x%08x "
            "pending_batches=%u pending_vertices=%u\n",
            (unsigned long long)++s_gpu_log_flushes, context.frame,
            reason ? reason : "<none>", context.list_id,
            context.list_addr, batches, vertices);
    }
    ged3d12_framebuffer_flush(reason);
}

static void device_vram_dirty(uint32_t address, uint32_t bytes) {
    ged3d12_texture_invalidate(address, bytes);
    ged3d12_framebuffer_invalidate(address, bytes);
}

static void device_vram_read(uint32_t address, uint32_t bytes) {
    ged3d12_framebuffer_read(address, bytes);
}

static void device_vram_write(uint32_t address, uint32_t bytes) {
    ged3d12_framebuffer_read(address, bytes);
}

static const GeBackend k_backend = {
    "d3d12", device_init, device_present, device_shutdown,
    ged3d12_vertex_capture_tri, ged3d12_vertex_capture_sprite,
    ged3d12_vertex_capture_line, ged3d12_vertex_capture_point,
    device_flush, device_vram_dirty, device_vram_read,
    device_vram_write, ged3d12_framebuffer_transfer,
};

extern "C" const GeBackend *ged3d12_backend(void) { return &k_backend; }
extern "C" uint32_t ged3d12_buttons(void) { return s_buttons; }
extern "C" void ged3d12_analog(uint8_t *lx, uint8_t *ly) {
    if (lx) *lx = s_lx;
    if (ly) *ly = s_ly;
}
extern "C" int ged3d12_pad_present(void) { return s_pad_present; }
extern "C" int ged3d12_capture_next(const char *path) {
    if (!path || !path[0] || s_capture_path[0]) return 0;
    size_t length = strlen(path);
    if (length >= sizeof(s_capture_path)) {
        fprintf(stderr, "ged3d12: capture path is too long\n");
        return 0;
    }
    memcpy(s_capture_path, path, length + 1);
    return 1;
}

ID3D12Device *ged3d12_device(void) { return s_device; }
ID3D12GraphicsCommandList *ged3d12_command_list(void) { return s_command_list; }
ID3D12CommandQueue *ged3d12_graphics_queue(void) { return s_graphics_queue; }
ID3D12CommandQueue *ged3d12_copy_queue(void) { return s_copy_queue; }
ID3D12DescriptorHeap *ged3d12_srv_heap(void) { return s_srv_heap; }
ID3D12DescriptorHeap *ged3d12_sampler_heap(void) { return s_sampler_heap; }
ID3D12DescriptorHeap *ged3d12_rtv_heap(void) { return s_rtv_heap; }
ID3D12DescriptorHeap *ged3d12_dsv_heap(void) { return s_dsv_heap; }
ID3D12RootSignature *ged3d12_root_signature(void) { return s_root_signature; }
D3D12_SHADER_BYTECODE ged3d12_vertex_shader(void) {
    D3D12_SHADER_BYTECODE shader = {k_psp_vs_dxil, sizeof(k_psp_vs_dxil)};
    return shader;
}
D3D12_SHADER_BYTECODE ged3d12_pixel_shader(void) {
    D3D12_SHADER_BYTECODE shader = {k_psp_ps_dxil, sizeof(k_psp_ps_dxil)};
    return shader;
}
bool ged3d12_graphics_wait_idle(void) {
    return wait_queue_idle(
        s_graphics_queue, s_graphics_fence, &s_next_graphics_fence,
        s_graphics_event, "wait for D3D12 texture cache eviction") != 0;
}
