/* Shared GE state + rasterizer backend API.
 *
 * GeState/GeVtx were moved here verbatim from ge.c so the optional GPU rasterizer
 * (src/rt/gpu_sdl3vk/ge_gpu.c) can read the live GE state without duplicating it.
 * ge.c remains the only writer of the state and the arbiter of GE semantics; the GPU
 * backend registers at runtime (ge_set_backend) and ge.c stays byte-identical
 * in behaviour when no hooks are registered (the GDI build never registers any).
 */
#ifndef GE_SHARED_H
#define GE_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t base, offset;    /* GE_BASE raw data and GE_OFFSETADDR << 8 */
    uint32_t fbp, fbw, fbfmt; /* framebuffer ptr/stride/format (0=5650,1=5551,2=4444,3=8888) */
    uint32_t zbp, zbw;
    uint32_t vaddr, iaddr;
    uint32_t vtype;           /* vertex type bits */
    int scis_x1, scis_y1, scis_x2, scis_y2;
    int ztest_enable, zwrite_disable, cull_enable;
    uint32_t ztest, cull, minz, maxz;
    /* GE_CLIPENABLE (0x1C): PPSSPP's "depth clip" flag. OFF (common case): any vertex whose
     * screen x/y/z falls outside the 4096x4096x65536 grid, or whose z/w passes +-1, drops the
     * WHOLE triangle. ON: z clamps instead and only all-3-outside-same-side drops. */
    uint32_t depth_clip;
    /* Matrices: column-major. World/View are 4×3 (12 floats), Proj is 4×4 (16 floats).
     * *_num is the auto-increment write cursor set by *MATRIXNUMBER commands. */
    float world[12], view[12], proj[16];
    int world_num, view_num, proj_num;
    float bone[96];                          /* 8 bone matrices, 4x3 each (skinning) */
    int bone_num;
    float morph_weight[8];
    uint32_t patch_division, patch_primitive, patch_facing;
    /* Viewport: raw 24-bit float mantissa words from VIEWPORTXSCALE..VIEWPORTZCENTER. */
    uint32_t viewport_raw[6];   /* [0]=XS [1]=YS [2]=ZS [3]=XC [4]=YC [5]=ZC */
    uint32_t offsetx, offsety;  /* sub-pixel offset (pixels = (raw&0xFFFF)/16.0) */
    uint32_t material;        /* flat material/ambient colour (0x00BBGGRR) */
    int material_alpha, material_alpha_set, material_set;
    int tex_enable, tex_swizzle, clear;
    uint32_t clear_mode;
    int blend_enable;
    uint32_t blend_mode, blend_fixa, blend_fixb;
    int logic_op_enable; uint32_t logic_op;
    uint32_t tex_addr, tex_bufw, tex_fmt;   /* level-0 texture base / stride(texels) / PSM */
    uint32_t tex_addr_levels[8], tex_bufw_levels[8];
    int tex_width_levels[8], tex_height_levels[8];
    uint32_t tex_mode, tex_level;
    int tex_max_level;
    uint32_t tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h;                        /* texture dimensions (from TEXSIZE log2) */
    uint32_t clut_addr, clut_fmt;            /* palette base and format word */
    /* Internal CLUT RAM, captured at GE_LOADCLUT like the hardware. Games reuse the staging
     * buffer right after LOADCLUT, so sampling the palette live from memory reads garbage. */
    uint8_t clutram[2048];
    /* Texture coordinate transform (transform-mode only; through-mode UV are raw texels). */
    float tex_scale_u, tex_scale_v, tex_off_u, tex_off_v;
    uint32_t tex_map_mode;                   /* bits0-1: 0=UV 1=tex matrix 2=envmap; bits8-9: proj source */
    uint32_t tex_shade_ls;                   /* envmap: light indices (bits0-1, 8-9) */
    float tgen[12];                          /* texgen matrix, 4x3 column-major like world/view */
    int tgen_num;
    /* Alpha test: atest = func | ref<<8 | mask<<16 (funcs match ztest_px order). */
    int atest_enable; uint32_t atest;
    /* Color test compares masked RGB; stencil uses destination alpha on the PSP. */
    int color_test_enable; uint32_t color_test, color_ref, color_test_mask;
    int stencil_test_enable; uint32_t stencil_test, stencil_op;
    /* Fog: colour = mix(fog_color, frag, clamp01((view_z + fog_end) * fog_slope)). */
    int fog_enable; float fog_end, fog_slope; uint32_t fog_color;
    /* Shading: 1 = gouraud (default), 0 = flat (provoking vertex = last). */
    int shade_gouraud;
    /* Lighting (transform-mode, per-vertex). Lights are in world space. */
    int lighting_enable, light_on[4], reverse_normal, light_mode;
    uint32_t light_type[4];                  /* bits0-1 comp, bits8-9 type */
    float light_pos[4][3], light_dir[4][3], light_att[4][3];
    float light_spot_exp[4], light_spot_cut[4];
    uint32_t light_acol[4], light_dcol[4], light_scol[4];
    uint32_t amb_color; int amb_alpha;
    uint32_t mat_emissive, mat_diffuse, mat_specular, mat_update;
    float spec_coef;
    /* Dither: 4x4 signed offsets applied to RGB before packing to 16-bit formats. */
    int dither_enable; int8_t dith[4][4];
    /* Write masks: bits set = write-disabled (per PSP semantics). */
    uint32_t mask_rgb; int mask_alpha;
    /* Bumped on every GE_LOADCLUT: lets the GPU backend detect palette changes without
     * hashing clutram per draw. */
    uint32_t clut_gen;
    /* Block transfer (GE_TRANSFER*): raw command words; decoded at GE_TRANSFERSTART with
     * PPSSPP's accessor masks. The GPU backend reads these on flush("xfersrc") to know
     * which guest range is about to be read. */
    uint32_t xf_src, xf_srcw, xf_dst, xf_dstw, xf_spos, xf_dpos, xf_size;
    /* Immediate vertex state, GE 0xF0-0xF9 (VSCX..VSCV). VAP (index 7) submits a vertex;
     * the current runtime records state and reports nonzero VAP drawing explicitly. */
    uint32_t imm[10];
} GeState;

enum {
    GE_TEXTURE_RAW_JOB_ABI_VERSION = 1,
    GE_TEXTURE_RAW_CLUT_BYTES = 2048,
};

typedef enum GeTextureRawJobFlags {
    GE_TEXTURE_RAW_SWIZZLED = 1u << 0,
    GE_TEXTURE_RAW_INDEXED = 1u << 1,
    GE_TEXTURE_RAW_BLOCK_COMPRESSED = 1u << 2,
    GE_TEXTURE_RAW_CLUT_SHARED = 1u << 3,
} GeTextureRawJobFlags;

/* Shader-neutral texture decode descriptor. Source and CLUT offsets are bytes
 * in their respective backend-owned raw buffers; the destination offset is in
 * RGBA8 pixels. The destination dimensions also define its row stride and
 * bounds. */
typedef struct GeTextureRawJob {
    uint32_t abi_version;
    uint32_t flags;
    uint32_t format;
    uint32_t mip_level;
    uint32_t source_byte_offset;
    uint32_t source_byte_count;
    uint32_t source_stride;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t clut_byte_offset;
    uint32_t clut_byte_count;
    uint32_t clut_format;
    uint32_t texture_mode;
    uint32_t destination_pixel_offset;
    uint32_t destination_width;
    uint32_t destination_height;
} GeTextureRawJob;

#if defined(__cplusplus)
static_assert(
    sizeof(GeTextureRawJob) == 64,
    "raw texture job ABI changed");
#else
_Static_assert(
    sizeof(GeTextureRawJob) == 64,
    "raw texture job ABI changed");
#endif

/* A rasterized vertex: screen position, reciprocal clip-w (for persp-correct interp), UV,
 * colour, and per-vertex fog factor (1 = no fog). In transform mode u/v are stored
 * pre-multiplied by rw (texel coords over w). */
typedef struct GeVtx { float x, y, z, rw; float u, v; float fog; int r, g, b, a; } GeVtx;

enum {
    GE_TNL_ABI_VERSION = 1,
};

typedef enum GeTnlVertexFlags {
    GE_TNL_VERTEX_NORMAL = 1u << 0,
    GE_TNL_VERTEX_TEXCOORD = 1u << 1,
    GE_TNL_VERTEX_COLOR = 1u << 2,
    GE_TNL_VERTEX_SKINNED = 1u << 3,
    GE_TNL_VERTEX_MORPHED = 1u << 4,
} GeTnlVertexFlags;

typedef enum GeTnlStateFlags {
    GE_TNL_STATE_TEXTURE = 1u << 0,
    GE_TNL_STATE_LIGHTING = 1u << 1,
    GE_TNL_STATE_REVERSE_NORMAL = 1u << 2,
    GE_TNL_STATE_SEPARATE_SPECULAR = 1u << 3,
    GE_TNL_STATE_FOG = 1u << 4,
    GE_TNL_STATE_GOURAUD = 1u << 5,
    GE_TNL_STATE_DEPTH_CLIP = 1u << 6,
    GE_TNL_STATE_CULL = 1u << 7,
    GE_TNL_STATE_CLEAR = 1u << 8,
    GE_TNL_STATE_MATERIAL_COLOR_SET = 1u << 9,
    GE_TNL_STATE_MATERIAL_ALPHA_SET = 1u << 10,
} GeTnlStateFlags;

typedef enum GeTnlPrimitive {
    GE_TNL_POINTS = 0,
    GE_TNL_LINES = 1,
    GE_TNL_LINE_STRIP = 2,
    GE_TNL_TRIANGLES = 3,
    GE_TNL_TRIANGLE_STRIP = 4,
    GE_TNL_TRIANGLE_FAN = 5,
    GE_TNL_SPRITES = 6,
} GeTnlPrimitive;

typedef enum GeTnlDrawFlags {
    GE_TNL_DRAW_PREPROCESS = 1u << 0,
    GE_TNL_DRAW_SPRITE_LINEAR_FILTER = 1u << 1,
} GeTnlDrawFlags;

/* CPU-decoded model vertex. Morphing and skinning are applied before this ABI;
 * color bytes are ordered red, green, blue, alpha. */
typedef struct GeTnlVertex {
    float position[3];
    float normal[3];
    float texcoord[2];
    uint8_t color[4];
} GeTnlVertex;

typedef struct GeTnlLight {
    uint32_t enabled;
    uint32_t type;
    float position[3];
    float direction[3];
    float attenuation[3];
    float spot_exponent;
    float spot_cutoff;
    uint32_t ambient_color;
    uint32_t diffuse_color;
    uint32_t specular_color;
} GeTnlLight;

/* Copied transform inputs. This structure owns no pointers, so a const snapshot
 * remains immutable even when later GE commands change live state. PSP colors
 * retain their register representation, 0x00BBGGRR. */
typedef struct GeTnlState {
    uint32_t abi_version;
    uint32_t flags;
    uint32_t vertex_flags;
    uint32_t source_vtype;
    float world[12];
    float view[12];
    float projection[16];
    float texture[12];
    float texture_scale[2];
    float texture_offset[2];
    float viewport_scale[3];
    float viewport_center[3];
    float viewport_offset[2];
    int32_t scissor[4];
    uint32_t texture_width;
    uint32_t texture_height;
    uint32_t texture_map_mode;
    uint32_t texture_shade_lights;
    uint32_t minimum_depth;
    uint32_t maximum_depth;
    uint32_t cull_mode;
    float fog_end;
    float fog_slope;
    uint32_t fog_color;
    uint32_t material_color;
    uint32_t material_alpha;
    uint32_t material_emissive;
    uint32_t material_diffuse;
    uint32_t material_specular;
    uint32_t material_update;
    uint32_t ambient_color;
    uint32_t ambient_alpha;
    float specular_coefficient;
    GeTnlLight lights[4];
} GeTnlState;

/* Submission-order indices reference the deduplicated canonical vertex table.
 * The state and pointed-to arrays are valid only for the duration of a backend
 * transform hook call unless the backend copies them. */
typedef struct GeTnlDraw {
    const GeTnlVertex *vertices;
    const uint16_t *indices;
    const GeTnlState *state;
    uint32_t vertex_count;
    uint32_t index_count;
    uint32_t primitive;
    uint32_t flags;
} GeTnlDraw;

typedef struct GeRgb { int32_t r, g, b; } GeRgb;

typedef struct GeDebugContext {
    uint32_t frame;
    uint32_t list_id;
    uint32_t list_addr;
    uint32_t draw_serial;
    uint32_t draw_pc;
    uint32_t draw_op;
} GeDebugContext;

static inline uint32_t ge_texture_bits_per_pixel(uint32_t format) {
    static const uint8_t bits[16] = {
        16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0,
    };
    return bits[format & 15u];
}

static inline uint32_t ge_texture_buffer_width(
    uint32_t format, uint32_t raw_width) {
    uint32_t mask;
    switch (format & 15u) {
        case 0: case 1: case 2: case 6: mask = 0x7f8u; break;
        case 3: case 7: mask = 0x7fcu; break;
        case 4: mask = 0x7e0u; break;
        case 5: mask = 0x7f0u; break;
        case 8: case 9: case 10: mask = 0x7ffu; break;
        default: return 0;
    }
    uint32_t width = raw_width & mask;
    if (!width) {
        uint32_t bits = ge_texture_bits_per_pixel(format);
        if (bits) width = 128u / bits;
    }
    return width;
}

static inline uint32_t ge_texture_texel_byte_offset(
    uint32_t row_pitch_pixels, uint32_t u, uint32_t v,
    uint32_t texel_bits, int swizzled) {
    if (!swizzled) {
        return v * ((row_pitch_pixels * texel_bits) >> 3) +
            ((u * texel_bits) >> 3);
    }
    const uint32_t tile_bits = 32;
    const uint32_t tiles_x = 4;
    const uint32_t tiles_y = 8;
    uint32_t texels_per_tile = tile_bits / texel_bits;
    uint32_t tile_u = u / texels_per_tile;
    uint32_t tile_index =
        (v % tiles_y) * tiles_x +
        (v / tiles_y) *
            ((row_pitch_pixels * texel_bits / tile_bits) * tiles_y) +
        (tile_u % tiles_x) +
        (tile_u / tiles_x) * (tiles_x * tiles_y);
    return tile_index * (tile_bits >> 3) +
        (((u % texels_per_tile) * texel_bits) >> 3);
}

static inline uint32_t ge_texture_unpack_direct_rgba(
    uint32_t raw, uint32_t format) {
    uint32_t red, green, blue, alpha;
    switch (format & 3u) {
        case 0:
            red = (raw & 0x1fu) * 255u / 31u;
            green = ((raw >> 5) & 0x3fu) * 255u / 63u;
            blue = ((raw >> 11) & 0x1fu) * 255u / 31u;
            alpha = 255u;
            break;
        case 1:
            red = (raw & 0x1fu) * 255u / 31u;
            green = ((raw >> 5) & 0x1fu) * 255u / 31u;
            blue = ((raw >> 10) & 0x1fu) * 255u / 31u;
            alpha = (raw & 0x8000u) ? 255u : 0u;
            break;
        case 2:
            red = (raw & 0x0fu) * 17u;
            green = ((raw >> 4) & 0x0fu) * 17u;
            blue = ((raw >> 8) & 0x0fu) * 17u;
            alpha = ((raw >> 12) & 0x0fu) * 17u;
            break;
        default:
            return raw;
    }
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

static inline uint32_t ge_texture_clut_index(
    uint32_t raw_index, uint32_t texture_format,
    uint32_t mip_level, uint32_t clut_format,
    uint32_t texture_mode) {
    uint32_t shift = (clut_format >> 2) & 0x1fu;
    uint32_t mask = (clut_format >> 8) & 0xffu;
    uint32_t start = ((clut_format >> 16) & 0x1fu) << 4;
    uint32_t start_mask =
        (clut_format & 3u) == 3u ? 0xffu : 0x1ffu;
    uint32_t index =
        ((raw_index >> shift) & mask) |
        (start & start_mask);
    if ((texture_mode & 0x100u) != 0) {
        if (texture_format == 4u)
            index += mip_level * 16u;
        else if (texture_format == 5u &&
                 (clut_format & 3u) != 3u)
            index += (mip_level & 1u) * 256u;
    }
    return index;
}

static inline uint32_t ge_texture_clut_rgba(
    const uint8_t *clut, uint32_t index,
    uint32_t clut_format) {
    if (!clut) return 0;
    uint32_t format = clut_format & 3u;
    uint32_t entry_bytes = format == 3u ? 4u : 2u;
    uint32_t offset =
        (index * entry_bytes) &
        (GE_TEXTURE_RAW_CLUT_BYTES - 1u);
    uint32_t raw = clut[offset];
    raw |= (uint32_t)clut[
        (offset + 1u) & (GE_TEXTURE_RAW_CLUT_BYTES - 1u)] << 8;
    if (entry_bytes == 4u) {
        raw |= (uint32_t)clut[
            (offset + 2u) &
                (GE_TEXTURE_RAW_CLUT_BYTES - 1u)] << 16;
        raw |= (uint32_t)clut[
            (offset + 3u) &
                (GE_TEXTURE_RAW_CLUT_BYTES - 1u)] << 24;
    }
    return ge_texture_unpack_direct_rgba(raw, format);
}

static inline uint32_t ge_texture_dxt_color(
    const uint8_t *block, uint32_t x, uint32_t y,
    uint32_t alpha) {
    uint32_t color1 =
        block[4] | ((uint32_t)block[5] << 8);
    uint32_t color2 =
        block[6] | ((uint32_t)block[7] << 8);
    uint32_t blue1 = (color1 << 3) & 0xf8u;
    uint32_t blue2 = (color2 << 3) & 0xf8u;
    uint32_t green1 = (color1 >> 3) & 0xfcu;
    uint32_t green2 = (color2 >> 3) & 0xfcu;
    uint32_t red1 = (color1 >> 8) & 0xf8u;
    uint32_t red2 = (color2 >> 8) & 0xf8u;
    uint32_t index =
        (block[y & 3u] >> ((x & 3u) * 2u)) & 3u;
    uint32_t red, green, blue;
    if (index == 0u) {
        red = red1;
        green = green1;
        blue = blue1;
    } else if (index == 1u) {
        red = red2;
        green = green2;
        blue = blue2;
    } else if (color1 > color2) {
        if (index == 2u) {
            red = (red1 * 2u + red2) / 3u;
            green = (green1 * 2u + green2) / 3u;
            blue = (blue1 * 2u + blue2) / 3u;
        } else {
            red = (red2 * 2u + red1) / 3u;
            green = (green2 * 2u + green1) / 3u;
            blue = (blue2 * 2u + blue1) / 3u;
        }
    } else if (index == 3u) {
        red = green = blue = alpha = 0;
    } else {
        red = (red1 + red2) / 2u;
        green = (green1 + green2) / 2u;
        blue = (blue1 + blue2) / 2u;
    }
    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

static inline uint32_t ge_texture_dxt_alpha_lerp(
    uint32_t alpha1, uint32_t alpha2,
    uint32_t numerator, uint32_t denominator) {
    uint32_t value1 =
        (alpha1 * ((denominator - numerator) << 8)) /
        denominator;
    uint32_t value2 =
        (alpha2 * (numerator << 8)) / denominator;
    return (value1 + value2 + 31u) >> 8;
}

static inline uint32_t ge_texture_dxt_decode(
    const uint8_t *block, uint32_t format,
    uint32_t x, uint32_t y) {
    if (!block || format < 8u || format > 10u) return 0;
    x &= 3u;
    y &= 3u;
    if (format == 8u)
        return ge_texture_dxt_color(block, x, y, 255u);
    if (format == 9u) {
        uint32_t alpha_line =
            block[8u + y * 2u] |
            ((uint32_t)block[9u + y * 2u] << 8);
        uint32_t alpha =
            (alpha_line >> (x * 4u)) & 15u;
        return ge_texture_dxt_color(block, x, y, 0) |
            (alpha << 28);
    }

    uint32_t alpha_bit = (y * 4u + x) * 3u;
    uint32_t alpha_byte = 8u + (alpha_bit >> 3);
    uint32_t alpha_bits =
        block[alpha_byte] |
        ((uint32_t)block[alpha_byte + 1u] << 8);
    uint32_t alpha_index =
        (alpha_bits >> (alpha_bit & 7u)) & 7u;
    uint32_t alpha1 = block[14];
    uint32_t alpha2 = block[15];
    uint32_t alpha;
    if (alpha_index == 0u)
        alpha = alpha1;
    else if (alpha_index == 1u)
        alpha = alpha2;
    else if (alpha1 > alpha2)
        alpha = ge_texture_dxt_alpha_lerp(
            alpha1, alpha2, alpha_index - 1u, 7u);
    else if (alpha_index == 6u)
        alpha = 0;
    else if (alpha_index == 7u)
        alpha = 255u;
    else
        alpha = ge_texture_dxt_alpha_lerp(
            alpha1, alpha2, alpha_index - 1u, 5u);
    return ge_texture_dxt_color(block, x, y, 0) |
        (alpha << 24);
}

/* Rasterizer backend. init/present/shutdown provide the lifecycle seam needed to select
 * Vulkan, D3D12, or the software fallback at runtime. The primitive hooks return 1 when
 * the primitive was taken by
 * the GPU backend (the software rasterizer is skipped) — in the full GPU renderer they
 * always take it. flush(reason) is called at GE sync points ("listend", "loadclut");
 * the backend decides what each requires. vram_read lazily materializes newer GPU data
 * before a CPU load; vram_write does the same before a CPU store, and vram_dirty then
 * invalidates the written range. present returns 1 when shown, 0 for a user-requested close,
 * -1 when the frontend should upload guest VRAM, and -2 for a fatal backend error. */
typedef struct GeBackend {
    const char *name;
    int  (*init)(void);
    int  (*present)(uint32_t fbaddr, int fmt, uint32_t stride);
    void (*shutdown)(void);
    int  (*tri)(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp);
    int  (*tri_list)(const GeVtx *vertices, uint32_t count, int persp);
    int  (*sprite)(const GeVtx *p0, const GeVtx *p1, int persp);
    int  (*line)(const GeVtx *a, const GeVtx *b, int persp);
    int  (*point)(const GeVtx *a, int persp);
    void (*flush)(const char *reason);
    void (*vram_dirty)(uint32_t addr, uint32_t bytes);
    void (*vram_read)(uint32_t addr, uint32_t bytes);
    void (*vram_write)(uint32_t addr, uint32_t bytes);
    /* GE block transfer (state in xf_*): returns 1 when the copy was performed GPU-side
     * (image-to-image; guest VRAM intentionally left stale, like any render target). 0 =
     * not representable (RAM endpoints, format reinterpretation): caller runs the CPU copy,
     * preceded by flush("xfersrc") and followed by vram_dirty on the destination. Games run
     * this blit per frame (motion blur, pause capture); the CPU route costs a GPU drain +
     * PCIe readback + re-upload each time, which dwarfs the frame itself on discrete GPUs. */
    int  (*xfer)(uint32_t startdata);
    /* Optional indexed triangle list: `table` holds projected vertices (Gouraud colors only),
     * `indices` reference it in submission order. Returns 0 to decline; the caller then
     * replays the triangles through the ordinary path. */
    int  (*tri_indexed)(const GeVtx *table, uint32_t table_count,
                        const uint16_t *indices, uint32_t index_count, int persp);
    /* Optional model-space transform draw. Returns 1 after taking ownership;
     * returning 0 preserves the complete CPU transform and raster path. */
    int  (*transform_draw)(const GeTnlDraw *draw);
} GeBackend;

/* Implemented in ge.c. */
void             ge_set_backend(const GeBackend *backend);
const GeBackend *ge_backend_get(void);
/* CPU access synchronization for guest ranges that may alias GPU-cached framebuffers.
 * All calls are no-ops when no GPU backend is registered. */
void      sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
void      sr_gpu_vram_read(uint32_t addr, uint32_t bytes);
void      sr_gpu_vram_prepare_write(uint32_t addr, uint32_t bytes);
void      sr_gpu_vram_watch_reads(int enabled);
void      sr_gpu_vram_watch_writes(int enabled);
GeState  *ge_state_ptr(void);
void      ge_debug_set_list(uint32_t list_id, uint32_t list_addr);
GeDebugContext ge_debug_context(void);
int       ge_debug_dump(const char *path);
uint16_t *ge_zbuf_ptr(void);        /* 272*512 entries, row stride = ge_zbuf_stride() */
uint32_t  ge_zbuf_stride(void);
/* Decode the current texture to RGBA8 through the exact sampler path the software
 * rasterizer uses (swizzle + CLUT + direct/DXT format). Layout is row-major,
 * r|g<<8|b<<16|a<<24. Level zero uses the legacy tex_addr/tex_w fields above. */
void      ge_decode_tex_rgba(uint32_t *out);
void      ge_decode_tex_rgba_level(uint32_t *out, int level);
uint64_t  ge_texture_level_byte_size(const GeState *state, int level);
/* Thread-safe texture decoding: capture copies the current texture's source bytes, palette and
 * decode state on the GE thread; the decode itself touches nothing else. It produces exactly
 * the pixels ge_decode_tex_rgba_level produces for the same state. */
typedef struct GeTexSnapshot GeTexSnapshot;
GeTexSnapshot *ge_tex_snapshot_capture(uint32_t level_count);
void      ge_tex_snapshot_decode_level(
              const GeTexSnapshot *snapshot, uint32_t *out, int level);
const uint8_t *ge_tex_snapshot_source_data(
              const GeTexSnapshot *snapshot, uint32_t level,
              uint32_t *byte_count);
const uint8_t *ge_tex_snapshot_clut_data(
              const GeTexSnapshot *snapshot, uint32_t *byte_count);
int       ge_tex_snapshot_build_raw_job(
              const GeTexSnapshot *snapshot, uint32_t level,
              uint32_t source_byte_offset,
              uint32_t clut_byte_offset,
              uint32_t destination_pixel_offset,
              uint32_t destination_width,
              uint32_t destination_height,
              GeTextureRawJob *out);
void      ge_tex_snapshot_free(GeTexSnapshot *snapshot);
int       ge_color_test_pass(
              uint32_t test, uint32_t reference, uint32_t mask,
              int red, int green, int blue);
int       ge_stencil_test_pass(uint32_t test, uint8_t stencil);
uint8_t   ge_stencil_apply(
              uint32_t operation, uint32_t format, uint8_t stencil,
              uint8_t reference);
GeRgb     ge_blend_rgb(
              uint32_t mode, uint32_t fixed_a, uint32_t fixed_b,
              uint32_t source, uint32_t destination);
uint32_t  ge_logic_apply(
              uint32_t operation, uint32_t source, uint32_t destination);

#ifdef __cplusplus
}
#endif

#endif /* GE_SHARED_H */
