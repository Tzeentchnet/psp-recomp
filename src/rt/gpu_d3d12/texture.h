/* PSP texture identity, decode capture, and D3D12 resource cache. */
#ifndef GE_D3D12_TEXTURE_H
#define GE_D3D12_TEXTURE_H

#include "../ge_shared.h"
#include "framebuffer.h"

#include <d3d12.h>
#include <stdint.h>
#include <string.h>


/* Fast non-cryptographic content hash: four independent multiply lanes over 8-byte words.
 * Used for texture/CLUT identity, where equal content must hash equally within one run. */
static inline uint64_t ged3d12_hash64(
    uint64_t seed, const void *data, size_t size) {
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    const uint64_t k0 = UINT64_C(0x9e3779b97f4a7c15);
    const uint64_t k1 = UINT64_C(0xc2b2ae3d27d4eb4f);
    uint64_t a = seed ^ k0, b = seed + k1, c = seed ^ k1, d = seed + k0;
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        uint64_t w[4];
        memcpy(w, bytes + i, sizeof(w));
        a = (a ^ w[0]) * k0; a ^= a >> 29;
        b = (b ^ w[1]) * k1; b ^= b >> 29;
        c = (c ^ w[2]) * k0; c ^= c >> 29;
        d = (d ^ w[3]) * k1; d ^= d >> 29;
    }
    uint64_t hash = a ^ (b * 3) ^ (c * 5) ^ (d * 7) ^ size;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        memcpy(&word, bytes + i, sizeof(word));
        hash = (hash ^ word) * k0;
        hash ^= hash >> 31;
    }
    for (; i < size; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    hash ^= hash >> 32;
    hash *= k1;
    hash ^= hash >> 29;
    return hash;
}

#define GE_D3D12_TEXTURE_LEVELS 8
#define GE_D3D12_TEXTURE_HANDLE_NONE UINT32_MAX
#define GE_D3D12_CLUT_HANDLE_NONE UINT16_MAX

enum GeD3D12TextureBindingFlag : uint8_t {
    GE_D3D12_TEXTURE_FRAMEBUFFER_CLUT = 1u,
    GE_D3D12_TEXTURE_UPSCALED = 2u,
    GE_D3D12_TEXTURE_FRAMEBUFFER_FORMAT_SHIFT = 2u,
    GE_D3D12_TEXTURE_FRAMEBUFFER_FORMAT_MASK =
        3u << GE_D3D12_TEXTURE_FRAMEBUFFER_FORMAT_SHIFT,
};

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
    uint32_t address;
    uint16_t width;
    uint16_t height;
    uint16_t clut_handle;
    uint8_t format;
    uint8_t flags;
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
    sizeof(GeD3D12TextureBinding) == 40,
    "D3D12 texture binding ABI changed");

#endif
