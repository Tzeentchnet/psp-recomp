#include "framebuffer.h"

#include <string.h>

static uint32_t framebuffer_address(uint32_t raw) {
    return 0x04000000u | (raw & 0x001fffffu);
}

static uint32_t framebuffer_stride(uint32_t raw) {
    uint32_t stride = raw & 0x7ffu;
    return stride ? stride : GE_D3D12_FRAMEBUFFER_WIDTH;
}

static uint32_t framebuffer_bytes_per_pixel(uint32_t format) {
    return (format & 3u) == 3u ? 4u : 2u;
}

bool ged3d12_framebuffer_key_build(
    const GeState *state, GeD3D12FramebufferKey *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!state) return false;
    uint32_t stride = framebuffer_stride(state->fbw);
    uint32_t depth_stride =
        state->zbw ? framebuffer_stride(state->zbw) : stride;
    if (stride > GE_D3D12_FRAMEBUFFER_WIDTH ||
        depth_stride > GE_D3D12_FRAMEBUFFER_WIDTH) {
        return false;
    }
    out->address = framebuffer_address(state->fbp);
    out->depth_address = framebuffer_address(state->zbp);
    out->stride = (uint16_t)stride;
    out->depth_stride = (uint16_t)depth_stride;
    out->format = (uint8_t)(state->fbfmt & 3u);
    uint64_t color_end =
        (uint64_t)out->address +
        (uint64_t)stride * GE_D3D12_FRAMEBUFFER_HEIGHT *
            framebuffer_bytes_per_pixel(out->format);
    uint64_t depth_end =
        (uint64_t)out->depth_address +
        (uint64_t)depth_stride * GE_D3D12_FRAMEBUFFER_HEIGHT * 2u;
    if (color_end > UINT64_C(0x04200000) ||
        depth_end > UINT64_C(0x04200000)) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    return true;
}

uint64_t ged3d12_framebuffer_key_bytes(
    const GeD3D12FramebufferKey *key) {
    if (!key || !key->stride || key->stride > GE_D3D12_FRAMEBUFFER_WIDTH ||
        key->format > 3u) {
        return 0;
    }
    return (uint64_t)key->stride * GE_D3D12_FRAMEBUFFER_HEIGHT *
           framebuffer_bytes_per_pixel(key->format);
}

bool ged3d12_framebuffer_key_overlaps(
    const GeD3D12FramebufferKey *key, uint32_t address, uint32_t bytes) {
    uint64_t target_size = ged3d12_framebuffer_key_bytes(key);
    if (!target_size || !bytes) return false;
    uint64_t target_begin = key->address & 0x1fffffffu;
    uint64_t target_end = target_begin + target_size;
    uint64_t dirty_begin = address & 0x1fffffffu;
    uint64_t dirty_end = dirty_begin + bytes;
    return dirty_begin < target_end && target_begin < dirty_end;
}

uint32_t ged3d12_framebuffer_decode_pixel(
    uint32_t pixel, uint32_t format) {
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    uint32_t alpha;
    switch (format & 3u) {
        case 0:
            red = (pixel & 0x1fu) * 255u / 31u;
            green = ((pixel >> 5) & 0x3fu) * 255u / 63u;
            blue = ((pixel >> 11) & 0x1fu) * 255u / 31u;
            alpha = 255;
            break;
        case 1:
            red = (pixel & 0x1fu) * 255u / 31u;
            green = ((pixel >> 5) & 0x1fu) * 255u / 31u;
            blue = ((pixel >> 10) & 0x1fu) * 255u / 31u;
            alpha = (pixel & 0x8000u) ? 255u : 0u;
            break;
        case 2:
            red = (pixel & 0x0fu) * 17u;
            green = ((pixel >> 4) & 0x0fu) * 17u;
            blue = ((pixel >> 8) & 0x0fu) * 17u;
            alpha = ((pixel >> 12) & 0x0fu) * 17u;
            break;
        default:
            red = pixel & 0xffu;
            green = (pixel >> 8) & 0xffu;
            blue = (pixel >> 16) & 0xffu;
            alpha = pixel >> 24;
            break;
    }
    return blue | (green << 8) | (red << 16) | (alpha << 24);
}

uint32_t ged3d12_framebuffer_encode_pixel(
    uint32_t pixel, uint32_t format) {
    uint32_t blue = pixel & 0xffu;
    uint32_t green = (pixel >> 8) & 0xffu;
    uint32_t red = (pixel >> 16) & 0xffu;
    uint32_t alpha = pixel >> 24;
    switch (format & 3u) {
        case 0:
            return (red >> 3) | ((green >> 2) << 5) |
                   ((blue >> 3) << 11);
        case 1:
            return (red >> 3) | ((green >> 3) << 5) |
                   ((blue >> 3) << 10) |
                   (alpha >= 128u ? 0x8000u : 0u);
        case 2:
            return (red >> 4) | ((green >> 4) << 4) |
                   ((blue >> 4) << 8) | ((alpha >> 4) << 12);
        default:
            return (alpha << 24) | (blue << 16) |
                   (green << 8) | red;
    }
}

bool ged3d12_framebuffer_alias(
    const GeD3D12FramebufferKey *target,
    uint32_t texture_address, uint32_t texture_stride,
    uint32_t texture_width, uint32_t texture_height,
    uint32_t texture_format, bool swizzled,
    GeD3D12FramebufferAlias *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    if (!target || swizzled || texture_format != target->format ||
        texture_stride != target->stride || !texture_width || !texture_height) {
        return false;
    }
    uint32_t bytes_per_pixel = framebuffer_bytes_per_pixel(target->format);
    uint32_t target_address = target->address & 0x1fffffffu;
    uint32_t source_address = texture_address & 0x1fffffffu;
    uint64_t target_size = ged3d12_framebuffer_key_bytes(target);
    if (source_address < target_address ||
        source_address >= (uint64_t)target_address + target_size) {
        return false;
    }
    uint32_t byte_offset = source_address - target_address;
    if (byte_offset % bytes_per_pixel) return false;
    uint32_t pixel_offset = byte_offset / bytes_per_pixel;
    uint32_t x = pixel_offset % target->stride;
    uint32_t y = pixel_offset / target->stride;
    if (x + texture_width > target->stride ||
        y + texture_height > GE_D3D12_FRAMEBUFFER_HEIGHT) {
        return false;
    }
    out->x = (uint16_t)x;
    out->y = (uint16_t)y;
    return true;
}

bool ged3d12_transfer_build(
    const GeState *state, uint32_t start_data, GeD3D12Transfer *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (!state) return false;
    uint32_t source_stride = state->xf_srcw & 0x7f8u;
    uint32_t destination_stride = state->xf_dstw & 0x7f8u;
    if (!source_stride || !destination_stride ||
        source_stride > 0x400u || destination_stride > 0x400u) {
        return false;
    }
    out->source_address =
        (state->xf_src & 0xfffff0u) | ((state->xf_srcw & 0xff0000u) << 8);
    out->destination_address =
        (state->xf_dst & 0xfffff0u) | ((state->xf_dstw & 0xff0000u) << 8);
    out->source_stride = (uint16_t)source_stride;
    out->destination_stride = (uint16_t)destination_stride;
    out->source_x = (uint16_t)(state->xf_spos & 0x3ffu);
    out->source_y = (uint16_t)((state->xf_spos >> 10) & 0x3ffu);
    out->destination_x = (uint16_t)(state->xf_dpos & 0x3ffu);
    out->destination_y = (uint16_t)((state->xf_dpos >> 10) & 0x3ffu);
    out->width = (uint16_t)((state->xf_size & 0x3ffu) + 1u);
    out->height = (uint16_t)(((state->xf_size >> 10) & 0x3ffu) + 1u);
    out->bytes_per_pixel = (uint8_t)((start_data & 1u) ? 4u : 2u);
    return true;
}

static bool transfer_origin(
    uint32_t address, uint32_t stride, uint32_t x, uint32_t y,
    uint32_t bytes_per_pixel, const GeD3D12FramebufferKey *target,
    uint32_t *out_x, uint32_t *out_y) {
    uint32_t target_bpp = framebuffer_bytes_per_pixel(target->format);
    uint32_t target_address = target->address & 0x1fffffffu;
    uint32_t transfer_address = address & 0x1fffffffu;
    uint64_t target_size = ged3d12_framebuffer_key_bytes(target);
    if (target_bpp != bytes_per_pixel || target->stride != stride ||
        transfer_address < target_address ||
        transfer_address >= (uint64_t)target_address + target_size) {
        return false;
    }
    uint32_t byte_offset = transfer_address - target_address;
    if (byte_offset % bytes_per_pixel) return false;
    uint32_t pixel_offset = byte_offset / bytes_per_pixel;
    *out_x = x + pixel_offset % target->stride;
    *out_y = y + pixel_offset / target->stride;
    return true;
}

bool ged3d12_transfer_coordinates(
    const GeD3D12Transfer *transfer,
    const GeD3D12FramebufferKey *source,
    const GeD3D12FramebufferKey *destination,
    D3D12_BOX *source_box, UINT *destination_x, UINT *destination_y) {
    if (!transfer || !source || !destination || !source_box ||
        !destination_x || !destination_y ||
        source->format != destination->format) {
        return false;
    }
    uint32_t source_x;
    uint32_t source_y;
    uint32_t dest_x;
    uint32_t dest_y;
    if (!transfer_origin(
            transfer->source_address, transfer->source_stride,
            transfer->source_x, transfer->source_y,
            transfer->bytes_per_pixel, source, &source_x, &source_y) ||
        !transfer_origin(
            transfer->destination_address, transfer->destination_stride,
            transfer->destination_x, transfer->destination_y,
            transfer->bytes_per_pixel, destination, &dest_x, &dest_y)) {
        return false;
    }
    if (source_x + transfer->width > GE_D3D12_FRAMEBUFFER_WIDTH ||
        dest_x + transfer->width > GE_D3D12_FRAMEBUFFER_WIDTH ||
        source_y + transfer->height > GE_D3D12_FRAMEBUFFER_HEIGHT ||
        dest_y + transfer->height > GE_D3D12_FRAMEBUFFER_HEIGHT) {
        return false;
    }
    source_box->left = source_x;
    source_box->top = source_y;
    source_box->front = 0;
    source_box->right = source_x + transfer->width;
    source_box->bottom = source_y + transfer->height;
    source_box->back = 1;
    *destination_x = dest_x;
    *destination_y = dest_y;
    return true;
}
