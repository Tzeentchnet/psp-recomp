/* Persistent PSP framebuffer/depth identity and D3D12 resource cache. */
#ifndef GE_D3D12_FRAMEBUFFER_H
#define GE_D3D12_FRAMEBUFFER_H

#include "../ge_shared.h"

#include <d3d12.h>
#include <stdint.h>

#define GE_D3D12_FRAMEBUFFER_WIDTH 512u
#define GE_D3D12_FRAMEBUFFER_HEIGHT 272u
#define GE_D3D12_FRAMEBUFFER_TEXTURE_HEIGHT 512u
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
    uint16_t draw_left;
    uint16_t draw_top;
    uint16_t draw_right;
    uint16_t draw_bottom;
};

struct GeD3D12FramebufferAlias {
    uint32_t handle;
    uint16_t x;
    uint16_t y;
    uint16_t snapshot_left;
    uint16_t snapshot_top;
    uint16_t snapshot_right;
    uint16_t snapshot_bottom;
};

struct GeD3D12UploadSlice;

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
bool ged3d12_framebuffer_feedback_alias(
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
bool ged3d12_transfer_scale_coordinates(
    uint32_t scale, D3D12_BOX *source_box,
    UINT *destination_x, UINT *destination_y);

bool ged3d12_framebuffer_configure(uint32_t scale);
uint32_t ged3d12_framebuffer_scale(void);
bool ged3d12_framebuffer_cache_init(void);
void ged3d12_framebuffer_cache_shutdown(void);
bool ged3d12_framebuffer_capture(GeD3D12FramebufferBinding *out);
bool ged3d12_framebuffer_prepare_pending(void);
bool ged3d12_framebuffer_bind(const GeD3D12FramebufferBinding *binding);
bool ged3d12_framebuffer_bind_destination(
    const GeD3D12FramebufferBinding *binding);
bool ged3d12_framebuffer_mark_rendered(
    const GeD3D12FramebufferBinding *binding);
bool ged3d12_framebuffer_texture_alias(
    const GeState *state, GeD3D12FramebufferAlias *out);
bool ged3d12_framebuffer_texture_feedback(
    const GeState *state, GeD3D12FramebufferAlias *out,
    uint8_t *source_format);
bool ged3d12_framebuffer_texture_bind(
    const GeD3D12FramebufferAlias *alias,
    const GeD3D12UploadSlice *clut,
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
void ged3d12_framebuffer_get_stats(
    uint64_t *readbacks, uint64_t *aliases,
    uint64_t *feedback_snapshots, uint64_t *feedback_snapshot_pixels,
    uint64_t *feedback_reuses);

static_assert(
    sizeof(GeD3D12FramebufferKey) == 16,
    "D3D12 framebuffer key must remain canonical");
static_assert(
    sizeof(GeD3D12FramebufferBinding) == 16,
    "D3D12 framebuffer binding ABI changed");
static_assert(
    sizeof(GeD3D12FramebufferAlias) == 16,
    "D3D12 framebuffer alias ABI changed");
static_assert(
    sizeof(GeD3D12Transfer) == 32,
    "D3D12 transfer ABI changed");

#endif
