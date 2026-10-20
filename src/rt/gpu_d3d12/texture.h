/* PSP texture identity, decode capture, and D3D12 resource cache. */
#ifndef GE_D3D12_TEXTURE_H
#define GE_D3D12_TEXTURE_H

#include "../ge_shared.h"
#include "framebuffer.h"

#include <d3d12.h>
#include <stdint.h>

#define GE_D3D12_TEXTURE_LEVELS 8
#define GE_D3D12_TEXTURE_HANDLE_NONE UINT32_MAX

struct GeD3D12TextureKey {
    uint32_t address[GE_D3D12_TEXTURE_LEVELS];
    uint16_t stride[GE_D3D12_TEXTURE_LEVELS];
    uint16_t width[GE_D3D12_TEXTURE_LEVELS];
    uint16_t height[GE_D3D12_TEXTURE_LEVELS];
    uint64_t clut_hash;
    uint32_t clut_format;
    uint8_t format;
    uint8_t swizzled;
    uint8_t mip_count;
    uint8_t clut_shared;
};

struct GeD3D12TextureSampler {
    uint8_t min_linear;
    uint8_t mag_linear;
    uint8_t mip_linear;
    uint8_t mip_enabled;
    uint8_t clamp_u;
    uint8_t clamp_v;
    int8_t lod_bias_16;
    uint8_t reserved;
};

struct GeD3D12TextureBinding {
    uint32_t handle;
    GeD3D12FramebufferAlias framebuffer;
    GeD3D12TextureSampler sampler;
};

void ged3d12_texture_key_build(
    const GeState *state, GeD3D12TextureKey *out);
uint64_t ged3d12_texture_key_hash(const GeD3D12TextureKey *key);
uint64_t ged3d12_texture_key_level_bytes(
    const GeD3D12TextureKey *key, uint32_t level);
bool ged3d12_texture_key_overlaps(
    const GeD3D12TextureKey *key, uint32_t address, uint32_t bytes);
void ged3d12_texture_sampler_build(
    const GeState *state, GeD3D12TextureSampler *out);

bool ged3d12_texture_cache_init(void);
void ged3d12_texture_cache_shutdown(void);
bool ged3d12_texture_capture(GeD3D12TextureBinding *out);
bool ged3d12_texture_prepare_pending(void);
bool ged3d12_texture_bind(const GeD3D12TextureBinding *binding);
void ged3d12_texture_invalidate(uint32_t address, uint32_t bytes);
void ged3d12_texture_invalidate_all(void);

static_assert(
    sizeof(GeD3D12TextureKey) == 96,
    "D3D12 texture key must remain canonical");
static_assert(
    sizeof(GeD3D12TextureSampler) == 8,
    "D3D12 texture sampler key must remain canonical");
static_assert(
    sizeof(GeD3D12TextureBinding) == 20,
    "D3D12 texture binding ABI changed");

#endif
