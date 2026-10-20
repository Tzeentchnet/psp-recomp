/* Persistent PSP framebuffer/depth identity and D3D12 resource cache. */
#ifndef GE_D3D12_FRAMEBUFFER_H
#define GE_D3D12_FRAMEBUFFER_H

#include "../ge_shared.h"

#include <d3d12.h>
#include <stdint.h>

#define GE_D3D12_FRAMEBUFFER_WIDTH 512u
#define GE_D3D12_FRAMEBUFFER_HEIGHT 272u
#define GE_D3D12_FRAMEBUFFER_HANDLE_NONE UINT32_MAX

struct GeD3D12FramebufferKey {
    uint32_t address;
    uint32_t depth_address;
    uint16_t stride;
    uint16_t depth_stride;
    uint8_t format;
    uint8_t reserved[3];
};

struct GeD3D12FramebufferBinding {
    uint32_t color_handle;
    uint32_t depth_handle;
};

struct GeD3D12FramebufferAlias {
    uint32_t handle;
    uint16_t x;
    uint16_t y;
};

struct GeD3D12Transfer {
    uint32_t source_address;
    uint32_t destination_address;
    uint16_t source_stride;
    uint16_t destination_stride;
    uint16_t source_x;
    uint16_t source_y;
    uint16_t destination_x;
    uint16_t destination_y;
    uint16_t width;
    uint16_t height;
    uint8_t bytes_per_pixel;
    uint8_t reserved[7];
};

bool ged3d12_framebuffer_key_build(
    const GeState *state, GeD3D12FramebufferKey *out);
uint64_t ged3d12_framebuffer_key_bytes(
    const GeD3D12FramebufferKey *key);
bool ged3d12_framebuffer_key_overlaps(
    const GeD3D12FramebufferKey *key, uint32_t address, uint32_t bytes);
uint32_t ged3d12_framebuffer_decode_pixel(
    uint32_t pixel, uint32_t format);
uint32_t ged3d12_framebuffer_encode_pixel(
    uint32_t pixel, uint32_t format);
bool ged3d12_framebuffer_alias(
    const GeD3D12FramebufferKey *target,
    uint32_t texture_address, uint32_t texture_stride,
    uint32_t texture_width, uint32_t texture_height,
    uint32_t texture_format, bool swizzled,
    GeD3D12FramebufferAlias *out);
bool ged3d12_transfer_build(
    const GeState *state, uint32_t start_data, GeD3D12Transfer *out);
bool ged3d12_transfer_coordinates(
    const GeD3D12Transfer *transfer,
    const GeD3D12FramebufferKey *source,
    const GeD3D12FramebufferKey *destination,
    D3D12_BOX *source_box, UINT *destination_x, UINT *destination_y);

bool ged3d12_framebuffer_cache_init(void);
void ged3d12_framebuffer_cache_shutdown(void);
bool ged3d12_framebuffer_capture(GeD3D12FramebufferBinding *out);
bool ged3d12_framebuffer_prepare_pending(void);
bool ged3d12_framebuffer_bind(const GeD3D12FramebufferBinding *binding);
bool ged3d12_framebuffer_mark_rendered(
    const GeD3D12FramebufferBinding *binding);
bool ged3d12_framebuffer_texture_alias(
    const GeState *state, GeD3D12FramebufferAlias *out);
bool ged3d12_framebuffer_texture_bind(
    const GeD3D12FramebufferAlias *alias,
    D3D12_GPU_DESCRIPTOR_HANDLE *descriptors);
bool ged3d12_framebuffer_present_source(
    uint32_t address, uint32_t format, uint32_t stride,
    D3D12_TEXTURE_COPY_LOCATION *source, D3D12_BOX *source_box);
bool ged3d12_framebuffer_safe_readback(
    uint32_t address, uint32_t format, uint32_t stride);
void ged3d12_framebuffer_invalidate(uint32_t address, uint32_t bytes);
void ged3d12_framebuffer_read(uint32_t address, uint32_t bytes);
int ged3d12_framebuffer_transfer(uint32_t start_data);
void ged3d12_framebuffer_flush(const char *reason);

static_assert(
    sizeof(GeD3D12FramebufferKey) == 16,
    "D3D12 framebuffer key must remain canonical");
static_assert(
    sizeof(GeD3D12FramebufferBinding) == 8,
    "D3D12 framebuffer binding ABI changed");
static_assert(
    sizeof(GeD3D12FramebufferAlias) == 8,
    "D3D12 framebuffer alias ABI changed");
static_assert(
    sizeof(GeD3D12Transfer) == 32,
    "D3D12 transfer ABI changed");

#endif
