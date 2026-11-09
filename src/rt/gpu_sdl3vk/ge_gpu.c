/* GPU GE rasterizer: full GPU rendering with persistent framebuffers (see README.md).
 *
 * AI disclosure: original Vulkan backend (NOT a port of PPSSPP's GPU), written with
 * substantial assistance from an LLM (Anthropic Claude). It reproduces PSP/GE pixel rules
 * derived from PPSSPP's software renderer (via ge.c), so it is GPLv2+. See CREDITS.md.
 *
 * Architecture: ge.c keeps doing vertex decode + T&L + clipping + primitive acceptance
 * (the parts validated against PPSSPP), then hands SCREEN-SPACE primitives to this
 * backend through the GeBackend seam. Every primitive renders on the GPU — there is no
 * software-rasterizer fallback. Provably compatible states use fixed-function Vulkan;
 * all destination-dependent states use ordered color/depth snapshots and exact shader
 * evaluation. GE framebuffers are persistent scaled 512x512 RGBA8 sampling envelopes
 * with scaled 512x272 D16 render regions. Guest VRAM is read when a target is created or
 * invalidated and written only when a CPU consumer needs GPU-newer content.
 *
 * Batches accumulate across GE lists and are submitted only when something consumes the
 * target: a retarget, a present, self-sampling (render-to-texture feedback), buffer
 * exhaustion. A submit is GPU-only (record + submit + fence); there is no pixel
 * conversion on the CPU in the steady state.
 *
 * Faithfulness notes (vs ge.c, the reference):
 *  - Coverage: x/y pre-snapped to the PSP 28.4 grid; Vulkan rasterizes at pixel centers
 *    with a top-left fill rule, matching raster_tri's integer edges.
 *  - Interpolation: all varyings noperspective; u*rw, v*rw, rw interpolate affinely and
 *    divide per pixel, exactly like the software inner loop.
 *  - Culling: draw_prim already reorders vertices for cull mode/strip parity; raster_tri
 *    keeps positive-cross (clockwise in Vulkan's framebuffer convention), so front face
 *    is CLOCKWISE with back-face culling.
 *  - RAM textures normally decode through ge.c's reference sampler. An opt-in,
 *    scale-1 compute path consumes the same immutable snapshots and raw-job ABI.
 *    Render-to-texture binds the target image directly when the address/stride match
 *    (a snapshot copy for feedback loops).
 *  - Destination alpha carries PSP stencil. The manual fragment path evaluates
 *    color/stencil/depth tests, all PSP blend factors/equations, dither, logic
 *    operations, native packing, and bit write masks in software-renderer order.
 *  - Lines and points remain 1px Vulkan quads rather than the software DDA.
 */
#include "ge_gpu.h"
#include "feedback_region.h"
#include "pipeline_cache_vk.h"
#include "sdl3vk.h"
#include "shader_state.h"
#include "tnl_classes.h"
#include "tnl_vertex_input.h"
#include "upload_scale.h"
#include "../graphics_config.h"
#include "../ge_tnl.h"
#include "../perf_stats.h"
#include "../platform.h"
#include "../texture_scaler.h"

#define _CRT_SECURE_NO_WARNINGS
#include "../recomp.h"
#include "../ge_shared.h"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define FB_W 512            /* full PSP framebuffer stride */
#define FB_H 272
#define FB_TEX_H 512        /* power-of-two framebuffer sampling envelope */
#define DRAW_W 480          /* pixels the GE can actually write */
#define MAX_VERTS  131072
#define MAX_TNL_INDICES (MAX_VERTS * 3)
#define MAX_BATCH  4096
#define MAX_PIPES  192
#define MAX_TEX    1024
#define MAX_PALETTE 128
#define MAX_PENDING_DECODE_BYTES (512ull * 1024ull * 1024ull)
#define MAX_GPU_TEXTURE_SOURCE_BYTES (512u * 512u * 4u)
#define MAX_GPU_TEXTURE_OUTPUT_BYTES (512u * 512u * 4u)
#define MAX_TGT    8
#define MAX_DEP    4

static const uint32_t k_vert_spv[] =
#include "psp_vert.inc"
;
static const uint32_t k_tnl_vert_spv[] =
#include "psp_tnl_vert.inc"
;
static const uint32_t k_tnl_validation_vert_spv[] =
#include "psp_tnl_validation_vert.inc"
;
static const uint32_t k_tnl_points_vert_spv[] =
#include "psp_tnl_points_vert.inc"
;
static const uint32_t k_tnl_sprites_vert_spv[] =
#include "psp_tnl_sprites_vert.inc"
;
static const uint32_t k_tnl_through_sprites_vert_spv[] =
#include "psp_tnl_through_sprites_vert.inc"
;
static const uint32_t k_tnl_transform_comp_spv[] =
#include "psp_tnl_transform_comp.inc"
;
static const uint32_t k_tnl_triangles_comp_spv[] =
#include "psp_tnl_triangles_comp.inc"
;
static const uint32_t k_texture_decode_comp_spv[] =
#include "psp_texture_decode_comp.inc"
;
static const uint32_t k_frag_spv[] =
#include "psp_frag.inc"
;

/* ---- captured vertex (must match psp.vert input layout) ------------------------------ */
typedef struct { float x, y, z, rw, u, v, fog; uint32_t rgba; } GpuVert;

typedef GeVkShaderState PushPC;

typedef struct {
    uint8_t blend_on, srcf, dstf, eq;      /* VkBlendFactor / VkBlendOp values */
    uint8_t cull;                          /* VkCullModeFlagBits */
    uint8_t ztest, zwrite, zfunc;          /* VkCompareOp */
    uint8_t cmask;                         /* VkColorComponentFlags */
    uint8_t manual;                        /* destination-aware shader path */
    uint8_t tnl;                           /* canonical/expanded vertex path */
} PipeKey;

typedef struct {
    PipeKey key;
    int32_t sx, sy, sw, sh;                /* scissor rect */
    float   bconst[4];                     /* blend constants (FIX factors) */
    PushPC  pc;
    VkDescriptorSet dset;
    VkDescriptorSet pset;
    int feedback;
    uint8_t preprocess;
    uint32_t draw_serial;
    uint32_t first, count;
} Batch;

typedef struct {
    uint32_t offsets[4];
    uint32_t counts[4];
} TnlComputeJob;

typedef struct {
    uint32_t addr, format, width, height, stride;
    uint32_t swizzle, linear, clamp_u, clamp_v;
    uint64_t palette_hash;
} TexKey;

typedef struct TexDecodeJob {
    GeTexSnapshot *snapshot;
    uint32_t *pixels;
    uint32_t *verify;
    uint32_t width, height, scale;
    uint64_t bytes;
    uint64_t decode_ticks, scale_ticks;
    int gpu, done, ok;
    struct TexDecodeJob *next;
} TexDecodeJob;

typedef struct {
    TexKey key;
    uint64_t hash;
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
    VkDescriptorSet set;
    int w, h;
    int pending;                           /* referenced by not-yet-submitted batches */
    TexDecodeJob *job;
    uint64_t lru;
} TexEnt;

typedef struct { PipeKey key; VkPipeline pipe; int used; } PipeEnt;

typedef struct {
    uint8_t data[2048];
    uint64_t hash, lru;
    uint32_t format;
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
    VkDescriptorSet set;
} PaletteEnt;

/* shared depth buffer, keyed by guest zbuf placement */
typedef struct {
    int used;
    uint32_t zba, zstride;                 /* key: zbp & 0x1FFFFF, stride */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
} DepthEnt;

/* persistent GE render target */
typedef struct {
    int used;
    uint32_t fba, stride, fmt;             /* key: framebuffer addr (masked), stride px, psm */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    VkDescriptorSet set_n, set_l;          /* sampled (nearest / linear) for RTT + blit src */
    DepthEnt *dep;
    VkFramebuffer fb;                      /* for (this color, this->dep) */
    int gpu_valid;                         /* GPU contents are the truth (newer than VRAM) */
    uint64_t lru;
    uint64_t render_gen;                   /* bumped per submit that rendered into this */
    uint64_t clean_gen;                    /* render_gen when guest VRAM last matched */
    uint64_t present_gen;
} Target;

/* ---- backend state -------------------------------------------------------------------- */
static int s_ready = 0;
static int s_error = 0;
static GeState  *s_ge;
static uint16_t *s_zbuf;

static VkPhysicalDevice s_pdev;
static VkDevice   s_dev;
static VkQueue    s_queue;
static VkCommandPool s_pool;
static VkCommandBuffer s_cmd;
static VkFence    s_fence;

static VkRenderPass s_rp;

static VkBuffer
    s_vbuf, s_tnl_vbuf, s_tnl_ibuf, s_tnl_sbuf,
    s_tnl_clip_buf, s_tnl_validation_buf, s_xfer,
    s_tex_decode_job_buf, s_tex_decode_source_buf,
    s_tex_decode_clut_buf;
static VkDeviceMemory
    s_vbuf_m, s_tnl_vbuf_m, s_tnl_ibuf_m, s_tnl_sbuf_m,
    s_tnl_clip_mem, s_tnl_validation_mem, s_xfer_m,
    s_tex_decode_job_mem, s_tex_decode_source_mem,
    s_tex_decode_clut_mem;
static GpuVert *s_vmap;
static GeTnlVertex *s_tnl_vmap;
static uint32_t *s_tnl_imap;
static GeTnlGpuState *s_tnl_smap;
static GeTnlValidationVertex *s_tnl_validation_map;
static void    *s_xfer_map;
static GeTextureRawJob *s_tex_decode_job_map;
static uint8_t *s_tex_decode_source_map;
static uint8_t *s_tex_decode_clut_map;
static VkImage s_upload_scale_image;
static VkDeviceMemory s_upload_scale_memory;
static VkImageLayout s_upload_scale_layout;
static int s_gpu_upload_scale;

static VkDescriptorSetLayout s_dlayout;
static VkDescriptorSetLayout s_dst_dlayout;
static VkDescriptorSetLayout s_tnl_dlayout;
static VkDescriptorSetLayout s_tnl_compute_dlayout;
static VkDescriptorSetLayout s_tex_decode_dlayout;
static VkDescriptorPool s_dpool_tex, s_dpool_fix;
static VkDescriptorPool s_tnl_dpool;
static VkDescriptorSet s_tnl_dset;
static VkDescriptorPool s_tnl_compute_dpool;
static VkDescriptorSet s_tnl_compute_dset;
static VkDescriptorPool s_tex_decode_dpool;
static VkDescriptorSet s_tex_decode_dset;
static VkPipelineLayout s_playout;
static VkPipelineLayout s_tnl_compute_layout;
static VkPipelineLayout s_tex_decode_layout;
static VkShaderModule
    s_vs, s_tnl_vs, s_tnl_validation_vs,
    s_tnl_points_vs, s_tnl_sprites_vs,
    s_tnl_through_sprites_vs, s_fs,
    s_tnl_transform_cs, s_tnl_triangles_cs,
    s_tex_decode_cs;
static VkPipeline s_tnl_transform_pipeline, s_tnl_triangles_pipeline;
static VkPipeline s_tex_decode_pipeline;
static VkPipelineCache s_pipeline_cache;

static TexEnt  s_tex[MAX_TEX];
static int     s_tex_n = 0;
static PaletteEnt s_palettes[MAX_PALETTE];
static int s_palette_n = 0;
static VkImage s_white; static VkDeviceMemory s_white_mem;
static VkImageView s_white_view; static VkSampler s_white_smp;
static VkDescriptorSet s_white_set;

/* snapshot copy of a target for self-sampling (feedback) draws */
static VkImage s_snapimg; static VkDeviceMemory s_snapimg_mem;
static VkImageView s_snap_view; static VkImageLayout s_snap_layout;
static VkSampler s_smp_n, s_smp_l;         /* shared nearest/linear samplers (clamp) */
static VkDescriptorSet s_snap_n, s_snap_l;

static VkImage s_dst_color, s_dst_depth;
static VkDeviceMemory s_dst_color_mem, s_dst_depth_mem;
static VkImageView s_dst_color_view, s_dst_depth_view;
static VkImageLayout s_dst_color_layout, s_dst_depth_layout;
static VkDescriptorSet s_dst_set;

static PipeEnt s_pipes[MAX_PIPES];
static int s_pipe_n = 0;

static Target   s_tgts[MAX_TGT];
static DepthEnt s_deps[MAX_DEP];
static Target  *s_cur = NULL;              /* target of the pending batches */
static uint64_t s_present_gen;
static uint64_t s_lru = 1;

static Batch    s_batch[MAX_BATCH];
static TnlComputeJob s_tnl_compute_jobs[MAX_BATCH];
static uint32_t s_nbatch = 0, s_nverts = 0;
static uint32_t s_tnl_nverts = 0, s_tnl_nindices = 0, s_tnl_nstates = 0;
static uint32_t s_tnl_compute_count;
static uint32_t s_tnl_compute_vertices;
static int s_tnl_preprocess_supported;
static int s_tnl_validation;
static int s_tnl_validation_exact;
static uint32_t s_tnl_validation_start;
static int s_tnl_validation_failed;
static int s_tnl_direct_compute;
static int s_gpu_primitive_expansion;
static uint64_t s_tnl_validation_vertices;

static int s_log = 0;
static int s_full_feedback = 0;
static unsigned long s_cnt_submit = 0, s_cnt_tri = 0, s_cnt_spr = 0, s_cnt_line = 0;
static unsigned long s_cnt_present_gpu = 0, s_cnt_present_cpu = 0, s_cnt_rtt = 0;
static unsigned long s_cnt_readback = 0, s_cnt_upload = 0, s_cnt_dirty = 0;
static unsigned long s_cnt_snap = 0, s_cnt_texup = 0, s_cnt_xferblit = 0;
static unsigned long s_cnt_snap_reuse = 0;
static unsigned long s_cnt_aliases = 0;
static uint64_t s_snap_pixels = 0;
static unsigned long s_cnt_direct = 0, s_cnt_manual = 0, s_cnt_dst_snap = 0;
static uint64_t s_pipeline_key_hits;
static uint64_t s_pipelines_created;
static uint64_t s_pipeline_create_ticks;
static uint64_t s_texture_cache_hits;
static uint64_t s_texture_cache_misses;
static uint64_t s_texture_decode_ticks;
static uint64_t s_texture_scale_ticks;
static uint64_t s_texture_decode_wait_ticks;
static uint64_t s_async_texture_decodes;
static uint64_t s_gpu_texture_decodes;
static uint64_t s_gpu_texture_decode_fallbacks;
static uint64_t s_host_upload_bytes;
static uint64_t s_host_readback_bytes;
static uint64_t s_framebuffer_upload_logical_bytes;
static uint64_t s_framebuffer_upload_host_bytes;
static uint64_t s_framebuffer_upload_expanded_pixels;
static uint64_t s_framebuffer_uploads;
static uint64_t s_framebuffer_upload_fallbacks;
static uint64_t s_framebuffer_upload_sync_ticks;
static uint32_t s_host_memory_type = UINT32_MAX;
static uint32_t s_device_memory_type = UINT32_MAX;

static int s_stage_log;
static uint64_t s_stage_flip;
static uint64_t s_stage_vertices_ticks;
static uint64_t s_stage_framebuffer_ticks;
static uint64_t s_stage_textures_ticks;
static uint64_t s_stage_draws_ticks;
static uint64_t s_stage_submit_ticks;
static uint32_t s_stage_vertices;
static uint32_t s_stage_batches;
static GeVkBackendTiming s_last_backend_timing;

static uint32_t s_texscratch[512 * 512];
static uint32_t *s_texscaled;
static uint32_t s_texture_scale = 1u;
static uint32_t s_render_scale = 1u;
static uint32_t s_gpu_tnl_classes;
static int s_sync_texture_decode;
static int s_verify_texture_decode;
static int s_gpu_texture_decode;
static uint64_t s_pending_decode_bytes;
static SDL_Mutex *s_decode_mutex;
static SDL_Condition *s_decode_wake;
static SDL_Condition *s_decode_done;
static SDL_Thread *s_decode_threads[3];
static uint32_t s_decode_thread_count;
static TexDecodeJob *s_decode_head;
static TexDecodeJob *s_decode_tail;
static int s_decode_stopping;
static int s_decode_pool_started;
static uint32_t s_pxscratch[FB_W * FB_TEX_H];
static uint32_t s_scaled_color_row[FB_W * SR_GRAPHICS_SCALE_MAX];
static uint16_t s_scaled_depth_row[FB_W * SR_GRAPHICS_SCALE_MAX];

#define VKC(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "gegpu: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

/* ---- small helpers -------------------------------------------------------------------- */

static uint64_t stage_counter(void) {
    return s_stage_log ? sr_time_now() : 0;
}

static int tex_prepare_pending(void);

static uint64_t stage_children(void) {
    return
        s_stage_vertices_ticks +
        s_stage_framebuffer_ticks +
        s_stage_textures_ticks +
        s_stage_draws_ticks +
        s_stage_submit_ticks;
}

static void stage_add_exclusive(
    uint64_t *ticks, uint64_t start, uint64_t children_before) {
    if (!start)
        return;
    uint64_t elapsed = sr_time_now() - start;
    uint64_t children = stage_children() - children_before;
    *ticks += elapsed > children ? elapsed - children : 0;
}

static double stage_milliseconds(uint64_t ticks) {
    uint64_t frequency = sr_time_frequency();
    return frequency
        ? (double)ticks * 1000.0 / (double)frequency
        : 0.0;
}

static uint64_t stage_microseconds(uint64_t ticks) {
    uint64_t frequency = sr_time_frequency();
    return frequency
        ? (uint64_t)(
            (double)ticks * 1000000.0 / (double)frequency)
        : 0;
}

static void stage_reset(void) {
    s_stage_vertices_ticks = 0;
    s_stage_framebuffer_ticks = 0;
    s_stage_textures_ticks = 0;
    s_stage_draws_ticks = 0;
    s_stage_submit_ticks = 0;
    s_stage_vertices = 0;
    s_stage_batches = 0;
}

static void stage_publish(uint64_t present_ticks) {
    if (!s_stage_log)
        return;
    memset(&s_last_backend_timing, 0, sizeof(s_last_backend_timing));
    s_last_backend_timing.flip = ++s_stage_flip;
    s_last_backend_timing.vertices_ms =
        stage_milliseconds(s_stage_vertices_ticks);
    s_last_backend_timing.framebuffer_ms =
        stage_milliseconds(s_stage_framebuffer_ticks);
    s_last_backend_timing.textures_ms =
        stage_milliseconds(s_stage_textures_ticks);
    s_last_backend_timing.draws_ms =
        stage_milliseconds(s_stage_draws_ticks);
    s_last_backend_timing.present_ms =
        stage_milliseconds(present_ticks);
    s_last_backend_timing.submit_ms =
        stage_milliseconds(s_stage_submit_ticks);
    s_last_backend_timing.vertices = s_stage_vertices;
    s_last_backend_timing.batches = s_stage_batches;
    stage_reset();
}

static uint32_t render_width(void) {
    return FB_W * s_render_scale;
}

static uint32_t render_height(void) {
    return FB_H * s_render_scale;
}

static uint32_t texture_height(void) {
    return FB_TEX_H * s_render_scale;
}

static int configure_render_scale(const SrGraphicsConfig *config) {
    const char *override = getenv("SR_VK_SCALE");
    uint32_t scale = config ? config->internal_resolution : 1u;
    if (override && override[0]) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed = strtoul(override, &end, 10);
        if (errno || !end || *end ||
            parsed < SR_GRAPHICS_SCALE_MIN ||
            parsed > SR_GRAPHICS_SCALE_MAX) {
            fprintf(
                stderr,
                "gegpu: SR_VK_SCALE must be an integer from %u to %u\n",
                SR_GRAPHICS_SCALE_MIN, SR_GRAPHICS_SCALE_MAX);
            return 0;
        }
        scale = (uint32_t)parsed;
    }
    if (scale < SR_GRAPHICS_SCALE_MIN ||
        scale > SR_GRAPHICS_SCALE_MAX) {
        fprintf(
            stderr,
            "gegpu: internal resolution must be from %u to %u\n",
            SR_GRAPHICS_SCALE_MIN, SR_GRAPHICS_SCALE_MAX);
        return 0;
    }
    s_render_scale = scale;
    return 1;
}

static int upload_scale_supported(void) {
    VkFormatProperties properties;
    VkFormatFeatureFlags required =
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT;
    vkGetPhysicalDeviceFormatProperties(
        s_pdev, VK_FORMAT_R8G8B8A8_UNORM, &properties);
    return
        (properties.optimalTilingFeatures & required) == required;
}

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage;
    VKC(vkCreateBuffer(s_dev, &bci, NULL, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        fprintf(stderr, "gegpu: no coherent host-visible Vulkan memory type\n");
        vkDestroyBuffer(s_dev, *buf, NULL);
        *buf = VK_NULL_HANDLE;
        return 0;
    }
    if (s_host_memory_type == UINT32_MAX)
        s_host_memory_type = mai.memoryTypeIndex;
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
    if (map) VKC(vkMapMemory(s_dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(s_dev, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) {
        fprintf(stderr, "gegpu: no device-local Vulkan image memory type\n");
        vkDestroyImage(s_dev, *img, NULL);
        *img = VK_NULL_HANDLE;
        return 0;
    }
    if (s_device_memory_type == UINT32_MAX)
        s_device_memory_type = mai.memoryTypeIndex;
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindImageMemory(s_dev, *img, *mem, 0));
    return 1;
}

static int make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView *view) {
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(s_dev, &vci, NULL, view));
    return 1;
}

/* Transition an image whose layout we track to `want` (full barrier; correctness over
 * fine-grained sync — these happen a handful of times per frame). */
static void to_layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.oldLayout = *cur; mb.newLayout = want;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = aspect;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    *cur = want;
}

/* one-shot command buffer */
static int cmd_begin(void) {
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(s_cmd, &bi));
    return 1;
}
static int cmd_submit_wait(void) {
    uint64_t start = stage_counter();
    VKC(vkEndCommandBuffer(s_cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    VKC(vkQueueSubmit(s_queue, 1, &si, s_fence));
    VKC(vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX));
    VKC(vkResetFences(s_dev, 1, &s_fence));
    if (start)
        s_stage_submit_ticks += sr_time_now() - start;
    return 1;
}

/* ---- guest framebuffer <-> RGBA8 (must match ge.c unpack_color / pack_fb exactly) ----- */

static uint32_t fb_unpack(uint32_t raw, uint32_t fmt) {
    uint32_t r, g, b, a;
    switch (fmt & 3) {
        case 0: r=(raw&0x1F)*255/31; g=((raw>>5)&0x3F)*255/63; b=((raw>>11)&0x1F)*255/31; a=255; break;
        case 1: r=(raw&0x1F)*255/31; g=((raw>>5)&0x1F)*255/31; b=((raw>>10)&0x1F)*255/31; a=(raw&0x8000)?255:0; break;
        case 2: r=(raw&0xF)*17; g=((raw>>4)&0xF)*17; b=((raw>>8)&0xF)*17; a=((raw>>12)&0xF)*17; break;
        default: return raw;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t fb_pack(uint32_t px, uint32_t fmt) {
    uint32_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF, a = px >> 24;
    switch (fmt & 3) {
        case 0: return ((r>>3)&0x1F) | (((g>>2)&0x3F)<<5) | (((b>>3)&0x1F)<<11);
        case 1: return ((r>>3)&0x1F) | (((g>>3)&0x1F)<<5) | (((b>>3)&0x1F)<<10) | (a>=128?0x8000u:0);
        case 2: return ((r>>4)&0xF) | (((g>>4)&0xF)<<4) | (((b>>4)&0xF)<<8) | (((a>>4)&0xF)<<12);
        default: return px;
    }
}

/* ---- pipelines ------------------------------------------------------------------------ */

static VkPipeline pipe_create(const PipeKey *k) {
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = k->tnl == 1u
        ? (s_tnl_validation ? s_tnl_validation_vs : s_tnl_vs)
        : k->tnl == 2u
            ? s_tnl_points_vs
            : k->tnl == 3u
                ? s_tnl_sprites_vs
                : k->tnl == 4u
                    ? s_tnl_through_sprites_vs
                    : s_vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_fs; st[1].pName = "main";

    VkVertexInputBindingDescription bind = { 0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription at[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       16 },
        { 2, 0, VK_FORMAT_R32_SFLOAT,          24 },
        { 3, 0, VK_FORMAT_R8G8B8A8_UINT,       28 },
    };
    if (k->tnl == 1u) {
        ge_vk_tnl_vertex_input_layout(&bind, at);
    }
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    if (k->tnl == 0u || k->tnl == 1u || k->tnl == 5u) {
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bind;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = at;
    }

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    /* raster_tri keeps POSITIVE cross product in y-down screen coords; Vulkan's signed
     * area has the opposite sign convention, so those triangles are Vulkan-"clockwise". */
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->ztest;
    ds.depthWriteEnable = k->zwrite;
    ds.depthCompareOp = (VkCompareOp)k->zfunc;

    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = k->blend_on;
    ba.srcColorBlendFactor = (VkBlendFactor)k->srcf;
    ba.dstColorBlendFactor = (VkBlendFactor)k->dstf;
    ba.colorBlendOp = (VkBlendOp)k->eq;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = k->cmask;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dn.dynamicStateCount = 3; dn.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb; pci.pDynamicState = &dn;
    pci.layout = s_playout; pci.renderPass = s_rp;

    VkPipeline p = VK_NULL_HANDLE;
    uint64_t start = s_stage_log ? sr_time_now() : 0;
    VkResult r = vkCreateGraphicsPipelines(
        s_dev, s_pipeline_cache, 1, &pci, NULL, &p);
    if (start)
        s_pipeline_create_ticks += sr_time_now() - start;
    if (r != VK_SUCCESS) fprintf(stderr, "gegpu: pipeline create failed: %d\n", (int)r);
    else s_pipelines_created++;
    return p;
}

static VkPipeline pipe_get(const PipeKey *k) {
    for (int i = 0; i < s_pipe_n; i++)
        if (!memcmp(&s_pipes[i].key, k, sizeof(*k))) {
            s_pipeline_key_hits++;
            return s_pipes[i].pipe;
        }
    if (s_pipe_n >= MAX_PIPES) return VK_NULL_HANDLE;
    VkPipeline p = pipe_create(k);
    if (!p) return VK_NULL_HANDLE;
    s_pipes[s_pipe_n].key = *k; s_pipes[s_pipe_n].pipe = p; s_pipe_n++;
    return p;
}

/* ---- descriptors ------------------------------------------------------------------------ */

static VkDescriptorSet make_descriptor(VkImageView view, VkSampler smp, VkDescriptorPool pool) {
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dlayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(s_dev, &dai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo dii = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = set; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &dii;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    return set;
}

static VkDescriptorSet make_destination_descriptor(void) {
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorImageInfo images[2] = {
        { s_smp_n, s_dst_color_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
        { s_smp_n, s_dst_depth_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkWriteDescriptorSet writes[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };
    dai.descriptorPool = s_dpool_fix;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &s_dst_dlayout;
    if (vkAllocateDescriptorSets(s_dev, &dai, &set) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < 2; i++) {
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(s_dev, 2, writes, 0, NULL);
    return set;
}

/* ---- batch submission (GPU only, no VRAM I/O) ------------------------------------------- */

static uint32_t s_flushgen = 1;            /* bumped per submit: invalidates state template */

static void batches_reset(void) {
    s_nbatch = 0;
    s_nverts = 0;
    s_tnl_nverts = 0;
    s_tnl_nindices = 0;
    s_tnl_nstates = 0;
    s_tnl_compute_count = 0;
    s_tnl_compute_vertices = 0;
    s_flushgen++;
    for (int i = 0; i < s_tex_n; i++) s_tex[i].pending = 0;
}

static void target_changed(Target *t) {
    t->render_gen++;
    if (++s_present_gen == 0) ++s_present_gen;
    t->present_gen = s_present_gen;
}

static void render_pass_begin(Target *t) {
    uint32_t width = render_width();
    uint32_t height = render_height();
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_layout(s_cmd, t->dep->img, VK_IMAGE_ASPECT_DEPTH_BIT, &t->dep->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = s_rp; rbi.framebuffer = t->fb;
    rbi.renderArea.extent.width = width;
    rbi.renderArea.extent.height = height;
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
}

static void render_pass_end(Target *t) {
    vkCmdEndRenderPass(s_cmd);
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void snapshot_destination(Target *t) {
    VkImageCopy copy = {0};
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, t->dep->img, VK_IMAGE_ASPECT_DEPTH_BIT, &t->dep->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, s_dst_color, VK_IMAGE_ASPECT_COLOR_BIT,
              &s_dst_color_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    to_layout(s_cmd, s_dst_depth, VK_IMAGE_ASPECT_DEPTH_BIT,
              &s_dst_depth_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.extent.width = render_width();
    copy.extent.height = render_height();
    copy.extent.depth = 1;
    vkCmdCopyImage(
        s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        s_dst_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImage(
        s_cmd, t->dep->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        s_dst_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    to_layout(s_cmd, s_dst_color, VK_IMAGE_ASPECT_COLOR_BIT,
              &s_dst_color_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(s_cmd, s_dst_depth, VK_IMAGE_ASPECT_DEPTH_BIT,
              &s_dst_depth_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s_cnt_dst_snap++;
}

static void snapshot_feedback(Target *t, GeVkRegion region) {
    /* A feedback draw must see the preceding draws, not the current attachment. */
    VkImageCopy copy = {0};
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.srcSubresource.layerCount = 1;
    copy.dstSubresource = copy.srcSubresource;
    copy.srcOffset.x = copy.dstOffset.x = region.x0;
    copy.srcOffset.y = copy.dstOffset.y = region.y0;
    copy.extent.width = (uint32_t)(region.x1 - region.x0);
    copy.extent.height = (uint32_t)(region.y1 - region.y0);
    copy.extent.depth = 1;
    vkCmdCopyImage(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s_snapimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    s_cnt_snap++;
    s_snap_pixels += (uint64_t)copy.extent.width * copy.extent.height;
}

static GeVkRegion feedback_sample_region(const Batch *batch, GeVkRegion whole) {
    uint32_t flags = (uint32_t)batch->pc.config[0] >> 8;
    if (!(flags & GE_VK_SHADER_PSP_SAMPLER)) return whole;
    uint32_t x = batch->pc.texture[3] & 0xffffu;
    uint32_t y = batch->pc.texture[3] >> 16;
    uint32_t w = batch->pc.texture[2] & 0xffffu;
    uint32_t h = batch->pc.texture[2] >> 16;
    if (!w || !h || x >= FB_W || y >= FB_TEX_H ||
        w > FB_W - x || h > FB_TEX_H - y) return whole;
    GeVkRegion region = {
        (int32_t)(x * s_render_scale), (int32_t)(y * s_render_scale),
        (int32_t)((x + w) * s_render_scale), (int32_t)((y + h) * s_render_scale),
    };
    return region;
}

static GeVkRegion batch_damage(const Batch *batch, GeVkRegion scissor) {
    if (batch->key.tnl == 4u) {
        if (batch->first % 3u || batch->count % 6u)
            return scissor;
        uint32_t first = batch->first / 3u;
        uint32_t count = batch->count / 3u;
        if (!count || first > s_tnl_nverts ||
            count > s_tnl_nverts - first) {
            return scissor;
        }
        const GeTnlVertex *controls = s_tnl_vmap + first;
        if (!isfinite(controls[0].position[0]) ||
            !isfinite(controls[0].position[1])) {
            return scissor;
        }
        float min_x = floorf(controls[0].position[0]);
        float max_x = min_x;
        float min_y = floorf(controls[0].position[1]);
        float max_y = min_y;
        for (uint32_t i = 1; i < count; i++) {
            float x = floorf(controls[i].position[0]);
            float y = floorf(controls[i].position[1]);
            if (!isfinite(x) || !isfinite(y)) return scissor;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
        return ge_vk_draw_damage(
            min_x, min_y, max_x, max_y,
            s_render_scale, scissor);
    }
    if (batch->key.tnl || batch->preprocess) return scissor;
    const GpuVert *vertices = s_vmap + batch->first;
    float min_x = vertices[0].x, max_x = min_x;
    float min_y = vertices[0].y, max_y = min_y;
    for (uint32_t i = 0; i < batch->count; i++) {
        float x = vertices[i].x, y = vertices[i].y;
        if (!isfinite(x) || !isfinite(y)) return scissor;
        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
    }
    return ge_vk_draw_damage(min_x, min_y, max_x, max_y, s_render_scale, scissor);
}

static int validate_tnl_outputs(void) {
    static const char *const component_names[12] = {
        "screen.x", "screen.y", "screen.z", "reciprocal_w",
        "uv.x", "uv.y", "fog", "padding",
        "color.r", "color.g", "color.b", "color.a",
    };
    uint64_t exact_mismatches[12] = {0};
    uint32_t first_batch[12] = {0};
    uint32_t first_index[12] = {0};
    uint32_t first_vertex[12] = {0};
    float first_expected[12] = {0};
    float first_actual[12] = {0};
    for (uint32_t batch_index = 0; batch_index < s_nbatch; batch_index++) {
        const Batch *batch = &s_batch[batch_index];
        if (!batch->key.tnl || batch->pc.tnl[1] == 0u)
            continue;
        uint32_t state_index = batch->pc.tnl[0];
        if (state_index >= s_tnl_nstates ||
            batch->first > s_tnl_nindices ||
            batch->count > s_tnl_nindices - batch->first) {
            fprintf(
                stderr,
                "gegpu: invalid GPU T&L validation range "
                "(batch=%u state=%u first=%u count=%u)\n",
                batch_index, state_index, batch->first, batch->count);
            return 0;
        }
        for (uint32_t i = 0; i < batch->count; i++) {
            uint32_t vertex_index =
                s_tnl_imap[batch->first + i];
            if (vertex_index >= s_tnl_nverts) {
                fprintf(
                    stderr,
                    "gegpu: invalid GPU T&L validation vertex "
                    "(batch=%u index=%u vertex=%u)\n",
                    batch_index, i, vertex_index);
                return 0;
            }
            GeTnlReferenceVertex reference;
            GeTnlValidationVertex expected;
            if (!ge_tnl_gpu_reference_transform(
                    &s_tnl_smap[state_index],
                    &s_tnl_vmap[vertex_index], &reference)) {
                fprintf(
                    stderr,
                    "gegpu: GPU T&L validation reference rejected "
                    "batch=%u index=%u vertex=%u\n",
                    batch_index, i, vertex_index);
                return 0;
            }
            ge_tnl_validation_pack(&reference, &expected);
            uint32_t component = 0;
            const GeTnlValidationVertex *actual =
                &s_tnl_validation_map[vertex_index];
            int matches = s_tnl_validation_exact
                ? ge_tnl_validation_compare_exact(
                    &expected, actual, &component)
                : ge_tnl_validation_compare(
                    &expected, actual, &component);
            if (!matches) {
                float expected_values[12];
                float actual_values[12];
                memcpy(
                    expected_values, &expected,
                    sizeof(expected_values));
                memcpy(
                    actual_values, actual,
                    sizeof(actual_values));
                if (s_tnl_validation_exact) {
                    for (uint32_t j = 0; j < 12; j++) {
                        if (j == 7u ||
                            expected_values[j] == actual_values[j]) {
                            continue;
                        }
                        if (exact_mismatches[j]++ == 0u) {
                            first_batch[j] = batch_index;
                            first_index[j] = i;
                            first_vertex[j] = vertex_index;
                            first_expected[j] = expected_values[j];
                            first_actual[j] = actual_values[j];
                        }
                    }
                    continue;
                }
                fprintf(
                    stderr,
                    "gegpu: GPU T&L validation mismatch "
                    "batch=%u index=%u vertex=%u component=%s "
                    "expected=%.9g actual=%.9g\n",
                    batch_index, i, vertex_index,
                    component_names[component],
                    expected_values[component],
                    actual_values[component]);
                return 0;
            }
            s_tnl_validation_vertices++;
        }
    }
    int exact_match = 1;
    for (uint32_t i = 0; i < 12; i++) {
        if (!exact_mismatches[i])
            continue;
        exact_match = 0;
        fprintf(
            stderr,
            "gegpu: GPU T&L exact validation mismatch "
            "component=%s count=%llu first_batch=%u first_index=%u "
            "first_vertex=%u expected=%.9g actual=%.9g\n",
            component_names[i],
            (unsigned long long)exact_mismatches[i],
            first_batch[i], first_index[i], first_vertex[i],
            first_expected[i], first_actual[i]);
    }
    if (!exact_match)
        return 0;
    return 1;
}

static int tnl_validation_pending(void) {
    if (!s_tnl_validation)
        return 0;
    for (uint32_t i = 0; i < s_nbatch; i++) {
        if (s_batch[i].key.tnl == 1u &&
            s_batch[i].pc.tnl[1] != 0u) {
            return 1;
        }
    }
    return 0;
}

static void dispatch_tnl_preprocess(void) {
    if (!s_tnl_compute_count) return;
    vkCmdBindDescriptorSets(
        s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_tnl_compute_layout, 0, 1,
        &s_tnl_compute_dset, 0, NULL);
    for (uint32_t i = 0; i < s_tnl_compute_count; i++) {
        const TnlComputeJob *job = &s_tnl_compute_jobs[i];
        vkCmdPushConstants(
            s_cmd, s_tnl_compute_layout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0, sizeof(*job), job);
        vkCmdBindPipeline(
            s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            s_tnl_transform_pipeline);
        vkCmdDispatch(
            s_cmd, (job->counts[0] + 63u) / 64u, 1, 1);
        if (job->counts[2] != 0u)
            continue;
        VkBufferMemoryBarrier clip_barrier = {
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
        };
        clip_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        clip_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        clip_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clip_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clip_barrier.buffer = s_tnl_clip_buf;
        clip_barrier.offset =
            (VkDeviceSize)job->offsets[0] *
            sizeof(GeTnlClipVertex);
        clip_barrier.size =
            (VkDeviceSize)job->counts[0] *
            sizeof(GeTnlClipVertex);
        vkCmdPipelineBarrier(
            s_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
            0, NULL, 1, &clip_barrier, 0, NULL);
        vkCmdBindPipeline(
            s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
            s_tnl_triangles_pipeline);
        vkCmdDispatch(
            s_cmd, (job->counts[1] + 63u) / 64u, 1, 1);
    }
    VkBufferMemoryBarrier screen_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    screen_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    screen_barrier.dstAccessMask =
        VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    screen_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    screen_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    screen_barrier.buffer = s_vbuf;
    screen_barrier.offset = 0;
    screen_barrier.size =
        (VkDeviceSize)s_nverts * sizeof(GpuVert);
    vkCmdPipelineBarrier(
        s_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0,
        0, NULL, 1, &screen_barrier, 0, NULL);
}

/* Render all pending batches into s_cur. Leaves the color image SHADER_READ_ONLY. */
static int submit_pending_impl(void) {
    int pass_open = 0;
    if (!s_nbatch || !s_cur) { if (s_nbatch == 0) batches_reset(); return 1; }
    for (uint32_t i = 0; i < s_nbatch; i++) {
        if (!s_batch[i].dset || !s_batch[i].pset) {
            fprintf(
                stderr,
                "gegpu: batch %u has incomplete descriptor state "
                "(tnl=%u)\n",
                i, s_batch[i].key.tnl);
            s_error = 1;
            batches_reset();
            return 0;
        }
    }
    Target *t = s_cur;
    if (!tex_prepare_pending()) {
        s_error = 1;
        batches_reset();
        return 0;
    }
    if (!cmd_begin()) { s_error = 1; batches_reset(); return 0; }

    for (uint32_t i = 0; i < s_nbatch; i++) {
        for (int j = 0; j < MAX_TGT; j++) {
            Target *sampled = &s_tgts[j];
            if (sampled->used && sampled != t &&
                (s_batch[i].dset == sampled->set_n ||
                 s_batch[i].dset == sampled->set_l)) {
                to_layout(s_cmd, sampled->img, VK_IMAGE_ASPECT_COLOR_BIT,
                          &sampled->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
        }
    }

    dispatch_tnl_preprocess();

    VkDeviceSize zero = 0;
    vkCmdBindDescriptorSets(
        s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout,
        1, 1, &s_dst_set, 0, NULL);
    vkCmdBindDescriptorSets(
        s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout,
        3, 1, &s_tnl_dset, 0, NULL);

    VkPipeline cur = VK_NULL_HANDLE;
    int bound_tnl = -1;
    GeVkFeedbackRegion feedback = {0};
    GeVkRegion whole = {0, 0, (int32_t)render_width(), (int32_t)texture_height()};
    for (uint32_t i = 0; i < s_nbatch; i++) {
        Batch *b = &s_batch[i];
        GeVkRegion feedback_copy;
        int copy_feedback = 0;
        if (b->feedback) {
            if (s_full_feedback) {
                feedback_copy = whole;
                copy_feedback = 1;
            } else {
                copy_feedback = ge_vk_feedback_copy_region(
                    &feedback, whole, feedback_sample_region(b, whole), &feedback_copy);
            }
        }
        if (b->feedback && !copy_feedback) s_cnt_snap_reuse++;
        if (b->key.manual)
            s_cnt_manual++;
        else
            s_cnt_direct++;
        if (copy_feedback || b->key.manual) {
            if (pass_open) {
                render_pass_end(t);
                pass_open = 0;
            }
            if (copy_feedback) snapshot_feedback(t, feedback_copy);
            if (b->key.manual) snapshot_destination(t);
            cur = VK_NULL_HANDLE;
        }
        if (!pass_open) {
            render_pass_begin(t);
            pass_open = 1;
        }
        VkPipeline p = pipe_get(&b->key);
        if (!p) continue;
        if (p != cur) { vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p); cur = p; }
        if (bound_tnl != (int)b->key.tnl) {
            if (b->key.tnl < 2u || b->key.tnl == 5u) {
                VkBuffer vertex_buffer =
                    b->key.tnl == 1u ? s_tnl_vbuf : s_vbuf;
                vkCmdBindVertexBuffers(
                    s_cmd, 0, 1, &vertex_buffer, &zero);
            }
            if (b->key.tnl == 1u || b->key.tnl == 5u)
                vkCmdBindIndexBuffer(
                    s_cmd, s_tnl_ibuf, 0, VK_INDEX_TYPE_UINT32);
            bound_tnl = b->key.tnl;
        }
        VkRect2D sc = { { b->sx, b->sy }, { (uint32_t)b->sw, (uint32_t)b->sh } };
        vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdSetBlendConstants(s_cmd, b->bconst);
        vkCmdPushConstants(
            s_cmd, s_playout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(b->pc), &b->pc);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1, &b->dset, 0, NULL);
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 2, 1, &b->pset, 0, NULL);
        if (b->key.tnl == 1u || b->key.tnl == 5u)
            vkCmdDrawIndexed(s_cmd, b->count, 1, b->first, 0, 0);
        else
            vkCmdDraw(s_cmd, b->count, 1, b->first, 0);
        if (feedback.valid && b->key.cmask) {
            GeVkRegion scissor = {b->sx, b->sy, b->sx + b->sw, b->sy + b->sh};
            if (!ge_vk_region_contains(feedback.dirty, scissor))
                ge_vk_feedback_record(&feedback, batch_damage(b, scissor));
        }
    }
    if (pass_open) render_pass_end(t);
    int validate_tnl = tnl_validation_pending();
    if (validate_tnl && s_tnl_nverts) {
        VkBufferMemoryBarrier barrier = {
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
        };
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = s_tnl_validation_buf;
        barrier.offset = 0;
        barrier.size =
            (VkDeviceSize)s_tnl_nverts *
            sizeof(GeTnlValidationVertex);
        vkCmdPipelineBarrier(
            s_cmd, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0,
            0, NULL, 1, &barrier, 0, NULL);
    }

    int ok = cmd_submit_wait();
    if (!ok) {
        s_error = 1;
        batches_reset();
        return 0;
    }
    if (validate_tnl && s_tnl_nverts &&
        !validate_tnl_outputs()) {
        s_tnl_validation_failed = 1;
        s_error = 1;
        batches_reset();
        return 0;
    }
    t->gpu_valid = 1;
    target_changed(t);
    s_host_upload_bytes +=
        (uint64_t)(s_nverts - s_tnl_compute_vertices) *
            sizeof(GpuVert) +
        (uint64_t)s_tnl_nverts * sizeof(GeTnlVertex) +
        (uint64_t)s_tnl_nindices * sizeof(uint32_t) +
        (uint64_t)s_tnl_nstates * sizeof(GeTnlGpuState);
    if (validate_tnl)
        s_host_readback_bytes +=
            (uint64_t)s_tnl_nverts *
            sizeof(GeTnlValidationVertex);
    s_cnt_submit++;
    if (s_log)
        fprintf(stderr, "GEGPU submit #%lu batches=%u verts=%u tgt=0x%08x/%u fmt=%u\n",
                s_cnt_submit, s_nbatch, s_nverts, t->fba, t->stride, t->fmt);
    batches_reset();
    return ok;
}

static int submit_pending(void) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    int result = submit_pending_impl();
    stage_add_exclusive(
        &s_stage_draws_ticks, start, children_before);
    return result;
}

static void stats_tick(void) {
    static unsigned long last_ms = 0;
    unsigned long now = (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC);
    if (now - last_ms < 5000) return;
    last_ms = now;
    fprintf(stderr, "GEGPU stats: submits=%lu tris=%lu spr=%lu lines=%lu "
            "batches[direct=%lu manual=%lu] rtt=%lu snap[feedback=%lu reused=%lu pixels=%llu destination=%lu] "
            "present[gpu=%lu cpu=%lu] upload=%lu readback=%lu aliases=%lu "
            "texup=%lu dirty=%lu xferblit=%lu pipes=%d texs=%d\n",
            s_cnt_submit, s_cnt_tri, s_cnt_spr, s_cnt_line,
            s_cnt_direct, s_cnt_manual, s_cnt_rtt, s_cnt_snap, s_cnt_snap_reuse,
            (unsigned long long)s_snap_pixels, s_cnt_dst_snap,
            s_cnt_present_gpu, s_cnt_present_cpu, s_cnt_upload,
            s_cnt_readback, s_cnt_aliases, s_cnt_texup, s_cnt_dirty,
            s_cnt_xferblit, s_pipe_n, s_tex_n);
}

/* ---- targets ----------------------------------------------------------------------------- */

/* Write the target's GPU contents back to guest VRAM (eviction, CLUT-from-VRAM). */
static int target_readback(Target *t) {
    uint32_t physical_width = render_width();
    uint32_t sample_offset = s_render_scale / 2u;
    if (!t->gpu_valid) return 1;
    if (t == s_cur && s_nbatch && !submit_pending()) return 0;
    if (t->clean_gen == t->render_gen) return 1;   /* VRAM already current */
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = physical_width;
    c.imageExtent.height = render_height();
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    if (!cmd_submit_wait()) return 0;
    s_host_readback_bytes +=
        (uint64_t)physical_width * render_height() * 4u;
    const uint32_t *src = (const uint32_t *)s_xfer_map;
    uint32_t wb = t->stride < DRAW_W ? t->stride : DRAW_W;
    for (uint32_t y = 0; y < FB_H; y++) {
        const uint32_t *row =
            src +
            (y * s_render_scale + sample_offset) * physical_width +
            sample_offset;
        if (t->fmt == 3) {
            uint32_t *dst = (uint32_t *)SR_HOST(
                (0x04000000u | t->fba) + y * t->stride * 4);
            if (s_render_scale == 1u) {
                memcpy(dst, row, wb * 4);
            } else {
                for (uint32_t x = 0; x < wb; x++)
                    dst[x] = row[x * s_render_scale];
            }
        } else {
            uint16_t *dst = (uint16_t *)SR_HOST((0x04000000u | t->fba) + y * t->stride * 2);
            for (uint32_t x = 0; x < wb; x++)
                dst[x] = (uint16_t)fb_pack(
                    row[x * s_render_scale], t->fmt);
        }
    }
    t->clean_gen = t->render_gen;
    s_cnt_readback++;
    return 1;
}

/* Fill the target's color image from guest VRAM (creation / CPU-dirty reacquire). */
static int target_materialize_upload_aliases(Target *target) {
    uint32_t target_bpp = target->fmt == 3 ? 4u : 2u;
    uint64_t target_begin = target->fba;
    uint64_t target_end =
        target_begin +
        (uint64_t)target->stride * FB_TEX_H * target_bpp;
    for (int i = 0; i < MAX_TGT; i++) {
        Target *other = &s_tgts[i];
        if (other == target || !other->used || !other->gpu_valid ||
            other->clean_gen == other->render_gen) {
            continue;
        }
        uint32_t other_bpp = other->fmt == 3 ? 4u : 2u;
        uint64_t other_begin = other->fba;
        uint64_t other_end =
            other_begin +
            (uint64_t)other->stride * FB_H * other_bpp;
        if (target_begin < other_end && other_begin < target_end) {
            if (!target_readback(other))
                return 0;
            s_cnt_aliases++;
        }
    }
    return 1;
}

static int target_upload(Target *t) {
    SrVkUploadScalePlan upload;
    if (!sr_vk_upload_scale_plan(
            FB_W, FB_TEX_H, s_render_scale, s_gpu_upload_scale,
            sr_perf_path_off(SR_PATH_GPU_UPLOAD), &upload)) {
        fprintf(stderr, "gegpu: invalid framebuffer upload dimensions\n");
        return 0;
    }
    uint32_t *physical = (uint32_t *)s_xfer_map;
    uint32_t *dst = upload.use_gpu ? physical : s_pxscratch;
    uint32_t physical_width = upload.physical_width;
    uint32_t bpp = t->fmt == 3 ? 4u : 2u;
    if (!target_materialize_upload_aliases(t))
        return 0;
    for (uint32_t y = 0; y < FB_TEX_H; y++) {
        uint64_t row_address =
            (uint64_t)(0x04000000u | t->fba) +
            (uint64_t)y * t->stride * bpp;
        uint64_t row_last =
            row_address + (uint64_t)t->stride * bpp - 1u;
        if (row_last > UINT32_MAX ||
            !sr_inrange((uint32_t)row_address) ||
            !sr_inrange((uint32_t)row_last)) {
            memset(dst + y * FB_W, 0, FB_W * sizeof(*dst));
            continue;
        }
        if (t->fmt == 3) {
            memcpy(dst + y * FB_W,
                   (const uint32_t *)SR_HOST((uint32_t)row_address),
                   t->stride * 4);
        } else {
            const uint16_t *src =
                (const uint16_t *)SR_HOST((uint32_t)row_address);
            for (uint32_t x = 0; x < t->stride; x++) dst[y * FB_W + x] = fb_unpack(src[x], t->fmt);
        }
        for (uint32_t x = t->stride; x < FB_W; x++) dst[y * FB_W + x] = 0;
    }
    if (!upload.use_gpu) {
        for (uint32_t y = 0; y < FB_TEX_H; y++) {
            const uint32_t *source_row = dst + y * FB_W;
            for (uint32_t x = 0; x < FB_W; x++)
                for (uint32_t sx = 0; sx < s_render_scale; sx++)
                    s_scaled_color_row[x * s_render_scale + sx] =
                        source_row[x];
            for (uint32_t sy = 0; sy < s_render_scale; sy++)
                memcpy(
                    physical +
                        (y * s_render_scale + sy) * physical_width,
                    s_scaled_color_row,
                    physical_width * sizeof(*physical));
        }
    }
    if (!cmd_begin()) return 0;
    if (upload.use_gpu) {
        to_layout(
            s_cmd, s_upload_scale_image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &s_upload_scale_layout,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy upload = {0};
        upload.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        upload.imageSubresource.layerCount = 1;
        upload.imageExtent.width = FB_W;
        upload.imageExtent.height = FB_TEX_H;
        upload.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(
            s_cmd, s_xfer, s_upload_scale_image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload);
        to_layout(
            s_cmd, s_upload_scale_image,
            VK_IMAGE_ASPECT_COLOR_BIT,
            &s_upload_scale_layout,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (upload.use_gpu) {
        VkImageBlit blit = {0};
        blit.srcSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = FB_W;
        blit.srcOffsets[1].y = FB_TEX_H;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1].x = (int32_t)physical_width;
        blit.dstOffsets[1].y = (int32_t)upload.physical_height;
        blit.dstOffsets[1].z = 1;
        vkCmdBlitImage(
            s_cmd, s_upload_scale_image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_NEAREST);
    } else {
        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask =
            VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = physical_width;
        copy.imageExtent.height = upload.physical_height;
        copy.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(
            s_cmd, s_xfer, t->img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    uint64_t sync_start = sr_time_now();
    int submitted = cmd_submit_wait();
    uint64_t sync_end = sr_time_now();
    if (!submitted) return 0;
    s_host_upload_bytes += upload.host_bytes;
    s_framebuffer_upload_logical_bytes += upload.logical_bytes;
    s_framebuffer_upload_host_bytes += upload.host_bytes;
    s_framebuffer_upload_expanded_pixels += upload.expanded_pixels;
    s_framebuffer_uploads++;
    s_framebuffer_upload_fallbacks += (uint64_t)upload.fallback;
    if (sync_end >= sync_start)
        s_framebuffer_upload_sync_ticks += sync_end - sync_start;
    t->gpu_valid = 1;
    target_changed(t);
    t->clean_gen = t->render_gen;          /* VRAM is the source: in sync by definition */
    s_cnt_upload++;
    return 1;
}

static void target_destroy(Target *t) {
    if (!t->used) return;
    if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
    if (t->set_n) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_n);
    if (t->set_l) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_l);
    if (t->view) vkDestroyImageView(s_dev, t->view, NULL);
    if (t->img) vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem) vkFreeMemory(s_dev, t->mem, NULL);
    memset(t, 0, sizeof(*t));
}

static DepthEnt *depth_acquire(uint32_t zba, uint32_t zstride) {
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && s_deps[i].zba == zba && s_deps[i].zstride == zstride)
            return &s_deps[i];
    DepthEnt *d = NULL;
    for (int i = 0; i < MAX_DEP; i++) if (!s_deps[i].used) { d = &s_deps[i]; break; }
    if (!d) {   /* evict slot 0 arbitrarily (depth contents are transient frame data) */
        vkDeviceWaitIdle(s_dev);
        d = &s_deps[0];
        vkDestroyImageView(s_dev, d->view, NULL);
        vkDestroyImage(s_dev, d->img, NULL);
        vkFreeMemory(s_dev, d->mem, NULL);
        /* targets holding this depth must drop their framebuffers */
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used && s_tgts[i].dep == d) {
                vkDestroyFramebuffer(s_dev, s_tgts[i].fb, NULL);
                s_tgts[i].fb = VK_NULL_HANDLE; s_tgts[i].dep = NULL;
            }
        memset(d, 0, sizeof(*d));
    }
    if (!make_image(render_width(), render_height(), VK_FORMAT_D16_UNORM,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &d->img, &d->mem)) return NULL;
    if (!make_view(d->img, VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, &d->view)) return NULL;
    d->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    d->zba = zba; d->zstride = zstride; d->used = 1;
    /* initialize from the (CPU) software z-buffer once — games clear depth before use,
     * this just avoids garbage on the very first frame */
    {
        uint16_t *physical = (uint16_t *)s_xfer_map;
        uint32_t physical_width = render_width();
        for (uint32_t y = 0; y < FB_H; y++) {
            const uint16_t *source_row = s_zbuf + y * FB_W;
            for (uint32_t x = 0; x < FB_W; x++)
                for (uint32_t sx = 0; sx < s_render_scale; sx++)
                    s_scaled_depth_row[x * s_render_scale + sx] =
                        source_row[x];
            for (uint32_t sy = 0; sy < s_render_scale; sy++)
                memcpy(
                    physical +
                        (y * s_render_scale + sy) * physical_width,
                    s_scaled_depth_row,
                    physical_width * sizeof(*physical));
        }
    }
    if (cmd_begin()) {
        to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c = {0};
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = render_width();
        c.imageExtent.height = render_height();
        c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(s_cmd, s_xfer, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        if (!cmd_submit_wait())
            return NULL;
        s_host_upload_bytes +=
            (uint64_t)render_width() * render_height() * 2u;
    }
    return d;
}

static Target *target_acquire(void) {
    uint32_t fba = s_ge->fbp & 0x001FFFFFu;
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    if (stride > 512) stride = 512;
    uint32_t fmt = s_ge->fbfmt & 3;
    uint32_t zba = s_ge->zbp & 0x001FFFFFu;
    uint32_t zstride = s_ge->zbw ? s_ge->zbw : stride;
    if (zstride > 512) zstride = 512;

    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].fba == fba) { t = &s_tgts[i]; break; }

    if (t && (t->stride != stride || t->fmt != fmt)) {
        /* same address reinterpreted: preserve pixels through VRAM */
        if (!target_readback(t)) return NULL;
        if (t == s_cur) { if (!submit_pending()) return NULL; s_cur = NULL; }
        target_destroy(t);
        t = NULL;
    }

    if (!t) {
        for (int i = 0; i < MAX_TGT; i++) if (!s_tgts[i].used) { t = &s_tgts[i]; break; }
        if (!t) {   /* LRU eviction, preserving contents */
            Target *old = NULL;
            for (int i = 0; i < MAX_TGT; i++)
                if (s_tgts[i].used && (!old || s_tgts[i].lru < old->lru) && &s_tgts[i] != s_cur)
                    old = &s_tgts[i];
            if (!old || !target_readback(old)) return NULL;
            target_destroy(old);
            t = old;
        }
        memset(t, 0, sizeof(*t));
        t->fba = fba; t->stride = stride; t->fmt = fmt; t->used = 1;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!make_image(render_width(), texture_height(),
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &t->img, &t->mem)) { t->used = 0; return NULL; }
        if (!make_view(t->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &t->view)) return NULL;
        t->set_n = make_descriptor(t->view, s_smp_n, s_dpool_fix);
        t->set_l = make_descriptor(t->view, s_smp_l, s_dpool_fix);
        if (!target_upload(t)) return NULL;   /* seed from guest VRAM */
    } else if (!t->gpu_valid) {
        if (!target_upload(t)) return NULL;   /* CPU dirtied VRAM (movie/DMA): re-seed */
    }

    DepthEnt *d = depth_acquire(zba, zstride);
    if (!d) return NULL;
    if (t->dep != d || !t->fb) {
        if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
        VkImageView views[2] = { t->view, d->view };
        VkFramebufferCreateInfo fbc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbc.renderPass = s_rp; fbc.attachmentCount = 2; fbc.pAttachments = views;
        fbc.width = render_width();
        fbc.height = render_height();
        fbc.layers = 1;
        if (vkCreateFramebuffer(s_dev, &fbc, NULL, &t->fb) != VK_SUCCESS) return NULL;
        t->dep = d;
    }
    t->lru = s_lru++;
    return t;
}

/* Ensure s_cur points at the GE's current target, submitting pending work on a switch. */
static int begin_target_impl(void) {
    uint32_t fba = s_ge->fbp & 0x001FFFFFu;
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    uint32_t zba = s_ge->zbp & 0x001FFFFFu;
    uint32_t zstride = s_ge->zbw ? s_ge->zbw : stride;
    if (stride > 512) stride = 512;
    if (zstride > 512) zstride = 512;
    if (s_cur && s_cur->used && s_cur->fba == fba &&
        s_cur->fmt == (s_ge->fbfmt & 3) &&
        s_cur->stride == stride && s_cur->gpu_valid &&
        s_cur->dep && s_cur->dep->zba == zba &&
        s_cur->dep->zstride == zstride) {
        s_cur->lru = s_lru++;
        return 1;
    }
    if (s_nbatch && !submit_pending()) return 0;
    s_cur = target_acquire();
    return s_cur != NULL;
}

static int begin_target(void) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    int result = begin_target_impl();
    stage_add_exclusive(
        &s_stage_framebuffer_ticks, start, children_before);
    return result;
}

/* ---- texture cache (RAM textures, decoded by ge.c's reference sampler) ------------------- */

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int tex_hash(uint64_t *result) {
    uint64_t bytes = ge_texture_level_byte_size(s_ge, 0);
    uint64_t last = (uint64_t)s_ge->tex_addr + bytes - 1u;
    if (!bytes || bytes > UINT32_MAX || last > UINT32_MAX ||
        !sr_inrange(s_ge->tex_addr) || !sr_inrange((uint32_t)last)) {
        fprintf(stderr, "gegpu: invalid texture range at 0x%08x (%llu bytes)\n",
                s_ge->tex_addr, (unsigned long long)bytes);
        return 0;
    }
    sr_prepare_read(s_ge->tex_addr, (uint32_t)bytes);
    if (s_error) return 0;
    const uint8_t *p = (const uint8_t *)SR_HOST(s_ge->tex_addr);
    uint64_t h = 1469598103934665603ull;
    h = fnv64(h, &bytes, sizeof(bytes));
    *result = fnv64(h, p, (size_t)bytes);
    return 1;
}

static uint64_t s_texlru = 1;

static int tex_upload(VkImage img, const uint32_t *px, int w, int h);

static int tex_decode_pixels(
    const GeTexSnapshot *snapshot, uint32_t *pixels,
    uint32_t width, uint32_t height, uint32_t scale,
    uint64_t *decode_ticks, uint64_t *scale_ticks) {
    uint32_t *decoded = NULL;
    uint32_t *target = pixels;
    if (scale > 1u) {
        uint64_t bytes =
            (uint64_t)width * height * sizeof(uint32_t);
        if (bytes > SIZE_MAX) return 0;
        decoded = (uint32_t *)malloc((size_t)bytes);
        if (!decoded) return 0;
        target = decoded;
    }

    uint64_t start = sr_time_now();
    if (snapshot)
        ge_tex_snapshot_decode_level(snapshot, target, 0);
    else
        ge_decode_tex_rgba_level(target, 0);
    *decode_ticks += sr_time_now() - start;

    if (scale > 1u) {
        start = sr_time_now();
        int scaled = sr_texture_scale_rgba(
            decoded, width, height, scale, pixels);
        *scale_ticks += sr_time_now() - start;
        free(decoded);
        if (!scaled) return 0;
    }
    return 1;
}

static int tex_decode_worker(void *unused) {
    (void)unused;
    for (;;) {
        SDL_LockMutex(s_decode_mutex);
        while (!s_decode_head && !s_decode_stopping)
            SDL_WaitCondition(s_decode_wake, s_decode_mutex);
        TexDecodeJob *job = s_decode_head;
        if (job) {
            s_decode_head = job->next;
            if (!s_decode_head) s_decode_tail = NULL;
        }
        SDL_UnlockMutex(s_decode_mutex);
        if (!job) return 0;

        int ok = tex_decode_pixels(
            job->snapshot, job->pixels,
            job->width, job->height, job->scale,
            &job->decode_ticks, &job->scale_ticks);
        SDL_LockMutex(s_decode_mutex);
        job->ok = ok;
        job->done = 1;
        SDL_BroadcastCondition(s_decode_done);
        SDL_UnlockMutex(s_decode_mutex);
    }
}

static int tex_decode_pool_start(void) {
    if (s_decode_pool_started)
        return s_decode_thread_count != 0;
    s_decode_pool_started = 1;
    s_decode_mutex = SDL_CreateMutex();
    s_decode_wake = SDL_CreateCondition();
    s_decode_done = SDL_CreateCondition();
    if (!s_decode_mutex || !s_decode_wake || !s_decode_done)
        goto fail;

    int cores = SDL_GetNumLogicalCPUCores();
    uint32_t target = cores > 0 ? (uint32_t)cores / 4u : 1u;
    if (target < 1u) target = 1u;
    if (target > 3u) target = 3u;
    for (uint32_t i = 0; i < target; i++) {
        SDL_Thread *thread = SDL_CreateThread(
            tex_decode_worker, "PSP texture decode", NULL);
        if (!thread) break;
        s_decode_threads[s_decode_thread_count++] = thread;
    }
    if (s_decode_thread_count) return 1;

fail:
    fprintf(
        stderr,
        "gegpu: texture decode workers unavailable (%s); "
        "decoding in line\n",
        SDL_GetError());
    if (s_decode_done) SDL_DestroyCondition(s_decode_done);
    if (s_decode_wake) SDL_DestroyCondition(s_decode_wake);
    if (s_decode_mutex) SDL_DestroyMutex(s_decode_mutex);
    s_decode_done = NULL;
    s_decode_wake = NULL;
    s_decode_mutex = NULL;
    s_sync_texture_decode = 1;
    return 0;
}

static void tex_decode_pool_stop(void) {
    if (!s_decode_pool_started || !s_decode_mutex) {
        s_decode_pool_started = 0;
        return;
    }
    SDL_LockMutex(s_decode_mutex);
    s_decode_stopping = 1;
    SDL_BroadcastCondition(s_decode_wake);
    SDL_UnlockMutex(s_decode_mutex);
    for (uint32_t i = 0; i < s_decode_thread_count; i++) {
        SDL_WaitThread(s_decode_threads[i], NULL);
        s_decode_threads[i] = NULL;
    }
    s_decode_thread_count = 0;
    SDL_DestroyCondition(s_decode_done);
    SDL_DestroyCondition(s_decode_wake);
    SDL_DestroyMutex(s_decode_mutex);
    s_decode_done = NULL;
    s_decode_wake = NULL;
    s_decode_mutex = NULL;
    s_decode_head = NULL;
    s_decode_tail = NULL;
    s_decode_stopping = 0;
    s_decode_pool_started = 0;
}

static void tex_decode_record_wait(uint64_t ticks) {
    uint64_t frequency = sr_time_frequency();
    uint64_t wait_us = frequency
        ? (uint64_t)((double)ticks * 1000000.0 /
                     (double)frequency)
        : 0;
    s_texture_decode_wait_ticks += ticks;
    sr_perf_counter_add(SR_PERF_TEXTURE_DECODE_WAIT_US, wait_us);
}

static int tex_decode_job_wait(TexDecodeJob *job) {
    uint64_t start = 0;
    SDL_LockMutex(s_decode_mutex);
    if (!job->done) start = sr_time_now();
    while (!job->done)
        SDL_WaitCondition(s_decode_done, s_decode_mutex);
    SDL_UnlockMutex(s_decode_mutex);
    if (start)
        tex_decode_record_wait(sr_time_now() - start);
    return job->ok;
}

static void tex_decode_job_free(TexDecodeJob *job) {
    if (!job) return;
    ge_tex_snapshot_free(job->snapshot);
    free(job->pixels);
    free(job->verify);
    if (job->bytes <= s_pending_decode_bytes)
        s_pending_decode_bytes -= job->bytes;
    else
        s_pending_decode_bytes = 0;
    free(job);
}

static int tex_gpu_decode(TexEnt *entry, TexDecodeJob *job) {
    uint32_t source_bytes = 0;
    uint32_t clut_bytes = 0;
    const uint8_t *source = ge_tex_snapshot_source_data(
        job->snapshot, 0, &source_bytes);
    const uint8_t *clut = ge_tex_snapshot_clut_data(
        job->snapshot, &clut_bytes);
    GeTextureRawJob raw_job;
    if (!source ||
        source_bytes > MAX_GPU_TEXTURE_SOURCE_BYTES ||
        !ge_tex_snapshot_build_raw_job(
            job->snapshot, 0, 0, 0, 0,
            job->width, job->height, &raw_job)) {
        fprintf(
            stderr,
            "gegpu: invalid pending GPU texture decode "
            "(fmt=%u %ux%u source=%u)\n",
            entry->key.format, job->width, job->height,
            source_bytes);
        return 0;
    }
    if ((raw_job.flags & GE_TEXTURE_RAW_INDEXED) != 0 &&
        (!clut || clut_bytes != GE_TEXTURE_RAW_CLUT_BYTES)) {
        fprintf(stderr, "gegpu: invalid GPU texture CLUT payload\n");
        return 0;
    }

    *s_tex_decode_job_map = raw_job;
    memcpy(s_tex_decode_source_map, source, source_bytes);
    if ((raw_job.flags & GE_TEXTURE_RAW_INDEXED) != 0)
        memcpy(s_tex_decode_clut_map, clut, clut_bytes);

    uint64_t start = sr_time_now();
    if (!cmd_begin()) return 0;
    vkCmdBindPipeline(
        s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_tex_decode_pipeline);
    vkCmdBindDescriptorSets(
        s_cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        s_tex_decode_layout, 0, 1,
        &s_tex_decode_dset, 0, NULL);
    uint32_t job_index = 0;
    vkCmdPushConstants(
        s_cmd, s_tex_decode_layout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0,
        sizeof(job_index), &job_index);
    vkCmdDispatch(
        s_cmd, (job->width + 7u) / 8u,
        (job->height + 7u) / 8u, 1);

    VkBufferMemoryBarrier output_barrier = {
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER
    };
    output_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    output_barrier.dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_HOST_READ_BIT;
    output_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    output_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    output_barrier.buffer = s_xfer;
    output_barrier.offset = 0;
    output_barrier.size =
        (VkDeviceSize)job->width * job->height * sizeof(uint32_t);
    vkCmdPipelineBarrier(
        s_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, NULL, 1, &output_barrier, 0, NULL);

    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_layout(
        s_cmd, entry->img, VK_IMAGE_ASPECT_COLOR_BIT,
        &layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {0};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = job->width;
    copy.imageExtent.height = job->height;
    copy.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(
        s_cmd, s_xfer, entry->img,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    to_layout(
        s_cmd, entry->img, VK_IMAGE_ASPECT_COLOR_BIT,
        &layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait()) return 0;

    uint64_t ticks = sr_time_now() - start;
    s_texture_decode_ticks += ticks;
    tex_decode_record_wait(ticks);
    if (job->verify) {
        uint64_t pixels = (uint64_t)job->width * job->height;
        const uint32_t *gpu = (const uint32_t *)s_xfer_map;
        uint64_t differing = 0;
        uint64_t first = 0;
        for (uint64_t i = 0; i < pixels; i++) {
            if (job->verify[i] == gpu[i]) continue;
            if (!differing) first = i;
            differing++;
        }
        if (differing) {
            fprintf(
                stderr,
                "gegpu: GPU texture decode mismatch "
                "(fmt=%u %ux%u differing=%llu/%llu first=%llu "
                "cpu=%08x gpu=%08x)\n",
                entry->key.format, entry->key.width,
                entry->key.height,
                (unsigned long long)differing,
                (unsigned long long)pixels,
                (unsigned long long)first,
                job->verify[first], gpu[first]);
            abort();
        }
    }
    s_host_upload_bytes += sizeof(raw_job) + source_bytes;
    if ((raw_job.flags & GE_TEXTURE_RAW_INDEXED) != 0)
        s_host_upload_bytes += clut_bytes;
    return 1;
}

static int tex_finish_job(TexEnt *entry, int upload) {
    TexDecodeJob *job = entry->job;
    if (!job) return 1;
    if (job->gpu) {
        int ok = !upload || tex_gpu_decode(entry, job);
        entry->job = NULL;
        tex_decode_job_free(job);
        return ok;
    }
    int ok = tex_decode_job_wait(job);
    s_texture_decode_ticks += job->decode_ticks;
    s_texture_scale_ticks += job->scale_ticks;
    if (ok && job->verify) {
        uint64_t pixels =
            (uint64_t)job->width * job->height *
            job->scale * job->scale;
        uint64_t differing = 0;
        uint64_t first = 0;
        for (uint64_t i = 0; i < pixels; i++) {
            if (job->verify[i] == job->pixels[i]) continue;
            if (!differing) first = i;
            differing++;
        }
        if (differing) {
            fprintf(
                stderr,
                "gegpu: snapshot texture decode mismatch "
                "(fmt=%u %ux%u differing=%llu/%llu first=%llu "
                "live=%08x snapshot=%08x)\n",
                entry->key.format, entry->key.width, entry->key.height,
                (unsigned long long)differing,
                (unsigned long long)pixels,
                (unsigned long long)first,
                job->verify[first], job->pixels[first]);
            abort();
        }
    }
    if (ok && upload)
        ok = tex_upload(
            entry->img, job->pixels, entry->w, entry->h);
    entry->job = NULL;
    tex_decode_job_free(job);
    return ok;
}

static void tex_gpu_decode_fallback(void) {
    s_gpu_texture_decode_fallbacks++;
    sr_perf_counter_add(
        SR_PERF_GPU_TEXTURE_DECODE_FALLBACKS, 1);
}

static int tex_schedule_gpu_decode(
    TexEnt *entry, uint32_t width, uint32_t height) {
    if (!s_gpu_texture_decode ||
        sr_perf_path_off(SR_PATH_GPU_TEXTURE_DECODE)) {
        return 0;
    }
    GeTexSnapshot *snapshot = ge_tex_snapshot_capture(1);
    uint32_t source_bytes = 0;
    const uint8_t *source = ge_tex_snapshot_source_data(
        snapshot, 0, &source_bytes);
    GeTextureRawJob raw_job;
    if (!source || source_bytes > MAX_GPU_TEXTURE_SOURCE_BYTES ||
        !ge_tex_snapshot_build_raw_job(
            snapshot, 0, 0, 0, 0,
            width, height, &raw_job)) {
        ge_tex_snapshot_free(snapshot);
        tex_gpu_decode_fallback();
        return 0;
    }
    uint64_t bytes = sizeof(raw_job) + source_bytes;
    if ((raw_job.flags & GE_TEXTURE_RAW_INDEXED) != 0)
        bytes += GE_TEXTURE_RAW_CLUT_BYTES;
    uint64_t verify_bytes = s_verify_texture_decode
        ? (uint64_t)width * height * sizeof(uint32_t)
        : 0;
    if (verify_bytes > SIZE_MAX ||
        bytes > UINT64_MAX - verify_bytes) {
        ge_tex_snapshot_free(snapshot);
        return -1;
    }
    bytes += verify_bytes;
    if (bytes > MAX_PENDING_DECODE_BYTES - s_pending_decode_bytes) {
        ge_tex_snapshot_free(snapshot);
        tex_gpu_decode_fallback();
        return 0;
    }

    TexDecodeJob *job =
        (TexDecodeJob *)calloc(1, sizeof(*job));
    if (!job) {
        ge_tex_snapshot_free(snapshot);
        tex_gpu_decode_fallback();
        return 0;
    }
    job->snapshot = snapshot;
    job->width = width;
    job->height = height;
    job->scale = 1;
    job->gpu = 1;
    if (s_verify_texture_decode) {
        job->verify = (uint32_t *)malloc((size_t)verify_bytes);
        if (!job->verify ||
            !tex_decode_pixels(
                snapshot, job->verify, width, height, 1,
                &s_texture_decode_ticks,
                &s_texture_scale_ticks)) {
            fprintf(
                stderr,
                "gegpu: cannot create GPU texture decode "
                "verification output\n");
            tex_decode_job_free(job);
            return -1;
        }
    }
    job->bytes = bytes;
    s_pending_decode_bytes += bytes;
    entry->job = job;
    s_gpu_texture_decodes++;
    sr_perf_counter_add(SR_PERF_GPU_TEXTURE_DECODE_JOBS, 1);
    sr_perf_counter_add(
        SR_PERF_GPU_TEXTURE_DECODE_SOURCE_BYTES, source_bytes);
    return 1;
}

static int tex_schedule_decode(TexEnt *entry, int width, int height) {
    if (s_sync_texture_decode ||
        sr_perf_path_off(SR_PATH_TEXTURE_DECODE)) {
        return 0;
    }
    int gpu_scheduled = tex_schedule_gpu_decode(
        entry, (uint32_t)width, (uint32_t)height);
    if (gpu_scheduled != 0) return gpu_scheduled;
    uint64_t pixels =
        (uint64_t)(uint32_t)width * (uint32_t)height *
        s_texture_scale * s_texture_scale;
    if (!pixels || pixels > SIZE_MAX / sizeof(uint32_t))
        return -1;
    uint64_t bytes = pixels * sizeof(uint32_t);
    if (bytes > MAX_PENDING_DECODE_BYTES - s_pending_decode_bytes)
        return 0;
    if (!tex_decode_pool_start()) return 0;

    GeTexSnapshot *snapshot = ge_tex_snapshot_capture(1);
    TexDecodeJob *job = snapshot
        ? (TexDecodeJob *)calloc(1, sizeof(*job))
        : NULL;
    if (!job) {
        ge_tex_snapshot_free(snapshot);
        return 0;
    }
    job->snapshot = snapshot;
    job->pixels = (uint32_t *)malloc((size_t)bytes);
    if (!job->pixels) {
        tex_decode_job_free(job);
        return 0;
    }
    job->width = (uint32_t)width;
    job->height = (uint32_t)height;
    job->scale = s_texture_scale;
    if (s_verify_texture_decode) {
        job->verify = (uint32_t *)malloc((size_t)bytes);
        if (!job->verify ||
            !tex_decode_pixels(
                NULL, job->verify, job->width, job->height,
                job->scale, &s_texture_decode_ticks,
                &s_texture_scale_ticks)) {
            fprintf(
                stderr,
                "gegpu: cannot create synchronous texture "
                "decode verification output\n");
            tex_decode_job_free(job);
            return -1;
        }
    }

    job->bytes = bytes;
    s_pending_decode_bytes += bytes;
    entry->job = job;
    s_async_texture_decodes++;
    SDL_LockMutex(s_decode_mutex);
    if (s_decode_tail)
        s_decode_tail->next = job;
    else
        s_decode_head = job;
    s_decode_tail = job;
    SDL_UnlockMutex(s_decode_mutex);
    SDL_SignalCondition(s_decode_wake);
    return 1;
}

/* Evict the least-recently-used quarter of the cache (the working set survives; the
 * old full-clear rebuilt EVERY texture every frame once a scene exceeded the cap). */
static void tex_evict_lru(void) {
    vkDeviceWaitIdle(s_dev);
    int goal = s_tex_n - MAX_TEX / 4;
    while (s_tex_n > goal) {
        int v = 0;
        for (int i = 1; i < s_tex_n; i++)
            if (s_tex[i].lru < s_tex[v].lru) v = i;
        TexEnt *e = &s_tex[v];
        if (!tex_finish_job(e, 0))
            fprintf(stderr, "gegpu: discarded texture decode failed\n");
        vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
        vkDestroySampler(s_dev, e->smp, NULL);
        vkDestroyImageView(s_dev, e->view, NULL);
        vkDestroyImage(s_dev, e->img, NULL);
        vkFreeMemory(s_dev, e->mem, NULL);
        *e = s_tex[--s_tex_n];
    }
}

static int tex_upload(VkImage img, const uint32_t *px, int w, int h) {
    memcpy(s_xfer_map, px, (size_t)w * (size_t)h * 4);
    if (!cmd_begin()) return 0;
    VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED;
    to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, &lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)w; bic.imageExtent.height = (uint32_t)h; bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_xfer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, &lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait())
        return 0;
    s_host_upload_bytes += (uint64_t)(uint32_t)w * (uint32_t)h * 4u;
    return 1;
}

static int tex_prepare_pending(void) {
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *entry = &s_tex[i];
        if (!entry->pending || !entry->job) continue;
        if (!tex_finish_job(entry, 1)) {
            fprintf(
                stderr,
                "gegpu: background texture decode or upload failed\n");
            return 0;
        }
    }
    return 1;
}

static const uint32_t *tex_decode_scaled(int w, int h) {
    uint64_t start = sr_time_now();
    ge_decode_tex_rgba(s_texscratch);
    s_texture_decode_ticks += sr_time_now() - start;
    if (s_texture_scale == 1u) return s_texscratch;
    start = sr_time_now();
    if (!s_texscaled || !sr_texture_scale_rgba(
            s_texscratch, (uint32_t)w, (uint32_t)h,
            s_texture_scale, s_texscaled)) {
        fprintf(stderr, "gegpu: xBRZ texture scaling failed\n");
        return NULL;
    }
    s_texture_scale_ticks += sr_time_now() - start;
    return s_texscaled;
}

static void tex_release(VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkSampler *smp) {
    if (*smp) vkDestroySampler(s_dev, *smp, NULL);
    if (*view) vkDestroyImageView(s_dev, *view, NULL);
    if (*img) vkDestroyImage(s_dev, *img, NULL);
    if (*mem) vkFreeMemory(s_dev, *mem, NULL);
    *smp = VK_NULL_HANDLE;
    *view = VK_NULL_HANDLE;
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
}

static int tex_create(
    int w, int h, int linear, int clamp_u, int clamp_v,
    VkImage *img, VkDeviceMemory *mem,
    VkImageView *view, VkSampler *smp) {
    if (!make_image((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img, mem))
        goto fail;
    if (!make_view(*img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, view)) goto fail;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkResult result = vkCreateSampler(s_dev, &sci, NULL, smp);
    if (result != VK_SUCCESS) {
        fprintf(stderr, "gegpu: vkCreateSampler failed: %d\n", (int)result);
        goto fail;
    }
    return 1;
fail:
    tex_release(img, mem, view, smp);
    return 0;
}

static int tex_make(
    const uint32_t *px, int w, int h,
    int linear, int clamp_u, int clamp_v,
    VkImage *img, VkDeviceMemory *mem,
    VkImageView *view, VkSampler *smp) {
    if (!tex_create(
            w, h, linear, clamp_u, clamp_v,
            img, mem, view, smp)) {
        return 0;
    }
    if (tex_upload(*img, px, w, h)) return 1;
    tex_release(img, mem, view, smp);
    return 0;
}

static VkDescriptorSet palette_get(void) {
    uint32_t format = s_ge->clut_fmt & 3u;
    uint64_t hash = fnv64(1469598103934665603ull, s_ge->clutram, sizeof(s_ge->clutram));
    hash = fnv64(hash, &format, sizeof(format));
    PaletteEnt *entry = NULL;
    int replace = s_palette_n >= MAX_PALETTE;
    for (int i = 0; i < s_palette_n; i++) {
        PaletteEnt *candidate = &s_palettes[i];
        if (candidate->hash == hash && candidate->format == format &&
            memcmp(candidate->data, s_ge->clutram, sizeof(candidate->data)) == 0) {
            candidate->lru = s_texlru++;
            return candidate->set;
        }
    }
    if (s_palette_n < MAX_PALETTE) {
        entry = &s_palettes[s_palette_n];
    } else {
        if (!submit_pending()) return VK_NULL_HANDLE;
        entry = &s_palettes[0];
        for (int i = 1; i < MAX_PALETTE; i++)
            if (s_palettes[i].lru < entry->lru) entry = &s_palettes[i];
        vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &entry->set);
        vkDestroySampler(s_dev, entry->smp, NULL);
        vkDestroyImageView(s_dev, entry->view, NULL);
        vkDestroyImage(s_dev, entry->img, NULL);
        vkFreeMemory(s_dev, entry->mem, NULL);
    }
    memset(entry, 0, sizeof(*entry));
    uint32_t pixels[512];
    for (uint32_t i = 0; i < 512; i++) {
        uint32_t raw = 0;
        uint32_t bytes = format == 3 ? 4u : 2u;
        memcpy(&raw, s_ge->clutram + ((i * bytes) & 2047u), bytes);
        pixels[i] = fb_unpack(raw, format);
    }
    if (!tex_make(pixels, 512, 1, 0, 1, 1,
                  &entry->img, &entry->mem, &entry->view, &entry->smp)) {
        fprintf(stderr, "gegpu: failed to create framebuffer palette\n");
        goto fail;
    }
    entry->set = make_descriptor(entry->view, entry->smp, s_dpool_tex);
    if (!entry->set) {
        fprintf(stderr, "gegpu: failed to allocate framebuffer palette descriptor\n");
        tex_release(&entry->img, &entry->mem, &entry->view, &entry->smp);
        goto fail;
    }
    memcpy(entry->data, s_ge->clutram, sizeof(entry->data));
    entry->hash = hash;
    entry->format = format;
    entry->lru = s_texlru++;
    if (!replace) s_palette_n++;
    return entry->set;
fail:
    if (replace) *entry = s_palettes[--s_palette_n];
    return VK_NULL_HANDLE;
}

static VkDescriptorSet tex_get_impl(void) {
    int w = s_ge->tex_w, h = s_ge->tex_h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) {
        fprintf(stderr, "gegpu: invalid texture dimensions %dx%d\n", w, h);
        return VK_NULL_HANDLE;
    }
    int linear  = (s_ge->tex_filter & 1) || ((s_ge->tex_filter >> 8) & 1);
    int clamp_u = s_ge->tex_wrap & 1, clamp_v = (s_ge->tex_wrap >> 8) & 1;
    TexKey key;
    memset(&key, 0, sizeof(key));
    key.addr = s_ge->tex_addr;
    key.format = s_ge->tex_fmt;
    key.width = (uint32_t)w;
    key.height = (uint32_t)h;
    key.stride = s_ge->tex_bufw;
    key.swizzle = (uint32_t)s_ge->tex_swizzle;
    key.linear = (uint32_t)linear;
    key.clamp_u = (uint32_t)clamp_u;
    key.clamp_v = (uint32_t)clamp_v;
    key.palette_hash = ge_vk_texture_palette_key(s_ge);
    uint64_t hash;
    if (!tex_hash(&hash)) return VK_NULL_HANDLE;
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        if (memcmp(&e->key, &key, sizeof(key)) != 0) continue;
        e->lru = s_texlru++;
        if (e->hash == hash) {
            s_texture_cache_hits++;
            e->pending = 1;
            return e->set;
        }
        /* same texture state, new contents: update in place (cache stays bounded) */
        s_texture_cache_misses++;
        sr_perf_counter_add(SR_PERF_TEXTURE_MISSES, 1);
        if (e->pending && !submit_pending()) return VK_NULL_HANDLE;
        if (e->job && !tex_finish_job(e, 0))
            return VK_NULL_HANDLE;
        int scheduled = tex_schedule_decode(e, w, h);
        if (scheduled < 0) return VK_NULL_HANDLE;
        if (!scheduled) {
            const uint32_t *pixels = tex_decode_scaled(w, h);
            if (!pixels || !tex_upload(e->img, pixels, e->w, e->h))
                return VK_NULL_HANDLE;
        }
        e->hash = hash;
        e->pending = 1;
        s_cnt_texup++;
        return e->set;
    }

    if (s_tex_n >= MAX_TEX) {
        if (!submit_pending()) return VK_NULL_HANDLE;
        tex_evict_lru();
    }
    s_texture_cache_misses++;
    sr_perf_counter_add(SR_PERF_TEXTURE_MISSES, 1);
    TexEnt *e = &s_tex[s_tex_n];
    memset(e, 0, sizeof(*e));
    int scaled_w = w * (int)s_texture_scale;
    int scaled_h = h * (int)s_texture_scale;
    e->key = key;
    e->hash = hash;
    e->w = scaled_w;
    e->h = scaled_h;
    int scheduled = tex_schedule_decode(e, w, h);
    if (scheduled < 0) return VK_NULL_HANDLE;
    if (scheduled) {
        if (!tex_create(
                scaled_w, scaled_h, linear, clamp_u, clamp_v,
                &e->img, &e->mem, &e->view, &e->smp)) {
            tex_finish_job(e, 0);
            return VK_NULL_HANDLE;
        }
    } else {
        const uint32_t *pixels = tex_decode_scaled(w, h);
        if (!pixels ||
            !tex_make(
                pixels, scaled_w, scaled_h,
                linear, clamp_u, clamp_v,
                &e->img, &e->mem, &e->view, &e->smp)) {
            return VK_NULL_HANDLE;
        }
    }
    e->set = make_descriptor(e->view, e->smp, s_dpool_tex);
    if (!e->set) {
        fprintf(stderr, "gegpu: failed to allocate texture descriptor\n");
        tex_finish_job(e, 0);
        tex_release(&e->img, &e->mem, &e->view, &e->smp);
        return VK_NULL_HANDLE;
    }
    e->pending = 1;
    e->lru = s_texlru++;
    s_cnt_texup++;
    s_tex_n++;
    return e->set;
}

static VkDescriptorSet tex_get(void) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    VkDescriptorSet result = tex_get_impl();
    stage_add_exclusive(
        &s_stage_textures_ticks, start, children_before);
    return result;
}

/* ---- capture --------------------------------------------------------------------------- */

static float snap16(float v) { return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f); }

static void put_vert(float x, float y, float z, float rw, float u, float v, float fog,
                     int r, int g, int b, int a) {
    GpuVert *o = &s_vmap[s_nverts++];
    o->x = snap16(x); o->y = snap16(y); o->z = z; o->rw = rw;
    o->u = u; o->v = v; o->fog = fog;
    uint32_t cr = (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    uint32_t cg = (uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    uint32_t cb = (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    uint32_t ca = (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    o->rgba = cr | (cg << 8) | (cb << 16) | (ca << 24);
}

static int map_factor(
    uint32_t f, int src_side, uint32_t fixed, int *need_const) {
    switch (f & 0xF) {
        case 0: return src_side ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
        case 1: return src_side ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 2: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 4: return VK_BLEND_FACTOR_DST_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:
            if ((fixed & 0xFFFFFF) == 0)        return VK_BLEND_FACTOR_ZERO;
            if ((fixed & 0xFFFFFF) == 0xFFFFFF) return VK_BLEND_FACTOR_ONE;
            *need_const = 1;
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
    }
}

static int build_state(int persp, int sprite, Batch *b) {
    GeState *g = s_ge;
    PipeKey *k = &b->key;
    uint32_t flags;
    int direct;
    int textured;
    memset(b, 0, sizeof(*b));
    ge_vk_shader_state_build(
        g, persp, sprite, s_render_scale, &b->pc);
    flags = (uint32_t)b->pc.config[0] >> 8;
    direct = (flags & GE_VK_SHADER_DIRECT) != 0;
    textured = (flags & GE_VK_SHADER_TEXTURE) != 0;

    /* scissor, clamped to the drawable region */
    int sx1 = g->scis_x1 > 0 ? g->scis_x1 : 0, sy1 = g->scis_y1 > 0 ? g->scis_y1 : 0;
    int sx2 = g->scis_x2 < DRAW_W - 1 ? g->scis_x2 : DRAW_W - 1;
    int sy2 = g->scis_y2 < FB_H - 1 ? g->scis_y2 : FB_H - 1;
    if (sx1 > sx2 || sy1 > sy2) { b->sw = 0; return 1; }
    b->sx = sx1 * (int32_t)s_render_scale;
    b->sy = sy1 * (int32_t)s_render_scale;
    b->sw = (sx2 - sx1 + 1) * (int32_t)s_render_scale;
    b->sh = (sy2 - sy1 + 1) * (int32_t)s_render_scale;

    k->manual = direct ? 0 : 1;
    if (!direct) {
        k->cmask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        k->ztest = 1;
        k->zwrite = 1;
        k->zfunc = VK_COMPARE_OP_ALWAYS;
    } else {
        k->cmask = VK_COLOR_COMPONENT_R_BIT |
                   VK_COLOR_COMPONENT_G_BIT |
                   VK_COLOR_COMPONENT_B_BIT;
        if (ge_vk_shader_direct_stencil_write(g))
            k->cmask |= VK_COLOR_COMPONENT_A_BIT;
        k->ztest = g->ztest_enable ? 1 : 0;
        k->zwrite = (k->ztest && !g->zwrite_disable) ? 1 : 0;
        static const uint8_t zmap[8] = {
            VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS,
            VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
            VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL,
            VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL,
        };
        k->zfunc = k->ztest
            ? zmap[g->ztest & 7] : VK_COMPARE_OP_ALWAYS;
    }

    b->bconst[3] = 1.0f;
    if (direct && g->blend_enable) {
        uint32_t eq = (g->blend_mode >> 8) & 7;
        int nc_a = 0, nc_b = 0;
        int sf = map_factor(
            g->blend_mode & 0xF, 1, g->blend_fixa, &nc_a);
        int df = map_factor(
            (g->blend_mode >> 4) & 0xF, 0, g->blend_fixb, &nc_b);
        uint32_t fixed = nc_a ? g->blend_fixa : g->blend_fixb;
        b->bconst[0] = (float)(fixed & 0xFF) / 255.0f;
        b->bconst[1] = (float)((fixed >> 8) & 0xFF) / 255.0f;
        b->bconst[2] = (float)((fixed >> 16) & 0xFF) / 255.0f;
        k->blend_on = 1;
        k->srcf = (uint8_t)sf; k->dstf = (uint8_t)df;
        switch (eq) {
            case 1:  k->eq = VK_BLEND_OP_SUBTRACT; break;
            case 2:  k->eq = VK_BLEND_OP_REVERSE_SUBTRACT; break;
            case 3:  k->eq = VK_BLEND_OP_MIN; break;          /* factors ignored: approx */
            case 4:  k->eq = VK_BLEND_OP_MAX; break;
            case 5:  k->eq = VK_BLEND_OP_SUBTRACT; break;     /* absdiff: approx */
            default: k->eq = VK_BLEND_OP_ADD; break;
        }
    }

    /* culling: draw_prim already reordered vertices; rasterizer keeps positive winding */
    k->cull = (!sprite && g->cull_enable && !g->clear)
        ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;

    if (textured) {
        {
            const char *setting = getenv("SR_GPU_PSP_SAMPLER");
            if (s_texture_scale == 1u &&
                (!setting || !setting[0] || strcmp(setting, "0") != 0)) {
                flags |= GE_VK_SHADER_PSP_SAMPLER;
            }
        }
    }

    /* texture binding: a render target sampled directly (render-to-texture), or the
     * RAM-texture cache, or white for untextured */
    b->dset = s_white_set;
    b->pset = s_white_set;
    if (textured) {
        Target *src = NULL;
        if ((g->tex_addr & 0x0F000000u) == 0x04000000u) {
            uint32_t ta = g->tex_addr & 0x001FFFFFu;
            for (int i = 0; i < MAX_TGT; i++) {
                Target *ti = &s_tgts[i];
                if (!ti->used || !ti->gpu_valid) continue;
                uint32_t bpp_t = ti->fmt == 3 ? 4 : 2;
                uint32_t flen = ti->stride * FB_H * bpp_t;
                if (ta >= ti->fba && ta < ti->fba + flen &&
                    (!src || ti->lru > src->lru)) src = ti;
            }
        }
        if (src) {
            static int nortt = -1;
            if (nortt < 0) { const char *nv = getenv("SR_GPU_NORTT");
                             nortt = (nv && nv[0] && strcmp(nv, "0") != 0) ? 1 : 0; }
            int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
            uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
            uint32_t toff = (g->tex_addr & 0x001FFFFFu) - src->fba;
            uint32_t pix = toff / bpp_s;
            s_cnt_rtt++;
            /* GPU-direct: bind the target image, addressing any pixel-aligned sub-rect
             * through the texel offset in texsize.zw — no readback, no CPU decode.
             * tex_fmt must EQUAL the target's psm: reinterpreting one 16-bit format as
             * another scrambles channels/alpha and must go through VRAM pack/unpack.
             * SR_GPU_NORTT=1 forces the readback path for all draws (debug bisect). */
            int indexed_fb =
                ge_vk_indexed_framebuffer_compatible(g, src->fmt, src->stride, toff);
            if (!nortt && (g->tex_fmt == src->fmt || indexed_fb) && !g->tex_swizzle &&
                ge_texture_buffer_width(g->tex_fmt, g->tex_bufw) == src->stride &&
                (toff % bpp_s) == 0 && g->tex_w > 0 && g->tex_h > 0 &&
                (uint32_t)g->tex_w <= FB_W - pix % src->stride &&
                (uint32_t)g->tex_h <= FB_TEX_H - pix / src->stride) {
                if (indexed_fb) {
                    b->pset = palette_get();
                    if (!b->pset) return 0;
                    flags |= GE_VK_SHADER_INDEXED_FRAMEBUFFER | GE_VK_SHADER_PSP_SAMPLER;
                }
                if (src == s_cur) {
                    b->feedback = 1;
                    b->dset = linear ? s_snap_l : s_snap_n;
                } else {
                    if (s_nbatch && src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                        !submit_pending()) return 0;
                    if (src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
                        if (!cmd_begin()) return 0;
                        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        if (!cmd_submit_wait()) return 0;
                    }
                    b->dset = linear ? src->set_l : src->set_n;
                }
                /* Texel coordinates address the PSP texture dimensions within the
                 * target's 512x512 sampling envelope. */
                b->pc.texture[2] =
                    (uint32_t)g->tex_w |
                    ((uint32_t)g->tex_h << 16);
                b->pc.texture[3] =
                    (pix % src->stride) |
                    ((pix / src->stride) << 16);
                flags |= GE_VK_SHADER_FRAMEBUFFER;
            } else {
                /* incompatible stride/format reinterpretation: VRAM + decoder (rare) */
                if (!target_readback(src)) return 0;
                b->dset = tex_get();
            }
        } else {
            b->dset = tex_get();
        }
        if (!b->dset) return 0;
    }
    b->pc.config[0] =
        (int32_t)((g->tex_func & 7u) | (flags << 8));
    return 1;
}

/* ---- built-state template (state rebuilt per CHANGE, not per primitive) ------------------ */

typedef struct {
    uint32_t clear, clear_mode;
    uint32_t fbp, fbw, zbp, zbw;
    uint32_t fbfmt;
    int blend_enable; uint32_t blend_mode, fixa, fixb;
    uint32_t mask_rgb; int mask_alpha;
    int scis[4];
    int ztest_enable, zwrite_disable; uint32_t ztest, minz, maxz;
    int cull_enable; uint32_t cull;
    int tex_enable; uint32_t tex_addr, tex_bufw, tex_fmt, tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h; int tex_swizzle;
    uint32_t clut_fmt, clut_gen;
    int atest_enable; uint32_t atest;
    int color_test_enable; uint32_t color_test, color_ref, color_test_mask;
    int stencil_test_enable; uint32_t stencil_test, stencil_op;
    int logic_op_enable; uint32_t logic_op;
    int dither_enable; int8_t dith[4][4];
    int fog_enable; uint32_t fog_color;
    int persp, sprite;
    uint32_t flushgen;
} StateSnap;
static StateSnap s_snapst;
static Batch     s_tmpl;
static int       s_tmpl_ok = 0;
static uint32_t  s_tmpl_draw_serial;

static void snap_fill(StateSnap *o, int persp, int sprite) {
    GeState *g = s_ge;
    memset(o, 0, sizeof(*o));
    o->clear = (uint32_t)g->clear; o->clear_mode = g->clear_mode;
    o->fbp = g->fbp; o->fbw = g->fbw; o->zbp = g->zbp; o->zbw = g->zbw;
    o->fbfmt = g->fbfmt;
    o->blend_enable = g->blend_enable; o->blend_mode = g->blend_mode;
    o->fixa = g->blend_fixa; o->fixb = g->blend_fixb;
    o->mask_rgb = g->mask_rgb; o->mask_alpha = g->mask_alpha;
    o->scis[0] = g->scis_x1; o->scis[1] = g->scis_y1; o->scis[2] = g->scis_x2; o->scis[3] = g->scis_y2;
    o->ztest_enable = g->ztest_enable; o->zwrite_disable = g->zwrite_disable;
    o->ztest = g->ztest; o->minz = g->minz; o->maxz = g->maxz;
    o->cull_enable = g->cull_enable; o->cull = g->cull;
    o->tex_enable = g->tex_enable; o->tex_addr = g->tex_addr; o->tex_bufw = g->tex_bufw;
    o->tex_fmt = g->tex_fmt; o->tex_wrap = g->tex_wrap; o->tex_func = g->tex_func;
    o->tex_env = g->tex_env; o->tex_filter = g->tex_filter;
    o->tex_w = g->tex_w; o->tex_h = g->tex_h; o->tex_swizzle = g->tex_swizzle;
    o->clut_fmt = g->clut_fmt; o->clut_gen = g->clut_gen;
    o->atest_enable = g->atest_enable; o->atest = g->atest;
    o->color_test_enable = g->color_test_enable;
    o->color_test = g->color_test;
    o->color_ref = g->color_ref;
    o->color_test_mask = g->color_test_mask;
    o->stencil_test_enable = g->stencil_test_enable;
    o->stencil_test = g->stencil_test;
    o->stencil_op = g->stencil_op;
    o->logic_op_enable = g->logic_op_enable;
    o->logic_op = g->logic_op;
    o->dither_enable = g->dither_enable;
    memcpy(o->dith, g->dith, sizeof(o->dith));
    o->fog_enable = g->fog_enable; o->fog_color = g->fog_color;
    o->persp = persp; o->sprite = sprite;
    o->flushgen = s_flushgen;
}

static int state_get(int persp, int sprite, Batch *out) {
    if (s_error) return 0;
    StateSnap sn; snap_fill(&sn, persp, sprite);
    uint32_t draw_serial = ge_debug_context().draw_serial;
    if (s_tmpl_ok && !memcmp(&sn, &s_snapst, sizeof(sn)) &&
        (!s_tmpl.feedback || draw_serial == s_tmpl_draw_serial)) {
        *out = s_tmpl;
        return 1;
    }
    if (!begin_target() || !build_state(persp, sprite, &s_tmpl)) {
        fprintf(stderr, "gegpu: failed to prepare draw state\n");
        s_error = 1;
        s_tmpl_ok = 0;
        return 0;
    }
    snap_fill(&s_snapst, persp, sprite);  /* re-fill: build_state may have submitted */
    s_tmpl_ok = 1;
    s_tmpl_draw_serial = draw_serial;
    *out = s_tmpl;
    return 1;
}

static void append(Batch *b, uint32_t first, uint32_t count) {
    if (b->sw <= 0 || count == 0) return;
    if (s_stage_log)
        s_stage_vertices += count;
    b->draw_serial = ge_debug_context().draw_serial;
    if (s_nbatch) {
        Batch *last = &s_batch[s_nbatch - 1];
        if (last->first + last->count == first &&
            (!(b->key.manual || b->feedback) || last->draw_serial == b->draw_serial) &&
            last->feedback == b->feedback &&
            !memcmp(&last->key, &b->key, sizeof(b->key)) &&
            last->sx == b->sx && last->sy == b->sy && last->sw == b->sw && last->sh == b->sh &&
            !memcmp(last->bconst, b->bconst, sizeof(b->bconst)) &&
            !memcmp(&last->pc, &b->pc, sizeof(b->pc)) && last->dset == b->dset &&
            last->pset == b->pset) {
            last->count += count;
            return;
        }
    }
    b->first = first; b->count = count;
    s_batch[s_nbatch++] = *b;
    if (s_stage_log)
        s_stage_batches++;
}

static int ensure_room(uint32_t verts) {
    if (s_error) return 0;
    if (s_nverts + verts > MAX_VERTS || s_nbatch >= MAX_BATCH - 1)
        return submit_pending();
    return 1;
}

static int hook_transform_draw(const GeTnlDraw *draw) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("SR_GPU_TNL");
        enabled =
            value && value[0] && strcmp(value, "0") != 0 ? 1 : 0;
    }
    int points = draw && draw->primitive == GE_TNL_POINTS;
    int sprites = draw && draw->primitive == GE_TNL_SPRITES;
    int triangles = draw &&
        (draw->primitive == GE_TNL_TRIANGLES ||
         draw->primitive == GE_TNL_TRIANGLE_STRIP ||
         draw->primitive == GE_TNL_TRIANGLE_FAN);
    if (!enabled || !s_ready || !draw || !draw->state ||
        draw->state->abi_version != GE_TNL_ABI_VERSION ||
        !draw->vertices || !draw->indices ||
        draw->vertex_count == 0 || draw->index_count == 0 ||
        (!points && !sprites && !triangles) ||
        (triangles && draw->index_count % 3u != 0) ||
        (sprites && draw->index_count % 2u != 0) ||
        (points &&
         (s_tnl_validation ||
          draw->index_count > MAX_TNL_INDICES / 6u)) ||
        (sprites &&
         (s_tnl_validation ||
          draw->index_count > MAX_TNL_INDICES / 3u)) ||
        draw->vertex_count > MAX_VERTS ||
        draw->index_count > MAX_TNL_INDICES) {
        return 0;
    }
    int preprocess =
        (draw->flags & GE_TNL_DRAW_PREPROCESS) != 0;
    if (preprocess && !triangles)
        return 0;
    if (preprocess && !s_tnl_preprocess_supported)
        return 0;
    uint32_t draw_class = preprocess
        ? GE_VK_TNL_CLASS_PREPROCESS
        : points
            ? GE_VK_TNL_CLASS_POINTS
            : sprites
                ? GE_VK_TNL_CLASS_SPRITES
                : GE_VK_TNL_CLASS_DIRECT;
    if ((s_gpu_tnl_classes & draw_class) == 0)
        return 0;
    int direct_compute =
        !preprocess && !points && !sprites &&
        s_tnl_direct_compute && s_tnl_preprocess_supported &&
        !s_tnl_validation;
    uint32_t compute_vertices = direct_compute
        ? draw->vertex_count : 0u;
    if (preprocess) {
        uint32_t triangle_count = draw->index_count / 3u;
        if (triangle_count > MAX_VERTS / 6u)
            return 0;
        compute_vertices = triangle_count * 6u;
    }
    int compute_output = preprocess || direct_compute;
    for (uint32_t i = 0; i < draw->index_count; i++)
        if (draw->indices[i] >= draw->vertex_count) return 0;
    if (draw->vertex_count > MAX_VERTS - s_tnl_nverts ||
        draw->index_count > MAX_TNL_INDICES - s_tnl_nindices ||
        (compute_output &&
         (compute_vertices > MAX_VERTS - s_nverts ||
          s_tnl_compute_count >= MAX_BATCH)) ||
        s_tnl_nstates >= MAX_BATCH ||
        s_nbatch >= MAX_BATCH - 1) {
        if (!submit_pending()) return 1;
    }
    if (draw->vertex_count > MAX_VERTS - s_tnl_nverts ||
        draw->index_count > MAX_TNL_INDICES - s_tnl_nindices ||
        (compute_output &&
         (compute_vertices > MAX_VERTS - s_nverts ||
          s_tnl_compute_count >= MAX_BATCH)) ||
        s_tnl_nstates >= MAX_BATCH ||
        s_nbatch >= MAX_BATCH - 1) {
        return 0;
    }

    Batch batch;
    if (!state_get(1, points || sprites, &batch)) return 1;
    if (batch.sw <= 0) return 1;
    if (!begin_target()) {
        s_error = 1;
        return 1;
    }
    uint32_t state_index = s_tnl_nstates;
    ge_tnl_gpu_state_build(
        draw->state, &s_tnl_smap[s_tnl_nstates++]);

    uint32_t vertex_base = s_tnl_nverts;
    uint32_t index_first = s_tnl_nindices;
    uint32_t output_base = s_nverts;
    uint32_t indexed_vertex_base =
        direct_compute ? output_base : vertex_base;
    memcpy(
        s_tnl_vmap + vertex_base, draw->vertices,
        (size_t)draw->vertex_count * sizeof(*draw->vertices));
    for (uint32_t i = 0; i < draw->index_count; i++)
        s_tnl_imap[s_tnl_nindices++] =
            indexed_vertex_base + draw->indices[i];
    s_tnl_nverts += draw->vertex_count;

    batch.draw_serial = ge_debug_context().draw_serial;
    if (compute_output) {
        TnlComputeJob *job =
            &s_tnl_compute_jobs[s_tnl_compute_count++];
        memset(job, 0, sizeof(*job));
        job->offsets[0] = vertex_base;
        job->offsets[1] = index_first;
        job->offsets[2] = state_index;
        job->offsets[3] = output_base;
        job->counts[0] = draw->vertex_count;
        job->counts[1] =
            preprocess ? draw->index_count / 3u : 0u;
        job->counts[2] = direct_compute ? 1u : 0u;
        s_nverts += compute_vertices;
        s_tnl_compute_vertices += compute_vertices;
        memset(batch.pc.tnl, 0, sizeof(batch.pc.tnl));
        if (preprocess) {
            batch.key.tnl = 0;
            batch.preprocess = 1;
            batch.first = output_base;
            batch.count = compute_vertices;
        } else {
            batch.key.tnl = 5;
            batch.first = index_first;
            batch.count = draw->index_count;
        }
    } else if (points) {
        batch.key.tnl = 2;
        batch.pc.tnl[0] = state_index;
        batch.pc.tnl[1] = 0;
        batch.pc.tnl[2] = index_first;
        batch.pc.tnl[3] = 0;
        batch.first = 0;
        batch.count = draw->index_count * 6u;
    } else if (sprites) {
        batch.key.tnl = 3;
        batch.pc.tnl[0] = state_index;
        batch.pc.tnl[1] = 0;
        batch.pc.tnl[2] = index_first;
        batch.pc.tnl[3] =
            (draw->flags &
             GE_TNL_DRAW_SPRITE_LINEAR_FILTER) != 0
                ? 0x80000000u : 0u;
        batch.first = 0;
        batch.count = draw->index_count * 3u;
    } else {
        batch.key.tnl = 1;
        batch.pc.tnl[0] = state_index;
        batch.pc.tnl[1] =
            s_tnl_validation &&
            sr_present_count() >= s_tnl_validation_start;
        batch.first = index_first;
        batch.count = draw->index_count;
    }
    s_batch[s_nbatch++] = batch;
    if (points)
        s_cnt_line += draw->index_count;
    else if (sprites)
        s_cnt_spr += draw->index_count / 2u;
    else
        s_cnt_tri += draw->index_count / 3u;
    if (s_stage_log) {
        s_stage_vertices += preprocess ? compute_vertices
            : points ? draw->index_count * 6u
            : sprites ? draw->index_count * 3u
            : draw->index_count;
        s_stage_batches++;
    }
    sr_perf_counter_add(
        SR_PERF_GPU_TNL_UPLOAD_BYTES,
        (uint64_t)draw->vertex_count * sizeof(*draw->vertices) +
            (uint64_t)draw->index_count * sizeof(uint32_t) +
            sizeof(GeTnlGpuState));
    return 1;
}

static int hook_through_sprite_compact(
    const GeVtx *first, const GeVtx *second) {
    if (!s_gpu_primitive_expansion || s_tnl_validation ||
        sr_perf_path_off(SR_PATH_GPU_PRIMITIVE_EXPANSION)) {
        return 0;
    }

    uint32_t required = 2u + (s_tnl_nverts & 1u);
    if (s_tnl_nverts > MAX_VERTS - required ||
        s_nbatch >= MAX_BATCH - 1u) {
        if (!submit_pending()) return 1;
    }
    required = 2u + (s_tnl_nverts & 1u);
    if (s_tnl_nverts > MAX_VERTS - required ||
        s_nbatch >= MAX_BATCH - 1u) {
        return 0;
    }

    Batch batch;
    if (!state_get(0, 1, &batch)) return 1;
    if (!begin_target()) {
        s_error = 1;
        return 1;
    }
    if (s_tnl_nverts & 1u) {
        memset(
            &s_tnl_vmap[s_tnl_nverts++], 0,
            sizeof(*s_tnl_vmap));
    }
    uint32_t vertex_base = s_tnl_nverts;
    const GeVtx *source[2] = {first, second};
    for (uint32_t i = 0; i < 2u; i++) {
        GeTnlVertex *destination =
            &s_tnl_vmap[s_tnl_nverts++];
        memset(destination, 0, sizeof(*destination));
        destination->position[0] = source[i]->x;
        destination->position[1] = source[i]->y;
        destination->position[2] = source[i]->z;
        destination->texcoord[0] = source[i]->u;
        destination->texcoord[1] = source[i]->v;
        destination->color[0] = (uint8_t)source[i]->r;
        destination->color[1] = (uint8_t)source[i]->g;
        destination->color[2] = (uint8_t)source[i]->b;
        destination->color[3] = (uint8_t)source[i]->a;
    }

    batch.key.tnl = 4u;
    memset(batch.pc.tnl, 0, sizeof(batch.pc.tnl));
    if ((s_ge->tex_filter & 1u) != 0u ||
        ((s_ge->tex_filter >> 8u) & 1u) != 0u) {
        batch.pc.tnl[3] = 0x80000000u;
    }
    append(&batch, vertex_base * 3u, 6u);
    s_cnt_spr++;
    return 1;
}

/* ---- primitive hooks ---------------------------------------------------------------------- */

static int hook_tri_impl(
    const GeVtx *A, const GeVtx *B, const GeVtx *C, int persp) {
    if (!s_ready) return 0;
    if (!ensure_room(3)) return 1;
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    Batch b;
    if (!state_get(persp, 0, &b)) return 1;
    if (nocull) b.key.cull = VK_CULL_MODE_NONE;
    if (!begin_target()) { s_error = 1; return 1; }
    uint32_t first = s_nverts;
    /* flat shading: provoking vertex is the LAST one on the PSP; clear triangles always
     * use C's color (raster_tri clear branch) */
    int flat = !s_ge->shade_gouraud || s_ge->clear;
    const GeVtx *ca = flat ? C : A;
    const GeVtx *cb = flat ? C : B;
    put_vert(A->x, A->y, A->z, A->rw, A->u, A->v, A->fog, ca->r, ca->g, ca->b, ca->a);
    put_vert(B->x, B->y, B->z, B->rw, B->u, B->v, B->fog, cb->r, cb->g, cb->b, cb->a);
    put_vert(C->x, C->y, C->z, C->rw, C->u, C->v, C->fog, C->r, C->g, C->b, C->a);
    append(&b, first, 3);
    s_cnt_tri++;
    return 1;
}

static int hook_sprite_impl(
    const GeVtx *p0, const GeVtx *p1, int persp) {
    if (!s_ready) return 0;
    if (!persp && hook_through_sprite_compact(p0, p1))
        return 1;
    if (!ensure_room(6)) return 1;
    Batch b;
    if (!state_get(persp, 1, &b)) return 1;
    if (!begin_target()) { s_error = 1; return 1; }

    float xa = floorf(fminf(p0->x, p1->x)), xb = floorf(fmaxf(p0->x, p1->x));
    float ya = floorf(fminf(p0->y, p1->y)), yb = floorf(fmaxf(p0->y, p1->y));
    if (xb <= xa || yb <= ya) return 1;

    float u0 = p0->u, v0 = p0->v, u1 = p1->u, v1 = p1->v;
    if (persp) {
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    float uw = (u1 - u0) / (xb - xa), vw = (v1 - v0) / (yb - ya);
    int linear =
        (s_ge->tex_filter & 1u) != 0 ||
        ((s_ge->tex_filter >> 8) & 1u) != 0;
    float ua = linear ? u0 : u0 - 0.5f * uw;
    float ub = linear ? u1 : u1 - 0.5f * uw;
    float va = linear ? v0 : v0 - 0.5f * vw;
    float vb = linear ? v1 : v1 - 0.5f * vw;

    float z = p1->z, fog = p1->fog;
    int r = p1->r, g = p1->g, bb_ = p1->b, a = p1->a;

    uint32_t first = s_nverts;
    put_vert(xa, ya, z, 1.0f, ua, va, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xb, yb, z, 1.0f, ub, vb, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    append(&b, first, 6);
    s_cnt_spr++;
    return 1;
}

/* Lines as 1-pixel-wide quads (the software DDA's pixel set, approximately). */
static int hook_line_impl(
    const GeVtx *A, const GeVtx *B, int persp) {
    if (!s_ready) return 0;
    if (!ensure_room(6)) return 1;
    Batch b;
    if (!state_get(persp, 1, &b)) return 1;
    if (!begin_target()) { s_error = 1; return 1; }

    float dx = B->x - A->x, dy = B->y - A->y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;   /* half-width perpendicular */
    float ex = dx / len * 0.5f, ey = dy / len * 0.5f;    /* endpoint extension */
    float ax = A->x + 0.5f, ay = A->y + 0.5f;            /* DDA truncates; sample centers */
    float bx = B->x + 0.5f, by = B->y + 0.5f;

    /* flat shading uses B (the provoking vertex) */
    int flat = !s_ge->shade_gouraud;
    const GeVtx *cA = flat ? B : A;

    uint32_t first = s_nverts;
    put_vert(ax - ex - nx, ay - ey - ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex + nx, by + ey + ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    append(&b, first, 6);
    s_cnt_line++;
    return 1;
}

static int hook_point_impl(const GeVtx *A, int persp) {
    if (!s_ready) return 0;
    if (!ensure_room(6)) return 1;
    Batch b;
    if (!state_get(persp, 1, &b)) return 1;
    if (!begin_target()) { s_error = 1; return 1; }
    float x0 = floorf(A->x), y0 = floorf(A->y);
    uint32_t first = s_nverts;
    put_vert(x0,        y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    append(&b, first, 6);
    s_cnt_line++;
    return 1;
}

static int hook_tri(
    const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    int result = hook_tri_impl(a, b, c, persp);
    stage_add_exclusive(
        &s_stage_vertices_ticks, start, children_before);
    return result;
}

static int hook_sprite(
    const GeVtx *a, const GeVtx *b, int persp) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    int result = hook_sprite_impl(a, b, persp);
    stage_add_exclusive(
        &s_stage_vertices_ticks, start, children_before);
    return result;
}

static int hook_line(
    const GeVtx *a, const GeVtx *b, int persp) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    int result = hook_line_impl(a, b, persp);
    stage_add_exclusive(
        &s_stage_vertices_ticks, start, children_before);
    return result;
}

static int hook_point(const GeVtx *a, int persp) {
    uint64_t start = stage_counter();
    uint64_t children_before = stage_children();
    int result = hook_point_impl(a, persp);
    stage_add_exclusive(
        &s_stage_vertices_ticks, start, children_before);
    return result;
}

/* ---- CPU writes to VRAM (movie decode, DMA): invalidate overlapping targets --------------- */

static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (!s_ready) return;
    if ((addr & 0x0F000000u) != 0x04000000u) return;
    uint32_t a0 = addr & 0x001FFFFFu;
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        uint32_t flen = t->stride * FB_TEX_H * bpp_t;
        if (a0 < t->fba + flen && t->fba < a0 + bytes) {
            if (t == s_cur && s_nbatch && !submit_pending()) return;
            t->gpu_valid = 0;             /* next use re-seeds from guest VRAM */
            s_cnt_dirty++;
        }
    }
}

static void hook_vram_read(uint32_t addr, uint32_t bytes) {
    if (!s_ready || !bytes ||
        (addr & 0x0F000000u) != 0x04000000u) {
        return;
    }
    uint32_t a0 = addr & 0x001FFFFFu;
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid) {
            continue;
        }
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        uint32_t flen = t->stride * FB_H * bpp_t;
        if (a0 < t->fba + flen && t->fba < a0 + bytes) {
            if (t == s_cur && s_nbatch && !submit_pending()) return;
            if (t->clean_gen != t->render_gen && !target_readback(t)) {
                s_error = 1;
                return;
            }
        }
    }
}

static void hook_vram_write(uint32_t addr, uint32_t bytes) {
    hook_vram_read(addr, bytes);
}

/* ---- GE block transfer: GPU-side image blit ------------------------------------------------ */

/* Find the target whose address range contains VRAM offset `a` (gpu_valid not required). */
static Target *target_containing(uint32_t a) {
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        if (a >= t->fba && a < t->fba + t->stride * FB_H * bpp_t) return t;
    }
    return NULL;
}

/* Perform the block transfer as a vkCmdCopyImage between two framebuffer targets. Returns 1
 * on success (guest VRAM left stale for the destination, like any rendered-to target); 0
 * falls back to the CPU copy in ge.c (readback + memmove + dirty-invalidate). */
static int hook_xfer(uint32_t startdata) {
    if (!s_ready) return 0;
    uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
    uint32_t dstBase = (s_ge->xf_dst & 0xFFFFF0u) | ((s_ge->xf_dstw & 0xFF0000u) << 8);
    if ((srcBase & 0x0F000000u) != 0x04000000u || (dstBase & 0x0F000000u) != 0x04000000u)
        return 0;                                       /* RAM endpoint: CPU path */
    uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
    uint32_t dstStride = s_ge->xf_dstw & 0x7F8u;
    uint32_t bpp = (startdata & 1) ? 4 : 2;
    Target *src = target_containing(srcBase & 0x001FFFFFu);
    Target *dst = target_containing(dstBase & 0x001FFFFFu);
    if (!src || !dst || src == dst || !src->gpu_valid) return 0;
    uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
    /* raw-byte copy must match the converted RGBA images texel-for-texel */
    if (src->fmt != dst->fmt || bpp_s != bpp) return 0;
    if (src->stride != srcStride || dst->stride != dstStride) return 0;
    uint32_t soff = ((srcBase & 0x001FFFFFu) - src->fba) / bpp;
    uint32_t doff = ((dstBase & 0x001FFFFFu) - dst->fba) / bpp;
    uint32_t sx = (s_ge->xf_spos & 0x3FFu) + soff % src->stride;
    uint32_t sy = ((s_ge->xf_spos >> 10) & 0x3FFu) + soff / src->stride;
    uint32_t dx = (s_ge->xf_dpos & 0x3FFu) + doff % dst->stride;
    uint32_t dy = ((s_ge->xf_dpos >> 10) & 0x3FFu) + doff / dst->stride;
    uint32_t w = (s_ge->xf_size & 0x3FFu) + 1, h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
    if (sx + w > FB_W || dx + w > FB_W || sy + h > FB_H || dy + h > FB_H) return 0;
    if (!dst->gpu_valid && !target_upload(dst)) return 0;   /* seed rows we don't overwrite */
    if (s_nbatch && (s_cur == src || s_cur == dst) && !submit_pending()) return 0;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.srcOffset.x = (int32_t)(sx * s_render_scale);
    ic.srcOffset.y = (int32_t)(sy * s_render_scale);
    ic.dstOffset.x = (int32_t)(dx * s_render_scale);
    ic.dstOffset.y = (int32_t)(dy * s_render_scale);
    ic.extent.width = w * s_render_scale;
    ic.extent.height = h * s_render_scale;
    ic.extent.depth = 1;
    vkCmdCopyImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait()) return 0;
    target_changed(dst);
    dst->lru = s_lru++;
    s_cnt_xferblit++;
    return 1;
}

/* ---- GE sync points ------------------------------------------------------------------------ */

void gegpu_flush(const char *reason) {
    if (!s_ready) return;
    if (reason && strcmp(reason, "listend") == 0) { stats_tick(); return; }
    if (reason && strcmp(reason, "texflush") == 0) {
        /* Re-evaluate the texture state; pending draws retain their own bindings. */
        s_flushgen++;
        return;
    }
    if (reason && strcmp(reason, "xfersrc") == 0) {
        /* GE block transfer is about to READ guest memory: materialize any GPU-resident
         * target overlapping the source rows into guest VRAM (the transfer itself runs on
         * the CPU in ge.c; the destination range is invalidated via vram_dirty after). */
        if (!submit_pending()) return;
        uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
        if ((srcBase & 0x0F000000u) != 0x04000000u) return;
        uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
        uint32_t srcY = (s_ge->xf_spos >> 10) & 0x3FFu;
        uint32_t h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
        uint32_t a0 = srcBase & 0x001FFFFFu;
        uint32_t bytes = (srcY + h) * (srcStride ? srcStride : 512u) * 4u;  /* conservative */
        for (int i = 0; i < MAX_TGT; i++) {
            Target *t = &s_tgts[i];
            if (!t->used || !t->gpu_valid) continue;
            uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
            uint32_t flen = t->stride * FB_H * bpp_t;
            if (a0 < t->fba + flen && t->fba < a0 + bytes)
                if (!target_readback(t)) { s_error = 1; return; }
        }
        return;
    }
    if (reason && strcmp(reason, "loadclut") == 0) {
        /* the CLUT loader is about to READ guest VRAM: materialize any target there */
        uint32_t ca = s_ge->clut_addr & 0x001FFFFFu;
        for (int i = 0; i < MAX_TGT; i++) {
            Target *t = &s_tgts[i];
            if (!t->used || !t->gpu_valid) continue;
            uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
            uint32_t flen = t->stride * FB_H * bpp_t;
            if (ca < t->fba + flen && t->fba < ca + 2048)
                if (!target_readback(t)) { s_error = 1; return; }
        }
        return;
    }
    submit_pending();
}

/* Present GPU-resident targets directly; return -1 for guest-VRAM presentation and
 * -2 for a backend failure. */
int gegpu_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    (void)stride;
    if (s_error) return -2;
    if (!s_ready) return -1;
    uint32_t fba = fbaddr & 0x001FFFFFu;
    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].fba == fba) { t = &s_tgts[i]; break; }
    if (!t || !t->gpu_valid || (int)t->fmt != (fmt & 3)) {
        s_cnt_present_cpu++;
        stage_publish(0);
        return -1;
    }
    if (t == s_cur && s_nbatch && !submit_pending()) return -2;

    static int safe = -1;
    if (safe < 0) { const char *sv = getenv("SR_GPU_SAFE"); safe = (sv && sv[0] && strcmp(sv, "0") != 0) ? 1 : 0; }
    if (safe && !target_readback(t)) {
        s_error = 1;
        return -2;
    }

    s_cnt_present_gpu++;
    stats_tick();
    int transitioned;
    uint64_t present_start = stage_counter();
    int result = sdl3vk_present_image(
        (void *)t->img,
        DRAW_W * s_render_scale,
        FB_H * s_render_scale,
        t->present_gen, (uint32_t)t->layout, &transitioned);
    uint64_t present_ticks =
        present_start ? sr_time_now() - present_start : 0;
    if (transitioned) t->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    stage_publish(present_ticks);
    return result;
}

/* ---- init ----------------------------------------------------------------------------------- */

static int gegpu_init(void);
static void gegpu_shutdown(void);

static const GeBackend k_backend = {
    "vk", gegpu_init, gegpu_present, gegpu_shutdown,
    hook_tri, NULL, hook_sprite, hook_line, hook_point, gegpu_flush,
    hook_vram_dirty, hook_vram_read, hook_vram_write, hook_xfer,
    NULL, hook_transform_draw,
};

const GeBackend *gegpu_backend(void) { return &k_backend; }

uint64_t gegpu_shader_hash(uint64_t hash) {
    hash = sr_vk_pipeline_cache_hash(
        hash, k_vert_spv, sizeof(k_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_vert_spv, sizeof(k_tnl_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_validation_vert_spv,
        sizeof(k_tnl_validation_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_points_vert_spv,
        sizeof(k_tnl_points_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_sprites_vert_spv,
        sizeof(k_tnl_sprites_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_through_sprites_vert_spv,
        sizeof(k_tnl_through_sprites_vert_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_transform_comp_spv,
        sizeof(k_tnl_transform_comp_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_tnl_triangles_comp_spv,
        sizeof(k_tnl_triangles_comp_spv));
    hash = sr_vk_pipeline_cache_hash(
        hash, k_texture_decode_comp_spv,
        sizeof(k_texture_decode_comp_spv));
    return sr_vk_pipeline_cache_hash(
        hash, k_frag_spv, sizeof(k_frag_spv));
}

int gegpu_last_backend_timing(GeVkBackendTiming *timing) {
    if (!timing || !s_last_backend_timing.flip)
        return 0;
    *timing = s_last_backend_timing;
    return 1;
}

int gegpu_framebuffer_stats(GeVkFramebufferStats *stats) {
    if (!stats || !s_ready)
        return 0;
    stats->readbacks = s_cnt_readback;
    stats->aliases = s_cnt_aliases;
    stats->feedback_snapshots = s_cnt_snap;
    stats->feedback_snapshot_pixels = s_snap_pixels;
    stats->feedback_reuses = s_cnt_snap_reuse;
    stats->pipeline_key_hits = s_pipeline_key_hits;
    stats->pipelines_created = s_pipelines_created;
    stats->pipeline_create_us =
        stage_microseconds(s_pipeline_create_ticks);
    stats->texture_cache_hits = s_texture_cache_hits;
    stats->texture_cache_misses = s_texture_cache_misses;
    stats->texture_uploads = s_cnt_texup;
    stats->texture_decode_us =
        stage_microseconds(s_texture_decode_ticks);
    stats->texture_scale_us =
        stage_microseconds(s_texture_scale_ticks);
    stats->host_upload_bytes = s_host_upload_bytes;
    stats->host_readback_bytes = s_host_readback_bytes;
    stats->framebuffer_upload_logical_bytes =
        s_framebuffer_upload_logical_bytes;
    stats->framebuffer_upload_host_bytes =
        s_framebuffer_upload_host_bytes;
    stats->framebuffer_upload_expanded_pixels =
        s_framebuffer_upload_expanded_pixels;
    stats->framebuffer_uploads = s_framebuffer_uploads;
    stats->framebuffer_upload_fallbacks =
        s_framebuffer_upload_fallbacks;
    stats->framebuffer_upload_sync_us =
        stage_microseconds(s_framebuffer_upload_sync_ticks);
    return 1;
}

static int gegpu_init(void) {
    s_error = 0;
    s_tmpl_ok = 0;
    const char *stage_log = getenv("SR_VULKAN_STAGE_LOG");
    s_stage_log =
        stage_log && stage_log[0] && strcmp(stage_log, "0") != 0;
    s_stage_flip = 0;
    memset(&s_last_backend_timing, 0, sizeof(s_last_backend_timing));
    stage_reset();
    s_cnt_readback = 0;
    s_cnt_upload = 0;
    s_cnt_texup = 0;
    s_cnt_aliases = 0;
    s_cnt_snap = 0;
    s_cnt_snap_reuse = 0;
    s_snap_pixels = 0;
    s_pipeline_key_hits = 0;
    s_pipelines_created = 0;
    s_pipeline_create_ticks = 0;
    s_texture_cache_hits = 0;
    s_texture_cache_misses = 0;
    s_texture_decode_ticks = 0;
    s_texture_scale_ticks = 0;
    s_texture_decode_wait_ticks = 0;
    s_async_texture_decodes = 0;
    s_gpu_texture_decodes = 0;
    s_gpu_texture_decode_fallbacks = 0;
    s_pending_decode_bytes = 0;
    s_decode_thread_count = 0;
    s_decode_head = NULL;
    s_decode_tail = NULL;
    s_decode_stopping = 0;
    s_decode_pool_started = 0;
    s_host_upload_bytes = 0;
    s_host_readback_bytes = 0;
    s_framebuffer_upload_logical_bytes = 0;
    s_framebuffer_upload_host_bytes = 0;
    s_framebuffer_upload_expanded_pixels = 0;
    s_framebuffer_uploads = 0;
    s_framebuffer_upload_fallbacks = 0;
    s_framebuffer_upload_sync_ticks = 0;
    s_host_memory_type = UINT32_MAX;
    s_device_memory_type = UINT32_MAX;
    s_pipeline_cache = VK_NULL_HANDLE;
    s_tnl_validation_vs = VK_NULL_HANDLE;
    s_tnl_points_vs = VK_NULL_HANDLE;
    s_tnl_sprites_vs = VK_NULL_HANDLE;
    s_tnl_through_sprites_vs = VK_NULL_HANDLE;
    s_upload_scale_image = VK_NULL_HANDLE;
    s_upload_scale_memory = VK_NULL_HANDLE;
    s_upload_scale_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_gpu_upload_scale = 0;
    s_gpu_texture_decode = 0;
    if (!ge_vk_tnl_class_mask(
            getenv("SR_VULKAN_GPU_TNL_CLASSES"),
            &s_gpu_tnl_classes)) {
        fprintf(
            stderr,
            "gegpu: SR_VULKAN_GPU_TNL_CLASSES must be a "
            "comma-separated subset of direct,preprocess,points,sprites\n");
        return 0;
    }
    {
        const char *primitive_expansion =
            getenv("SR_VULKAN_GPU_PRIMITIVE_EXPANSION");
        s_gpu_primitive_expansion =
            primitive_expansion && primitive_expansion[0] &&
            strcmp(primitive_expansion, "0") != 0;
    }
    {
        const char *direct_compute =
            getenv("SR_VULKAN_GPU_TNL_DIRECT_COMPUTE");
        s_tnl_direct_compute =
            direct_compute && direct_compute[0] &&
            strcmp(direct_compute, "0") != 0;
    }
    const char *tnl_validation = getenv("SR_GPU_TNL_VALIDATE");
    s_tnl_validation =
        tnl_validation && tnl_validation[0] &&
        strcmp(tnl_validation, "0") != 0;
    const char *tnl_validation_exact =
        getenv("SR_GPU_TNL_VALIDATE_EXACT");
    s_tnl_validation_exact =
        tnl_validation_exact && tnl_validation_exact[0] &&
        strcmp(tnl_validation_exact, "0") != 0;
    s_tnl_validation_start = 0;
    const char *tnl_validation_start =
        getenv("SR_GPU_TNL_VALIDATE_START");
    if (tnl_validation_start && tnl_validation_start[0]) {
        char *end = NULL;
        errno = 0;
        unsigned long parsed =
            strtoul(tnl_validation_start, &end, 10);
        if (errno || !end || *end || parsed > UINT32_MAX) {
            fprintf(
                stderr,
                "gegpu: SR_GPU_TNL_VALIDATE_START must be an "
                "integer from 0 to %u\n",
                UINT32_MAX);
            return 0;
        }
        s_tnl_validation_start = (uint32_t)parsed;
    }
    s_tnl_validation_vertices = 0;
    s_tnl_validation_failed = 0;
    const char *full_feedback = getenv("SR_GPU_FULL_FEEDBACK");
    s_full_feedback = full_feedback && full_feedback[0] &&
                      strcmp(full_feedback, "0") != 0;
    Sdl3VkInfo vi;
    if (!sdl3vk_get_vk(&vi)) { fprintf(stderr, "gegpu: sdl3vk not initialized\n"); return 0; }
    s_pdev  = (VkPhysicalDevice)vi.physical;
    s_dev   = (VkDevice)vi.device;
    s_queue = (VkQueue)vi.queue;
    s_pipeline_cache = (VkPipelineCache)vi.pipeline_cache;
    {
        const char *tnl = getenv("SR_GPU_TNL");
        const char *gpu_texture =
            getenv("SR_VULKAN_GPU_TEXTURE_DECODE");
        int tnl_requested =
            tnl && tnl[0] && strcmp(tnl, "0") != 0;
        int gpu_texture_requested =
            gpu_texture && gpu_texture[0] &&
            strcmp(gpu_texture, "0") != 0;
        VkPhysicalDeviceProperties properties;
        VkQueueFamilyProperties queue_properties[16];
        uint32_t queue_property_count = 16;
        vkGetPhysicalDeviceProperties(s_pdev, &properties);
        vkGetPhysicalDeviceQueueFamilyProperties(
            s_pdev, &queue_property_count, queue_properties);
        int compute_queue =
            vi.queue_family < queue_property_count &&
            (queue_properties[vi.queue_family].queueFlags &
             VK_QUEUE_COMPUTE_BIT) != 0;
        s_tnl_preprocess_supported =
            tnl_requested && compute_queue &&
            properties.limits.maxPerStageDescriptorStorageBuffers >= 5u &&
            properties.limits.maxDescriptorSetStorageBuffers >= 5u;
        s_gpu_texture_decode =
            gpu_texture_requested && compute_queue &&
            properties.limits.maxPerStageDescriptorStorageBuffers >= 4u &&
            properties.limits.maxDescriptorSetStorageBuffers >= 4u &&
            properties.limits.maxStorageBufferRange >=
                MAX_GPU_TEXTURE_SOURCE_BYTES;
        if (tnl_requested && !s_tnl_preprocess_supported) {
            fprintf(
                stderr,
                "gegpu: GPU T&L preprocessing unavailable "
                "(requires a compute queue and five storage buffers); "
                "clipped draws use CPU fallback\n");
        }
        if (gpu_texture_requested && !s_gpu_texture_decode) {
            fprintf(
                stderr,
                "gegpu: GPU texture decode unavailable "
                "(requires a compute queue, four storage buffers "
                "and a 1 MiB storage-buffer range); "
                "using CPU decode\n");
        }
    }
    s_ge    = ge_state_ptr();
    s_zbuf  = ge_zbuf_ptr();
    {
        const char *lg = getenv("SR_GPU_LOG");
        s_log = (lg && lg[0] && strcmp(lg, "0") != 0) ? 1 : 0;
    }
    {
        const char *sync =
            getenv("SR_VULKAN_SYNC_TEXTURE_DECODE");
        const char *verify =
            getenv("SR_VULKAN_VERIFY_TEXTURE_DECODE");
        s_sync_texture_decode =
            sync && sync[0] && strcmp(sync, "0") != 0;
        s_verify_texture_decode =
            verify && verify[0] && strcmp(verify, "0") != 0;
        if (s_gpu_texture_decode && s_sync_texture_decode) {
            fprintf(
                stderr,
                "gegpu: synchronous texture decode override "
                "disables GPU texture decode\n");
            s_gpu_texture_decode = 0;
        }
    }
    {
        const SrGraphicsConfig *config = sr_graphics_config_get();
        uint64_t pixels;
        uint32_t transfer_scale;
        if (!configure_render_scale(config)) return 0;
        const char *gpu_upload_scale =
            getenv("SR_VULKAN_GPU_UPLOAD_SCALE");
        int gpu_upload_scale_requested =
            !gpu_upload_scale || !gpu_upload_scale[0] ||
            strcmp(gpu_upload_scale, "0") != 0;
        if (s_render_scale > 1u && gpu_upload_scale_requested) {
            if (upload_scale_supported()) {
                s_gpu_upload_scale = 1;
            } else {
                fprintf(
                    stderr,
                    "gegpu: scaled upload blits unsupported; "
                    "using CPU expansion\n");
            }
        }
        s_texture_scale = config ? config->texture_scale : 1u;
        if (s_gpu_texture_decode && s_texture_scale != 1u) {
            fprintf(
                stderr,
                "gegpu: GPU texture decode does not yet support "
                "xBRZ scaling; using CPU decode at %ux\n",
                s_texture_scale);
            s_gpu_texture_decode = 0;
        }
        pixels =
            (uint64_t)512u * 512u *
            s_texture_scale * s_texture_scale;
        if (pixels > SIZE_MAX / sizeof(uint32_t)) {
            fprintf(stderr, "gegpu: texture scaler allocation is too large\n");
            return 0;
        }
        if (s_texture_scale > 1u) {
            s_texscaled = (uint32_t *)malloc(
                (size_t)pixels * sizeof(uint32_t));
            if (!s_texscaled) {
                fprintf(stderr, "gegpu: cannot allocate texture scaler buffer\n");
                return 0;
            }
        }
        transfer_scale = s_render_scale > s_texture_scale
            ? s_render_scale : s_texture_scale;
        pixels =
            (uint64_t)512u * 512u *
            transfer_scale * transfer_scale;
        if (pixels > SIZE_MAX / sizeof(uint32_t)) {
            fprintf(stderr, "gegpu: transfer allocation is too large\n");
            return 0;
        }
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vi.queue_family;
    VKC(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VKC(vkAllocateCommandBuffers(s_dev, &cbi, &s_cmd));
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VKC(vkCreateFence(s_dev, &fci, NULL, &s_fence));

    /* render pass: LOAD/STORE both attachments; color ends SHADER_READ_ONLY so finished
     * targets are always samplable/presentable, depth stays an attachment */
    VkAttachmentDescription at[2] = {0};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1] = at[0];
    at[1].format = VK_FORMAT_D16_UNORM;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference zref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &zref;
    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpc.attachmentCount = 2; rpc.pAttachments = at;
    rpc.subpassCount = 1; rpc.pSubpasses = &sub;
    VKC(vkCreateRenderPass(s_dev, &rpc, NULL, &s_rp));

    if (!make_buffer(
            MAX_VERTS * sizeof(GpuVert),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     &s_vbuf, &s_vbuf_m, (void **)&s_vmap)) return 0;
    if (!make_buffer(
            MAX_VERTS * sizeof(GeTnlVertex),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tnl_vbuf, &s_tnl_vbuf_m, (void **)&s_tnl_vmap) ||
        !make_buffer(
            MAX_TNL_INDICES * sizeof(uint32_t),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tnl_ibuf, &s_tnl_ibuf_m, (void **)&s_tnl_imap) ||
        !make_buffer(
            MAX_BATCH * sizeof(GeTnlGpuState),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tnl_sbuf, &s_tnl_sbuf_m, (void **)&s_tnl_smap) ||
        !make_buffer(
            (VkDeviceSize)(s_tnl_validation ? MAX_VERTS : 1u) *
                sizeof(GeTnlValidationVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tnl_validation_buf, &s_tnl_validation_mem,
            (void **)&s_tnl_validation_map)) {
        return 0;
    }
    if (s_tnl_preprocess_supported &&
        !make_buffer(
            MAX_VERTS * sizeof(GeTnlClipVertex),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tnl_clip_buf, &s_tnl_clip_mem, NULL)) {
        return 0;
    }
    if (s_tnl_validation)
        fprintf(
            stderr,
            "gegpu: GPU T&L shader-output validation enabled%s "
            "from presentation %u\n",
            s_tnl_validation_exact ? " (bit-exact)" : "",
            s_tnl_validation_start);
    {
        uint32_t transfer_scale =
            s_render_scale > s_texture_scale
                ? s_render_scale : s_texture_scale;
        if (!make_buffer(
            (VkDeviceSize)512u * 512u * 4u *
                transfer_scale * transfer_scale,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_xfer, &s_xfer_m, &s_xfer_map)) return 0;
    }
    if (s_gpu_texture_decode &&
        (!make_buffer(
            sizeof(GeTextureRawJob),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tex_decode_job_buf, &s_tex_decode_job_mem,
            (void **)&s_tex_decode_job_map) ||
         !make_buffer(
            MAX_GPU_TEXTURE_SOURCE_BYTES,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tex_decode_source_buf, &s_tex_decode_source_mem,
            (void **)&s_tex_decode_source_map) ||
         !make_buffer(
            GE_TEXTURE_RAW_CLUT_BYTES,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            &s_tex_decode_clut_buf, &s_tex_decode_clut_mem,
            (void **)&s_tex_decode_clut_map))) {
        return 0;
    }
    if (s_gpu_upload_scale &&
        !make_image(
            FB_W, FB_TEX_H, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            &s_upload_scale_image,
            &s_upload_scale_memory)) {
        if (s_upload_scale_image)
            vkDestroyImage(
                s_dev, s_upload_scale_image, NULL);
        if (s_upload_scale_memory)
            vkFreeMemory(
                s_dev, s_upload_scale_memory, NULL);
        s_upload_scale_image = VK_NULL_HANDLE;
        s_upload_scale_memory = VK_NULL_HANDLE;
        s_gpu_upload_scale = 0;
        fprintf(
            stderr,
            "gegpu: scaled upload image unavailable; "
            "using CPU expansion\n");
    }

    VkDescriptorSetLayoutBinding db = {0};
    db.binding = 0; db.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db.descriptorCount = 1; db.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dlc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 1; dlc.pBindings = &db;
    VKC(vkCreateDescriptorSetLayout(s_dev, &dlc, NULL, &s_dlayout));
    VkDescriptorSetLayoutBinding dst_bindings[2] = { db, db };
    dst_bindings[0].binding = 0;
    dst_bindings[1].binding = 1;
    dlc.bindingCount = 2;
    dlc.pBindings = dst_bindings;
    VKC(vkCreateDescriptorSetLayout(
        s_dev, &dlc, NULL, &s_dst_dlayout));
    VkDescriptorSetLayoutBinding tnl_bindings[4] = {0};
    for (uint32_t i = 0; i < 4; i++) {
        tnl_bindings[i].binding = i;
        tnl_bindings[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tnl_bindings[i].descriptorCount = 1;
        tnl_bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    }
    dlc.bindingCount = 4;
    dlc.pBindings = tnl_bindings;
    VKC(vkCreateDescriptorSetLayout(
        s_dev, &dlc, NULL, &s_tnl_dlayout));
    if (s_tnl_preprocess_supported) {
        VkDescriptorSetLayoutBinding tnl_compute_bindings[5] = {0};
        for (uint32_t i = 0; i < 5; i++) {
            tnl_compute_bindings[i].binding = i;
            tnl_compute_bindings[i].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            tnl_compute_bindings[i].descriptorCount = 1;
            tnl_compute_bindings[i].stageFlags =
                VK_SHADER_STAGE_COMPUTE_BIT;
        }
        dlc.bindingCount = 5;
        dlc.pBindings = tnl_compute_bindings;
        VKC(vkCreateDescriptorSetLayout(
            s_dev, &dlc, NULL, &s_tnl_compute_dlayout));
    }
    if (s_gpu_texture_decode) {
        VkDescriptorSetLayoutBinding texture_bindings[4] = {0};
        for (uint32_t i = 0; i < 4; i++) {
            texture_bindings[i].binding = i;
            texture_bindings[i].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            texture_bindings[i].descriptorCount = 1;
            texture_bindings[i].stageFlags =
                VK_SHADER_STAGE_COMPUTE_BIT;
        }
        dlc.bindingCount = 4;
        dlc.pBindings = texture_bindings;
        VKC(vkCreateDescriptorSetLayout(
            s_dev, &dlc, NULL, &s_tex_decode_dlayout));
    }

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEX + MAX_PALETTE };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = MAX_TEX + MAX_PALETTE; dpc.poolSizeCount = 1; dpc.pPoolSizes = &dps;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;   /* LRU eviction */
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_tex));
    VkDescriptorPoolSize dpsf = {
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        2 * MAX_TGT + 5
    };
    dpc.maxSets = 2 * MAX_TGT + 4; dpc.pPoolSizes = &dpsf;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_fix));
    VkDescriptorPoolSize tnl_dps = {
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4
    };
    dpc.maxSets = 1;
    dpc.poolSizeCount = 1;
    dpc.pPoolSizes = &tnl_dps;
    dpc.flags = 0;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_tnl_dpool));
    VkDescriptorSetAllocateInfo tnl_dai = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
    };
    tnl_dai.descriptorPool = s_tnl_dpool;
    tnl_dai.descriptorSetCount = 1;
    tnl_dai.pSetLayouts = &s_tnl_dlayout;
    VKC(vkAllocateDescriptorSets(s_dev, &tnl_dai, &s_tnl_dset));
    VkDescriptorBufferInfo tnl_buffer_info[4] = {
        {
            s_tnl_sbuf, 0,
            MAX_BATCH * sizeof(GeTnlGpuState)
        },
        {
            s_tnl_validation_buf, 0,
            (VkDeviceSize)(s_tnl_validation ? MAX_VERTS : 1u) *
                sizeof(GeTnlValidationVertex)
        },
        {
            s_tnl_vbuf, 0,
            MAX_VERTS * sizeof(GeTnlVertex)
        },
        {
            s_tnl_ibuf, 0,
            MAX_TNL_INDICES * sizeof(uint32_t)
        },
    };
    VkWriteDescriptorSet tnl_writes[4] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };
    for (uint32_t i = 0; i < 4; i++) {
        tnl_writes[i].dstSet = s_tnl_dset;
        tnl_writes[i].dstBinding = i;
        tnl_writes[i].descriptorCount = 1;
        tnl_writes[i].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        tnl_writes[i].pBufferInfo = &tnl_buffer_info[i];
    }
    vkUpdateDescriptorSets(s_dev, 4, tnl_writes, 0, NULL);
    if (s_tnl_preprocess_supported) {
        VkDescriptorPoolSize tnl_compute_dps = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5
        };
        dpc.maxSets = 1;
        dpc.poolSizeCount = 1;
        dpc.pPoolSizes = &tnl_compute_dps;
        dpc.flags = 0;
        VKC(vkCreateDescriptorPool(
            s_dev, &dpc, NULL, &s_tnl_compute_dpool));
        VkDescriptorSetAllocateInfo tnl_compute_dai = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        tnl_compute_dai.descriptorPool = s_tnl_compute_dpool;
        tnl_compute_dai.descriptorSetCount = 1;
        tnl_compute_dai.pSetLayouts = &s_tnl_compute_dlayout;
        VKC(vkAllocateDescriptorSets(
            s_dev, &tnl_compute_dai, &s_tnl_compute_dset));
        VkDescriptorBufferInfo tnl_compute_buffer_info[5] = {
            {s_tnl_vbuf, 0, MAX_VERTS * sizeof(GeTnlVertex)},
            {s_tnl_ibuf, 0, MAX_TNL_INDICES * sizeof(uint32_t)},
            {s_tnl_sbuf, 0, MAX_BATCH * sizeof(GeTnlGpuState)},
            {s_tnl_clip_buf, 0, MAX_VERTS * sizeof(GeTnlClipVertex)},
            {s_vbuf, 0, MAX_VERTS * sizeof(GpuVert)},
        };
        VkWriteDescriptorSet tnl_compute_writes[5] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
        };
        for (uint32_t i = 0; i < 5; i++) {
            tnl_compute_writes[i].dstSet = s_tnl_compute_dset;
            tnl_compute_writes[i].dstBinding = i;
            tnl_compute_writes[i].descriptorCount = 1;
            tnl_compute_writes[i].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            tnl_compute_writes[i].pBufferInfo =
                &tnl_compute_buffer_info[i];
        }
        vkUpdateDescriptorSets(
            s_dev, 5, tnl_compute_writes, 0, NULL);
    }
    if (s_gpu_texture_decode) {
        VkDescriptorPoolSize texture_dps = {
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4
        };
        dpc.maxSets = 1;
        dpc.poolSizeCount = 1;
        dpc.pPoolSizes = &texture_dps;
        dpc.flags = 0;
        VKC(vkCreateDescriptorPool(
            s_dev, &dpc, NULL, &s_tex_decode_dpool));
        VkDescriptorSetAllocateInfo texture_dai = {
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO
        };
        texture_dai.descriptorPool = s_tex_decode_dpool;
        texture_dai.descriptorSetCount = 1;
        texture_dai.pSetLayouts = &s_tex_decode_dlayout;
        VKC(vkAllocateDescriptorSets(
            s_dev, &texture_dai, &s_tex_decode_dset));
        VkDescriptorBufferInfo texture_buffer_info[4] = {
            {
                s_tex_decode_job_buf, 0,
                sizeof(GeTextureRawJob)
            },
            {
                s_tex_decode_source_buf, 0,
                MAX_GPU_TEXTURE_SOURCE_BYTES
            },
            {
                s_tex_decode_clut_buf, 0,
                GE_TEXTURE_RAW_CLUT_BYTES
            },
            {
                s_xfer, 0,
                MAX_GPU_TEXTURE_OUTPUT_BYTES
            },
        };
        VkWriteDescriptorSet texture_writes[4] = {
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
            {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
        };
        for (uint32_t i = 0; i < 4; i++) {
            texture_writes[i].dstSet = s_tex_decode_dset;
            texture_writes[i].dstBinding = i;
            texture_writes[i].descriptorCount = 1;
            texture_writes[i].descriptorType =
                VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            texture_writes[i].pBufferInfo =
                &texture_buffer_info[i];
        }
        vkUpdateDescriptorSets(
            s_dev, 4, texture_writes, 0, NULL);
    }

    VkPushConstantRange pcr = {
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        0, sizeof(PushPC)
    };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayout set_layouts[4] = {
        s_dlayout, s_dst_dlayout, s_dlayout, s_tnl_dlayout
    };
    plc.setLayoutCount = 4; plc.pSetLayouts = set_layouts;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(s_dev, &plc, NULL, &s_playout));
    if (s_tnl_preprocess_supported) {
        VkPushConstantRange tnl_compute_push = {
            VK_SHADER_STAGE_COMPUTE_BIT, 0,
            sizeof(TnlComputeJob)
        };
        plc.setLayoutCount = 1;
        plc.pSetLayouts = &s_tnl_compute_dlayout;
        plc.pushConstantRangeCount = 1;
        plc.pPushConstantRanges = &tnl_compute_push;
        VKC(vkCreatePipelineLayout(
            s_dev, &plc, NULL, &s_tnl_compute_layout));
    }
    if (s_gpu_texture_decode) {
        VkPushConstantRange texture_push = {
            VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t)
        };
        plc.setLayoutCount = 1;
        plc.pSetLayouts = &s_tex_decode_dlayout;
        plc.pushConstantRangeCount = 1;
        plc.pPushConstantRanges = &texture_push;
        VKC(vkCreatePipelineLayout(
            s_dev, &plc, NULL, &s_tex_decode_layout));
    }

    VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smc.codeSize = sizeof(k_vert_spv); smc.pCode = k_vert_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_vs));
    smc.codeSize = sizeof(k_tnl_vert_spv);
    smc.pCode = k_tnl_vert_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_tnl_vs));
    if (s_tnl_validation) {
        smc.codeSize = sizeof(k_tnl_validation_vert_spv);
        smc.pCode = k_tnl_validation_vert_spv;
        VKC(vkCreateShaderModule(
            s_dev, &smc, NULL, &s_tnl_validation_vs));
    }
    smc.codeSize = sizeof(k_tnl_points_vert_spv);
    smc.pCode = k_tnl_points_vert_spv;
    VKC(vkCreateShaderModule(
        s_dev, &smc, NULL, &s_tnl_points_vs));
    smc.codeSize = sizeof(k_tnl_sprites_vert_spv);
    smc.pCode = k_tnl_sprites_vert_spv;
    VKC(vkCreateShaderModule(
        s_dev, &smc, NULL, &s_tnl_sprites_vs));
    smc.codeSize = sizeof(k_tnl_through_sprites_vert_spv);
    smc.pCode = k_tnl_through_sprites_vert_spv;
    VKC(vkCreateShaderModule(
        s_dev, &smc, NULL, &s_tnl_through_sprites_vs));
    smc.codeSize = sizeof(k_frag_spv); smc.pCode = k_frag_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_fs));
    if (s_tnl_preprocess_supported) {
        smc.codeSize = sizeof(k_tnl_transform_comp_spv);
        smc.pCode = k_tnl_transform_comp_spv;
        VKC(vkCreateShaderModule(
            s_dev, &smc, NULL, &s_tnl_transform_cs));
        smc.codeSize = sizeof(k_tnl_triangles_comp_spv);
        smc.pCode = k_tnl_triangles_comp_spv;
        VKC(vkCreateShaderModule(
            s_dev, &smc, NULL, &s_tnl_triangles_cs));
        VkComputePipelineCreateInfo tnl_compute_pipeline = {
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        tnl_compute_pipeline.layout = s_tnl_compute_layout;
        tnl_compute_pipeline.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        tnl_compute_pipeline.stage.stage =
            VK_SHADER_STAGE_COMPUTE_BIT;
        tnl_compute_pipeline.stage.pName = "main";
        tnl_compute_pipeline.stage.module = s_tnl_transform_cs;
        VKC(vkCreateComputePipelines(
            s_dev, s_pipeline_cache, 1, &tnl_compute_pipeline,
            NULL, &s_tnl_transform_pipeline));
        tnl_compute_pipeline.stage.module = s_tnl_triangles_cs;
        VKC(vkCreateComputePipelines(
            s_dev, s_pipeline_cache, 1, &tnl_compute_pipeline,
            NULL, &s_tnl_triangles_pipeline));
    }
    if (s_gpu_texture_decode) {
        smc.codeSize = sizeof(k_texture_decode_comp_spv);
        smc.pCode = k_texture_decode_comp_spv;
        VKC(vkCreateShaderModule(
            s_dev, &smc, NULL, &s_tex_decode_cs));
        VkComputePipelineCreateInfo texture_pipeline = {
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO
        };
        texture_pipeline.layout = s_tex_decode_layout;
        texture_pipeline.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        texture_pipeline.stage.stage =
            VK_SHADER_STAGE_COMPUTE_BIT;
        texture_pipeline.stage.pName = "main";
        texture_pipeline.stage.module = s_tex_decode_cs;
        VKC(vkCreateComputePipelines(
            s_dev, s_pipeline_cache, 1, &texture_pipeline,
            NULL, &s_tex_decode_pipeline));
    }

    /* shared clamp samplers (render-target sampling + snapshot) */
    {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_n));
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_l));
    }

    /* 1x1 white texture for untextured draws */
    {
        uint32_t white = 0xFFFFFFFFu;
        if (!tex_make(&white, 1, 1, 0, 1, 1, &s_white, &s_white_mem, &s_white_view, &s_white_smp))
            return 0;
        s_white_set = make_descriptor(s_white_view, s_white_smp, s_dpool_fix);
        if (!s_white_set) return 0;
    }

    /* snapshot image for feedback (self-sampling) draws */
    if (!make_image(render_width(), texture_height(),
                    VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &s_snapimg, &s_snapimg_mem)) return 0;
    if (!make_view(s_snapimg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_view)) return 0;
    s_snap_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_snap_n = make_descriptor(s_snap_view, s_smp_n, s_dpool_fix);
    s_snap_l = make_descriptor(s_snap_view, s_smp_l, s_dpool_fix);
    if (!s_snap_n || !s_snap_l) return 0;

    if (!make_image(
            render_width(), render_height(),
            VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &s_dst_color, &s_dst_color_mem)) return 0;
    if (!make_view(
            s_dst_color, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_ASPECT_COLOR_BIT, &s_dst_color_view)) return 0;
    if (!make_image(
            render_width(), render_height(),
            VK_FORMAT_D16_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &s_dst_depth, &s_dst_depth_mem)) return 0;
    if (!make_view(
            s_dst_depth, VK_FORMAT_D16_UNORM,
            VK_IMAGE_ASPECT_DEPTH_BIT, &s_dst_depth_view)) return 0;
    s_dst_color_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    s_dst_depth_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!cmd_begin()) return 0;
    to_layout(
        s_cmd, s_dst_color, VK_IMAGE_ASPECT_COLOR_BIT,
        &s_dst_color_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(
        s_cmd, s_dst_depth, VK_IMAGE_ASPECT_DEPTH_BIT,
        &s_dst_depth_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait()) return 0;
    s_dst_set = make_destination_descriptor();
    if (!s_dst_set) return 0;

    s_ready = 1;
    sr_gpu_vram_watch_reads(1);
    sr_gpu_vram_watch_writes(1);
    fprintf(
        stderr,
        "gegpu: full GPU GE active (persistent targets, no software fallback, "
        "internal resolution %ux, xBRZ scale %ux, upload scaling %s, "
        "texture decode %s, primitive expansion %s, stage timing %s)\n",
        s_render_scale, s_texture_scale,
        s_gpu_upload_scale ? "gpu" : "cpu",
        s_gpu_texture_decode
            ? "GPU compute (opt-in)"
            : (s_sync_texture_decode
                ? "synchronous CPU" : "asynchronous CPU"),
        s_gpu_primitive_expansion
            ? "GPU through-sprite (opt-in)" : "CPU",
        s_stage_log ? "enabled" : "disabled");
    if (s_tnl_direct_compute)
        fprintf(
            stderr,
            "gegpu: direct GPU T&L transform-once compute %s\n",
            s_tnl_preprocess_supported
                ? "enabled" : "unavailable; using vertex shader");
    {
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(s_pdev, &memory);
        VkMemoryPropertyFlags host_flags =
            memory.memoryTypes[s_host_memory_type].propertyFlags;
        VkMemoryPropertyFlags device_flags =
            memory.memoryTypes[s_device_memory_type].propertyFlags;
        fprintf(
            stderr,
            "gegpu: memory host_type=%u flags=0x%x unified=%s "
            "image_type=%u flags=0x%x\n",
            s_host_memory_type, host_flags,
            (host_flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                ? "yes" : "no",
            s_device_memory_type, device_flags);
    }
    return 1;
}

static void gegpu_shutdown(void) {
    if (!s_ready) return;
    submit_pending();
    s_ready = 0;
    sr_gpu_vram_watch_reads(0);
    sr_gpu_vram_watch_writes(0);
    vkDeviceWaitIdle(s_dev);
    if (s_stage_log) {
        fprintf(
            stderr,
            "gegpu: pipeline summary created=%llu key_hits=%llu "
            "create_us=%llu\n",
            (unsigned long long)s_pipelines_created,
            (unsigned long long)s_pipeline_key_hits,
            (unsigned long long)stage_microseconds(
                s_pipeline_create_ticks));
    }
    if (s_log) {
        fprintf(
            stderr,
            "gegpu: texture cache hits=%llu misses=%llu uploads=%lu "
            "background=%llu gpu=%llu gpu_fallbacks=%llu "
            "decode_wait_us=%llu\n",
            (unsigned long long)s_texture_cache_hits,
            (unsigned long long)s_texture_cache_misses,
            s_cnt_texup,
            (unsigned long long)s_async_texture_decodes,
            (unsigned long long)s_gpu_texture_decodes,
            (unsigned long long)s_gpu_texture_decode_fallbacks,
            (unsigned long long)stage_microseconds(
                s_texture_decode_wait_ticks));
    }
    if (s_tnl_validation)
        fprintf(
            stderr,
            "gegpu: GPU T&L shader-output validation %s "
            "%llu indexed vertices\n",
            s_tnl_validation_failed ? "failed after" : "passed",
            (unsigned long long)s_tnl_validation_vertices);

    for (int i = 0; i < MAX_TGT; i++)
        target_destroy(&s_tgts[i]);
    s_cur = NULL;
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *depth = &s_deps[i];
        if (depth->view)
            vkDestroyImageView(s_dev, depth->view, NULL);
        if (depth->img)
            vkDestroyImage(s_dev, depth->img, NULL);
        if (depth->mem)
            vkFreeMemory(s_dev, depth->mem, NULL);
        memset(depth, 0, sizeof(*depth));
    }
    for (int i = 0; i < s_tex_n; i++)
        if (s_tex[i].job && !tex_finish_job(&s_tex[i], 0))
            fprintf(stderr, "gegpu: pending texture decode failed during shutdown\n");
    tex_decode_pool_stop();
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *texture = &s_tex[i];
        if (texture->set)
            vkFreeDescriptorSets(
                s_dev, s_dpool_tex, 1, &texture->set);
        if (texture->smp)
            vkDestroySampler(s_dev, texture->smp, NULL);
        if (texture->view)
            vkDestroyImageView(s_dev, texture->view, NULL);
        if (texture->img)
            vkDestroyImage(s_dev, texture->img, NULL);
        if (texture->mem)
            vkFreeMemory(s_dev, texture->mem, NULL);
    }
    s_tex_n = 0;
    for (int i = 0; i < s_palette_n; i++) {
        PaletteEnt *palette = &s_palettes[i];
        if (palette->set) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &palette->set);
        if (palette->smp) vkDestroySampler(s_dev, palette->smp, NULL);
        if (palette->view) vkDestroyImageView(s_dev, palette->view, NULL);
        if (palette->img) vkDestroyImage(s_dev, palette->img, NULL);
        if (palette->mem) vkFreeMemory(s_dev, palette->mem, NULL);
    }
    s_palette_n = 0;

    for (int i = 0; i < s_pipe_n; i++)
        if (s_pipes[i].pipe)
            vkDestroyPipeline(s_dev, s_pipes[i].pipe, NULL);
    s_pipe_n = 0;
    if (s_vs) vkDestroyShaderModule(s_dev, s_vs, NULL);
    if (s_tnl_vs) vkDestroyShaderModule(s_dev, s_tnl_vs, NULL);
    if (s_tnl_validation_vs)
        vkDestroyShaderModule(
            s_dev, s_tnl_validation_vs, NULL);
    if (s_tnl_points_vs)
        vkDestroyShaderModule(s_dev, s_tnl_points_vs, NULL);
    if (s_tnl_sprites_vs)
        vkDestroyShaderModule(s_dev, s_tnl_sprites_vs, NULL);
    if (s_tnl_through_sprites_vs)
        vkDestroyShaderModule(
            s_dev, s_tnl_through_sprites_vs, NULL);
    if (s_fs) vkDestroyShaderModule(s_dev, s_fs, NULL);
    if (s_tnl_transform_pipeline)
        vkDestroyPipeline(
            s_dev, s_tnl_transform_pipeline, NULL);
    if (s_tnl_triangles_pipeline)
        vkDestroyPipeline(
            s_dev, s_tnl_triangles_pipeline, NULL);
    if (s_tex_decode_pipeline)
        vkDestroyPipeline(
            s_dev, s_tex_decode_pipeline, NULL);
    if (s_tnl_transform_cs)
        vkDestroyShaderModule(s_dev, s_tnl_transform_cs, NULL);
    if (s_tnl_triangles_cs)
        vkDestroyShaderModule(s_dev, s_tnl_triangles_cs, NULL);
    if (s_tex_decode_cs)
        vkDestroyShaderModule(s_dev, s_tex_decode_cs, NULL);
    if (s_playout)
        vkDestroyPipelineLayout(s_dev, s_playout, NULL);
    if (s_tnl_compute_layout)
        vkDestroyPipelineLayout(
            s_dev, s_tnl_compute_layout, NULL);
    if (s_tex_decode_layout)
        vkDestroyPipelineLayout(
            s_dev, s_tex_decode_layout, NULL);
    if (s_dpool_tex)
        vkDestroyDescriptorPool(s_dev, s_dpool_tex, NULL);
    if (s_dpool_fix)
        vkDestroyDescriptorPool(s_dev, s_dpool_fix, NULL);
    if (s_tnl_dpool)
        vkDestroyDescriptorPool(s_dev, s_tnl_dpool, NULL);
    if (s_tnl_compute_dpool)
        vkDestroyDescriptorPool(
            s_dev, s_tnl_compute_dpool, NULL);
    if (s_tex_decode_dpool)
        vkDestroyDescriptorPool(
            s_dev, s_tex_decode_dpool, NULL);

    if (s_white_smp)
        vkDestroySampler(s_dev, s_white_smp, NULL);
    if (s_white_view)
        vkDestroyImageView(s_dev, s_white_view, NULL);
    if (s_white)
        vkDestroyImage(s_dev, s_white, NULL);
    if (s_white_mem)
        vkFreeMemory(s_dev, s_white_mem, NULL);
    if (s_snap_view)
        vkDestroyImageView(s_dev, s_snap_view, NULL);
    if (s_snapimg)
        vkDestroyImage(s_dev, s_snapimg, NULL);
    if (s_snapimg_mem)
        vkFreeMemory(s_dev, s_snapimg_mem, NULL);
    if (s_dst_color_view)
        vkDestroyImageView(s_dev, s_dst_color_view, NULL);
    if (s_dst_depth_view)
        vkDestroyImageView(s_dev, s_dst_depth_view, NULL);
    if (s_dst_color)
        vkDestroyImage(s_dev, s_dst_color, NULL);
    if (s_dst_depth)
        vkDestroyImage(s_dev, s_dst_depth, NULL);
    if (s_dst_color_mem)
        vkFreeMemory(s_dev, s_dst_color_mem, NULL);
    if (s_dst_depth_mem)
        vkFreeMemory(s_dev, s_dst_depth_mem, NULL);
    if (s_smp_n)
        vkDestroySampler(s_dev, s_smp_n, NULL);
    if (s_smp_l)
        vkDestroySampler(s_dev, s_smp_l, NULL);

    if (s_dlayout)
        vkDestroyDescriptorSetLayout(s_dev, s_dlayout, NULL);
    if (s_dst_dlayout)
        vkDestroyDescriptorSetLayout(s_dev, s_dst_dlayout, NULL);
    if (s_tnl_dlayout)
        vkDestroyDescriptorSetLayout(s_dev, s_tnl_dlayout, NULL);
    if (s_tnl_compute_dlayout)
        vkDestroyDescriptorSetLayout(
            s_dev, s_tnl_compute_dlayout, NULL);
    if (s_tex_decode_dlayout)
        vkDestroyDescriptorSetLayout(
            s_dev, s_tex_decode_dlayout, NULL);
    if (s_rp)
        vkDestroyRenderPass(s_dev, s_rp, NULL);
    if (s_vmap)
        vkUnmapMemory(s_dev, s_vbuf_m);
    if (s_tnl_vmap)
        vkUnmapMemory(s_dev, s_tnl_vbuf_m);
    if (s_tnl_imap)
        vkUnmapMemory(s_dev, s_tnl_ibuf_m);
    if (s_tnl_smap)
        vkUnmapMemory(s_dev, s_tnl_sbuf_m);
    if (s_tnl_validation_map)
        vkUnmapMemory(s_dev, s_tnl_validation_mem);
    if (s_xfer_map)
        vkUnmapMemory(s_dev, s_xfer_m);
    if (s_tex_decode_job_map)
        vkUnmapMemory(s_dev, s_tex_decode_job_mem);
    if (s_tex_decode_source_map)
        vkUnmapMemory(s_dev, s_tex_decode_source_mem);
    if (s_tex_decode_clut_map)
        vkUnmapMemory(s_dev, s_tex_decode_clut_mem);
    if (s_vbuf)
        vkDestroyBuffer(s_dev, s_vbuf, NULL);
    if (s_tnl_vbuf)
        vkDestroyBuffer(s_dev, s_tnl_vbuf, NULL);
    if (s_tnl_ibuf)
        vkDestroyBuffer(s_dev, s_tnl_ibuf, NULL);
    if (s_tnl_sbuf)
        vkDestroyBuffer(s_dev, s_tnl_sbuf, NULL);
    if (s_tnl_clip_buf)
        vkDestroyBuffer(s_dev, s_tnl_clip_buf, NULL);
    if (s_tnl_validation_buf)
        vkDestroyBuffer(s_dev, s_tnl_validation_buf, NULL);
    if (s_xfer)
        vkDestroyBuffer(s_dev, s_xfer, NULL);
    if (s_tex_decode_job_buf)
        vkDestroyBuffer(s_dev, s_tex_decode_job_buf, NULL);
    if (s_tex_decode_source_buf)
        vkDestroyBuffer(s_dev, s_tex_decode_source_buf, NULL);
    if (s_tex_decode_clut_buf)
        vkDestroyBuffer(s_dev, s_tex_decode_clut_buf, NULL);
    if (s_upload_scale_image)
        vkDestroyImage(
            s_dev, s_upload_scale_image, NULL);
    if (s_vbuf_m)
        vkFreeMemory(s_dev, s_vbuf_m, NULL);
    if (s_tnl_vbuf_m)
        vkFreeMemory(s_dev, s_tnl_vbuf_m, NULL);
    if (s_tnl_ibuf_m)
        vkFreeMemory(s_dev, s_tnl_ibuf_m, NULL);
    if (s_tnl_sbuf_m)
        vkFreeMemory(s_dev, s_tnl_sbuf_m, NULL);
    if (s_tnl_clip_mem)
        vkFreeMemory(s_dev, s_tnl_clip_mem, NULL);
    if (s_tnl_validation_mem)
        vkFreeMemory(s_dev, s_tnl_validation_mem, NULL);
    if (s_xfer_m)
        vkFreeMemory(s_dev, s_xfer_m, NULL);
    if (s_tex_decode_job_mem)
        vkFreeMemory(s_dev, s_tex_decode_job_mem, NULL);
    if (s_tex_decode_source_mem)
        vkFreeMemory(s_dev, s_tex_decode_source_mem, NULL);
    if (s_tex_decode_clut_mem)
        vkFreeMemory(s_dev, s_tex_decode_clut_mem, NULL);
    if (s_upload_scale_memory)
        vkFreeMemory(
            s_dev, s_upload_scale_memory, NULL);
    if (s_fence)
        vkDestroyFence(s_dev, s_fence, NULL);
    if (s_pool)
        vkDestroyCommandPool(s_dev, s_pool, NULL);

    free(s_texscaled);
    s_texscaled = NULL;
    s_render_scale = 1u;
    s_texture_scale = 1u;
    s_sync_texture_decode = 0;
    s_verify_texture_decode = 0;
    s_gpu_texture_decode = 0;
    s_gpu_tnl_classes = GE_VK_TNL_CLASS_ALL;
    s_gpu_primitive_expansion = 0;
    s_tex_decode_job_map = NULL;
    s_tex_decode_source_map = NULL;
    s_tex_decode_clut_map = NULL;
    s_tex_decode_job_buf = VK_NULL_HANDLE;
    s_tex_decode_source_buf = VK_NULL_HANDLE;
    s_tex_decode_clut_buf = VK_NULL_HANDLE;
    s_tex_decode_job_mem = VK_NULL_HANDLE;
    s_tex_decode_source_mem = VK_NULL_HANDLE;
    s_tex_decode_clut_mem = VK_NULL_HANDLE;
    s_tex_decode_dlayout = VK_NULL_HANDLE;
    s_tex_decode_dpool = VK_NULL_HANDLE;
    s_tex_decode_dset = VK_NULL_HANDLE;
    s_tex_decode_layout = VK_NULL_HANDLE;
    s_tex_decode_cs = VK_NULL_HANDLE;
    s_tex_decode_pipeline = VK_NULL_HANDLE;
    s_gpu_upload_scale = 0;
    s_tnl_validation = 0;
    s_tnl_validation_exact = 0;
    s_tnl_validation_start = 0;
    s_tnl_validation_failed = 0;
    s_tnl_direct_compute = 0;
    s_tnl_validation_map = NULL;
}
