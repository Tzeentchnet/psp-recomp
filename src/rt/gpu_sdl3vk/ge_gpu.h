/* Vulkan GE backend descriptor (see README.md). */
#ifndef GE_GPU_H
#define GE_GPU_H

#include "../ge_shared.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SDL3/Vulkan presenter must be initialized before backend->init(). */
const GeBackend *gegpu_backend(void);

typedef struct GeVkBackendTiming {
    uint64_t flip;
    double begin_ms;
    double vertices_ms;
    double framebuffer_ms;
    double textures_ms;
    double draws_ms;
    double present_ms;
    double submit_ms;
    uint32_t vertices;
    uint32_t batches;
} GeVkBackendTiming;

int gegpu_last_backend_timing(GeVkBackendTiming *timing);

typedef struct GeVkFramebufferStats {
    uint64_t readbacks;
    uint64_t aliases;
    uint64_t feedback_snapshots;
    uint64_t feedback_snapshot_pixels;
    uint64_t feedback_reuses;
    uint64_t pipeline_key_hits;
    uint64_t pipelines_created;
    uint64_t pipeline_create_us;
    uint64_t texture_cache_hits;
    uint64_t texture_cache_misses;
    uint64_t texture_uploads;
    uint64_t texture_decode_us;
    uint64_t texture_scale_us;
    uint64_t host_upload_bytes;
    uint64_t host_readback_bytes;
} GeVkFramebufferStats;

int gegpu_framebuffer_stats(GeVkFramebufferStats *stats);
uint64_t gegpu_shader_hash(uint64_t hash);

#ifdef __cplusplus
}
#endif

#endif
