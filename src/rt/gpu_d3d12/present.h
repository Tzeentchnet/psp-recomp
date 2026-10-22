/* Aspect-correct display-texture presentation. */
#ifndef GE_D3D12_PRESENT_H
#define GE_D3D12_PRESENT_H

#include <d3d12.h>
#include <stdint.h>

struct GeD3D12PresentViewport {
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
};

struct GeD3D12CaptureLayout {
    UINT width;
    UINT height;
    UINT source_step;
    UINT sample_offset;
};

void ged3d12_present_viewport(
    UINT output_width, UINT output_height, GeD3D12PresentViewport *out);
void ged3d12_present_capture_layout(
    UINT render_scale, bool internal_resolution,
    GeD3D12CaptureLayout *out);
uint32_t ged3d12_present_capture_rgb(
    uint32_t rgb, uint32_t format);

bool ged3d12_present_init(void);
void ged3d12_present_shutdown(void);
bool ged3d12_present_record(
    const D3D12_TEXTURE_COPY_LOCATION *source,
    const D3D12_BOX *source_box,
    ID3D12Resource *backbuffer,
    D3D12_CPU_DESCRIPTOR_HANDLE backbuffer_rtv,
    UINT output_width,
    UINT output_height);
bool ged3d12_present_capture_record(void);
bool ged3d12_present_capture_write(
    const char *path, uint32_t format);

#endif
