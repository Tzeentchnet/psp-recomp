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

/* A rasterized vertex: screen position, reciprocal clip-w (for persp-correct interp), UV,
 * colour, and per-vertex fog factor (1 = no fog). In transform mode u/v are stored
 * pre-multiplied by rw (texel coords over w). */
typedef struct GeVtx { float x, y, z, rw; float u, v; float fog; int r, g, b, a; } GeVtx;

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
