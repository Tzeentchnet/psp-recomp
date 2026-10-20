/* SDL3 window and D3D12 device foundation. */
#ifndef GE_D3D12_DEVICE_H
#define GE_D3D12_DEVICE_H

#include "../ge_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the SDL3 window whose Win32 HWND is consumed by backend->init(). */
int  ged3d12_window_init(const char *title);
void ged3d12_window_shutdown(void);

const GeBackend *ged3d12_backend(void);

uint32_t ged3d12_buttons(void);
void     ged3d12_analog(uint8_t *lx, uint8_t *ly);
int      ged3d12_pad_present(void);
int ged3d12_capture_next(const char *path);

#ifdef __cplusplus
}

#include <d3d12.h>

struct GeD3D12UploadSlice {
    ID3D12Resource *resource;
    void *cpu;
    D3D12_GPU_VIRTUAL_ADDRESS gpu;
    UINT64 offset;
    UINT64 size;
};

struct GeD3D12DescriptorSlice {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    UINT count;
};

struct GeD3D12CpuDescriptor {
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    UINT index;
};

enum GeD3D12RootParameter : UINT {
    GE_D3D12_ROOT_GE_STATE = 0,
    GE_D3D12_ROOT_TEXTURES = 1,
    GE_D3D12_ROOT_SAMPLER = 2,
    GE_D3D12_ROOT_DESTINATION = 3,
    GE_D3D12_ROOT_VERTICES = 4,
};

enum GeD3D12SamplerRegister : UINT {
    GE_D3D12_SAMPLER_DYNAMIC = 0,
    GE_D3D12_SAMPLER_NEAREST = 1,
    GE_D3D12_SAMPLER_LINEAR = 2,
};

ID3D12Device *ged3d12_device(void);
ID3D12GraphicsCommandList *ged3d12_command_list(void);
ID3D12CommandQueue *ged3d12_graphics_queue(void);
ID3D12CommandQueue *ged3d12_copy_queue(void);
ID3D12DescriptorHeap *ged3d12_srv_heap(void);
ID3D12DescriptorHeap *ged3d12_sampler_heap(void);
ID3D12DescriptorHeap *ged3d12_rtv_heap(void);
ID3D12DescriptorHeap *ged3d12_dsv_heap(void);
ID3D12RootSignature *ged3d12_root_signature(void);
D3D12_SHADER_BYTECODE ged3d12_vertex_shader(void);
D3D12_SHADER_BYTECODE ged3d12_pixel_shader(void);
bool ged3d12_graphics_wait_idle(void);

/* Allocations are valid until the current frame slot is reused after its fence completes. */
bool ged3d12_upload_alloc(UINT64 size, UINT64 alignment, GeD3D12UploadSlice *out);
bool ged3d12_srv_alloc(UINT count, GeD3D12DescriptorSlice *out);
bool ged3d12_sampler_alloc(UINT count, GeD3D12DescriptorSlice *out);
bool ged3d12_rtv_alloc(GeD3D12CpuDescriptor *out);
bool ged3d12_dsv_alloc(GeD3D12CpuDescriptor *out);
bool ged3d12_rtv_free(UINT index);
bool ged3d12_dsv_free(UINT index);
#endif

#endif
