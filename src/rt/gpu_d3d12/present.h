/* Aspect-correct display-texture presentation. */
#ifndef GE_D3D12_PRESENT_H
#define GE_D3D12_PRESENT_H

#include <d3d12.h>

struct GeD3D12PresentViewport {
    D3D12_VIEWPORT viewport;
    D3D12_RECT scissor;
};

void ged3d12_present_viewport(
    UINT output_width, UINT output_height, GeD3D12PresentViewport *out);

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
bool ged3d12_present_capture_write(const char *path);

#endif
