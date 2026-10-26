#include "framebuffer.h"

#include "device.h"
#include "texture.h"

extern "C" {
#include "../recomp.h"
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_TARGETS 16u
#define MAX_DEPTHS 16u
#define MAX_SNAPSHOT_DIRTY_RECTS 256u
#define RELEASE_COM(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

struct ColorTarget {
    GeD3D12FramebufferKey key;
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    D3D12_GPU_DESCRIPTOR_HANDLE descriptors;
    uint64_t lru;
    uint64_t capture_generation;
    uint64_t descriptor_generation;
    uint64_t cpu_hash;
    uint32_t serial;
    uint32_t rtv_index;
    bool occupied;
    bool pending_upload;
    bool gpu_newer;
};

struct DepthTarget {
    uint32_t address;
    uint16_t stride;
    uint16_t reserved;
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv;
    uint64_t lru;
    uint64_t capture_generation;
    uint32_t serial;
    uint32_t dsv_index;
    bool occupied;
    bool pending_upload;
};

struct SnapshotRect {
    uint16_t left;
    uint16_t top;
    uint16_t right;
    uint16_t bottom;
};

static ColorTarget s_targets[MAX_TARGETS];
static DepthTarget s_depths[MAX_DEPTHS];
static ColorTarget *s_active_target;
static DepthTarget *s_bound_depth;
static uint32_t s_bound_generation;
static ID3D12Resource *s_snapshot;
static D3D12_RESOURCE_STATES s_snapshot_state;
static D3D12_GPU_DESCRIPTOR_HANDLE s_snapshot_descriptors;
static uint64_t s_snapshot_descriptor_generation;
static ID3D12Resource *s_depth_snapshot;
static D3D12_RESOURCE_STATES s_depth_snapshot_state;
static D3D12_GPU_DESCRIPTOR_HANDLE s_destination_descriptors;
static uint64_t s_destination_descriptor_generation;

static ID3D12Resource *s_readback;
static ID3D12CommandAllocator *s_immediate_allocator;
static ID3D12GraphicsCommandList *s_immediate_list;
static ID3D12Fence *s_immediate_fence;
static HANDLE s_immediate_event;
static UINT64 s_immediate_fence_value;

static uint64_t s_lru;
static uint64_t s_capture_generation;
static uint64_t s_descriptor_generation;
static uint32_t s_next_color_serial;
static uint32_t s_next_depth_serial;
static uint32_t s_target_count;
static uint32_t s_depth_count;
static uint64_t s_captures;
static uint64_t s_uploads;
static uint64_t s_depth_uploads;
static uint64_t s_readbacks;
static uint64_t s_aliases;
static uint64_t s_invalidations;
static uint64_t s_transfer_attempts;
static uint64_t s_transfer_fallbacks;
static uint64_t s_transfers;
static uint64_t s_present_hits;
static uint64_t s_destination_snapshots;
static uint64_t s_feedback_snapshots;
static uint64_t s_feedback_snapshot_pixels;
static uint64_t s_feedback_snapshot_reuses;
static ColorTarget *s_feedback_snapshot_target;
static SnapshotRect s_snapshot_dirty[MAX_SNAPSHOT_DIRTY_RECTS];
static uint32_t s_snapshot_dirty_count;
static bool s_snapshot_dirty_all;
static uint32_t s_render_scale = 1u;
static bool s_initialized;
/* Last framebuffer capture, valid until the capture generation changes or a target is released. */
static GeD3D12FramebufferKey s_capture_memo_key;
static GeD3D12FramebufferBinding s_capture_memo_binding;
static uint64_t s_capture_memo_generation;
static uint32_t s_release_epoch = 1;
static uint32_t s_capture_memo_epoch;
static bool s_log;
static bool s_legacy_depth_uploads;
static bool s_legacy_feedback_snapshots;
static bool s_legacy_destination_snapshots;
static bool s_disable_feedback_aliases;
/* Upload heaps are write-combined: scaled rows are built here, then only written to the heap.
 * Duplicating rows by reading the heap back cost ~56 ms per movie frame at 2x. */
static uint32_t s_scaled_color_row[GE_D3D12_FRAMEBUFFER_WIDTH * 8u];
static uint16_t s_scaled_depth_row[GE_D3D12_FRAMEBUFFER_WIDTH * 8u];

static bool check_hr(HRESULT hr, const char *operation) {
    if (SUCCEEDED(hr)) return true;
    fprintf(stderr, "ged3d12: %s failed (HRESULT 0x%08lx)\n",
            operation, (unsigned long)hr);
    return false;
}

static uint32_t render_width(void) {
    return GE_D3D12_FRAMEBUFFER_WIDTH * s_render_scale;
}

static uint32_t render_height(void) {
    return GE_D3D12_FRAMEBUFFER_HEIGHT * s_render_scale;
}

static uint32_t texture_height(void) {
    return GE_D3D12_FRAMEBUFFER_TEXTURE_HEIGHT * s_render_scale;
}

static void reset_feedback_snapshot_tracking(void) {
    s_feedback_snapshot_target = nullptr;
    s_snapshot_dirty_count = 0;
    s_snapshot_dirty_all = false;
}

static SnapshotRect feedback_source_rect(
    const GeD3D12FramebufferAlias *alias) {
    SnapshotRect rect = {
        alias->snapshot_left,
        alias->snapshot_top,
        alias->snapshot_right,
        alias->snapshot_bottom,
    };
    if (rect.right <= rect.left || rect.bottom <= rect.top ||
        rect.right > GE_D3D12_FRAMEBUFFER_WIDTH ||
        rect.bottom > GE_D3D12_FRAMEBUFFER_TEXTURE_HEIGHT) {
        rect.left = 0;
        rect.top = 0;
        rect.right = GE_D3D12_FRAMEBUFFER_WIDTH;
        rect.bottom = GE_D3D12_FRAMEBUFFER_TEXTURE_HEIGHT;
    }
    return rect;
}

static bool rects_overlap(
    const SnapshotRect *left, const SnapshotRect *right) {
    return left->left < right->right &&
           right->left < left->right &&
           left->top < right->bottom &&
           right->top < left->bottom;
}

static bool feedback_snapshot_needs_refresh(
    ColorTarget *target, const GeD3D12FramebufferAlias *alias) {
    if (s_legacy_feedback_snapshots ||
        s_feedback_snapshot_target != target ||
        s_snapshot_dirty_all) {
        return true;
    }
    SnapshotRect source = feedback_source_rect(alias);
    for (uint32_t i = 0; i < s_snapshot_dirty_count; i++)
        if (rects_overlap(&source, &s_snapshot_dirty[i]))
            return true;
    return false;
}

static void feedback_snapshot_mark_dirty(
    ColorTarget *target,
    const GeD3D12FramebufferBinding *binding) {
    if (target != s_feedback_snapshot_target ||
        s_snapshot_dirty_all) {
        return;
    }
    SnapshotRect rect = {
        binding->draw_left,
        binding->draw_top,
        binding->draw_right,
        binding->draw_bottom,
    };
    if (rect.right <= rect.left || rect.bottom <= rect.top ||
        rect.right > GE_D3D12_FRAMEBUFFER_WIDTH ||
        rect.bottom > GE_D3D12_FRAMEBUFFER_HEIGHT ||
        s_snapshot_dirty_count >= MAX_SNAPSHOT_DIRTY_RECTS) {
        s_snapshot_dirty_all = true;
        return;
    }
    s_snapshot_dirty[s_snapshot_dirty_count++] = rect;
}

static SnapshotRect feedback_snapshot_refresh_rect(
    SnapshotRect source) {
    for (uint32_t i = 0; i < s_snapshot_dirty_count; i++) {
        const SnapshotRect *dirty = &s_snapshot_dirty[i];
        if (dirty->left < source.left) source.left = dirty->left;
        if (dirty->top < source.top) source.top = dirty->top;
        if (dirty->right > source.right) source.right = dirty->right;
        if (dirty->bottom > source.bottom)
            source.bottom = dirty->bottom;
    }
    return source;
}

bool ged3d12_framebuffer_configure(uint32_t scale) {
    if (s_initialized || scale < 1u || scale > 8u) return false;
    s_render_scale = scale;
    return true;
}

uint32_t ged3d12_framebuffer_scale(void) {
    return s_render_scale;
}

static void transition(
    ID3D12GraphicsCommandList *command_list, ID3D12Resource *resource,
    D3D12_RESOURCE_STATES *state, D3D12_RESOURCE_STATES next) {
    if (*state == next) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = *state;
    barrier.Transition.StateAfter = next;
    command_list->ResourceBarrier(1, &barrier);
    *state = next;
}

static uint32_t next_serial(uint32_t *value) {
    uint32_t serial = (*value)++ & 0xffffu;
    if (!serial) serial = (*value)++ & 0xffffu;
    return serial;
}

static uint32_t color_handle(uint32_t slot, const ColorTarget *target) {
    return (target->serial << 16) | slot;
}

static uint32_t depth_handle(uint32_t slot, const DepthTarget *depth) {
    return (depth->serial << 16) | slot;
}

static ColorTarget *color_from_handle(uint32_t handle) {
    uint32_t slot = handle & 0xffffu;
    uint32_t serial = handle >> 16;
    if (slot >= MAX_TARGETS || !serial) return nullptr;
    ColorTarget *target = &s_targets[slot];
    return target->occupied && target->serial == serial ? target : nullptr;
}

static DepthTarget *depth_from_handle(uint32_t handle) {
    uint32_t slot = handle & 0xffffu;
    uint32_t serial = handle >> 16;
    if (slot >= MAX_DEPTHS || !serial) return nullptr;
    DepthTarget *depth = &s_depths[slot];
    return depth->occupied && depth->serial == serial ? depth : nullptr;
}

static uint32_t target_bytes_per_pixel(const ColorTarget *target) {
    return target->key.format == 3u ? 4u : 2u;
}

static uint64_t depth_bytes(const DepthTarget *depth) {
    return (uint64_t)depth->stride * GE_D3D12_FRAMEBUFFER_HEIGHT * 2u;
}

static bool range_overlaps(
    uint32_t first_address, uint64_t first_size,
    uint32_t second_address, uint64_t second_size) {
    uint64_t first_begin = first_address & 0x1fffffffu;
    uint64_t first_end = first_begin + first_size;
    uint64_t second_begin = second_address & 0x1fffffffu;
    uint64_t second_end = second_begin + second_size;
    return first_begin < second_end && second_begin < first_end;
}

static bool target_cpu_hash(
    const ColorTarget *target, uint64_t *out) {
    uint64_t size = ged3d12_framebuffer_key_bytes(&target->key);
    uint64_t last = (uint64_t)target->key.address + size - 1u;
    if (!size || last > UINT32_MAX ||
        !sr_inrange(target->key.address) ||
        !sr_inrange((uint32_t)last)) {
        return false;
    }
    const uint8_t *data =
        static_cast<const uint8_t *>(SR_HOST(target->key.address));
    uint64_t hash = UINT64_C(14695981039346656037);
    for (uint64_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    *out = hash;
    return true;
}

static bool create_color_resource(ColorTarget *target) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = render_width();
    desc.Height = texture_height();
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&target->resource)), "create framebuffer target")) {
        return false;
    }
    GeD3D12CpuDescriptor descriptor = {};
    if (!ged3d12_rtv_alloc(&descriptor)) return false;
    target->rtv = descriptor.cpu;
    target->rtv_index = descriptor.index;
    ged3d12_device()->CreateRenderTargetView(
        target->resource, nullptr, target->rtv);
    target->state = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

static bool create_depth_resource(DepthTarget *depth) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = render_width();
    desc.Height = render_height();
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&depth->resource)), "create framebuffer depth")) {
        return false;
    }
    GeD3D12CpuDescriptor descriptor = {};
    if (!ged3d12_dsv_alloc(&descriptor)) return false;
    depth->dsv = descriptor.cpu;
    depth->dsv_index = descriptor.index;
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_D16_UNORM;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    ged3d12_device()->CreateDepthStencilView(
        depth->resource, &view, depth->dsv);
    depth->state = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

static void update_read_watch(void) {
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        if (s_targets[i].occupied && s_targets[i].gpu_newer) {
            sr_gpu_vram_watch_reads(1);
            sr_gpu_vram_watch_writes(1);
            return;
        }
    }
    sr_gpu_vram_watch_reads(0);
    sr_gpu_vram_watch_writes(0);
}

static bool immediate_begin(void) {
    if (!check_hr(s_immediate_allocator->Reset(),
                  "reset framebuffer immediate allocator")) {
        return false;
    }
    return check_hr(s_immediate_list->Reset(s_immediate_allocator, nullptr),
                    "reset framebuffer immediate command list");
}

static bool immediate_submit(void) {
    if (!check_hr(s_immediate_list->Close(),
                  "close framebuffer immediate command list")) {
        return false;
    }
    ID3D12CommandList *lists[] = {s_immediate_list};
    ged3d12_graphics_queue()->ExecuteCommandLists(1, lists);
    UINT64 value = s_immediate_fence_value++;
    if (!check_hr(ged3d12_graphics_queue()->Signal(s_immediate_fence, value),
                  "signal framebuffer immediate fence")) {
        return false;
    }
    if (s_immediate_fence->GetCompletedValue() < value) {
        if (!check_hr(s_immediate_fence->SetEventOnCompletion(
                value, s_immediate_event),
                "arm framebuffer immediate fence")) {
            return false;
        }
        if (WaitForSingleObject(s_immediate_event, INFINITE) != WAIT_OBJECT_0) {
            fprintf(stderr, "ged3d12: wait for framebuffer immediate fence failed\n");
            return false;
        }
    }
    return true;
}

static bool upload_color(ColorTarget *target) {
    const uint32_t physical_width = render_width();
    const uint32_t physical_texture_height = texture_height();
    const uint32_t row_pitch = physical_width * 4u;
    const uint64_t upload_size =
        (uint64_t)row_pitch * physical_texture_height;
    uint64_t source_size = ged3d12_framebuffer_key_bytes(&target->key);
    uint64_t source_last = (uint64_t)target->key.address + source_size - 1u;
    if (!source_size || source_last > UINT32_MAX ||
        !sr_inrange(target->key.address) ||
        !sr_inrange((uint32_t)source_last)) {
        fprintf(stderr, "ged3d12: framebuffer source range is outside guest memory\n");
        return false;
    }
    GeD3D12UploadSlice upload = {};
    if (!ged3d12_upload_alloc(
            upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &upload)) {
        return false;
    }
    uint32_t *destination = static_cast<uint32_t *>(upload.cpu);
    uint32_t format = target->key.format;
    uint32_t stride = target->key.stride;
    uint32_t bytes_per_pixel = target_bytes_per_pixel(target);
    memset(destination, 0, (size_t)upload_size);
    for (uint32_t y = 0;
         y < GE_D3D12_FRAMEBUFFER_TEXTURE_HEIGHT; y++) {
        uint64_t row_address =
            (uint64_t)target->key.address +
            (uint64_t)y * stride * bytes_per_pixel;
        uint64_t row_last =
            row_address + (uint64_t)stride * bytes_per_pixel - 1u;
        if (row_last > UINT32_MAX ||
            !sr_inrange((uint32_t)row_address) ||
            !sr_inrange((uint32_t)row_last)) {
            break;
        }
        const uint8_t *source =
            static_cast<const uint8_t *>(SR_HOST((uint32_t)row_address));
        uint32_t *row = s_scaled_color_row;
        if (bytes_per_pixel == 4) {
            const uint32_t *pixels =
                reinterpret_cast<const uint32_t *>(source);
            for (uint32_t x = 0; x < stride; x++) {
                uint32_t pixel = ged3d12_framebuffer_decode_pixel(
                    pixels[x], format);
                for (uint32_t sx = 0; sx < s_render_scale; sx++)
                    row[x * s_render_scale + sx] = pixel;
            }
        } else {
            const uint16_t *pixels =
                reinterpret_cast<const uint16_t *>(source);
            for (uint32_t x = 0; x < stride; x++) {
                uint32_t pixel = ged3d12_framebuffer_decode_pixel(
                    pixels[x], format);
                for (uint32_t sx = 0; sx < s_render_scale; sx++)
                    row[x * s_render_scale + sx] = pixel;
            }
        }
        for (uint32_t sy = 0; sy < s_render_scale; sy++)
            memcpy(
                destination +
                    (y * s_render_scale + sy) * physical_width,
                row, (size_t)row_pitch);
    }

    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    transition(command_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination_location = {};
    destination_location.pResource = target->resource;
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source_location = {};
    source_location.pResource = upload.resource;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source_location.PlacedFootprint.Offset = upload.offset;
    source_location.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;
    source_location.PlacedFootprint.Footprint.Width =
        physical_width;
    source_location.PlacedFootprint.Footprint.Height =
        physical_texture_height;
    source_location.PlacedFootprint.Footprint.Depth = 1;
    source_location.PlacedFootprint.Footprint.RowPitch = row_pitch;
    command_list->CopyTextureRegion(
        &destination_location, 0, 0, 0, &source_location, nullptr);
    transition(command_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    target->pending_upload = false;
    target->gpu_newer = false;
    if (!target_cpu_hash(target, &target->cpu_hash)) return false;
    s_uploads++;
    return true;
}

static bool upload_depth(DepthTarget *depth) {
    const uint32_t physical_width = render_width();
    const uint32_t physical_height = render_height();
    const uint32_t row_pitch = physical_width * 2u;
    const uint64_t upload_size =
        (uint64_t)row_pitch * physical_height;
    GeD3D12UploadSlice upload = {};
    if (!ged3d12_upload_alloc(
            upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &upload)) {
        return false;
    }
    const uint16_t *source_pixels =
        static_cast<const uint16_t *>(ge_zbuf_ptr());
    uint16_t *destination_pixels =
        static_cast<uint16_t *>(upload.cpu);
    for (uint32_t y = 0; y < GE_D3D12_FRAMEBUFFER_HEIGHT; y++) {
        uint16_t *row = s_scaled_depth_row;
        for (uint32_t x = 0; x < GE_D3D12_FRAMEBUFFER_WIDTH; x++) {
            uint16_t pixel =
                source_pixels[
                    y * GE_D3D12_FRAMEBUFFER_WIDTH + x];
            for (uint32_t sx = 0; sx < s_render_scale; sx++)
                row[x * s_render_scale + sx] = pixel;
        }
        for (uint32_t sy = 0; sy < s_render_scale; sy++)
            memcpy(
                destination_pixels +
                    (y * s_render_scale + sy) * physical_width,
                row, (size_t)row_pitch);
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    transition(command_list, depth->resource, &depth->state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = depth->resource;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = upload.resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint.Offset = upload.offset;
    source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_D16_UNORM;
    source.PlacedFootprint.Footprint.Width = physical_width;
    source.PlacedFootprint.Footprint.Height = physical_height;
    source.PlacedFootprint.Footprint.Depth = 1;
    source.PlacedFootprint.Footprint.RowPitch = row_pitch;
    command_list->CopyTextureRegion(
        &destination, 0, 0, 0, &source, nullptr);
    transition(command_list, depth->resource, &depth->state,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    depth->pending_upload = false;
    s_depth_uploads++;
    return true;
}

static bool readback_color(ColorTarget *target);

static bool readback_color_rect(
    ColorTarget *target, uint32_t address, uint32_t bytes) {
    if (!target->gpu_newer) return true;
    address = SR_PHYS(address);
    uint32_t bpp = target_bytes_per_pixel(target);
    uint64_t key_bytes = ged3d12_framebuffer_key_bytes(&target->key);
    uint32_t target_address = target->key.address & 0x1fffffffu;
    if (!bytes || !key_bytes || address < target_address ||
        (uint64_t)address + bytes >
            (uint64_t)target_address + key_bytes) {
        return readback_color(target);
    }

    uint32_t first = (address - target_address) / bpp;
    uint64_t last_address = (uint64_t)address + bytes - 1u;
    uint32_t last = (uint32_t)(last_address - target_address) / bpp;
    uint32_t left = first % target->key.stride;
    uint32_t top = first / target->key.stride;
    uint32_t right = (last % target->key.stride) + 1u;
    uint32_t bottom = (last / target->key.stride) + 1u;
    if (right <= left || bottom <= top ||
        right > target->key.stride ||
        bottom > GE_D3D12_FRAMEBUFFER_HEIGHT) {
        return readback_color(target);
    }
    if (top == bottom - 1u && left == 0u &&
        right == target->key.stride) {
        return readback_color(target);
    }

    const uint32_t sample_offset = s_render_scale / 2u;
    const uint32_t physical_width = render_width();
    const uint32_t physical_left = left * s_render_scale;
    const uint32_t physical_top = top * s_render_scale;
    const uint32_t physical_right = right * s_render_scale;
    const uint32_t physical_bottom = bottom * s_render_scale;
    if (physical_right > physical_width ||
        physical_bottom > render_height()) {
        return readback_color(target);
    }

    if (!immediate_begin()) return false;
    transition(s_immediate_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = target->resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = s_readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width = physical_width;
    destination.PlacedFootprint.Footprint.Height = render_height();
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = physical_width * 4u;
    D3D12_BOX source_box = {
        physical_left, physical_top, 0,
        physical_right, physical_bottom, 1,
    };
    s_immediate_list->CopyTextureRegion(
        &destination, physical_left, physical_top, 0, &source, &source_box);
    transition(s_immediate_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!immediate_submit()) return false;

    const uint64_t readback_bytes =
        (uint64_t)physical_width * render_height() * 4u;
    D3D12_RANGE read_range = {0, (SIZE_T)readback_bytes};
    void *mapped = nullptr;
    if (!check_hr(s_readback->Map(0, &read_range, &mapped),
                  "map partial framebuffer readback")) {
        return false;
    }
    const uint32_t *source_pixels = static_cast<const uint32_t *>(mapped);
    uint8_t *destination_bytes =
        static_cast<uint8_t *>(SR_HOST(target->key.address));
    for (uint32_t y = top; y < bottom; y++) {
        const uint32_t *row =
            source_pixels + (y * s_render_scale + sample_offset) * physical_width;
        if (target->key.format == 3u) {
            uint32_t *destination_row =
                reinterpret_cast<uint32_t *>(
                    destination_bytes + y * target->key.stride * 4u);
            for (uint32_t x = left; x < right; x++) {
                destination_row[x] = ged3d12_framebuffer_encode_pixel(
                    row[x * s_render_scale + sample_offset], target->key.format);
            }
        } else {
            uint16_t *destination_row =
                reinterpret_cast<uint16_t *>(
                    destination_bytes + y * target->key.stride * 2u);
            for (uint32_t x = left; x < right; x++) {
                destination_row[x] = (uint16_t)ged3d12_framebuffer_encode_pixel(
                    row[x * s_render_scale + sample_offset], target->key.format);
            }
        }
    }
    D3D12_RANGE written = {0, 0};
    s_readback->Unmap(0, &written);
    /* Keep GPU-newer set: outside this rectangle is still unread. */
    target->pending_upload = false;
    s_readbacks++;
    update_read_watch();
    return true;
}

static bool readback_color(ColorTarget *target) {
    const uint32_t physical_width = render_width();
    const uint32_t physical_height = render_height();
    const uint32_t sample_offset = s_render_scale / 2u;
    if (!target->gpu_newer) return true;
    if (!immediate_begin()) return false;
    transition(s_immediate_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION source = {};
    source.pResource = target->resource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination = {};
    destination.pResource = s_readback;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Footprint.Format =
        DXGI_FORMAT_B8G8R8A8_UNORM;
    destination.PlacedFootprint.Footprint.Width =
        physical_width;
    destination.PlacedFootprint.Footprint.Height =
        physical_height;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch =
        physical_width * 4u;
    D3D12_BOX source_box = {
        0, 0, 0,
        physical_width, physical_height, 1,
    };
    s_immediate_list->CopyTextureRegion(
        &destination, 0, 0, 0, &source, &source_box);
    transition(s_immediate_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!immediate_submit()) return false;

    const uint64_t bytes =
        (uint64_t)physical_width * physical_height * 4u;
    D3D12_RANGE read_range = {0, (SIZE_T)bytes};
    void *mapped = nullptr;
    if (!check_hr(s_readback->Map(0, &read_range, &mapped),
                  "map framebuffer readback")) {
        return false;
    }
    const uint32_t *source_pixels = static_cast<const uint32_t *>(mapped);
    uint8_t *destination_bytes =
        static_cast<uint8_t *>(SR_HOST(target->key.address));
    for (uint32_t y = 0; y < GE_D3D12_FRAMEBUFFER_HEIGHT; y++) {
        const uint32_t *row =
            source_pixels +
            (y * s_render_scale + sample_offset) * physical_width;
        if (target->key.format == 3u) {
            uint32_t *destination_row =
                reinterpret_cast<uint32_t *>(
                    destination_bytes + y * target->key.stride * 4u);
            for (uint32_t x = 0; x < target->key.stride; x++)
                destination_row[x] =
                    ged3d12_framebuffer_encode_pixel(
                        row[x * s_render_scale + sample_offset],
                        target->key.format);
        } else {
            uint16_t *destination_row =
                reinterpret_cast<uint16_t *>(
                    destination_bytes + y * target->key.stride * 2u);
            for (uint32_t x = 0; x < target->key.stride; x++)
                destination_row[x] = (uint16_t)
                    ged3d12_framebuffer_encode_pixel(
                        row[x * s_render_scale + sample_offset],
                        target->key.format);
        }
    }
    D3D12_RANGE written = {0, 0};
    s_readback->Unmap(0, &written);
    target->gpu_newer = false;
    target->pending_upload = false;
    if (!target_cpu_hash(target, &target->cpu_hash)) return false;
    s_readbacks++;
    update_read_watch();
    return true;
}

static void release_color(ColorTarget *target) {
    if (!target->occupied) return;
    s_release_epoch++;
    if (s_active_target == target) s_active_target = nullptr;
    if (target->rtv.ptr) ged3d12_rtv_free(target->rtv_index);
    RELEASE_COM(target->resource);
    memset(target, 0, sizeof(*target));
    if (s_target_count) s_target_count--;
}

static void release_depth(DepthTarget *depth) {
    if (!depth->occupied) return;
    s_release_epoch++;
    if (s_bound_depth == depth) s_bound_depth = nullptr;
    if (depth->dsv.ptr) ged3d12_dsv_free(depth->dsv_index);
    RELEASE_COM(depth->resource);
    memset(depth, 0, sizeof(*depth));
    if (s_depth_count) s_depth_count--;
}

static ColorTarget *acquire_color(
    const GeD3D12FramebufferKey *key, uint32_t *slot_out) {
    uint64_t requested_size = ged3d12_framebuffer_key_bytes(key);
    ColorTarget *exact = nullptr;
    uint32_t exact_slot = 0;
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (target->occupied &&
            target->key.address == key->address &&
            target->key.stride == key->stride &&
            target->key.format == key->format) {
            exact = target;
            exact_slot = i;
            break;
        }
    }
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *alias = &s_targets[i];
        if (alias->occupied && alias != exact && alias->gpu_newer &&
            range_overlaps(
                key->address, requested_size,
                alias->key.address,
                ged3d12_framebuffer_key_bytes(&alias->key)) &&
            !readback_color(alias)) {
            return nullptr;
        }
    }
    if (exact) {
        *slot_out = exact_slot;
        return exact;
    }
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (!target->occupied || target->key.address != key->address ||
            target->capture_generation == s_capture_generation) {
            continue;
        }
        if (!ged3d12_graphics_wait_idle() || !readback_color(target))
            return nullptr;
        release_color(target);
        break;
    }

    uint32_t slot = MAX_TARGETS;
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        if (!s_targets[i].occupied) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_TARGETS) {
        ColorTarget *victim = nullptr;
        for (uint32_t i = 0; i < MAX_TARGETS; i++) {
            ColorTarget *candidate = &s_targets[i];
            if (candidate->capture_generation == s_capture_generation)
                continue;
            if (!victim || candidate->lru < victim->lru) victim = candidate;
        }
        if (!victim || !ged3d12_graphics_wait_idle() ||
            !readback_color(victim)) {
            fprintf(stderr, "ged3d12: framebuffer cache exhausted\n");
            return nullptr;
        }
        slot = (uint32_t)(victim - s_targets);
        release_color(victim);
    }
    ColorTarget *target = &s_targets[slot];
    memset(target, 0, sizeof(*target));
    target->key = *key;
    target->serial = next_serial(&s_next_color_serial);
    target->occupied = true;
    target->pending_upload = true;
    s_target_count++;
    if (!create_color_resource(target)) {
        release_color(target);
        return nullptr;
    }
    if (s_log) {
        fprintf(stderr,
                "ged3d12: framebuffer target address=0x%08x "
                "stride=%u format=%u slot=%u\n",
                target->key.address, target->key.stride,
                target->key.format, slot);
    }
    *slot_out = slot;
    return target;
}

static DepthTarget *acquire_depth(
    const GeD3D12FramebufferKey *key, uint32_t *slot_out) {
    for (uint32_t i = 0; i < MAX_DEPTHS; i++) {
        DepthTarget *depth = &s_depths[i];
        if (depth->occupied &&
            depth->address == key->depth_address &&
            depth->stride == key->depth_stride) {
            *slot_out = i;
            return depth;
        }
    }
    uint32_t slot = MAX_DEPTHS;
    for (uint32_t i = 0; i < MAX_DEPTHS; i++) {
        if (!s_depths[i].occupied) {
            slot = i;
            break;
        }
    }
    if (slot == MAX_DEPTHS) {
        DepthTarget *victim = nullptr;
        for (uint32_t i = 0; i < MAX_DEPTHS; i++) {
            DepthTarget *candidate = &s_depths[i];
            if (candidate->capture_generation == s_capture_generation)
                continue;
            if (!victim || candidate->lru < victim->lru) victim = candidate;
        }
        if (!victim || !ged3d12_graphics_wait_idle()) {
            fprintf(stderr, "ged3d12: depth cache exhausted\n");
            return nullptr;
        }
        slot = (uint32_t)(victim - s_depths);
        release_depth(victim);
    }
    DepthTarget *depth = &s_depths[slot];
    memset(depth, 0, sizeof(*depth));
    depth->address = key->depth_address;
    depth->stride = key->depth_stride;
    depth->serial = next_serial(&s_next_depth_serial);
    depth->occupied = true;
    depth->pending_upload = true;
    s_depth_count++;
    if (!create_depth_resource(depth)) {
        release_depth(depth);
        return nullptr;
    }
    if (s_log) {
        fprintf(stderr,
                "ged3d12: framebuffer depth address=0x%08x "
                "stride=%u slot=%u\n",
                depth->address, depth->stride, slot);
    }
    *slot_out = slot;
    return depth;
}

static bool create_auxiliary_resources(void) {
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    default_heap.CreationNodeMask = 1;
    default_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC texture = {};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = render_width();
    texture.Height = texture_height();
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    texture.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&s_snapshot)), "create framebuffer snapshot")) {
        return false;
    }
    s_snapshot_state = D3D12_RESOURCE_STATE_COPY_DEST;

    texture.Height = render_height();
    texture.Format = DXGI_FORMAT_R16_TYPELESS;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &default_heap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&s_depth_snapshot)),
            "create framebuffer depth snapshot")) {
        return false;
    }
    s_depth_snapshot_state = D3D12_RESOURCE_STATE_COPY_DEST;

    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    readback_heap.CreationNodeMask = 1;
    readback_heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = (uint64_t)render_width() * render_height() * 4u;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&s_readback)), "create framebuffer readback")) {
        return false;
    }
    return true;
}

static bool create_immediate_context(void) {
    if (!check_hr(ged3d12_device()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&s_immediate_allocator)),
            "create framebuffer immediate allocator")) {
        return false;
    }
    if (!check_hr(ged3d12_device()->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_immediate_allocator,
            nullptr, IID_PPV_ARGS(&s_immediate_list)),
            "create framebuffer immediate command list")) {
        return false;
    }
    if (!check_hr(s_immediate_list->Close(),
                  "close initial framebuffer immediate command list")) {
        return false;
    }
    if (!check_hr(ged3d12_device()->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_immediate_fence)),
            "create framebuffer immediate fence")) {
        return false;
    }
    s_immediate_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_immediate_event) {
        fprintf(stderr, "ged3d12: create framebuffer immediate event failed\n");
        return false;
    }
    s_immediate_fence_value = 1;
    return true;
}

bool ged3d12_framebuffer_cache_init(void) {
    if (s_initialized) {
        fprintf(stderr, "ged3d12: framebuffer cache already initialized\n");
        return false;
    }
    memset(s_targets, 0, sizeof(s_targets));
    memset(s_depths, 0, sizeof(s_depths));
    s_active_target = nullptr;
    s_lru = 1;
    s_capture_generation = 1;
    s_descriptor_generation = 1;
    s_next_color_serial = 1;
    s_next_depth_serial = 1;
    s_target_count = s_depth_count = 0;
    s_captures = s_uploads = s_depth_uploads = 0;
    s_readbacks = s_aliases = 0;
    s_invalidations = s_transfer_attempts = s_transfer_fallbacks = 0;
    s_transfers = s_present_hits = 0;
    s_destination_snapshots = 0;
    s_feedback_snapshots = s_feedback_snapshot_pixels = 0;
    s_feedback_snapshot_reuses = 0;
    reset_feedback_snapshot_tracking();
    const char *log = getenv("SR_D3D12_FRAMEBUFFER_LOG");
    s_log = log && log[0] && strcmp(log, "0") != 0;
    s_legacy_depth_uploads =
        getenv("SR_D3D12_LEGACY_DEPTH_UPLOADS") != nullptr;
    s_legacy_feedback_snapshots =
        getenv("SR_D3D12_LEGACY_FEEDBACK_SNAPSHOTS") != nullptr;
    s_legacy_destination_snapshots =
        getenv("SR_D3D12_LEGACY_DESTINATION_SNAPSHOTS") != nullptr;
    s_disable_feedback_aliases =
        getenv("SR_D3D12_DISABLE_FEEDBACK_ALIASES") != nullptr;
    fprintf(
        stderr, "ged3d12: internal resolution %ux (%ux%u visible)\n",
        s_render_scale, 480u * s_render_scale,
        GE_D3D12_FRAMEBUFFER_HEIGHT * s_render_scale);
    s_initialized = true;
    if (!create_auxiliary_resources() || !create_immediate_context()) {
        ged3d12_framebuffer_cache_shutdown();
        return false;
    }
    return true;
}

void ged3d12_framebuffer_cache_shutdown(void) {
    if (s_initialized) {
        for (uint32_t i = 0; i < MAX_TARGETS; i++)
            if (s_targets[i].occupied && s_targets[i].gpu_newer)
                readback_color(&s_targets[i]);
    }
    if (s_log && s_initialized) {
        fprintf(stderr,
                "ged3d12: framebuffer cache captures=%llu "
                "color_uploads=%llu depth_uploads=%llu "
                "readbacks=%llu aliases=%llu invalidations=%llu "
                "transfer_attempts=%llu fallbacks=%llu transfers=%llu "
                "present_hits=%llu destination_snapshots=%llu "
                "feedback_snapshots=%llu feedback_reuses=%llu "
                "feedback_pixels=%llu "
                "colors=%u depths=%u\n",
                (unsigned long long)s_captures,
                (unsigned long long)s_uploads,
                (unsigned long long)s_depth_uploads,
                (unsigned long long)s_readbacks,
                (unsigned long long)s_aliases,
                (unsigned long long)s_invalidations,
                (unsigned long long)s_transfer_attempts,
                (unsigned long long)s_transfer_fallbacks,
                (unsigned long long)s_transfers,
                (unsigned long long)s_present_hits,
                (unsigned long long)s_destination_snapshots,
                (unsigned long long)s_feedback_snapshots,
                (unsigned long long)s_feedback_snapshot_reuses,
                (unsigned long long)s_feedback_snapshot_pixels,
                s_target_count, s_depth_count);
    }
    for (uint32_t i = 0; i < MAX_TARGETS; i++)
        release_color(&s_targets[i]);
    for (uint32_t i = 0; i < MAX_DEPTHS; i++)
        release_depth(&s_depths[i]);
    if (s_immediate_event) CloseHandle(s_immediate_event);
    s_immediate_event = nullptr;
    RELEASE_COM(s_immediate_fence);
    RELEASE_COM(s_immediate_list);
    RELEASE_COM(s_immediate_allocator);
    RELEASE_COM(s_readback);
    RELEASE_COM(s_depth_snapshot);
    RELEASE_COM(s_snapshot);
    s_snapshot_state = D3D12_RESOURCE_STATE_COMMON;
    s_depth_snapshot_state = D3D12_RESOURCE_STATE_COMMON;
    s_snapshot_descriptors.ptr = 0;
    s_snapshot_descriptor_generation = 0;
    s_destination_descriptors.ptr = 0;
    s_destination_descriptor_generation = 0;
    reset_feedback_snapshot_tracking();
    s_initialized = false;
    s_log = false;
    s_legacy_depth_uploads = false;
    s_legacy_feedback_snapshots = false;
    s_disable_feedback_aliases = false;
    sr_gpu_vram_watch_reads(0);
    sr_gpu_vram_watch_writes(0);
}

bool ged3d12_framebuffer_capture(GeD3D12FramebufferBinding *out) {
    if (!out || !s_initialized) return false;
    memset(out, 0, sizeof(*out));
    out->color_handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    out->depth_handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    GeD3D12FramebufferKey key = {};
    if (!ged3d12_framebuffer_key_build(ge_state_ptr(), &key)) {
        fprintf(stderr, "ged3d12: invalid framebuffer state\n");
        return false;
    }
    if (s_capture_memo_epoch == s_release_epoch &&
        s_capture_memo_generation == s_capture_generation &&
        memcmp(&key, &s_capture_memo_key, sizeof(key)) == 0) {
        *out = s_capture_memo_binding;
        s_captures++;
        return true;
    }
    uint32_t color_slot;
    ColorTarget *target = acquire_color(&key, &color_slot);
    if (!target) return false;
    uint32_t depth_slot;
    DepthTarget *depth = acquire_depth(&key, &depth_slot);
    if (!depth) return false;
    if (target->capture_generation != s_capture_generation) {
        target->capture_generation = s_capture_generation;
        target->pending_upload = !target->gpu_newer;
    }
    if (depth->capture_generation != s_capture_generation) {
        depth->capture_generation = s_capture_generation;
        if (s_legacy_depth_uploads)
            depth->pending_upload = true;
    }
    target->lru = s_lru++;
    depth->lru = s_lru++;
    out->color_handle = color_handle(color_slot, target);
    out->depth_handle = depth_handle(depth_slot, depth);
    s_capture_memo_key = key;
    s_capture_memo_binding = *out;
    s_capture_memo_generation = s_capture_generation;
    s_capture_memo_epoch = s_release_epoch;
    s_captures++;
    return true;
}

bool ged3d12_framebuffer_prepare_pending(void) {
    if (!s_initialized) return false;
    if (s_log && s_capture_generation > 1u &&
        (s_capture_generation - 1u) % 120u == 0) {
        fprintf(
            stderr,
            "ged3d12: feedback through generation=%llu "
            "snapshots=%llu reuses=%llu pixels=%llu\n",
            (unsigned long long)(s_capture_generation - 1u),
            (unsigned long long)s_feedback_snapshots,
            (unsigned long long)s_feedback_snapshot_reuses,
            (unsigned long long)s_feedback_snapshot_pixels);
    }
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (target->occupied && !target->pending_upload &&
            !target->gpu_newer) {
            uint64_t hash;
            if (!target_cpu_hash(target, &hash)) return false;
            if (hash != target->cpu_hash) {
                target->pending_upload = true;
                s_invalidations++;
            }
        }
        if (target->occupied && target->pending_upload &&
            !upload_color(target)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < MAX_DEPTHS; i++) {
        DepthTarget *depth = &s_depths[i];
        if (depth->occupied && depth->pending_upload &&
            !upload_depth(depth)) {
            return false;
        }
    }
    s_active_target = nullptr;
    reset_feedback_snapshot_tracking();
    s_descriptor_generation++;
    if (!s_descriptor_generation) s_descriptor_generation++;
    s_capture_generation++;
    if (!s_capture_generation) s_capture_generation++;
    update_read_watch();
    return true;
}

bool ged3d12_framebuffer_bind(const GeD3D12FramebufferBinding *binding) {
    if (!binding || !s_initialized) return false;
    ColorTarget *target = color_from_handle(binding->color_handle);
    DepthTarget *depth = depth_from_handle(binding->depth_handle);
    if (!target || !depth || target->pending_upload || depth->pending_upload) {
        fprintf(stderr, "ged3d12: stale or unprepared framebuffer binding\n");
        return false;
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    transition(command_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(command_list, depth->resource, &depth->state,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);
    uint32_t generation = ged3d12_command_list_generation();
    if (s_active_target != target || s_bound_depth != depth ||
        s_bound_generation != generation) {
        command_list->OMSetRenderTargets(
            1, &target->rtv, FALSE, &depth->dsv);
        D3D12_VIEWPORT viewport = {};
        viewport.Width = (float)render_width();
        viewport.Height = (float)render_height();
        viewport.MaxDepth = 1.0f;
        command_list->RSSetViewports(1, &viewport);
        s_bound_depth = depth;
        s_bound_generation = generation;
    }
    /* The per-draw scissor is set by ged3d12_shader_constants_bind. */
    target->lru = s_lru++;
    depth->lru = s_lru++;
    s_active_target = target;
    return true;
}

bool ged3d12_framebuffer_bind_destination(
    const GeD3D12FramebufferBinding *binding) {
    if (!binding || !s_initialized) return false;
    ColorTarget *target = color_from_handle(binding->color_handle);
    DepthTarget *depth = depth_from_handle(binding->depth_handle);
    if (!target || !depth || target->pending_upload || depth->pending_upload) {
        fprintf(stderr, "ged3d12: stale destination snapshot binding\n");
        return false;
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    transition(command_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(command_list, depth->resource, &depth->state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(command_list, s_snapshot, &s_snapshot_state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    transition(command_list, s_depth_snapshot, &s_depth_snapshot_state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    /* The pixel shader only loads the destination at fragment positions, which lie inside
     * the draw's vertex bounds. Copy that rectangle (one PSP pixel of margin) rather than the
     * complete render target; texels outside it are never read for this draw. */
    D3D12_BOX box = {
        0, 0, 0, render_width(), render_height(), 1,
    };
    if (!s_legacy_destination_snapshots &&
        binding->draw_right > binding->draw_left &&
        binding->draw_bottom > binding->draw_top) {
        uint32_t left = binding->draw_left ? binding->draw_left - 1u : 0u;
        uint32_t top = binding->draw_top ? binding->draw_top - 1u : 0u;
        uint32_t right = binding->draw_right + 1u;
        uint32_t bottom = binding->draw_bottom + 1u;
        if (right > GE_D3D12_FRAMEBUFFER_WIDTH)
            right = GE_D3D12_FRAMEBUFFER_WIDTH;
        if (bottom > GE_D3D12_FRAMEBUFFER_HEIGHT)
            bottom = GE_D3D12_FRAMEBUFFER_HEIGHT;
        if (right > left && bottom > top) {
            box.left = left * s_render_scale;
            box.top = top * s_render_scale;
            box.right = right * s_render_scale;
            box.bottom = bottom * s_render_scale;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION color_source = {};
    color_source.pResource = target->resource;
    color_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION color_destination = {};
    color_destination.pResource = s_snapshot;
    color_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command_list->CopyTextureRegion(
        &color_destination, box.left, box.top, 0, &color_source, &box);
    D3D12_TEXTURE_COPY_LOCATION depth_source = {};
    depth_source.pResource = depth->resource;
    depth_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION depth_destination = {};
    depth_destination.pResource = s_depth_snapshot;
    depth_destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    command_list->CopyTextureRegion(
        &depth_destination, box.left, box.top, 0, &depth_source, &box);
    if (s_log && !s_destination_snapshots)
        fprintf(stderr, "ged3d12: destination snapshots active\n");
    s_destination_snapshots++;
    transition(command_list, s_snapshot, &s_snapshot_state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(command_list, s_depth_snapshot, &s_depth_snapshot_state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(command_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_RENDER_TARGET);
    transition(command_list, depth->resource, &depth->state,
               D3D12_RESOURCE_STATE_DEPTH_WRITE);

    if (s_destination_descriptor_generation != s_descriptor_generation) {
        GeD3D12DescriptorSlice descriptors = {};
        if (!ged3d12_srv_alloc(2, &descriptors)) return false;
        D3D12_SHADER_RESOURCE_VIEW_DESC color_view = {};
        color_view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        color_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        color_view.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        color_view.Texture2D.MipLevels = 1;
        ged3d12_device()->CreateShaderResourceView(
            s_snapshot, &color_view, descriptors.cpu);
        D3D12_SHADER_RESOURCE_VIEW_DESC depth_view = {};
        depth_view.Format = DXGI_FORMAT_R16_UNORM;
        depth_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depth_view.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depth_view.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE depth_cpu = descriptors.cpu;
        depth_cpu.ptr += ged3d12_device()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ged3d12_device()->CreateShaderResourceView(
            s_depth_snapshot, &depth_view, depth_cpu);
        s_destination_descriptors = descriptors.gpu;
        s_destination_descriptor_generation = s_descriptor_generation;
    }
    command_list->SetGraphicsRootDescriptorTable(
        GE_D3D12_ROOT_DESTINATION, s_destination_descriptors);
    return true;
}

bool ged3d12_framebuffer_mark_rendered(
    const GeD3D12FramebufferBinding *binding) {
    if (!binding || !s_initialized) return false;
    ColorTarget *target = color_from_handle(binding->color_handle);
    if (!target || target->pending_upload) return false;
    uint64_t target_size = ged3d12_framebuffer_key_bytes(&target->key);
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *alias = &s_targets[i];
        if (!alias->occupied || alias == target ||
            !range_overlaps(
                target->key.address, target_size,
                alias->key.address,
                ged3d12_framebuffer_key_bytes(&alias->key))) {
            continue;
        }
        if (alias->gpu_newer) {
            fprintf(stderr,
                    "ged3d12: overlapping framebuffer owners became GPU-newer\n");
            return false;
        }
        alias->pending_upload = true;
    }
    target->gpu_newer = true;
    target->pending_upload = false;
    feedback_snapshot_mark_dirty(target, binding);
    update_read_watch();
    return true;
}

bool ged3d12_framebuffer_texture_alias(
    const GeState *state, GeD3D12FramebufferAlias *out) {
    if (!out || !state || !s_initialized) return false;
    memset(out, 0, sizeof(*out));
    out->handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    if ((state->tex_filter & 4u) != 0 && state->tex_max_level > 0)
        return false;
    uint32_t stride = ge_texture_buffer_width(state->tex_fmt, state->tex_bufw);
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        GeD3D12FramebufferAlias alias = {};
        if (!target->occupied ||
            !ged3d12_framebuffer_alias(
                &target->key, state->tex_addr, stride,
                (uint32_t)state->tex_w, (uint32_t)state->tex_h,
                state->tex_fmt, state->tex_swizzle != 0, &alias)) {
            continue;
        }
        alias.handle = color_handle(i, target);
        *out = alias;
        target->lru = s_lru++;
        s_aliases++;
        return true;
    }
    return false;
}

bool ged3d12_framebuffer_texture_feedback(
    const GeState *state, GeD3D12FramebufferAlias *out,
    uint8_t *source_format) {
    if (!out || !source_format || !state || !s_initialized) return false;
    memset(out, 0, sizeof(*out));
    out->handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    *source_format = 0;
    if (s_disable_feedback_aliases) return false;
    if ((state->tex_filter & 4u) != 0 && state->tex_max_level > 0)
        return false;
    uint32_t stride = ge_texture_buffer_width(state->tex_fmt, state->tex_bufw);
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        GeD3D12FramebufferAlias alias = {};
        if (!target->occupied ||
            !ged3d12_framebuffer_feedback_alias(
                &target->key, state->tex_addr, stride,
                (uint32_t)state->tex_w, (uint32_t)state->tex_h,
                state->tex_fmt, state->tex_swizzle != 0, &alias)) {
            continue;
        }
        alias.handle = color_handle(i, target);
        *out = alias;
        *source_format = target->key.format;
        target->lru = s_lru++;
        s_aliases++;
        return true;
    }
    return false;
}

static bool create_alias_descriptors(
    ID3D12Resource *resource, const GeD3D12UploadSlice *clut,
    D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
    if (clut && (!clut->resource || clut->size < 2048u ||
                 (clut->offset & 3u) != 0 ||
                 clut->offset / 4u > UINT_MAX)) {
        return false;
    }
    GeD3D12DescriptorSlice descriptors = {};
    if (!ged3d12_srv_alloc(2, &descriptors)) return false;
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    ged3d12_device()->CreateShaderResourceView(
        resource, &view, descriptors.cpu);
    D3D12_SHADER_RESOURCE_VIEW_DESC null_view = {};
    null_view.Format = DXGI_FORMAT_R32_UINT;
    null_view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_view.Buffer.NumElements = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE null_cpu = descriptors.cpu;
    null_cpu.ptr += ged3d12_device()->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (clut) {
        null_view.Buffer.FirstElement = (UINT)(clut->offset / 4u);
        null_view.Buffer.NumElements = 512u;
        ged3d12_device()->CreateShaderResourceView(
            clut->resource, &null_view, null_cpu);
    } else {
        ged3d12_device()->CreateShaderResourceView(
            nullptr, &null_view, null_cpu);
    }
    *gpu = descriptors.gpu;
    return true;
}

bool ged3d12_framebuffer_texture_bind(
    const GeD3D12FramebufferAlias *alias,
    const GeD3D12UploadSlice *clut,
    D3D12_GPU_DESCRIPTOR_HANDLE *descriptors) {
    if (!alias || !descriptors || !s_initialized) return false;
    ColorTarget *target = color_from_handle(alias->handle);
    if (!target || target->pending_upload) {
        fprintf(stderr, "ged3d12: stale framebuffer texture alias\n");
        return false;
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    if (target == s_active_target) {
        if (feedback_snapshot_needs_refresh(target, alias)) {
            SnapshotRect source = feedback_source_rect(alias);
            bool target_changed =
                s_feedback_snapshot_target != target;
            bool full_snapshot =
                s_legacy_feedback_snapshots ||
                target_changed ||
                s_snapshot_dirty_all;
            transition(command_list, target->resource, &target->state,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            transition(command_list, s_snapshot, &s_snapshot_state,
                       D3D12_RESOURCE_STATE_COPY_DEST);
            if (full_snapshot) {
                command_list->CopyResource(
                    s_snapshot, target->resource);
                s_snapshot_dirty_count = 0;
                s_snapshot_dirty_all = false;
            } else {
                source = feedback_snapshot_refresh_rect(source);
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = s_snapshot;
                destination.Type =
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination.SubresourceIndex = 0;
                D3D12_TEXTURE_COPY_LOCATION origin = {};
                origin.pResource = target->resource;
                origin.Type =
                    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                origin.SubresourceIndex = 0;
                D3D12_BOX source_box = {
                    (UINT)source.left * s_render_scale,
                    (UINT)source.top * s_render_scale,
                    0,
                    (UINT)source.right * s_render_scale,
                    (UINT)source.bottom * s_render_scale,
                    1,
                };
                command_list->CopyTextureRegion(
                    &destination,
                    (UINT)source.left * s_render_scale,
                    (UINT)source.top * s_render_scale,
                    0, &origin, &source_box);
                s_snapshot_dirty_count = 0;
            }
            transition(command_list, s_snapshot, &s_snapshot_state,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transition(command_list, target->resource, &target->state,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
            s_feedback_snapshot_target = target;
            s_feedback_snapshots++;
            s_feedback_snapshot_pixels +=
                full_snapshot
                    ? (uint64_t)render_width() * texture_height()
                    : (uint64_t)(source.right - source.left) *
                      (uint64_t)(source.bottom - source.top) *
                      (uint64_t)s_render_scale *
                      (uint64_t)s_render_scale;
            if (s_log && s_feedback_snapshots <= 32u) {
                fprintf(
                    stderr,
                    "ged3d12: feedback snapshot #%llu "
                    "request=%u,%u-%u,%u scale=%u\n",
                    (unsigned long long)s_feedback_snapshots,
                    source.left, source.top,
                    source.right, source.bottom, s_render_scale);
            }
        } else {
            s_feedback_snapshot_reuses++;
        }
        if (clut) {
            return create_alias_descriptors(
                s_snapshot, clut, descriptors);
        }
        if (s_snapshot_descriptor_generation != s_descriptor_generation) {
            if (!create_alias_descriptors(
                    s_snapshot, nullptr, &s_snapshot_descriptors)) {
                return false;
            }
            s_snapshot_descriptor_generation = s_descriptor_generation;
        }
        *descriptors = s_snapshot_descriptors;
        return true;
    }
    transition(command_list, target->resource, &target->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (clut) {
        return create_alias_descriptors(
            target->resource, clut, descriptors);
    }
    if (target->descriptor_generation != s_descriptor_generation) {
        if (!create_alias_descriptors(
                target->resource, nullptr, &target->descriptors)) {
            return false;
        }
        target->descriptor_generation = s_descriptor_generation;
    }
    *descriptors = target->descriptors;
    return true;
}

bool ged3d12_framebuffer_present_source(
    uint32_t address, uint32_t format, uint32_t stride,
    D3D12_TEXTURE_COPY_LOCATION *source, D3D12_BOX *source_box) {
    if (!source || !source_box || !s_initialized) return false;
    uint32_t physical = 0x04000000u | (address & 0x001fffffu);
    uint32_t normalized_stride = stride & 0x7ffu;
    if (!normalized_stride) normalized_stride = GE_D3D12_FRAMEBUFFER_WIDTH;
    if (normalized_stride < 480u) return false;
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (!target->occupied || target->pending_upload ||
            target->key.address != physical ||
            target->key.format != (format & 3u) ||
            target->key.stride != normalized_stride) {
            continue;
        }
        transition(ged3d12_command_list(), target->resource, &target->state,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        source->pResource = target->resource;
        source->Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source->SubresourceIndex = 0;
        source_box->left = 0;
        source_box->top = 0;
        source_box->front = 0;
        source_box->right = 480u * s_render_scale;
        source_box->bottom = render_height();
        source_box->back = 1;
        s_active_target = nullptr;
        target->lru = s_lru++;
        s_present_hits++;
        return true;
    }
    return false;
}

bool ged3d12_framebuffer_safe_readback(
    uint32_t address, uint32_t format, uint32_t stride) {
    if (!s_initialized) return false;
    uint32_t physical = 0x04000000u | (address & 0x001fffffu);
    uint32_t normalized_stride = stride & 0x7ffu;
    if (!normalized_stride) normalized_stride = GE_D3D12_FRAMEBUFFER_WIDTH;
    bool found_display = false;
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (!target->occupied || target->pending_upload) continue;
        if (target->key.address == physical &&
            target->key.format == (format & 3u) &&
            target->key.stride == normalized_stride) {
            found_display = true;
        }
        target->gpu_newer = true;
        update_read_watch();
        if (!readback_color(target)) return false;
    }
    return found_display;
}

void ged3d12_framebuffer_invalidate(uint32_t address, uint32_t bytes) {
    if (!s_initialized || !bytes) return;
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (!target->occupied ||
            !ged3d12_framebuffer_key_overlaps(
                &target->key, address, bytes)) {
            continue;
        }
        if (target->gpu_newer) {
            fprintf(stderr,
                    "ged3d12: CPU write invalidated a newer GPU framebuffer\n");
        }
        if (!target->pending_upload) s_invalidations++;
        target->gpu_newer = false;
        target->pending_upload = true;
    }
    for (uint32_t i = 0; i < MAX_DEPTHS; i++) {
        DepthTarget *depth = &s_depths[i];
        if (depth->occupied &&
            range_overlaps(depth->address, depth_bytes(depth),
                           address, bytes)) {
            depth->pending_upload = true;
        }
    }
    update_read_watch();
}

void ged3d12_framebuffer_read(uint32_t address, uint32_t bytes) {
    if (!s_initialized || !bytes) return;
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *target = &s_targets[i];
        if (target->occupied && target->gpu_newer &&
            ged3d12_framebuffer_key_overlaps(
                &target->key, address, bytes)) {
            if (!readback_color_rect(target, address, bytes)) {
                fprintf(stderr, "ged3d12: framebuffer partial readback failed\n");
                return;
            }
        }
    }
}

int ged3d12_framebuffer_transfer(uint32_t start_data) {
    if (!s_initialized) return false;
    s_transfer_attempts++;
    GeD3D12Transfer transfer = {};
    if (!ged3d12_transfer_build(
            ge_state_ptr(), start_data, &transfer)) {
        return false;
    }
    D3D12_BOX source_box = {};
    UINT destination_x;
    UINT destination_y;
    ColorTarget *source = nullptr;
    ColorTarget *destination = nullptr;
    for (uint32_t source_index = 0;
         source_index < MAX_TARGETS && !source; source_index++) {
        ColorTarget *source_candidate = &s_targets[source_index];
        if (!source_candidate->occupied ||
            source_candidate->pending_upload ||
            !source_candidate->gpu_newer) {
            continue;
        }
        for (uint32_t destination_index = 0;
             destination_index < MAX_TARGETS; destination_index++) {
            ColorTarget *destination_candidate =
                &s_targets[destination_index];
            if (!destination_candidate->occupied ||
                destination_candidate->pending_upload ||
                source_candidate == destination_candidate) {
                continue;
            }
            if (ged3d12_transfer_coordinates(
                    &transfer, &source_candidate->key,
                    &destination_candidate->key, &source_box,
                    &destination_x, &destination_y)) {
                source = source_candidate;
                destination = destination_candidate;
                break;
            }
        }
    }
    if (!source || !destination ||
        !readback_color(source) || !readback_color(destination)) {
        s_transfer_fallbacks++;
        if (s_log && s_transfer_fallbacks <= 32u) {
            fprintf(
                stderr,
                "ged3d12: transfer fallback #%llu "
                "src=0x%08x/%u dst=0x%08x/%u size=%ux%u bpp=%u\n",
                (unsigned long long)s_transfer_fallbacks,
                transfer.source_address, transfer.source_stride,
                transfer.destination_address, transfer.destination_stride,
                transfer.width, transfer.height,
                transfer.bytes_per_pixel);
        }
        return false;
    }
    if (!ged3d12_transfer_scale_coordinates(
            s_render_scale, &source_box,
            &destination_x, &destination_y) ||
        !immediate_begin()) {
        return false;
    }
    transition(s_immediate_list, source->resource, &source->state,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(s_immediate_list, destination->resource, &destination->state,
               D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION source_location = {};
    source_location.pResource = source->resource;
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination_location = {};
    destination_location.pResource = destination->resource;
    destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    s_immediate_list->CopyTextureRegion(
        &destination_location, destination_x, destination_y, 0,
        &source_location, &source_box);
    transition(s_immediate_list, source->resource, &source->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(s_immediate_list, destination->resource, &destination->state,
               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!immediate_submit()) return false;

    for (uint32_t y = 0; y < transfer.height; y++) {
        uint32_t source_offset =
            transfer.source_address +
            ((uint32_t)(transfer.source_y + y) * transfer.source_stride +
             transfer.source_x) * transfer.bytes_per_pixel;
        uint32_t destination_offset =
            transfer.destination_address +
            ((uint32_t)(transfer.destination_y + y) *
                 transfer.destination_stride +
             transfer.destination_x) * transfer.bytes_per_pixel;
        memmove(SR_HOST(destination_offset), SR_HOST(source_offset),
                (size_t)transfer.width * transfer.bytes_per_pixel);
    }
    uint32_t dirty_address =
        transfer.destination_address +
        ((uint32_t)transfer.destination_y * transfer.destination_stride +
         transfer.destination_x) * transfer.bytes_per_pixel;
    uint32_t dirty_bytes =
        ((uint32_t)(transfer.height - 1u) *
             transfer.destination_stride +
         transfer.width) * transfer.bytes_per_pixel;
    ged3d12_texture_invalidate(dirty_address, dirty_bytes);
    for (uint32_t i = 0; i < MAX_TARGETS; i++) {
        ColorTarget *alias = &s_targets[i];
        if (alias->occupied && alias != destination &&
            ged3d12_framebuffer_key_overlaps(
                &alias->key, dirty_address, dirty_bytes)) {
            alias->gpu_newer = false;
            alias->pending_upload = true;
        }
    }
    for (uint32_t i = 0; i < MAX_DEPTHS; i++) {
        DepthTarget *depth = &s_depths[i];
        if (depth->occupied &&
            range_overlaps(
                depth->address, depth_bytes(depth),
                dirty_address, dirty_bytes)) {
            depth->pending_upload = true;
        }
    }
    destination->gpu_newer = false;
    destination->pending_upload = false;
    if (!target_cpu_hash(destination, &destination->cpu_hash))
        return false;
    destination->lru = s_lru++;
    source->lru = s_lru++;
    s_transfers++;
    return true;
}

void ged3d12_framebuffer_flush(const char *reason) {
    if (!s_initialized || !reason) return;
    const GeState *state = ge_state_ptr();
    if (strcmp(reason, "xfersrc") == 0) {
        GeD3D12Transfer transfer = {};
        if (ged3d12_transfer_build(state, 0, &transfer)) {
            uint64_t bytes =
                ((uint64_t)transfer.source_y + transfer.height) *
                transfer.source_stride * transfer.bytes_per_pixel;
            if (bytes > UINT32_MAX) bytes = UINT32_MAX;
            ged3d12_framebuffer_read(
                transfer.source_address, (uint32_t)bytes);
        }

    } else if (strcmp(reason, "loadclut") == 0) {
        ged3d12_framebuffer_read(state->clut_addr, sizeof(state->clutram));
    }
}

void ged3d12_framebuffer_get_stats(
    uint64_t *readbacks, uint64_t *aliases,
    uint64_t *feedback_snapshots, uint64_t *feedback_snapshot_pixels,
    uint64_t *feedback_reuses) {
    if (readbacks) *readbacks = s_readbacks;
    if (aliases) *aliases = s_aliases;
    if (feedback_snapshots)
        *feedback_snapshots = s_feedback_snapshots;
    if (feedback_snapshot_pixels)
        *feedback_snapshot_pixels = s_feedback_snapshot_pixels;
    if (feedback_reuses)
        *feedback_reuses = s_feedback_snapshot_reuses;
}
