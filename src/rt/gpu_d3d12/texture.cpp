#include "texture.h"

#include "device.h"

extern "C" {
#include "../graphics_config.h"
#include "../perf_stats.h"
#include "../recomp.h"
#include "../texture_scaler.h"
}

#include <atomic>
#include <limits.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TEXTURES 512u
#define EVICT_TEXTURES 128u
#define MAX_CLUTS 256u
#define MAX_PENDING_DECODE_BYTES (512ull * 1024ull * 1024ull)
#define RELEASE_COM(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

/* Background decode. A texture first captured while a frame is recorded has its source bytes,
 * palette and decode state copied out of guest memory on the GE thread (ge_tex_snapshot_*);
 * workers decode and xBRZ-scale the copy while the frame goes on, and the frame's texture
 * preparation waits for the result before recording the upload. SR_D3D12_SYNC_TEXTURE_DECODE=1
 * (or the "texdecode" A/B alternation) restores the in-line decode, and
 * SR_D3D12_VERIFY_TEXTURE_DECODE=1 decodes every texture both ways and aborts on a difference. */
struct DecodeJob {
    GeTexSnapshot *snapshot;
    uint32_t *pixels;            /* the entry's buffer; written by the worker */
    uint32_t *verify;            /* live-decoded copy for the verify mode, else null */
    uint32_t level_count;
    uint32_t scale;
    uint32_t width[GE_D3D12_TEXTURE_LEVELS];
    uint32_t height[GE_D3D12_TEXTURE_LEVELS];
    uint32_t offset[GE_D3D12_TEXTURE_LEVELS];
    uint32_t pixel_count;
    DecodeJob *next;
    bool ok;
    std::atomic<bool> done;
};

static void job_free(DecodeJob *job);

struct TextureEntry {
    GeD3D12TextureKey key;
    uint64_t key_hash;
    uint64_t quick_hash;
    uint64_t content_hash;
    uint64_t lru;
    uint64_t capture_generation;
    uint64_t verified_generation;
    uint64_t descriptor_generation;
    uint32_t serial;
    uint32_t pixel_offset[GE_D3D12_TEXTURE_LEVELS];
    uint32_t pixel_count;
    uint32_t *pixels;
    DecodeJob *job;
    ID3D12Resource *resource;
    D3D12_RESOURCE_STATES state;
    D3D12_GPU_DESCRIPTOR_HANDLE descriptors;
    bool occupied;
    bool valid;
    bool pending;
};

struct SamplerBinding {
    GeD3D12TextureSampler key;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
};

struct ClutEntry {
    uint8_t data[2048];
    uint64_t hash;
    uint64_t lru;
    uint64_t capture_generation;
    uint64_t upload_generation;
    GeD3D12UploadSlice upload;
    bool occupied;
};

static TextureEntry s_textures[MAX_TEXTURES];
static ClutEntry s_cluts[MAX_CLUTS];
static SamplerBinding s_samplers[256];
static uint32_t s_sampler_count;
static uint32_t s_texture_count;
static uint32_t s_next_serial = 1;
static uint64_t s_lru = 1;
static uint64_t s_capture_generation = 1;
static uint64_t s_descriptor_generation = 1;
static uint64_t s_pending_decode_bytes;
static uint64_t s_lookups;
static uint64_t s_hits;
static uint64_t s_misses;
static uint64_t s_uploads;
static uint64_t s_invalidations;
static uint64_t s_evictions;
static uint32_t s_texture_scale = 1u;
static bool s_initialized;
static bool s_log;
static bool s_sync_decode;
static bool s_verify_decode;
static uint64_t s_async_decodes;
static uint64_t s_decode_wait_us;

static CRITICAL_SECTION s_pool_lock;
static CONDITION_VARIABLE s_pool_wake;
static HANDLE s_pool_threads[4];
static uint32_t s_pool_thread_count;
static DecodeJob *s_pool_head;
static DecodeJob *s_pool_tail;
static bool s_pool_stopping;
static bool s_pool_started;

static bool check_hr(HRESULT hr, const char *operation) {
    if (SUCCEEDED(hr)) return true;
    fprintf(stderr, "ged3d12: %s failed (HRESULT 0x%08lx)\n",
            operation, (unsigned long)hr);
    return false;
}

static bool capture_clut(const GeState *state, uint16_t *handle) {
    uint64_t hash = ged3d12_hash64(
        UINT64_C(14695981039346656037),
        state->clutram, sizeof(state->clutram));
    ClutEntry *entry = nullptr;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < MAX_CLUTS; i++) {
        ClutEntry *candidate = &s_cluts[i];
        if (candidate->occupied && candidate->hash == hash &&
            memcmp(candidate->data, state->clutram,
                   sizeof(candidate->data)) == 0) {
            entry = candidate;
            slot = i;
            break;
        }
    }
    if (!entry) {
        for (uint32_t i = 0; i < MAX_CLUTS; i++) {
            ClutEntry *candidate = &s_cluts[i];
            if (!candidate->occupied) {
                entry = candidate;
                slot = i;
                break;
            }
            if (candidate->capture_generation != s_capture_generation &&
                (!entry || candidate->lru < entry->lru)) {
                entry = candidate;
                slot = i;
            }
        }
        if (!entry || entry->capture_generation == s_capture_generation) {
            fprintf(stderr,
                    "ged3d12: framebuffer feedback CLUT cache exhausted\n");
            return false;
        }
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->data, state->clutram, sizeof(entry->data));
        entry->hash = hash;
        entry->occupied = true;
    }
    entry->lru = s_lru++;
    entry->capture_generation = s_capture_generation;
    *handle = (uint16_t)slot;
    return true;
}

static bool prepare_clut(
    uint16_t handle, const GeD3D12UploadSlice **upload) {
    if (handle >= MAX_CLUTS || !s_cluts[handle].occupied) {
        fprintf(stderr, "ged3d12: stale framebuffer feedback CLUT\n");
        return false;
    }
    ClutEntry *entry = &s_cluts[handle];
    if (entry->upload_generation != s_descriptor_generation) {
        GeD3D12UploadSlice allocated = {};
        if (!ged3d12_upload_alloc(
                sizeof(entry->data), 4u, &allocated)) {
            fprintf(stderr,
                    "ged3d12: could not upload framebuffer feedback CLUT\n");
            return false;
        }
        memcpy(allocated.cpu, entry->data, sizeof(entry->data));
        entry->upload = allocated;
        entry->upload_generation = s_descriptor_generation;
    }
    *upload = &entry->upload;
    return true;
}

static bool texture_memory(
    const GeD3D12TextureKey *key, uint32_t level,
    const uint8_t **data, uint64_t *size) {
    *size = ged3d12_texture_key_level_bytes(key, level);
    if (!*size || *size > UINT32_MAX) return false;
    uint32_t address = key->address[level];
    uint64_t last = (uint64_t)address + *size - 1u;
    if (last > UINT32_MAX ||
        !sr_inrange(address) || !sr_inrange((uint32_t)last)) {
        fprintf(stderr,
                "ged3d12: texture level %u range is outside guest memory "
                "(address=0x%08x, bytes=%llu)\n",
                level, address, (unsigned long long)*size);
        return false;
    }
    /* Incompatible framebuffer reinterpretations fall back to guest VRAM. */
    sr_prepare_read(address, (uint32_t)*size);
    *data = static_cast<const uint8_t *>(SR_HOST(address));
    return true;
}

static bool texture_content_hash(
    const GeD3D12TextureKey *key, bool full, uint64_t *out) {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = ged3d12_hash64(hash, key, sizeof(*key));
    for (uint32_t level = 0; level < key->mip_count; level++) {
        const uint8_t *data;
        uint64_t size;
        if (!texture_memory(key, level, &data, &size)) return false;
        hash = ged3d12_hash64(hash, &size, sizeof(size));
        if (full || size <= 65536u) {
            hash = ged3d12_hash64(hash, data, (size_t)size);
            continue;
        }
        uint64_t step = size / 256u;
        if (step < 16u) step = 16u;
        for (uint64_t offset = 0; offset + 16u <= size; offset += step)
            hash = ged3d12_hash64(hash, data + offset, 16);
        hash = ged3d12_hash64(hash, data + size - 16u, 16);
    }
    *out = hash;
    return true;
}

static void release_entry(TextureEntry *entry) {
    if (!entry->occupied) return;
    if (entry->job) {
        DecodeJob *job = entry->job;
        while (!job->done.load(std::memory_order_acquire)) YieldProcessor();
        entry->job = nullptr;
        job_free(job);
    }
    if (entry->pixels) {
        uint64_t bytes = (uint64_t)entry->pixel_count * sizeof(uint32_t);
        if (bytes <= s_pending_decode_bytes) s_pending_decode_bytes -= bytes;
        else s_pending_decode_bytes = 0;
        free(entry->pixels);
    }
    RELEASE_COM(entry->resource);
    memset(entry, 0, sizeof(*entry));
    if (s_texture_count) s_texture_count--;
}

static bool evict_lru(void) {
    if (!ged3d12_graphics_wait_idle()) return false;
    for (uint32_t n = 0; n < EVICT_TEXTURES; n++) {
        TextureEntry *victim = nullptr;
        for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
            TextureEntry *entry = &s_textures[i];
            if (!entry->occupied ||
                entry->capture_generation == s_capture_generation) {
                continue;
            }
            if (!victim || entry->lru < victim->lru) victim = entry;
        }
        if (!victim) break;
        release_entry(victim);
        s_evictions++;
    }
    return s_texture_count < MAX_TEXTURES;
}

static TextureEntry *allocate_entry(uint32_t *slot) {
    if (s_texture_count >= MAX_TEXTURES && !evict_lru()) {
        fprintf(stderr,
                "ged3d12: texture cache exhausted by %u textures "
                "referenced in one frame\n", MAX_TEXTURES);
        return nullptr;
    }
    for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
        if (!s_textures[i].occupied) {
            TextureEntry *entry = &s_textures[i];
            memset(entry, 0, sizeof(*entry));
            entry->occupied = true;
            entry->valid = true;
            entry->serial = s_next_serial++ & 0xffffu;
            if (!entry->serial) {
                entry->serial = s_next_serial++ & 0xffffu;
            }
            s_texture_count++;
            *slot = i;
            return entry;
        }
    }
    return nullptr;
}

/* Decodes every level of a texture, scaling each when scale > 1. A null snapshot decodes the
 * live GE state (GE thread only); otherwise the snapshot is the only input. Both orders use
 * the same arithmetic, which is what the verify mode asserts. */
static bool decode_levels(
    GeTexSnapshot *snapshot, uint32_t level_count, uint32_t scale,
    const uint32_t *width, const uint32_t *height, const uint32_t *offset,
    uint32_t *pixels) {
    uint32_t *decoded = nullptr;
    if (scale > 1u) {
        uint64_t decoded_bytes =
            (uint64_t)width[0] * height[0] * sizeof(uint32_t);
        decoded = static_cast<uint32_t *>(malloc((size_t)decoded_bytes));
        if (!decoded) {
            fprintf(stderr,
                    "ged3d12: could not allocate %llu bytes for texture scaling\n",
                    (unsigned long long)decoded_bytes);
            return false;
        }
    }
    for (uint32_t level = 0; level < level_count; level++) {
        uint32_t *destination = pixels + offset[level];
        uint32_t *target = scale == 1u ? destination : decoded;
        if (snapshot) ge_tex_snapshot_decode_level(snapshot, target, (int)level);
        else ge_decode_tex_rgba_level(target, (int)level);
        if (scale == 1u) continue;
        if (!sr_texture_scale_rgba(
                decoded, width[level], height[level], scale, destination)) {
            fprintf(stderr, "ged3d12: xBRZ texture scaling failed\n");
            free(decoded);
            return false;
        }
    }
    free(decoded);
    return true;
}

static void pool_run(DecodeJob *job) {
    job->ok = decode_levels(
        job->snapshot, job->level_count, job->scale,
        job->width, job->height, job->offset, job->pixels);
    job->done.store(true, std::memory_order_release);
}

static DWORD WINAPI pool_worker(void *) {
    for (;;) {
        EnterCriticalSection(&s_pool_lock);
        while (!s_pool_head && !s_pool_stopping)
            SleepConditionVariableCS(&s_pool_wake, &s_pool_lock, INFINITE);
        DecodeJob *job = s_pool_head;
        if (job) {
            s_pool_head = job->next;
            if (!s_pool_head) s_pool_tail = nullptr;
        }
        LeaveCriticalSection(&s_pool_lock);
        if (!job) return 0;             /* stopping and drained */
        pool_run(job);
    }
}

static bool pool_start(void) {
    if (s_pool_started) return s_pool_thread_count != 0;
    s_pool_started = true;
    InitializeCriticalSection(&s_pool_lock);
    InitializeConditionVariable(&s_pool_wake);
    s_pool_stopping = false;
    s_pool_head = s_pool_tail = nullptr;
    DWORD processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    uint32_t threads = processors / 4u;
    if (threads < 1u) threads = 1u;
    if (threads > 3u) threads = 3u;
    for (uint32_t i = 0; i < threads; i++) {
        HANDLE thread = CreateThread(nullptr, 0, pool_worker, nullptr, 0, nullptr);
        if (!thread) break;
        s_pool_threads[s_pool_thread_count++] = thread;
    }
    if (!s_pool_thread_count) {
        fprintf(stderr,
                "ged3d12: texture decode workers unavailable (error=%lu); "
                "decoding in line\n", (unsigned long)GetLastError());
        return false;
    }
    return true;
}

static void pool_stop(void) {
    if (!s_pool_started) return;
    EnterCriticalSection(&s_pool_lock);
    s_pool_stopping = true;
    LeaveCriticalSection(&s_pool_lock);
    WakeAllConditionVariable(&s_pool_wake);
    for (uint32_t i = 0; i < s_pool_thread_count; i++) {
        WaitForSingleObject(s_pool_threads[i], INFINITE);
        CloseHandle(s_pool_threads[i]);
        s_pool_threads[i] = nullptr;
    }
    s_pool_thread_count = 0;
    DeleteCriticalSection(&s_pool_lock);
    s_pool_started = false;
}

static void pool_submit(DecodeJob *job) {
    job->next = nullptr;
    EnterCriticalSection(&s_pool_lock);
    if (s_pool_tail) s_pool_tail->next = job;
    else s_pool_head = job;
    s_pool_tail = job;
    LeaveCriticalSection(&s_pool_lock);
    WakeConditionVariable(&s_pool_wake);
}

/* Blocks until the job's pixels are complete. Returns whether the decode succeeded. */
static bool job_wait(DecodeJob *job) {
    if (!job->done.load(std::memory_order_acquire)) {
        LARGE_INTEGER frequency, begin, end;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        while (!job->done.load(std::memory_order_acquire)) YieldProcessor();
        QueryPerformanceCounter(&end);
        uint64_t us = (uint64_t)(end.QuadPart - begin.QuadPart) * 1000000ull /
                      (uint64_t)frequency.QuadPart;
        s_decode_wait_us += us;
        sr_perf_counter_add(SR_PERF_TEXTURE_DECODE_WAIT_US, us);
    }
    return job->ok;
}

static void job_free(DecodeJob *job) {
    if (!job) return;
    ge_tex_snapshot_free(job->snapshot);
    free(job->verify);
    delete job;
}

static bool decode_entry(TextureEntry *entry) {
    uint64_t pixel_count = 0;
    DecodeJob scratch = {};
    for (uint32_t level = 0; level < entry->key.mip_count; level++) {
        entry->pixel_offset[level] = (uint32_t)pixel_count;
        scratch.width[level] = entry->key.width[level];
        scratch.height[level] = entry->key.height[level];
        scratch.offset[level] = (uint32_t)pixel_count;
        pixel_count +=
            (uint64_t)entry->key.width[level] * s_texture_scale *
            (uint64_t)entry->key.height[level] * s_texture_scale;
    }
    if (!pixel_count || pixel_count > UINT32_MAX ||
        pixel_count > SIZE_MAX / sizeof(uint32_t)) {
        fprintf(stderr, "ged3d12: decoded texture is too large\n");
        return false;
    }
    uint64_t bytes = pixel_count * sizeof(uint32_t);
    if (bytes > MAX_PENDING_DECODE_BYTES - s_pending_decode_bytes) {
        fprintf(stderr,
                "ged3d12: pending decoded textures exceed %llu MiB\n",
                (unsigned long long)(
                    MAX_PENDING_DECODE_BYTES / (1024u * 1024u)));
        return false;
    }
    /* Zeroed so a level the decoder declines to write is deterministic. */
    entry->pixels = static_cast<uint32_t *>(calloc(1, (size_t)bytes));
    if (!entry->pixels) {
        fprintf(stderr,
                "ged3d12: could not allocate %llu bytes for decoded texture\n",
                (unsigned long long)bytes);
        return false;
    }
    entry->pixel_count = (uint32_t)pixel_count;
    s_pending_decode_bytes += bytes;

    auto fail = [&](void) {
        free(entry->pixels);
        entry->pixels = nullptr;
        entry->pixel_count = 0;
        s_pending_decode_bytes -= bytes;
        return false;
    };

    bool background = !s_sync_decode &&
                      !sr_perf_path_off(SR_PATH_TEXTURE_DECODE) &&
                      pool_start();
    if (background) {
        GeTexSnapshot *snapshot =
            ge_tex_snapshot_capture(entry->key.mip_count);
        DecodeJob *job = snapshot ? new (std::nothrow) DecodeJob() : nullptr;
        if (job) {
            job->snapshot = snapshot;
            job->pixels = entry->pixels;
            job->level_count = entry->key.mip_count;
            job->scale = s_texture_scale;
            memcpy(job->width, scratch.width, sizeof(job->width));
            memcpy(job->height, scratch.height, sizeof(job->height));
            memcpy(job->offset, scratch.offset, sizeof(job->offset));
            job->pixel_count = (uint32_t)pixel_count;
            if (s_verify_decode) {
                job->verify = static_cast<uint32_t *>(
                    calloc(1, (size_t)bytes));
                if (!job->verify ||
                    !decode_levels(
                        nullptr, job->level_count, job->scale, job->width,
                        job->height, job->offset, job->verify)) {
                    job_free(job);
                    return fail();
                }
            }
            entry->job = job;
            entry->pending = true;
            s_async_decodes++;
            pool_submit(job);
            return true;
        }
        ge_tex_snapshot_free(snapshot);     /* out of memory: decode in line */
    }

    if (!decode_levels(
            nullptr, entry->key.mip_count, s_texture_scale,
            scratch.width, scratch.height, scratch.offset, entry->pixels)) {
        return fail();
    }
    entry->pending = true;
    return true;
}

/* Completes an entry's background decode (and the optional verification) before its upload. */
static bool finish_decode(TextureEntry *entry) {
    DecodeJob *job = entry->job;
    if (!job) return true;
    bool ok = job_wait(job);
    if (ok && job->verify &&
        memcmp(job->verify, entry->pixels,
               (size_t)job->pixel_count * sizeof(uint32_t)) != 0) {
        uint32_t differing = 0, first = 0;
        for (uint32_t i = 0; i < job->pixel_count; i++) {
            if (job->verify[i] == entry->pixels[i]) continue;
            if (!differing) first = i;
            differing++;
        }
        fprintf(stderr,
                "ged3d12: background texture decode differs from the live "
                "decode (key=%016llx fmt=%u %ux%u): %u of %u pixels, first "
                "at %u (live %08x, background %08x)\n",
                (unsigned long long)entry->key_hash, entry->key.format,
                entry->key.width[0], entry->key.height[0], differing,
                job->pixel_count, first, job->verify[first],
                entry->pixels[first]);
        abort();
    }
    entry->job = nullptr;
    job_free(job);
    return ok;
}

static bool create_resource(TextureEntry *entry) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = entry->key.width[0] * s_texture_scale;
    desc.Height = entry->key.height[0] * s_texture_scale;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = entry->key.mip_count;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (!check_hr(ged3d12_device()->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&entry->resource)), "create cached texture")) {
        return false;
    }
    entry->state = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
}

static bool upload_entry(TextureEntry *entry) {
    if (!entry->resource && !create_resource(entry)) return false;
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    for (uint32_t level = 0; level < entry->key.mip_count; level++) {
        UINT width = entry->key.width[level] * s_texture_scale;
        UINT height = entry->key.height[level] * s_texture_scale;
        UINT row_pitch = (width * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
                         ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        UINT64 upload_size = (UINT64)row_pitch * height;
        GeD3D12UploadSlice upload = {};
        if (!ged3d12_upload_alloc(
                upload_size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &upload)) {
            fprintf(stderr,
                    "ged3d12: could not allocate %llu bytes for texture mip %u\n",
                    (unsigned long long)upload_size, level);
            return false;
        }
        const uint8_t *source = reinterpret_cast<const uint8_t *>(
            entry->pixels + entry->pixel_offset[level]);
        uint8_t *destination = static_cast<uint8_t *>(upload.cpu);
        for (UINT y = 0; y < height; y++) {
            memcpy(destination + (UINT64)y * row_pitch,
                   source + (UINT64)y * width * 4u,
                   (size_t)width * 4u);
            if (row_pitch > width * 4u) {
                memset(destination + (UINT64)y * row_pitch + width * 4u,
                       0, row_pitch - width * 4u);
            }
        }

        D3D12_TEXTURE_COPY_LOCATION source_location = {};
        source_location.pResource = nullptr;
        source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source_location.PlacedFootprint.Offset = upload.offset;
        source_location.PlacedFootprint.Footprint.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        source_location.PlacedFootprint.Footprint.Width = width;
        source_location.PlacedFootprint.Footprint.Height = height;
        source_location.PlacedFootprint.Footprint.Depth = 1;
        source_location.PlacedFootprint.Footprint.RowPitch = row_pitch;
        source_location.pResource = upload.resource;

        D3D12_TEXTURE_COPY_LOCATION destination_location = {};
        destination_location.pResource = entry->resource;
        destination_location.Type =
            D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination_location.SubresourceIndex = level;
        command_list->CopyTextureRegion(
            &destination_location, 0, 0, 0, &source_location, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = entry->resource;
    barrier.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    command_list->ResourceBarrier(1, &barrier);
    entry->state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    uint64_t bytes = (uint64_t)entry->pixel_count * sizeof(uint32_t);
    free(entry->pixels);
    entry->pixels = nullptr;
    entry->pixel_count = 0;
    entry->pending = false;
    if (bytes <= s_pending_decode_bytes) s_pending_decode_bytes -= bytes;
    else s_pending_decode_bytes = 0;
    s_uploads++;
    return true;
}

static D3D12_FILTER sampler_filter(const GeD3D12TextureSampler *sampler) {
    uint32_t bits = sampler->min_linear |
                    ((uint32_t)sampler->mag_linear << 1) |
                    ((uint32_t)sampler->mip_linear << 2);
    static const D3D12_FILTER filters[8] = {
        D3D12_FILTER_MIN_MAG_MIP_POINT,
        D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT,
        D3D12_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT,
        D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT,
        D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR,
        D3D12_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR,
        D3D12_FILTER_MIN_POINT_MAG_MIP_LINEAR,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
    };
    return filters[bits & 7u];
}

static bool sampler_binding(
    const GeD3D12TextureSampler *sampler,
    D3D12_GPU_DESCRIPTOR_HANDLE *gpu) {
    for (uint32_t i = 0; i < s_sampler_count; i++) {
        if (memcmp(&s_samplers[i].key, sampler, sizeof(*sampler)) == 0) {
            *gpu = s_samplers[i].gpu;
            return true;
        }
    }
    if (s_sampler_count >= sizeof(s_samplers) / sizeof(s_samplers[0])) {
        fprintf(stderr, "ged3d12: per-frame texture sampler cache exhausted\n");
        return false;
    }
    GeD3D12DescriptorSlice descriptor = {};
    if (!ged3d12_sampler_alloc(1, &descriptor)) return false;
    D3D12_SAMPLER_DESC desc = {};
    desc.Filter = sampler_filter(sampler);
    desc.AddressU = sampler->clamp_u
        ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
        : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressV = sampler->clamp_v
        ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP
        : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    desc.MipLODBias = (float)sampler->lod_bias_16 * (1.0f / 16.0f);
    desc.MaxAnisotropy = 1;
    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    desc.MinLOD = 0.0f;
    desc.MaxLOD = sampler->mip_enabled ? D3D12_FLOAT32_MAX : 0.0f;
    ged3d12_device()->CreateSampler(&desc, descriptor.cpu);
    SamplerBinding *binding = &s_samplers[s_sampler_count++];
    binding->key = *sampler;
    binding->gpu = descriptor.gpu;
    *gpu = descriptor.gpu;
    return true;
}

bool ged3d12_texture_cache_init(void) {
    if (s_initialized) {
        fprintf(stderr, "ged3d12: texture cache already initialized\n");
        return false;
    }
    memset(s_textures, 0, sizeof(s_textures));
    memset(s_cluts, 0, sizeof(s_cluts));
    s_sampler_count = 0;
    s_texture_count = 0;
    s_next_serial = 1;
    s_lru = 1;
    s_capture_generation = 1;
    s_descriptor_generation = 1;
    s_pending_decode_bytes = 0;
    s_lookups = s_hits = s_misses = s_uploads = 0;
    s_invalidations = s_evictions = 0;
    const SrGraphicsConfig *config = sr_graphics_config_get();
    s_texture_scale = config ? config->texture_scale : 1u;
    const char *log = getenv("SR_D3D12_TEXTURE_LOG");
    s_log = log && log[0] && strcmp(log, "0") != 0;
    const char *sync = getenv("SR_D3D12_SYNC_TEXTURE_DECODE");
    s_sync_decode = sync && sync[0] && strcmp(sync, "0") != 0;
    const char *verify = getenv("SR_D3D12_VERIFY_TEXTURE_DECODE");
    s_verify_decode = verify && verify[0] && strcmp(verify, "0") != 0;
    s_async_decodes = 0;
    s_decode_wait_us = 0;
    s_initialized = true;
    fprintf(
        stderr, "ged3d12: texture cache xBRZ scale %ux\n",
        s_texture_scale);
    return true;
}

void ged3d12_texture_cache_shutdown(void) {
    if (s_log && s_initialized) {
        fprintf(stderr,
                "ged3d12: texture cache lookups=%llu hits=%llu misses=%llu "
                "uploads=%llu invalidations=%llu evictions=%llu live=%u "
                "background=%llu decode_wait_us=%llu\n",
                (unsigned long long)s_lookups,
                (unsigned long long)s_hits,
                (unsigned long long)s_misses,
                (unsigned long long)s_uploads,
                (unsigned long long)s_invalidations,
                (unsigned long long)s_evictions,
                s_texture_count,
                (unsigned long long)s_async_decodes,
                (unsigned long long)s_decode_wait_us);
    }
    for (uint32_t i = 0; i < MAX_TEXTURES; i++)
        release_entry(&s_textures[i]);
    pool_stop();
    memset(s_samplers, 0, sizeof(s_samplers));
    memset(s_cluts, 0, sizeof(s_cluts));
    s_sampler_count = 0;
    s_texture_scale = 1u;
    s_initialized = false;
    s_log = false;
}

bool ged3d12_texture_capture(GeD3D12TextureBinding *out) {
    if (!out || !s_initialized) return false;
    memset(out, 0, sizeof(*out));
    out->handle = GE_D3D12_TEXTURE_HANDLE_NONE;
    out->framebuffer.handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    out->clut_handle = GE_D3D12_CLUT_HANDLE_NONE;
    const GeState *state = ge_state_ptr();
    out->address = state->tex_addr;
    out->width = (uint16_t)state->tex_w;
    out->height = (uint16_t)state->tex_h;
    out->format = (uint8_t)state->tex_fmt;
    ged3d12_texture_sampler_build(state, &out->sampler);
    if (!state->tex_enable || state->clear || !state->tex_addr) return true;
    if (ged3d12_framebuffer_texture_alias(
            state, &out->framebuffer)) {
        return true;
    }
    uint8_t framebuffer_format = 0;
    if (ged3d12_framebuffer_texture_feedback(
            state, &out->framebuffer, &framebuffer_format)) {
        if (!capture_clut(state, &out->clut_handle)) return false;
        out->flags |=
            GE_D3D12_TEXTURE_FRAMEBUFFER_CLUT |
            (uint8_t)(
                framebuffer_format <<
                GE_D3D12_TEXTURE_FRAMEBUFFER_FORMAT_SHIFT);
        return true;
    }
    if (s_texture_scale > 1u)
        out->flags |= GE_D3D12_TEXTURE_UPSCALED;

    GeD3D12TextureKey key = {};
    ged3d12_texture_key_build(state, &key);
    if (!key.mip_count || key.format > 10 ||
        key.width[0] > 512 || key.height[0] > 512) {
        fprintf(stderr,
                "ged3d12: invalid texture state "
                "(format=%u, levels=%u, size=%ux%u)\n",
                key.format, key.mip_count, key.width[0], key.height[0]);
        return false;
    }
    uint64_t key_hash = ged3d12_texture_key_hash(&key);
    uint64_t quick_hash;
    if (!texture_content_hash(&key, false, &quick_hash)) return false;
    uint64_t content_hash = 0;
    bool content_hash_ready = false;
    s_lookups++;
    for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
        TextureEntry *entry = &s_textures[i];
        if (!entry->occupied || !entry->valid ||
            entry->key_hash != key_hash ||
            entry->quick_hash != quick_hash ||
            memcmp(&entry->key, &key, sizeof(key)) != 0) {
            continue;
        }
        if (entry->verified_generation != s_capture_generation) {
            if (!content_hash_ready) {
                if (!texture_content_hash(&key, true, &content_hash))
                    return false;
                content_hash_ready = true;
            }
            if (entry->content_hash != content_hash) continue;
            entry->verified_generation = s_capture_generation;
        }
        entry->lru = s_lru++;
        entry->capture_generation = s_capture_generation;
        out->handle = (entry->serial << 16) | i;
        s_hits++;
        return true;
    }

    if (!content_hash_ready) {
        if (!texture_content_hash(&key, true, &content_hash)) return false;
        content_hash_ready = true;
    }
    for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
        TextureEntry *entry = &s_textures[i];
        if (!entry->occupied || entry->key_hash != key_hash ||
            entry->content_hash != content_hash ||
            memcmp(&entry->key, &key, sizeof(key)) != 0) {
            continue;
        }
        entry->valid = true;
        entry->quick_hash = quick_hash;
        entry->verified_generation = s_capture_generation;
        entry->lru = s_lru++;
        entry->capture_generation = s_capture_generation;
        entry->verified_generation = s_capture_generation;
        out->handle = (entry->serial << 16) | i;
        s_hits++;
        return true;
    }

    uint32_t slot;
    TextureEntry *entry = allocate_entry(&slot);
    if (!entry) return false;
    entry->key = key;
    entry->key_hash = key_hash;
    entry->quick_hash = quick_hash;
    entry->content_hash = content_hash;
    entry->lru = s_lru++;
    entry->capture_generation = s_capture_generation;
    if (!decode_entry(entry)) {
        release_entry(entry);
        return false;
    }
    out->handle = (entry->serial << 16) | slot;
    s_misses++;
    sr_perf_counter_add(SR_PERF_TEXTURE_MISSES, 1);
    if (s_log) {
        /* Background decodes are still running; only in-line ones can be counted here. */
        uint32_t nonzero_pixels = 0;
        if (!entry->job) {
            for (uint32_t i = 0; i < entry->pixel_count; i++)
                nonzero_pixels += entry->pixels[i] != 0;
        }
        fprintf(stderr,
                "ged3d12: texture miss key=%016llx content=%016llx "
                "fmt=%u %ux%u mips=%u slot=%u nonzero=%u/%u\n",
                (unsigned long long)key_hash,
                (unsigned long long)content_hash,
                key.format, key.width[0], key.height[0],
                key.mip_count, slot, nonzero_pixels,
                entry->pixel_count);
    }
    return true;
}

bool ged3d12_texture_prepare_pending(void) {
    if (!s_initialized) return false;
    for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
        TextureEntry *entry = &s_textures[i];
        if (!entry->occupied || !entry->pending) continue;
        if (!finish_decode(entry)) {
            fprintf(stderr, "ged3d12: background texture decode failed\n");
            return false;
        }
        if (!upload_entry(entry)) return false;
    }
    s_sampler_count = 0;
    s_descriptor_generation++;
    if (!s_descriptor_generation) s_descriptor_generation++;
    s_capture_generation++;
    if (!s_capture_generation) s_capture_generation++;
    return true;
}

bool ged3d12_texture_bind(const GeD3D12TextureBinding *binding) {
    if (!binding || !s_initialized) return false;
    D3D12_GPU_DESCRIPTOR_HANDLE descriptors = {};
    const GeD3D12UploadSlice *clut = nullptr;
    if ((binding->flags & GE_D3D12_TEXTURE_FRAMEBUFFER_CLUT) != 0) {
        if (binding->framebuffer.handle ==
                GE_D3D12_FRAMEBUFFER_HANDLE_NONE ||
            !prepare_clut(binding->clut_handle, &clut)) {
            return false;
        }
    }
    if (binding->framebuffer.handle != GE_D3D12_FRAMEBUFFER_HANDLE_NONE) {
        if (!ged3d12_framebuffer_texture_bind(
                &binding->framebuffer, clut, &descriptors)) {
            return false;
        }
    } else if (binding->handle != GE_D3D12_TEXTURE_HANDLE_NONE) {
        uint32_t slot = binding->handle & 0xffffu;
        uint32_t serial = binding->handle >> 16;
        if (slot >= MAX_TEXTURES) return false;
        TextureEntry *entry = &s_textures[slot];
        if (!entry->occupied || entry->serial != serial ||
            !entry->resource || entry->pending) {
            fprintf(stderr, "ged3d12: stale or unprepared texture binding\n");
            return false;
        }
        if (entry->descriptor_generation != s_descriptor_generation) {
            GeD3D12DescriptorSlice allocated = {};
            if (!ged3d12_srv_alloc(2, &allocated)) return false;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = entry->key.mip_count;
            ged3d12_device()->CreateShaderResourceView(
                entry->resource, &srv, allocated.cpu);

            D3D12_SHADER_RESOURCE_VIEW_DESC null_srv = {};
            null_srv.Format = DXGI_FORMAT_R32_UINT;
            null_srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            null_srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            null_srv.Buffer.NumElements = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE null_cpu = allocated.cpu;
            null_cpu.ptr +=
                ged3d12_device()->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            ged3d12_device()->CreateShaderResourceView(
                nullptr, &null_srv, null_cpu);
            entry->descriptors = allocated.gpu;
            entry->descriptor_generation = s_descriptor_generation;
        }
        descriptors = entry->descriptors;
    } else {
        return true;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE sampler = {};
    if (!sampler_binding(&binding->sampler, &sampler)) return false;
    static D3D12_GPU_DESCRIPTOR_HANDLE s_bound_descriptors;
    static D3D12_GPU_DESCRIPTOR_HANDLE s_bound_sampler;
    static uint32_t s_bound_generation;
    uint32_t generation = ged3d12_command_list_generation();
    if (s_bound_generation == generation &&
        s_bound_descriptors.ptr == descriptors.ptr &&
        s_bound_sampler.ptr == sampler.ptr) {
        return true;
    }
    ID3D12GraphicsCommandList *command_list = ged3d12_command_list();
    command_list->SetGraphicsRootDescriptorTable(
        GE_D3D12_ROOT_TEXTURES, descriptors);
    command_list->SetGraphicsRootDescriptorTable(
        GE_D3D12_ROOT_SAMPLER, sampler);
    s_bound_descriptors = descriptors;
    s_bound_sampler = sampler;
    s_bound_generation = generation;
    return true;
}

void ged3d12_texture_invalidate(uint32_t address, uint32_t bytes) {
    if (!s_initialized || !bytes) return;
    for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
        TextureEntry *entry = &s_textures[i];
        if (entry->occupied && entry->valid &&
            ged3d12_texture_key_overlaps(&entry->key, address, bytes)) {
            entry->valid = false;
            s_invalidations++;
        }
    }
}

void ged3d12_texture_invalidate_all(void) {
    if (!s_initialized) return;
    for (uint32_t i = 0; i < MAX_TEXTURES; i++) {
        if (s_textures[i].occupied && s_textures[i].valid) {
            s_textures[i].valid = false;
            s_invalidations++;
        }
    }
}
