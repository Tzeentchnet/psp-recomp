#include "texture.h"

#include <string.h>

static void texture_level(
    const GeState *state, uint32_t level,
    uint32_t *address, uint32_t *stride, int *width, int *height) {
    if (level == 0) {
        *address = state->tex_addr;
        *stride = ge_texture_buffer_width(
            state->tex_fmt, state->tex_bufw);
        *width = state->tex_w;
        *height = state->tex_h;
    } else {
        *address = state->tex_addr_levels[level];
        *stride = ge_texture_buffer_width(
            state->tex_fmt, state->tex_bufw_levels[level]);
        *width = state->tex_width_levels[level];
        *height = state->tex_height_levels[level];
    }
}

void ged3d12_texture_key_build(
    const GeState *state, GeD3D12TextureKey *out) {
    memset(out, 0, sizeof(*out));
    if (!state) return;
    out->format = (uint8_t)(state->tex_fmt & 15u);
    out->swizzled = state->tex_swizzle != 0;
    out->clut_shared = (state->tex_mode & 0x100u) == 0;

    uint32_t requested_levels =
        (state->tex_filter & 4u) != 0
            ? (uint32_t)state->tex_max_level + 1u : 1u;
    if (requested_levels > GE_D3D12_TEXTURE_LEVELS)
        requested_levels = GE_D3D12_TEXTURE_LEVELS;
    for (uint32_t level = 0; level < requested_levels; level++) {
        uint32_t address;
        uint32_t stride;
        int width;
        int height;
        texture_level(
            state, level, &address, &stride, &width, &height);
        if (!address || !stride || width <= 0 || height <= 0 ||
            stride > UINT16_MAX || width > UINT16_MAX ||
            height > UINT16_MAX) {
            break;
        }
        if (level > 0) {
            uint32_t expected_width = out->width[0] >> level;
            uint32_t expected_height = out->height[0] >> level;
            if (!expected_width) expected_width = 1;
            if (!expected_height) expected_height = 1;
            if ((uint32_t)width != expected_width ||
                (uint32_t)height != expected_height) {
                break;
            }
        }
        out->address[level] = address;
        out->stride[level] = (uint16_t)stride;
        out->width[level] = (uint16_t)width;
        out->height[level] = (uint16_t)height;
        out->mip_count = (uint8_t)(level + 1u);
        if (level == 0) {
            uint32_t dimension =
                (uint32_t)(width > height ? width : height);
            uint32_t possible_levels = 1;
            while (dimension > 1) {
                dimension >>= 1;
                possible_levels++;
            }
            if (requested_levels > possible_levels)
                requested_levels = possible_levels;
        }
    }

    if (out->format >= 4 && out->format <= 7) {
        uint64_t hash = UINT64_C(14695981039346656037);
        hash = ged3d12_hash64(hash, state->clutram, sizeof(state->clutram));
        hash = ged3d12_hash64(hash, &state->clut_fmt, sizeof(state->clut_fmt));
        hash = ged3d12_hash64(hash, &out->clut_shared, sizeof(out->clut_shared));
        out->clut_hash = hash;
        out->clut_format = state->clut_fmt;
    }
}

uint64_t ged3d12_texture_key_hash(const GeD3D12TextureKey *key) {
    if (!key) return 0;
    return ged3d12_hash64(
        UINT64_C(14695981039346656037), key, sizeof(*key));
}

uint64_t ged3d12_texture_key_level_bytes(
    const GeD3D12TextureKey *key, uint32_t level) {
    if (!key || level >= key->mip_count ||
        level >= GE_D3D12_TEXTURE_LEVELS) {
        return 0;
    }
    uint32_t format = key->format;
    uint32_t bits = ge_texture_bits_per_pixel(format);
    if (!bits || !key->stride[level] ||
        !key->width[level] || !key->height[level]) {
        return 0;
    }
    if (format >= 8 && format <= 10) {
        uint64_t blocks_x = ((uint64_t)key->stride[level] + 3u) / 4u;
        uint64_t blocks_y = ((uint64_t)key->height[level] + 3u) / 4u;
        return blocks_x * blocks_y * (format == 8 ? 8u : 16u);
    }
    uint64_t rows = key->swizzled
        ? ((uint64_t)key->height[level] + 7u) & ~UINT64_C(7)
        : (uint64_t)key->height[level];
    return ((uint64_t)key->stride[level] * rows * bits + 7u) >> 3;
}

bool ged3d12_texture_key_overlaps(
    const GeD3D12TextureKey *key, uint32_t address, uint32_t bytes) {
    if (!key || !bytes) return false;
    uint64_t dirty_begin = address & 0x1fffffffu;
    uint64_t dirty_end = dirty_begin + bytes;
    for (uint32_t level = 0; level < key->mip_count; level++) {
        uint64_t size = ged3d12_texture_key_level_bytes(key, level);
        uint64_t texture_begin = key->address[level] & 0x1fffffffu;
        uint64_t texture_end = texture_begin + size;
        if (dirty_begin < texture_end && texture_begin < dirty_end)
            return true;
    }
    return false;
}

void ged3d12_texture_sampler_build(
    const GeState *state, GeD3D12TextureSampler *out) {
    memset(out, 0, sizeof(*out));
    if (!state) return;
    uint32_t min_filter = state->tex_filter & 7u;
    out->min_linear = (uint8_t)(min_filter & 1u);
    out->mag_linear = (uint8_t)((state->tex_filter >> 8) & 1u);
    out->mip_linear = (uint8_t)((min_filter >> 1) & 1u);
    out->mip_enabled =
        (uint8_t)(((min_filter & 4u) != 0) && state->tex_max_level > 0);
    out->clamp_u = (uint8_t)(state->tex_wrap & 1u);
    out->clamp_v = (uint8_t)((state->tex_wrap >> 8) & 1u);
    out->lod_bias_16 = (int8_t)((state->tex_level >> 16) & 0xffu);
}
