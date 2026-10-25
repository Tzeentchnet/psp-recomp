/* sceGe display-list GPU.
 *
 * Walks a PSP GE command list and maintains the rendering state, then rasterises PRIM commands
 * into the framebuffer in guest VRAM.
 *
 * Through-mode (2D, screen-space UI/HUD) and transform-mode (3D, full T&L pipeline) are both
 * handled. Transform-mode does: world*view*proj matrix multiply, perspective divide, viewport
 * transform, per-vertex lighting (ambient/diffuse/specular, directional+point lights), fog,
 * depth test, perspective-correct UV interpolation with TEXSCALE/TEXOFFSET, and flat/gouraud
 * shading. Points, lines, triangles (list/strip/fan) and sprites are rasterised.
 *
 * Per-fragment: texturing (all direct, CLUT, and DXT formats; swizzled or linear; bilinear),
 * programmable alpha test, alpha blending (full factor/equation set), 4x4 ordered dithering,
 * colour/alpha write masks, scissor, and a software 16-bit depth buffer.
 *
 * Not emulated: automatic software-rasterizer mip selection.
 *
 * Each GE command word is (cmd<<24)|data24. Command numbers follow PPSSPP GECommands.h.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "ge_shared.h"
#include "perf_stats.h"

#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "sr_env.h"

/* Wall-clock ms (MinGW clock() is wall time with CLOCKS_PER_SEC=1000). */
static unsigned long wall_ms(void) { return (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC); }

/* ---- command numbers (subset) ---- */
enum {
    GE_NOP = 0x00, GE_VADDR = 0x01, GE_IADDR = 0x02, GE_PRIM = 0x04,
    GE_BEZIER = 0x05, GE_SPLINE = 0x06, GE_BOUNDINGBOX = 0x07,
    GE_JUMP = 0x08, GE_BJUMP = 0x09, GE_CALL = 0x0A, GE_RET = 0x0B,
    GE_END = 0x0C, GE_FINISH = 0x0F, GE_SIGNAL = 0x0E,
    GE_VERTEXTYPE = 0x12,
    GE_BASE = 0x10, GE_OFFSETADDR = 0x13, GE_ORIGIN = 0x14,
    GE_REGION1 = 0x15, GE_REGION2 = 0x16,
    GE_LIGHTINGENABLE = 0x17, GE_LIGHTENABLE0 = 0x18,
    GE_CLIPENABLE = 0x1C, GE_CULLFACEENABLE = 0x1D, GE_TEXTUREMAPENABLE = 0x1E,
    GE_FOGENABLE = 0x1F, GE_DITHERENABLE = 0x20, GE_ALPHABLENDENABLE = 0x21,
    GE_ALPHATESTENABLE = 0x22, GE_ZTESTENABLE = 0x23, GE_STENCILTESTENABLE = 0x24,
    GE_ANTIALIASENABLE = 0x25, GE_PATCHCULLENABLE = 0x26,
    GE_COLORTESTENABLE = 0x27, GE_LOGICOPENABLE = 0x28,
    GE_MORPHWEIGHT0 = 0x2C, GE_MORPHWEIGHT7 = 0x33,
    GE_PATCHDIVISION = 0x36, GE_PATCHPRIMITIVE = 0x37, GE_PATCHFACING = 0x38,
    GE_WORLDMATRIXNUMBER = 0x3A, GE_WORLDMATRIXDATA = 0x3B,
    GE_VIEWMATRIXNUMBER  = 0x3C, GE_VIEWMATRIXDATA  = 0x3D,
    GE_PROJMATRIXNUMBER  = 0x3E, GE_PROJMATRIXDATA  = 0x3F,
    GE_TGENMATRIXNUMBER = 0x40, GE_TGENMATRIXDATA = 0x41,
    GE_VIEWPORTXSCALE = 0x42, GE_VIEWPORTYSCALE = 0x43, GE_VIEWPORTZSCALE = 0x44,
    GE_VIEWPORTXCENTER = 0x45, GE_VIEWPORTYCENTER = 0x46, GE_VIEWPORTZCENTER = 0x47,
    GE_TEXSCALEU = 0x48, GE_TEXSCALEV = 0x49, GE_TEXOFFSETU = 0x4A, GE_TEXOFFSETV = 0x4B,
    GE_OFFSETX = 0x4C, GE_OFFSETY = 0x4D,
    GE_SHADEMODE = 0x50, GE_REVERSENORMAL = 0x51, GE_MATERIALUPDATE = 0x53,
    GE_MATERIALEMISSIVE = 0x54, GE_MATERIALAMBIENT = 0x55, GE_MATERIALDIFFUSE = 0x56,
    GE_MATERIALSPECULAR = 0x57, GE_MATERIALALPHA = 0x58, GE_MATERIALSPECULARCOEF = 0x5B,
    GE_AMBIENTCOLOR = 0x5C, GE_AMBIENTALPHA = 0x5D, GE_LIGHTMODE = 0x5E,
    GE_LIGHTTYPE0 = 0x5F,                       /* ..0x62 */
    GE_LX0 = 0x63, GE_LDX0 = 0x6F, GE_LKA0 = 0x7B,   /* pos / dir / attenuation, 12 each */
    GE_LKS0 = 0x87, GE_LKO0 = 0x8B,                  /* spot exponent / cutoff, 4 each */
    GE_LAC0 = 0x8F,                                  /* light colours amb/diff/spec ×4 = 12 */
    GE_CULL = 0x9B,
    GE_FRAMEBUFPTR = 0x9C, GE_FRAMEBUFWIDTH = 0x9D, GE_ZBUFPTR = 0x9E, GE_ZBUFWIDTH = 0x9F,
    GE_TEXADDR0 = 0xA0, GE_TEXBUFWIDTH0 = 0xA8,
    GE_CLUTADDR = 0xB0, GE_CLUTADDRUPPER = 0xB1,
    GE_TEXSIZE0 = 0xB8,
    GE_TEXMAPMODE = 0xC0, GE_TEXSHADELS = 0xC1, GE_TEXMODE = 0xC2, GE_TEXFORMAT = 0xC3,
    GE_LOADCLUT = 0xC4, GE_CLUTFORMAT = 0xC5, GE_TEXFILTER = 0xC6, GE_TEXWRAP = 0xC7,
    GE_TEXLEVEL = 0xC8, GE_TEXFUNC = 0xC9, GE_TEXENVCOLOR = 0xCA,
    GE_TEXFLUSH = 0xCB, GE_TEXSYNC = 0xCC,
    GE_FOG1 = 0xCD, GE_FOG2 = 0xCE, GE_FOGCOLOR = 0xCF, GE_TEXLODSLOPE = 0xD0,
    GE_FRAMEBUFPIXFORMAT = 0xD2, GE_CLEARMODE = 0xD3,
    GE_SCISSOR1 = 0xD4, GE_SCISSOR2 = 0xD5,
    GE_MINZ = 0xD6, GE_MAXZ = 0xD7,
    GE_COLORTEST = 0xD8, GE_COLORREF = 0xD9, GE_COLORTESTMASK = 0xDA,
    GE_ALPHATEST = 0xDB, GE_STENCILTEST = 0xDC, GE_STENCILOP = 0xDD, GE_ZTEST = 0xDE,
    GE_BLENDMODE = 0xDF, GE_BLENDFIXEDA = 0xE0, GE_BLENDFIXEDB = 0xE1,
    GE_DITH0 = 0xE2, GE_DITH1 = 0xE3, GE_DITH2 = 0xE4, GE_DITH3 = 0xE5,
    GE_LOGICOP = 0xE6, GE_ZWRITEDISABLE = 0xE7,
    GE_MASKRGB = 0xE8, GE_MASKALPHA = 0xE9,
    GE_TRANSFERSRC = 0xB2, GE_TRANSFERSRCW = 0xB3,
    GE_TRANSFERDST = 0xB4, GE_TRANSFERDSTW = 0xB5,
    GE_TRANSFERSTART = 0xEA, GE_TRANSFERSRCPOS = 0xEB,
    GE_TRANSFERDSTPOS = 0xEC, GE_TRANSFERSIZE = 0xEE,
};

/* GeState now lives in ge_shared.h (shared with the optional GPU backend). */
static GeState ge;
static int s_ge_inited = 0;
static uint32_t s_ge_frame = 0;
static GeDebugContext s_debug_context = {
    0, UINT32_MAX, 0, 0, 0, 0
};

/* Optional rasterizer backend (ge_shared.h). NULL = the reference software path. */
static const GeBackend *s_backend = NULL;
int g_sr_gpu_vram_read_watch = 0;
int g_sr_gpu_vram_write_watch = 0;
void ge_set_backend(const GeBackend *backend) {
    s_backend = backend;
    if (!backend) {
        g_sr_gpu_vram_read_watch = 0;
        g_sr_gpu_vram_write_watch = 0;
    }
}
const GeBackend *ge_backend_get(void) { return s_backend; }
GeState *ge_state_ptr(void) { return &ge; }
void ge_debug_set_list(uint32_t list_id, uint32_t list_addr) {
    s_debug_context.list_id = list_id;
    s_debug_context.list_addr = list_addr;
    s_debug_context.draw_pc = 0;
    s_debug_context.draw_op = 0;
}
GeDebugContext ge_debug_context(void) {
    GeDebugContext context = s_debug_context;
    context.frame = s_ge_frame;
    return context;
}
static void ge_debug_begin_draw(uint32_t pc, uint32_t op) {
    s_debug_context.draw_serial++;
    s_debug_context.draw_pc = pc;
    s_debug_context.draw_op = op;
}
void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (s_backend && s_backend->vram_dirty) s_backend->vram_dirty(addr, bytes);
}
void sr_gpu_vram_read(uint32_t addr, uint32_t bytes) {
    if (s_backend && s_backend->vram_read) {
        SrPerfCategory previous = sr_perf_switch(SR_PERF_SYNC);
        s_backend->vram_read(addr, bytes);
        sr_perf_switch(previous);
    }
}
void sr_gpu_vram_prepare_write(uint32_t addr, uint32_t bytes) {
    if (s_backend && s_backend->vram_write) {
        SrPerfCategory previous = sr_perf_switch(SR_PERF_SYNC);
        s_backend->vram_write(addr, bytes);
        sr_perf_switch(previous);
    }
}
void sr_gpu_vram_watch_reads(int enabled) {
    g_sr_gpu_vram_read_watch = enabled != 0;
}
void sr_gpu_vram_watch_writes(int enabled) {
    g_sr_gpu_vram_write_watch = enabled != 0;
}

/* Decode a GE 24-bit float mantissa word to IEEE-754 float.
 * The GE stores floats as the upper 24 bits of a 32-bit float (mantissa only; exp+sign). */
static float decode_float24(uint32_t data) {
    uint32_t f = data << 8;
    float r; memcpy(&r, &f, 4); return r;
}

static void ge_state_init(void) {
    ge.scis_x2 = 479; ge.scis_y2 = 271;
    ge.maxz = 0xFFFF;
    ge.tex_scale_u = ge.tex_scale_v = 1.0f;
    ge.shade_gouraud = 1;
    ge.amb_alpha = 0xFF;   /* lit alpha multiplies by this; 0 would blank lit geometry pre-init */
    s_ge_inited = 1;
}

static void ge_debug_dump_floats(
    FILE *file, const char *name, const float *values, size_t count) {
    fprintf(file, "%s=", name);
    for (size_t i = 0; i < count; i++)
        fprintf(file, "%s%a", i ? "," : "", (double)values[i]);
    fputc('\n', file);
}

int ge_debug_dump(const char *path) {
    if (!path || !path[0]) {
        fprintf(stderr, "GE: state dump requires a non-empty path\n");
        return 0;
    }
    if (!s_ge_inited) ge_state_init();

    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "GE: could not open state dump %s: %s\n",
                path, strerror(errno));
        return 0;
    }

    GeDebugContext context = ge_debug_context();
    const char *backend = s_backend && s_backend->name ? s_backend->name : "sw";
    fprintf(file, "KZL_GE_STATE_V1\n");
    fprintf(file, "[context]\n");
    fprintf(file, "backend=%s\n", backend);
    fprintf(file, "frame=%u\n", context.frame);
    fprintf(file, "list_id=%u\n", context.list_id);
    fprintf(file, "list_handle=0x%08x\n",
            context.list_id == UINT32_MAX
                ? UINT32_MAX : context.list_id ^ 0x35000000u);
    fprintf(file, "list_addr=0x%08x\n", context.list_addr);
    fprintf(file, "draw_serial=%u\n", context.draw_serial);
    fprintf(file, "draw_pc=0x%08x\n", context.draw_pc);
    fprintf(file, "draw_op=0x%08x\n", context.draw_op);

    fprintf(file, "[addresses]\n");
    fprintf(file,
            "base=0x%08x offset=0x%08x vaddr=0x%08x iaddr=0x%08x "
            "vtype=0x%08x\n",
            ge.base, ge.offset, ge.vaddr, ge.iaddr, ge.vtype);
    fprintf(file,
            "framebuffer=0x%08x stride=%u format=%u "
            "depth=0x%08x depth_stride=%u\n",
            ge.fbp, ge.fbw, ge.fbfmt, ge.zbp, ge.zbw);

    fprintf(file, "[raster]\n");
    fprintf(file,
            "scissor=%d,%d,%d,%d depth_test=%d depth_func=%u "
            "depth_write_disable=%d minz=%u maxz=%u depth_clip=%u\n",
            ge.scis_x1, ge.scis_y1, ge.scis_x2, ge.scis_y2,
            ge.ztest_enable, ge.ztest, ge.zwrite_disable,
            ge.minz, ge.maxz, ge.depth_clip);
    fprintf(file,
            "cull_enable=%d cull=%u clear=%d clear_mode=0x%08x "
            "shade_gouraud=%d\n",
            ge.cull_enable, ge.cull, ge.clear, ge.clear_mode,
            ge.shade_gouraud);
    fprintf(file,
            "blend_enable=%d blend_mode=0x%08x fixed_a=0x%08x "
            "fixed_b=0x%08x logic_enable=%d logic_op=%u\n",
            ge.blend_enable, ge.blend_mode, ge.blend_fixa, ge.blend_fixb,
            ge.logic_op_enable, ge.logic_op);
    fprintf(file,
            "alpha_test=%d/0x%08x color_test=%d/0x%08x/0x%08x/0x%08x "
            "stencil_test=%d/0x%08x/0x%08x\n",
            ge.atest_enable, ge.atest,
            ge.color_test_enable, ge.color_test, ge.color_ref,
            ge.color_test_mask,
            ge.stencil_test_enable, ge.stencil_test, ge.stencil_op);
    fprintf(file,
            "mask_rgb=0x%08x mask_alpha=0x%08x dither_enable=%d "
            "fog=%d/%a/%a/0x%08x\n",
            ge.mask_rgb, (uint32_t)ge.mask_alpha, ge.dither_enable,
            ge.fog_enable, (double)ge.fog_end, (double)ge.fog_slope,
            ge.fog_color);

    fprintf(file, "[texture]\n");
    fprintf(file,
            "enable=%d addr=0x%08x stride=%u format=%u size=%dx%d "
            "swizzle=%d max_level=%d mode=0x%08x level=0x%08x\n",
            ge.tex_enable, ge.tex_addr, ge.tex_bufw, ge.tex_fmt,
            ge.tex_w, ge.tex_h, ge.tex_swizzle, ge.tex_max_level,
            ge.tex_mode, ge.tex_level);
    fprintf(file,
            "wrap=0x%08x filter=0x%08x function=0x%08x env=0x%08x "
            "clut=0x%08x/0x%08x generation=%u\n",
            ge.tex_wrap, ge.tex_filter, ge.tex_func, ge.tex_env,
            ge.clut_addr, ge.clut_fmt, ge.clut_gen);
    fprintf(file,
            "map_mode=0x%08x shade_ls=0x%08x scale=%a,%a offset=%a,%a\n",
            ge.tex_map_mode, ge.tex_shade_ls,
            (double)ge.tex_scale_u, (double)ge.tex_scale_v,
            (double)ge.tex_off_u, (double)ge.tex_off_v);
    for (int level = 0; level < 8; level++) {
        fprintf(file,
                "level%d=0x%08x,%u,%d,%d\n",
                level, ge.tex_addr_levels[level],
                ge.tex_bufw_levels[level], ge.tex_width_levels[level],
                ge.tex_height_levels[level]);
    }

    fprintf(file, "[geometry]\n");
    fprintf(file,
            "patch_division=0x%08x patch_primitive=%u patch_facing=%u "
            "lighting=%d reverse_normal=%d light_mode=%d\n",
            ge.patch_division, ge.patch_primitive, ge.patch_facing,
            ge.lighting_enable, ge.reverse_normal, ge.light_mode);
    ge_debug_dump_floats(file, "world", ge.world, 12);
    ge_debug_dump_floats(file, "view", ge.view, 12);
    ge_debug_dump_floats(file, "projection", ge.proj, 16);
    ge_debug_dump_floats(file, "bones", ge.bone, 96);
    ge_debug_dump_floats(file, "morph_weights", ge.morph_weight, 8);
    ge_debug_dump_floats(file, "texgen", ge.tgen, 12);

    fprintf(file, "[transfer]\n");
    fprintf(file,
            "source=0x%08x source_width=0x%08x destination=0x%08x "
            "destination_width=0x%08x source_pos=0x%08x "
            "destination_pos=0x%08x size=0x%08x\n",
            ge.xf_src, ge.xf_srcw, ge.xf_dst, ge.xf_dstw,
            ge.xf_spos, ge.xf_dpos, ge.xf_size);

    fprintf(file, "[raw-state]\n");
    const unsigned char *bytes = (const unsigned char *)&ge;
    for (size_t offset = 0; offset < sizeof(ge); offset += 16) {
        fprintf(file, "%04zx:", offset);
        size_t end = offset + 16 < sizeof(ge) ? offset + 16 : sizeof(ge);
        for (size_t i = offset; i < end; i++)
            fprintf(file, " %02x", bytes[i]);
        fputc('\n', file);
    }

    int failed = ferror(file);
    if (fclose(file) != 0) failed = 1;
    if (failed) {
        fprintf(stderr, "GE: failed while writing state dump %s: %s\n",
                path, strerror(errno));
        return 0;
    }
    return 1;
}

/* ---- Depth buffer ----
 * Software 16-bit depth buffer, one entry per pixel, max stride 512.
 * Only active when ge.ztest_enable is set. */
static uint16_t s_zbuf[272 * 512];

static uint32_t zbuf_stride(void) { return ge.zbw ? ge.zbw : (ge.fbw ? ge.fbw : 512); }

uint16_t *ge_zbuf_ptr(void) { return s_zbuf; }
uint32_t  ge_zbuf_stride(void) { return zbuf_stride(); }

static void zbuf_fill_rect(int x0, int y0, int x1, int y1, uint16_t z) {
    uint32_t stride = zbuf_stride();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    for (int y = y0; y <= y1; y++) {
        uint32_t base = (uint32_t)y * stride;
        for (int x = x0; x <= x1; x++) {
            uint32_t idx = base + (uint32_t)x;
            if (idx < 272u * 512u) s_zbuf[idx] = z;
        }
    }
}

static void stat_zfail(void);
static int ztest_px(int x, int y, uint16_t z) {
    if (!ge.ztest_enable) return 1;
    uint32_t idx = (uint32_t)(y * (int)zbuf_stride() + x);
    if (idx >= 272u * 512u) return 0;
    uint16_t cur = s_zbuf[idx];
    int pass;
    switch (ge.ztest & 7) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = (z == cur); break;
        case 3: pass = (z != cur); break;
        case 4: pass = (z < cur); break;
        case 5: pass = (z <= cur); break;
        case 6: pass = (z > cur); break;
        case 7: pass = (z >= cur); break;
        default: pass = 1;
    }
    if (!pass) stat_zfail();
    return pass;
}

static void zwrite_px(int x, int y, uint16_t z) {
    if (!ge.ztest_enable || ge.zwrite_disable) return;
    uint32_t idx = (uint32_t)(y * (int)zbuf_stride() + x);
    if (idx < 272u * 512u) s_zbuf[idx] = z;
}

/* Clamp interpolated depth to the 16-bit range. minz/maxz are NOT a clamp: the PSP
 * depth-range test DISCARDS transform-mode pixels outside [minz,maxz] (see zrange_ok). */
static uint16_t clamp_z(float zf) {
    if (zf < 0.0f) zf = 0.0f; else if (zf > 65535.0f) zf = 65535.0f;
    return (uint16_t)zf;
}

/* PSP depth-range test (PPSSPP DrawPixel.cpp DrawSinglePixel): every transform-mode pixel
 * with z outside [minz,maxz] is dropped — even in clear mode, even with z-test disabled.
 * Games use this to mask effect passes (e.g. the hangar reflection copy of the aircraft).
 * Clamping instead of discarding made those passes paint over the scene. */
static int zrange_ok(float zf, int persp) {
    if (!persp) return 1;
    int z = (int)zf;
    return z >= (int)ge.minz && z <= (int)ge.maxz;
}

/* Decode the vertex format. Supports 8/16/float texcoord, colour, normal, and position.
 * Returns the stride; fills offsets (-1 if absent). through=1 means screen-space (2D). */
typedef struct {
    int stride, tc_off, tc_fmt, col_off, col_fmt;
    int nrm_off, nrm_fmt;    /* normal (lighting; -1 if absent) */
    int pos_off, pos_fmt;
    int w_off, w_fmt, w_n;   /* skinning weights (w_n==0 if absent) */
    int one_size, morph_n;
    int through;
} VFmt;

static int clamp8(int v);

static void decode_vtype(uint32_t vt, VFmt *v) {
    memset(v, 0, sizeof(*v));
    v->nrm_off = -1; v->tc_off = -1; v->col_off = -1;
    int tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3;
    int wt = (vt >> 9) & 3;   /* weight format (skinning) */
    int wc = (vt >> 14) & 7;  /* weight count */
    v->through = (vt >> 23) & 1;

    static const int csz[4] = { 0, 1, 2, 4 };               /* none/8-bit/16-bit/float */
    static const int colsz[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }; /* 4=5650,5=5551,6=4444,7=8888 */
    int off = 0, maxa = 1;

    /* Skinning weights come first in the vertex if present. The count field is n-1: wc==0
     * with a nonzero format means ONE weight (the old `wt && wc` guard skipped that case,
     * misaligning every later field — the hangar aircraft uses single-weight vertices). */
    if (wt) {
        int a = csz[wt];
        off = (off + a - 1) & ~(a - 1);
        v->w_fmt = wt; v->w_n = wc + 1; v->w_off = off;
        off += a * (wc + 1);
        if (a > maxa) maxa = a;
    }

    v->tc_fmt = tc;
    if (tc) { int a = csz[tc]; off = (off+a-1)&~(a-1); v->tc_off = off; off += a*2; if (a>maxa) maxa=a; }

    v->col_fmt = col;
    if (col) { int a = colsz[col]; off = (off+a-1)&~(a-1); v->col_off = off; off += a; if (a>maxa) maxa=a; }

    v->nrm_fmt = nrm;
    if (nrm) { int a = csz[nrm]; off = (off+a-1)&~(a-1); v->nrm_off = off; off += a*3; if (a>maxa) maxa=a; }

    v->pos_fmt = pos;
    { int a = csz[pos ? pos : 3]; off = (off+a-1)&~(a-1); v->pos_off = off; off += a*3; if (a>maxa) maxa=a; }

    v->one_size = (off + maxa - 1) & ~(maxa - 1);
    v->morph_n = (int)((vt >> 18) & 7u) + 1;
    v->stride = v->one_size * v->morph_n;
}

/* Read position from a vertex. For transform-mode, all components are signed (model space).
 * For through-mode, x and y are screen pixels; z is a 16-bit unsigned depth (0..65535). */
/* Transform-mode fixed-point positions are normalized like the hardware: s8/128, s16/32768
 * (PPSSPP VertexDecoder Step_PosS8ToFloat/Step_PosS16ToFloat). Through-mode keeps raw screen
 * units (x/y signed, z unsigned depth). Reading transform-mode s16 unscaled made every
 * fixed-point model (briefing map, hangar geometry) 32768x too large. */
static void read_pos_raw(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    uint32_t a = addr + (uint32_t)v->pos_off;
    if (v->pos_fmt == 3) {
        uint32_t xi = MEM_R32(a), yi = MEM_R32(a+4), zi = MEM_R32(a+8);
        memcpy(x, &xi, 4); memcpy(y, &yi, 4); memcpy(z, &zi, 4);
    } else if (v->pos_fmt == 2) {
        if (v->through) {
            *x = (float)(int16_t)MEM_R16(a);
            *y = (float)(int16_t)MEM_R16(a+2);
            *z = (float)(uint16_t)MEM_R16(a+4);
        } else {
            *x = (float)(int16_t)MEM_R16(a)   * (1.0f / 32768.0f);
            *y = (float)(int16_t)MEM_R16(a+2) * (1.0f / 32768.0f);
            *z = (float)(int16_t)MEM_R16(a+4) * (1.0f / 32768.0f);
        }
    } else {
        if (v->through) {
            *x = (float)(int8_t)MEM_R8(a);
            *y = (float)(int8_t)MEM_R8(a+1);
            *z = (float)(uint8_t)MEM_R8(a+2);
        } else {
            *x = (float)(int8_t)MEM_R8(a)   * (1.0f / 128.0f);
            *y = (float)(int8_t)MEM_R8(a+1) * (1.0f / 128.0f);
            *z = (float)(int8_t)MEM_R8(a+2) * (1.0f / 128.0f);
        }
    }
}

static void read_pos(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    if (v->morph_n <= 1) {
        read_pos_raw(addr, v, x, y, z);
        return;
    }
    *x = *y = *z = 0.0f;
    for (int i = 0; i < v->morph_n; i++) {
        float px, py, pz;
        read_pos_raw(
            addr + (uint32_t)(i * v->one_size), v, &px, &py, &pz);
        *x += px * ge.morph_weight[i];
        *y += py * ge.morph_weight[i];
        *z += pz * ge.morph_weight[i];
    }
}

/* Read texcoords. Through-mode: absolute texel coordinates. Transform-mode: UNSIGNED u8
 * scaled by 1/128 and UNSIGNED u16 by 1/32768 (PPSSPP Step_TcU8ToFloat/Step_TcU16ToFloat).
 * Reading these as signed flips UVs >= half range (used for texture tiling: 1.0..2.0)
 * to small negatives, which sample the wrong texels. */
static void read_tc_raw(uint32_t addr, const VFmt *v, float *u, float *tv) {
    if (v->tc_off < 0) { *u = 0; *tv = 0; return; }
    uint32_t a = addr + (uint32_t)v->tc_off;
    if (v->tc_fmt == 3) {
        uint32_t ui = MEM_R32(a), vi = MEM_R32(a+4);
        memcpy(u, &ui, 4); memcpy(tv, &vi, 4);
    } else if (v->tc_fmt == 2) {
        if (v->through) {
            *u  = (float)(uint16_t)MEM_R16(a);
            *tv = (float)(uint16_t)MEM_R16(a+2);
        } else {
            *u  = (float)(uint16_t)MEM_R16(a)  * (1.0f / 32768.0f);
            *tv = (float)(uint16_t)MEM_R16(a+2) * (1.0f / 32768.0f);
        }
    } else {
        if (v->through) {
            *u  = (float)MEM_R8(a);
            *tv = (float)MEM_R8(a+1);
        } else {
            *u  = (float)MEM_R8(a)  * (1.0f / 128.0f);
            *tv = (float)MEM_R8(a+1) * (1.0f / 128.0f);
        }
    }
}

static void read_tc(uint32_t addr, const VFmt *v, float *u, float *tv) {
    if (v->tc_off < 0 || v->morph_n <= 1) {
        read_tc_raw(addr, v, u, tv);
        return;
    }
    *u = *tv = 0.0f;
    for (int i = 0; i < v->morph_n; i++) {
        float tu, ttv;
        read_tc_raw(
            addr + (uint32_t)(i * v->one_size), v, &tu, &ttv);
        *u += tu * ge.morph_weight[i];
        *tv += ttv * ge.morph_weight[i];
    }
}

/* Read a vertex normal (model space). Defaults to +Z when absent. */
static void read_nrm_raw(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    if (v->nrm_off < 0) { *x = 0; *y = 0; *z = 1; return; }
    uint32_t a = addr + (uint32_t)v->nrm_off;
    if (v->nrm_fmt == 3) {
        uint32_t xi = MEM_R32(a), yi = MEM_R32(a+4), zi = MEM_R32(a+8);
        memcpy(x, &xi, 4); memcpy(y, &yi, 4); memcpy(z, &zi, 4);
    } else if (v->nrm_fmt == 2) {
        *x = (float)(int16_t)MEM_R16(a)   * (1.0f / 32768.0f);
        *y = (float)(int16_t)MEM_R16(a+2) * (1.0f / 32768.0f);
        *z = (float)(int16_t)MEM_R16(a+4) * (1.0f / 32768.0f);
    } else {
        *x = (float)(int8_t)MEM_R8(a)   * (1.0f / 128.0f);
        *y = (float)(int8_t)MEM_R8(a+1) * (1.0f / 128.0f);
        *z = (float)(int8_t)MEM_R8(a+2) * (1.0f / 128.0f);
    }
}

static void read_nrm(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    if (v->nrm_off < 0 || v->morph_n <= 1) {
        read_nrm_raw(addr, v, x, y, z);
    } else {
        *x = *y = *z = 0.0f;
        for (int i = 0; i < v->morph_n; i++) {
            float nx, ny, nz;
            read_nrm_raw(
                addr + (uint32_t)(i * v->one_size), v, &nx, &ny, &nz);
            *x += nx * ge.morph_weight[i];
            *y += ny * ge.morph_weight[i];
            *z += nz * ge.morph_weight[i];
        }
    }
    if (ge.reverse_normal) {
        *x = -*x;
        *y = -*y;
        *z = -*z;
    }
}

/* Skinning weights: unsigned fixed-point, u8/128 and u16/32768 (PPSSPP VertexDecoder
 * Step_WeightsU8/U16), float raw. */
static float read_weight_raw(
    uint32_t addr, const VFmt *v, int index) {
    uint32_t a = addr + (uint32_t)v->w_off;
    float weight;
    if (v->w_fmt == 3) {
        uint32_t bits = MEM_R32(a + 4u * (uint32_t)index);
        memcpy(&weight, &bits, 4);
    } else if (v->w_fmt == 2) {
        weight = (float)MEM_R16(a + 2u * (uint32_t)index) *
            (1.0f / 32768.0f);
    } else {
        weight = (float)MEM_R8(a + (uint32_t)index) *
            (1.0f / 128.0f);
    }
    return weight;
}

static void read_weights(uint32_t addr, const VFmt *v, float *w) {
    for (int i = 0; i < v->w_n; i++) {
        if (v->morph_n <= 1) {
            w[i] = read_weight_raw(addr, v, i);
            continue;
        }
        w[i] = 0.0f;
        for (int morph = 0; morph < v->morph_n; morph++) {
            w[i] += read_weight_raw(
                addr + (uint32_t)(morph * v->one_size), v, i) *
                ge.morph_weight[morph];
        }
    }
}

/* Convert a colour in PSP pixel format `psm` (0=5650,1=5551,2=4444,3=8888) read from `raw` into
 * 8-bit R,G,B,A. For 16-bit formats `raw` holds the 16-bit value; for 8888 the full 32 bits. */
static void unpack_color(uint32_t raw, int psm, int *r, int *g, int *b, int *a) {
    switch (psm & 3) {
        case 0: *r=(int)((raw&0x1F)*255/31); *g=(int)(((raw>>5)&0x3F)*255/63); *b=(int)(((raw>>11)&0x1F)*255/31); *a=255; break;
        case 1: *r=(int)((raw&0x1F)*255/31); *g=(int)(((raw>>5)&0x1F)*255/31); *b=(int)(((raw>>10)&0x1F)*255/31); *a=(raw&0x8000)?255:0; break;
        case 2: *r=(int)((raw&0xF)*17); *g=(int)(((raw>>4)&0xF)*17); *b=(int)(((raw>>8)&0xF)*17); *a=(int)(((raw>>12)&0xF)*17); break;
        default: *r=raw&0xFF; *g=(raw>>8)&0xFF; *b=(raw>>16)&0xFF; *a=(raw>>24)&0xFF; break;
    }
}

/* Read a vertex colour into 8-bit RGBA (col_fmt: 4=5650,5=5551,6=4444,7=8888). */
static void read_color_raw(
    uint32_t addr, const VFmt *v, int *r, int *g, int *b, int *a) {
    if (v->col_off < 0) {
        if (ge.material_set || ge.material_alpha_set) {
            *r = ge.material_set ? (ge.material & 0xFF) : 255;
            *g = ge.material_set ? ((ge.material >> 8) & 0xFF) : 255;
            *b = ge.material_set ? ((ge.material >> 16) & 0xFF) : 255;
            *a = ge.material_alpha_set ? ge.material_alpha : 255;
        } else {
            *r=*g=*b=*a=255;
        }
        return;
    }
    uint32_t ca = addr + (uint32_t)v->col_off;
    if (v->col_fmt == 7) unpack_color(MEM_R32(ca), 3, r, g, b, a);
    else unpack_color(MEM_R16(ca), v->col_fmt - 4, r, g, b, a);
}

static void read_color(
    uint32_t addr, const VFmt *v, int *r, int *g, int *b, int *a) {
    if (v->col_off < 0 || v->morph_n <= 1) {
        read_color_raw(addr, v, r, g, b, a);
        return;
    }
    float color[4] = {0, 0, 0, 0};
    for (int i = 0; i < v->morph_n; i++) {
        int mr, mg, mb, ma;
        read_color_raw(
            addr + (uint32_t)(i * v->one_size), v,
            &mr, &mg, &mb, &ma);
        color[0] += (float)mr * ge.morph_weight[i];
        color[1] += (float)mg * ge.morph_weight[i];
        color[2] += (float)mb * ge.morph_weight[i];
        color[3] += (float)ma * ge.morph_weight[i];
    }
    *r = clamp8((int)color[0]);
    *g = clamp8((int)color[1]);
    *b = clamp8((int)color[2]);
    *a = clamp8((int)color[3]);
}

/* Look up a CLUT entry (palette index -> RGBA). */
static void clut_lookup_level(uint32_t index, int level, uint32_t tex_fmt,
                              int *r, int *g, int *b, int *a) {
    uint32_t shift = (ge.clut_fmt >> 2) & 0x1F;
    uint32_t mask  = (ge.clut_fmt >> 8) & 0xFF;
    uint32_t start = ((ge.clut_fmt >> 16) & 0x1F) << 4;
    uint32_t start_mask = (ge.clut_fmt & 3) == 3 ? 0xFFu : 0x1FFu;
    index = ((index >> shift) & mask) | (start & start_mask);
    if ((ge.tex_mode & 0x100u) != 0) {
        if (tex_fmt == 4) index += (uint32_t)level * 16u;
        else if (tex_fmt == 5 && (ge.clut_fmt & 3) != 3)
            index += (uint32_t)(level & 1) * 256u;
    }
    int epsm = ge.clut_fmt & 3;
    if (epsm == 3) { uint32_t raw; memcpy(&raw, &ge.clutram[(index * 4) & 2047], 4); unpack_color(raw, 3, r, g, b, a); }
    else           { uint16_t raw; memcpy(&raw, &ge.clutram[(index * 2) & 2047], 2); unpack_color(raw, epsm, r, g, b, a); }
}

unsigned long g_tex_samples = 0, g_tex_nonzero = 0;

/* ---- Per-frame statistics (SR_GESTAT=1: print every 60 frames) ----
 * Used to diagnose "black screen" states: shows whether 3D geometry is being culled,
 * near-clipped, z-rejected, alpha-killed, or drawn. */
static struct {
    unsigned long tri2d, tri3d, spr2d, spr3d, cull, nearclip, zfail, afail, px2d, px3d, zrange;
    unsigned long mw_world, mw_view, mw_proj;   /* matrix data words captured */
    unsigned long blk3d;                        /* 3D pixels written with final RGB == 0 */
    uint32_t fbs[4]; unsigned long fbn[4];      /* distinct draw-buffer addrs hit + pixel counts */
    unsigned long fog0, fogfull, texa0;         /* full-fog pixels, no-fog pixels, alpha==0 texels */
    unsigned long skin, bonew;                  /* skinned (weighted) 3D prims, bone matrix writes */
    unsigned long deg3d;                        /* zero-area 3D triangles (zeroed/missing vertex data) */
    unsigned long line3d;                       /* 3D line segments submitted (wireframe map etc.) */
} s_stat;

/* SR_GEMATW=1: trace the first view/proj matrix register writes (cursor + decoded value)
 * to distinguish "uploads never arrive" from "uploads land in the wrong slots". */
static void trace_mtx_write(const char *name, int cursor, uint32_t data) {
    static int on = -1, n = 0;
    if (on < 0) on = getenv("SR_GEMATW") ? 1 : 0;
    if (!on || n >= 120) return;
    n++;
    fprintf(stderr, "GEMATW f=%u %s[%d] = 0x%06x (%g)\n",
            s_ge_frame, name, cursor, data, decode_float24(data));
}
static void ge_snapshot_plain(const char *path);   /* defined near ge_snapshot below */

static int s_stat_on = -1;
static unsigned long s_ge_ms_acc = 0;   /* wall ms spent inside ge_run_list this stat window */
static unsigned long s_wall_last = 0;   /* wall ms at end of previous stat window */
static int s_in3d = 0;   /* current raster path is transform-mode (for px2d/px3d split) */
static int s_ashade = 0; /* per-window budget for alpha-test failure dumps (SR_GESTAT) */
static int s_pixwho_n = 0; /* per-window budget for SR_PIXWHO pixel-ownership dumps */
static int s_pxpass = 0; /* per-window budget for passing-3D-pixel dumps (SR_GESTAT) */
static int s_pxc = 0;    /* per-window budget for centre-pixel write traces (SR_GESTAT) */
static int s_neardump = 0; /* per-window budget for fully-near-clipped primitive dumps (SR_GESTAT) */
static int s_ge3d = 0;     /* per-window budget for per-3D-draw GE3D lines (SR_GESTAT) */
static uint32_t s_ge3d_sigs[48];  /* distinct draw signatures already printed this window */

/* ---- SR_RTRACE=1: exhaustive render trace ----
 * For the first SR_RTRACE_FRAMES (default 2) transform-mode frames of every 60-frame stat
 * window, logs ONE line per 3D draw call with the complete GE state plus per-reason
 * accept/drop counters (TRIDRW/TRIDRW+), and one line per triangle with clip-space coords,
 * screen-grid coords and acceptance flags (TRIDEC) — capped per draw, EXCEPT triangles whose
 * screen bbox covers the SR_PIXWHO probe pixel, which always log (drawn or dropped). */
static int s_rtrace = -1;            /* env flag, lazy init */
static int s_rt_fbudget = 2;         /* frames traced per window (SR_RTRACE_FRAMES) */
static int s_rt_frames = 0;          /* frames traced so far this window */
static uint32_t s_rt_cur_frame = 0xFFFFFFFFu;
static int s_rt_active = 0;          /* tracing the current frame */
static uint32_t s_rt_draw = 0;       /* global 3D draw sequence number */
static int s_rt_draws_f = 0;         /* draws logged this frame (cap) */

/* Per-texture alpha-test outcome counters for transform-mode fragments (SR_GESTAT). Shows which
 * textures a scene's 3D actually samples and which of them the alpha test eats. */
#define ATEX_N 16
static struct { uint32_t addr; unsigned long fail, pass; } s_atex[ATEX_N];
static void atex_note(uint32_t addr, int pass) {
    for (int i = 0; i < ATEX_N; i++) {
        if (s_atex[i].addr == addr || (s_atex[i].fail | s_atex[i].pass) == 0) {
            s_atex[i].addr = addr;
            if (pass) s_atex[i].pass++; else s_atex[i].fail++;
            return;
        }
    }
}
static void stat_zfail(void) { s_stat.zfail++; }

/* Through-mode primitives honour the depth test like the hardware; SR_NO2DZ=1 disables
 * that for bisecting bad-depth-state symptoms. */
static int thru_ztest(void) {
    static int no2dz = -1;
    if (no2dz < 0) no2dz = getenv("SR_NO2DZ") ? 1 : 0;
    return ge.ztest_enable && !no2dz;
}

uint32_t ge_get_frame(void) { return s_ge_frame; }

void ge_set_frame(uint32_t frame) {
    s_ge_frame = frame;
    if (s_stat_on < 0) s_stat_on = getenv("SR_GESTAT") ? 1 : 0;
    if (s_stat_on && frame && (frame % 60) == 0) {
        unsigned long now = wall_ms();
        fprintf(stderr,
            "GESTAT f=%u wall=%lums ge=%lums tri2d=%lu tri3d=%lu spr2d=%lu spr3d=%lu cull=%lu near=%lu zfail=%lu afail=%lu px2d=%lu px3d=%lu mw=%lu/%lu/%lu\n",
            frame, now - s_wall_last, s_ge_ms_acc,
            s_stat.tri2d, s_stat.tri3d, s_stat.spr2d, s_stat.spr3d,
            s_stat.cull, s_stat.nearclip, s_stat.zfail, s_stat.afail, s_stat.px2d, s_stat.px3d,
            s_stat.mw_world, s_stat.mw_view, s_stat.mw_proj);
        s_wall_last = now;
        s_ge_ms_acc = 0;
        fprintf(stderr,
            "GESTAT+ f=%u blk3d=%lu skin=%lu bonew=%lu deg3d=%lu line3d=%lu zrange=%lu zr=[%u,%u] fmt=%u zbp=0x%08x fb=[0x%08x:%lu 0x%08x:%lu 0x%08x:%lu 0x%08x:%lu]\n",
            frame, s_stat.blk3d, s_stat.skin, s_stat.bonew, s_stat.deg3d, s_stat.line3d,
            s_stat.zrange, ge.minz, ge.maxz, ge.fbfmt, ge.zbp,
            s_stat.fbs[0], s_stat.fbn[0], s_stat.fbs[1], s_stat.fbn[1],
            s_stat.fbs[2], s_stat.fbn[2], s_stat.fbs[3], s_stat.fbn[3]);
        fprintf(stderr,
            "GESTAT* f=%u fog0=%lu fogfull=%lu texa0=%lu/%lu atest=%d/0x%06x fogc=0x%06x fend=%g fslope=%g vp=(%.4g,%.4g,%.4g,%.4g) off=(%.4g,%.4g)\n",
            frame, s_stat.fog0, s_stat.fogfull, s_stat.texa0, g_tex_samples,
            ge.atest_enable, ge.atest, ge.fog_color, ge.fog_end, ge.fog_slope,
            decode_float24(ge.viewport_raw[0]), decode_float24(ge.viewport_raw[3]),
            decode_float24(ge.viewport_raw[1]), decode_float24(ge.viewport_raw[4]),
            (float)(ge.offsetx & 0xFFFF) / 16.0f, (float)(ge.offsety & 0xFFFF) / 16.0f);
        for (int i = 0; i < ATEX_N; i++) {
            if (s_atex[i].fail + s_atex[i].pass < 500) continue;
            fprintf(stderr, "ATEX f=%u addr=0x%08x fail=%lu pass=%lu\n",
                    frame, s_atex[i].addr, s_atex[i].fail, s_atex[i].pass);
        }
        memset(s_atex, 0, sizeof(s_atex));
        s_ashade = 0;
        s_pxpass = 0;
        s_pxc = 0;
        s_neardump = 0;
        s_ge3d = 0;
        s_pixwho_n = 0;
        s_rt_frames = 0;
        memset(&s_stat, 0, sizeof(s_stat));
        /* SR_FBSNAP=1: dump the current GE framebuffer once per stat window (max 40 files,
         * or SR_FBSNAP=<n> for a custom cap) so misrendered scenes can be inspected
         * pixel-exactly alongside the trace. */
        {
            static int fbsnap = -1; static int nsnap = 0;
            if (fbsnap < 0) {
                const char *fs = getenv("SR_FBSNAP");
                fbsnap = 0;
                if (fs && fs[0]) { fbsnap = atoi(fs); if (fbsnap < 2) fbsnap = 40; }
            }
            if (fbsnap && nsnap < fbsnap) {
                char sp[96];
                snprintf(sp, sizeof(sp), "snap_f%05u.ppm", frame);
                ge_snapshot_plain(sp);
                nsnap++;
            }
        }
    }
}

static uint32_t texel_offset(uint32_t row_pitch_pixels, uint32_t u, uint32_t v,
                             uint32_t texel_bits, int swizzled) {
    if (!swizzled)
        return v * ((row_pitch_pixels * texel_bits) >> 3) + ((u * texel_bits) >> 3);

    /* PPSSPP Software/Sampler.cpp GetPixelDataOffset() */
    const uint32_t tile_bits = 32, tiles_x = 4, tiles_y = 8;
    uint32_t texels_per_tile = tile_bits / texel_bits;
    uint32_t tile_u   = u / texels_per_tile;
    uint32_t tile_idx = (v % tiles_y) * tiles_x
        + (v / tiles_y) * ((row_pitch_pixels * texel_bits / tile_bits) * tiles_y)
        + (tile_u % tiles_x)
        + (tile_u / tiles_x) * (tiles_x * tiles_y);
    return tile_idx * (tile_bits >> 3) + (((u % texels_per_tile) * texel_bits) >> 3);
}

static int tex_coord(int v, int size, int clamp_mode) {
    if (size <= 0) return 0;
    if (clamp_mode) {
        if (v < 0) return 0;
        if (v >= size) return size - 1;
        return v;
    }
    return v & (size - 1) & 511;
}

typedef struct {
    uint8_t lines[4];
    uint16_t color1, color2;
} GeDxt1Block;

typedef struct {
    GeDxt1Block color;
    uint16_t alpha_lines[4];
} GeDxt3Block;

typedef struct {
    GeDxt1Block color;
    uint32_t alpha_data2;
    uint16_t alpha_data1;
    uint8_t alpha1, alpha2;
} GeDxt5Block;

static void texture_level_info(int level, uint32_t *address, uint32_t *stride,
                               int *width, int *height) {
    if (level <= 0) {
        *address = ge.tex_addr;
        *stride = ge_texture_buffer_width(ge.tex_fmt, ge.tex_bufw);
        *width = ge.tex_w;
        *height = ge.tex_h;
        return;
    }
    *address = ge.tex_addr_levels[level];
    *width = ge.tex_width_levels[level];
    *height = ge.tex_height_levels[level];
    *stride = ge_texture_buffer_width(
        ge.tex_fmt, ge.tex_bufw_levels[level]);
}

uint64_t ge_texture_level_byte_size(const GeState *state, int level) {
    if (!state || level < 0 || level >= 8) return 0;
    uint32_t format = state->tex_fmt & 15u;
    uint32_t bits = ge_texture_bits_per_pixel(format);
    uint32_t stride;
    int width;
    int height;
    if (level == 0) {
        width = state->tex_w;
        height = state->tex_h;
        stride = ge_texture_buffer_width(state->tex_fmt, state->tex_bufw);
    } else {
        width = state->tex_width_levels[level];
        height = state->tex_height_levels[level];
        stride = ge_texture_buffer_width(
            state->tex_fmt, state->tex_bufw_levels[level]);
    }
    if (!bits || width <= 0 || height <= 0 || !stride) return 0;
    if (format >= 8 && format <= 10) {
        uint64_t blocks_x = ((uint64_t)stride + 3u) / 4u;
        uint64_t blocks_y = ((uint64_t)(uint32_t)height + 3u) / 4u;
        return blocks_x * blocks_y * (format == 8 ? 8u : 16u);
    }
    uint64_t rows = state->tex_swizzle
        ? ((uint64_t)(uint32_t)height + 7u) & ~UINT64_C(7)
        : (uint64_t)(uint32_t)height;
    return ((uint64_t)stride * rows * bits + 7u) >> 3;
}

static uint32_t dxt_color(const GeDxt1Block *block, int x, int y, int alpha) {
    uint16_t c1 = block->color1;
    uint16_t c2 = block->color2;
    int b1 = (c1 << 3) & 0xF8, b2 = (c2 << 3) & 0xF8;
    int g1 = (c1 >> 3) & 0xFC, g2 = (c2 >> 3) & 0xFC;
    int r1 = (c1 >> 8) & 0xF8, r2 = (c2 >> 8) & 0xF8;
    int index = (block->lines[y] >> (x * 2)) & 3;
    int r, g, b, a = alpha;
    if (index == 0) {
        r = r1; g = g1; b = b1;
    } else if (index == 1) {
        r = r2; g = g2; b = b2;
    } else if (c1 > c2) {
        if (index == 2) {
            r = (r1 + r1 + r2) / 3;
            g = (g1 + g1 + g2) / 3;
            b = (b1 + b1 + b2) / 3;
        } else {
            r = (r2 + r2 + r1) / 3;
            g = (g2 + g2 + g1) / 3;
            b = (b2 + b2 + b1) / 3;
        }
    } else if (index == 3) {
        r = g = b = a = 0;
    } else {
        r = (r1 + r2) / 2;
        g = (g1 + g2) / 2;
        b = (b1 + b2) / 2;
    }
    return (uint32_t)r | ((uint32_t)g << 8) |
           ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static uint8_t dxt_alpha_lerp(uint8_t a1, uint8_t a2, int n, int denominator) {
    int v1 = (a1 * ((denominator - n) << 8)) / denominator;
    int v2 = (a2 * (n << 8)) / denominator;
    return (uint8_t)((v1 + v2 + 31) >> 8);
}

static uint32_t dxt_decode(
    const uint8_t *bytes, uint32_t format, uint32_t u, uint32_t v) {
    if (format == 8) {
        GeDxt1Block block;
        memcpy(&block, bytes, sizeof(block));
        return dxt_color(&block, (int)(u & 3), (int)(v & 3), 255);
    }
    if (format == 9) {
        GeDxt3Block block;
        memcpy(&block, bytes, sizeof(block));
        uint32_t color = dxt_color(
            &block.color, (int)(u & 3), (int)(v & 3), 0);
        uint32_t alpha =
            (block.alpha_lines[v & 3] >> ((u & 3) * 4)) & 15u;
        return color | (alpha << 28);
    }
    GeDxt5Block block;
    memcpy(&block, bytes, sizeof(block));
    uint32_t color = dxt_color(
        &block.color, (int)(u & 3), (int)(v & 3), 0);
    uint64_t alpha_data =
        ((uint64_t)block.alpha_data1 << 32) | block.alpha_data2;
    uint32_t alpha_index =
        (uint32_t)((alpha_data >> ((v & 3) * 12 + (u & 3) * 3)) & 7u);
    uint8_t alpha;
    if (alpha_index == 0) alpha = block.alpha1;
    else if (alpha_index == 1) alpha = block.alpha2;
    else if (block.alpha1 > block.alpha2)
        alpha = dxt_alpha_lerp(
            block.alpha1, block.alpha2, (int)alpha_index - 1, 7);
    else if (alpha_index == 6) alpha = 0;
    else if (alpha_index == 7) alpha = 255;
    else
        alpha = dxt_alpha_lerp(
            block.alpha1, block.alpha2, (int)alpha_index - 1, 5);
    return color | ((uint32_t)alpha << 24);
}

static uint32_t dxt_sample(uint32_t address, uint32_t stride, uint32_t format,
                           uint32_t u, uint32_t v) {
    uint32_t blocks_per_row = (stride + 3u) >> 2;
    if (!blocks_per_row) blocks_per_row = 1;
    uint32_t block_index = (v >> 2) * blocks_per_row + (u >> 2);
    uint32_t block_size = format == 8 ? 8u : 16u;
    uint8_t bytes[16] = {0};
    for (uint32_t i = 0; i < block_size; i++)
        bytes[i] = MEM_R8(address + block_index * block_size + i);
    return dxt_decode(bytes, format, u, v);
}

static void sample_tex_level(int level, int tu, int tv,
                             int *r, int *g, int *b, int *a) {
    g_tex_samples++;
    uint32_t address, stride;
    int width, height;
    texture_level_info(level, &address, &stride, &width, &height);
    if (width > 0) {
        tu = tex_coord(tu, width, ge.tex_wrap & 1);
        tv = tex_coord(tv, height, (ge.tex_wrap >> 8) & 1);
    }
    uint32_t u = (uint32_t)tu, v = (uint32_t)tv;
    switch (ge.tex_fmt) {
        case 0: case 1: case 2:
            unpack_color(MEM_R16(address + texel_offset(
                stride, u, v, 16, ge.tex_swizzle)), ge.tex_fmt, r, g, b, a); break;
        case 3:
            unpack_color(MEM_R32(address + texel_offset(
                stride, u, v, 32, ge.tex_swizzle)), 3, r, g, b, a); break;
        case 4: {
            uint32_t off = texel_offset(stride, u, v, 4, ge.tex_swizzle);
            uint8_t byte = MEM_R8(address + off);
            int ni = (u & 1) ? (byte >> 4) : (byte & 0xF);
            clut_lookup_level((uint32_t)ni, level, ge.tex_fmt, r, g, b, a); break;
        }
        case 5:
            clut_lookup_level(MEM_R8(address + texel_offset(
                stride, u, v, 8, ge.tex_swizzle)), level, ge.tex_fmt,
                r, g, b, a); break;
        case 6:
            clut_lookup_level(MEM_R16(address + texel_offset(
                stride, u, v, 16, ge.tex_swizzle)), level, ge.tex_fmt,
                r, g, b, a); break;
        case 7:
            clut_lookup_level(MEM_R32(address + texel_offset(
                stride, u, v, 32, ge.tex_swizzle)), level, ge.tex_fmt,
                r, g, b, a); break;
        case 8: case 9: case 10: {
            uint32_t color = dxt_sample(address, stride, ge.tex_fmt, u, v);
            *r = color & 0xff;
            *g = (color >> 8) & 0xff;
            *b = (color >> 16) & 0xff;
            *a = color >> 24;
            break;
        }
        default: *r=*g=*b=*a=255; break;
    }
    if (*r | *g | *b) g_tex_nonzero++;
    if (s_stat_on > 0 && *a == 0) s_stat.texa0++;
}

static void sample_tex(int tu, int tv, int *r, int *g, int *b, int *a) {
    sample_tex_level(0, tu, tv, r, g, b, a);
}

#define TEXEL_CACHE_SIZE 1024
typedef struct GeTexelCacheEntry {
    int u;
    int v;
    uint32_t rgba;
    uint32_t generation;
} GeTexelCacheEntry;

static GeTexelCacheEntry s_texel_cache[TEXEL_CACHE_SIZE];
static uint32_t s_texel_cache_generation = 1;

static void texel_cache_begin(void) {
    s_texel_cache_generation++;
    if (!s_texel_cache_generation) {
        memset(s_texel_cache, 0, sizeof(s_texel_cache));
        s_texel_cache_generation = 1;
    }
}

static void sample_tex_cached(
    int tu, int tv, int *r, int *g, int *b, int *a) {
    uint32_t slot =
        ((uint32_t)tu * 73856093u ^ (uint32_t)tv * 19349663u) &
        (TEXEL_CACHE_SIZE - 1u);
    GeTexelCacheEntry *entry = &s_texel_cache[slot];
    if (entry->generation == s_texel_cache_generation &&
        entry->u == tu && entry->v == tv) {
        uint32_t color = entry->rgba;
        *r = color & 0xff;
        *g = (color >> 8) & 0xff;
        *b = (color >> 16) & 0xff;
        *a = color >> 24;
        g_tex_samples++;
        if (*r | *g | *b) g_tex_nonzero++;
        if (s_stat_on > 0 && *a == 0) s_stat.texa0++;
        return;
    }
    sample_tex(tu, tv, r, g, b, a);
    entry->u = tu;
    entry->v = tv;
    entry->rgba =
        (uint32_t)*r | ((uint32_t)*g << 8) |
        ((uint32_t)*b << 16) | ((uint32_t)*a << 24);
    entry->generation = s_texel_cache_generation;
}

static void sample_tex_f(float fu, float fv_coord, int *r, int *g, int *b, int *a) {
    static int nobil = -1; if (nobil < 0) nobil = getenv("SR_NOBILINEAR") ? 1 : 0;
    int use_linear = !nobil && ((ge.tex_filter & 1) || ((ge.tex_filter >> 8) & 1));
    if (!use_linear) { sample_tex((int)(fu + 0.5f), (int)(fv_coord + 0.5f), r, g, b, a); return; }
    fu -= 0.5f; fv_coord -= 0.5f;
    int u0 = (int)floorf(fu), v0 = (int)floorf(fv_coord);
    int fx = (int)((fu - u0) * 256.0f), fy = (int)((fv_coord - v0) * 256.0f);
    if (fx < 0) fx = 0;
    if (fx > 256) fx = 256;
    if (fy < 0) fy = 0;
    if (fy > 256) fy = 256;
    if (fx == 0 && fy == 0) {
        sample_tex_cached(u0, v0, r, g, b, a);
        return;
    }
    if (fx == 0) {
        int r0,g0,b0,a0, r1,g1,b1,a1;
        sample_tex_cached(u0, v0,   &r0,&g0,&b0,&a0);
        sample_tex_cached(u0, v0+1, &r1,&g1,&b1,&a1);
#define LERP_VERTICAL(c0,c1) ((c0) + (((c1)-(c0))*fy>>8))
        *r=LERP_VERTICAL(r0,r1); *g=LERP_VERTICAL(g0,g1);
        *b=LERP_VERTICAL(b0,b1); *a=LERP_VERTICAL(a0,a1);
#undef LERP_VERTICAL
        return;
    }
    if (fy == 0) {
        int r0,g0,b0,a0, r1,g1,b1,a1;
        sample_tex_cached(u0,   v0, &r0,&g0,&b0,&a0);
        sample_tex_cached(u0+1, v0, &r1,&g1,&b1,&a1);
#define LERP_HORIZONTAL(c0,c1) ((c0) + (((c1)-(c0))*fx>>8))
        *r=LERP_HORIZONTAL(r0,r1); *g=LERP_HORIZONTAL(g0,g1);
        *b=LERP_HORIZONTAL(b0,b1); *a=LERP_HORIZONTAL(a0,a1);
#undef LERP_HORIZONTAL
        return;
    }
    int r0,g0,b0,a0, r1,g1,b1,a1, r2,g2,b2,a2, r3,g3,b3,a3;
    sample_tex_cached(u0,   v0,   &r0,&g0,&b0,&a0);
    sample_tex_cached(u0+1, v0,   &r1,&g1,&b1,&a1);
    sample_tex_cached(u0,   v0+1, &r2,&g2,&b2,&a2);
    sample_tex_cached(u0+1, v0+1, &r3,&g3,&b3,&a3);
#define LERP1(c0,c1,f) ((c0) + (((c1)-(c0))*(f)>>8))
    int tr=LERP1(r0,r1,fx), tg=LERP1(g0,g1,fx), tb=LERP1(b0,b1,fx), ta=LERP1(a0,a1,fx);
    int br=LERP1(r2,r3,fx), bg=LERP1(g2,g3,fx), bb=LERP1(b2,b3,fx), ba=LERP1(a2,a3,fx);
    *r=LERP1(tr,br,fy); *g=LERP1(tg,bg,fy); *b=LERP1(tb,bb,fy); *a=LERP1(ta,ba,fy);
#undef LERP1
}

/* SR_TEXDUMP=1: write each distinct texture used by a transform-mode draw once per run as
 * tex_ADDR_fF_WxH.ppm (and alpha as a grayscale _a.pgm), decoded through the EXACT
 * sampler path the rasterizer uses (swizzle + CLUT included). Separates "texture decodes
 * wrong" from "UV/lighting wrong" without guessing. */
static void texdump_maybe(void) {
    static int on = -1;
    if (on < 0) on = getenv("SR_TEXDUMP") ? 1 : 0;
    if (!on || !ge.tex_enable || !ge.tex_addr) return;
    static uint32_t seen[32]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == ge.tex_addr) return;
    if (n >= 32) return;
    seen[n++] = ge.tex_addr;
    int w = (int)ge.tex_w, h = (int)ge.tex_h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    char path[160];
    snprintf(path, sizeof(path), "tex_%08x_f%u_%dx%d.ppm", ge.tex_addr, ge.tex_fmt, w, h);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    char patha[160];
    snprintf(patha, sizeof(patha), "tex_%08x_f%u_%dx%d_a.pgm", ge.tex_addr, ge.tex_fmt, w, h);
    FILE *fa = fopen(patha, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    if (fa) fprintf(fa, "P5\n%d %d\n255\n", w, h);
    for (int v = 0; v < h; v++)
        for (int u = 0; u < w; u++) {
            int r, g, b, a;
            sample_tex(u, v, &r, &g, &b, &a);
            unsigned char px[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
            fwrite(px, 1, 3, fp);
            if (fa) { unsigned char pa = (unsigned char)a; fwrite(&pa, 1, 1, fa); }
        }
    fclose(fp);
    if (fa) fclose(fa);
    fprintf(stderr, "TEXDUMP f=%u wrote %s clut=0x%08x/0x%08x sw=%u bufw=%u filter=0x%03x\n",
            s_ge_frame, path, ge.clut_addr, ge.clut_fmt, ge.tex_swizzle, ge.tex_bufw, ge.tex_filter);
}

static uint16_t prepared_r16(const uint8_t *data, uint32_t offset) {
    uint16_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static uint32_t prepared_r32(const uint8_t *data, uint32_t offset) {
    uint32_t value;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

static int prepare_texture_level(
    int level, uint32_t address, const uint8_t **data) {
    uint64_t size = ge_texture_level_byte_size(&ge, level);
    if (!size || size > UINT32_MAX) return 0;
    uint64_t last = (uint64_t)address + size - 1u;
    if (last > UINT32_MAX ||
        !sr_inrange(address) || !sr_inrange((uint32_t)last)) {
        return 0;
    }
    sr_prepare_read(address, (uint32_t)size);
    *data = (const uint8_t *)SR_HOST(address);
    return 1;
}

static void decode_prepared_texel(
    const uint8_t *data, uint32_t stride, int level,
    uint32_t u, uint32_t v, int *r, int *g, int *b, int *a) {
    g_tex_samples++;
    switch (ge.tex_fmt) {
        case 0: case 1: case 2:
            unpack_color(
                prepared_r16(
                    data, texel_offset(
                        stride, u, v, 16, ge.tex_swizzle)),
                ge.tex_fmt, r, g, b, a);
            break;
        case 3:
            unpack_color(
                prepared_r32(
                    data, texel_offset(
                        stride, u, v, 32, ge.tex_swizzle)),
                3, r, g, b, a);
            break;
        case 4: {
            uint32_t offset =
                texel_offset(stride, u, v, 4, ge.tex_swizzle);
            uint8_t packed = data[offset];
            uint32_t index =
                (u & 1u) ? packed >> 4 : packed & 0x0fu;
            clut_lookup_level(
                index, level, ge.tex_fmt, r, g, b, a);
            break;
        }
        case 5:
            clut_lookup_level(
                data[texel_offset(
                    stride, u, v, 8, ge.tex_swizzle)],
                level, ge.tex_fmt, r, g, b, a);
            break;
        case 6:
            clut_lookup_level(
                prepared_r16(
                    data, texel_offset(
                        stride, u, v, 16, ge.tex_swizzle)),
                level, ge.tex_fmt, r, g, b, a);
            break;
        case 7:
            clut_lookup_level(
                prepared_r32(
                    data, texel_offset(
                        stride, u, v, 32, ge.tex_swizzle)),
                level, ge.tex_fmt, r, g, b, a);
            break;
        case 8: case 9: case 10: {
            uint32_t blocks_per_row = (stride + 3u) >> 2;
            if (!blocks_per_row) blocks_per_row = 1;
            uint32_t block_size = ge.tex_fmt == 8 ? 8u : 16u;
            uint32_t block_index =
                (v >> 2) * blocks_per_row + (u >> 2);
            uint32_t color = dxt_decode(
                data + block_index * block_size,
                ge.tex_fmt, u, v);
            *r = color & 0xff;
            *g = (color >> 8) & 0xff;
            *b = (color >> 16) & 0xff;
            *a = color >> 24;
            break;
        }
        default:
            *r = *g = *b = *a = 255;
            break;
    }
    if (*r | *g | *b) g_tex_nonzero++;
    if (s_stat_on > 0 && *a == 0) s_stat.texa0++;
}

/* GPU backend texture decode: synchronize each level once, then decode its bounded
 * host span without invoking the watched VRAM callback for every texel. */
void ge_decode_tex_rgba_level(uint32_t *out, int level) {
    if (!out || level < 0 || level >= 8) return;
    uint32_t address, stride;
    int width, height;
    texture_level_info(level, &address, &stride, &width, &height);
    if (!address || width <= 0 || height <= 0) return;
    const uint8_t *data;
    if (!prepare_texture_level(level, address, &data)) return;
    if (!ge.tex_swizzle && ge.tex_fmt <= 3) {
        for (int v = 0; v < height; v++) {
            uint32_t *dst = out + (uint32_t)v * (uint32_t)width;
            if (ge.tex_fmt == 3) {
                const uint32_t *src =
                    (const uint32_t *)(
                        data + (uint32_t)v * stride * 4);
                memcpy(dst, src, (size_t)width * 4);
            } else {
                const uint16_t *src =
                    (const uint16_t *)(
                        data + (uint32_t)v * stride * 2);
                for (int u = 0; u < width; u++) {
                    int r, g, b, a;
                    unpack_color(src[u], (int)ge.tex_fmt, &r, &g, &b, &a);
                    dst[u] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
                }
            }
        }
        return;
    }
    for (int v = 0; v < height; v++)
        for (int u = 0; u < width; u++) {
            int r, g, b, a;
            decode_prepared_texel(
                data, stride, level,
                (uint32_t)u, (uint32_t)v, &r, &g, &b, &a);
            out[v * width + u] =
                (uint32_t)(r & 0xFF) | ((uint32_t)(g & 0xFF) << 8) |
                ((uint32_t)(b & 0xFF) << 16) | ((uint32_t)(a & 0xFF) << 24);
        }
}

void ge_decode_tex_rgba(uint32_t *out) {
    ge_decode_tex_rgba_level(out, 0);
}

static uint16_t pack565(int r, int g, int b)           { return (uint16_t)(((r>>3)&0x1F)|(((g>>2)&0x3F)<<5)|(((b>>3)&0x1F)<<11)); }
static uint16_t pack5551(int r, int g, int b, int a)   { return (uint16_t)(((r>>3)&0x1F)|(((g>>3)&0x1F)<<5)|(((b>>3)&0x1F)<<10)|(a>=128?0x8000:0)); }
static uint16_t pack4444(int r, int g, int b, int a)   { return (uint16_t)(((r>>4)&0xF)|(((g>>4)&0xF)<<4)|(((b>>4)&0xF)<<8)|(((a>>4)&0xF)<<12)); }
static uint32_t pack8888(int r, int g, int b, int a)   { return (uint32_t)(r&0xFF)|((uint32_t)(g&0xFF)<<8)|((uint32_t)(b&0xFF)<<16)|((uint32_t)(a&0xFF)<<24); }
static uint32_t pack_fb(int r, int g, int b, int a) {
    switch (ge.fbfmt & 3) {
        case 1: return pack5551(r,g,b,a);
        case 2: return pack4444(r,g,b,a);
        case 3: return pack8888(r,g,b,a);
        default: return pack565(r,g,b);
    }
}

static uint32_t ge_rel_addr(uint32_t data) {
    uint32_t base_ext = ((ge.base & 0x000F0000u) << 8) | (data & 0x00FFFFFFu);
    return (ge.offset + base_ext) & 0x0FFFFFFFu;
}

static uint32_t ge_fb_addr(void) { return 0x04000000u | (ge.fbp & 0x001FFFFFu); }

unsigned long g_ge_pixels = 0;
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static float clampf01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static void read_fb_px(int x, int y, int *r, int *g, int *b, int *a) {
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t off = (uint32_t)(y*(int)stride + x);
    if ((ge.fbfmt & 3) == 3) unpack_color(MEM_R32(ge_fb_addr() + off*4), 3, r,g,b,a);
    else                     unpack_color(MEM_R16(ge_fb_addr() + off*2), ge.fbfmt&3, r,g,b,a);
}

int ge_color_test_pass(
    uint32_t test, uint32_t reference, uint32_t mask,
    int red, int green, int blue) {
    uint32_t rgb =
        ((uint32_t)red & 0xffu) |
        (((uint32_t)green & 0xffu) << 8) |
        (((uint32_t)blue & 0xffu) << 16);
    mask &= 0xffffffu;
    rgb &= mask;
    reference &= mask;
    switch (test & 3u) {
        case 0: return 0;
        case 1: return 1;
        case 2: return rgb == reference;
        default: return rgb != reference;
    }
}

int ge_stencil_test_pass(uint32_t test, uint8_t stencil) {
    uint8_t mask = (uint8_t)(test >> 16);
    uint8_t reference = (uint8_t)(test >> 8) & mask;
    uint8_t value = stencil & mask;
    switch (test & 7u) {
        case 0: return 0;
        case 1: return 1;
        case 2: return reference == value;
        case 3: return reference != value;
        case 4: return reference < value;
        case 5: return reference <= value;
        case 6: return reference > value;
        default: return reference >= value;
    }
}

uint8_t ge_stencil_apply(
    uint32_t operation, uint32_t format, uint8_t stencil,
    uint8_t reference) {
    switch (operation & 7u) {
        case 0:
            return stencil;
        case 1:
            return 0;
        case 2:
            return reference;
        case 3:
            return (uint8_t)~stencil;
        case 4:
            if ((format & 3u) == 0u) return 0;
            if ((format & 3u) == 1u) return 0xff;
            if ((format & 3u) == 2u)
                return stencil < 0xf0u
                    ? (uint8_t)(stencil + 0x10u) : stencil;
            return stencil < 0xffu ? (uint8_t)(stencil + 1u) : stencil;
        case 5:
            if ((format & 3u) == 0u) return 0;
            if ((format & 3u) == 1u) return 0;
            if ((format & 3u) == 2u)
                return stencil >= 0x10u
                    ? (uint8_t)(stencil - 0x10u) : stencil;
            return stencil ? (uint8_t)(stencil - 1u) : stencil;
        default:
            return stencil;
    }
}

static int pixel_in_bounds(int x, int y) {
    return x >= 0 && x < 480 && y >= 0 && y < 272 &&
           x >= ge.scis_x1 && x <= ge.scis_x2 &&
           y >= ge.scis_y1 && y <= ge.scis_y2;
}

static uint8_t read_stencil_px(int x, int y) {
    int r, g, b, a;
    read_fb_px(x, y, &r, &g, &b, &a);
    switch (ge.fbfmt & 3u) {
        case 0: return 0;
        case 1: return a >= 128 ? 0xff : 0;
        default: return (uint8_t)a;
    }
}

static void write_stencil_px(int x, int y, uint8_t stencil) {
    if (!pixel_in_bounds(x, y) || (ge.fbfmt & 3u) == 0u) return;
    int r, g, b, old_stencil;
    read_fb_px(x, y, &r, &g, &b, &old_stencil);
    int mask = ge.mask_alpha & 0xff;
    int output =
        ((int)stencil & ~mask) | (old_stencil & mask);
    uint32_t color = pack_fb(r, g, b, output);
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t address =
        ge_fb_addr() + (uint32_t)(y * (int)stride + x) *
        ((ge.fbfmt & 3u) == 3u ? 4u : 2u);
    if ((ge.fbfmt & 3u) == 3u) MEM_W32(address, color);
    else MEM_W16(address, (uint16_t)color);
}

static int color_component(uint32_t color, int channel) {
    return (int)((color >> (channel * 8)) & 0xffu);
}

static int blend_factor(
    uint32_t factor, int channel, uint32_t source, uint32_t destination,
    uint32_t fixed, int source_side) {
    int source_alpha = color_component(source, 3);
    int destination_alpha = color_component(destination, 3);
    int other = color_component(source_side ? destination : source, channel);
    switch (factor & 0xfu) {
        case 0: return other;
        case 1: return 255 - other;
        case 2: return source_alpha;
        case 3: return 255 - source_alpha;
        case 4: return destination_alpha;
        case 5: return 255 - destination_alpha;
        case 6: return source_alpha * 2;
        case 7: return 255 - source_alpha * 2;
        case 8: return destination_alpha * 2;
        case 9: return 255 - destination_alpha * 2;
        default: return color_component(fixed, channel);
    }
}

static int blend_term(int color, int factor) {
    int magnitude =
        ((color * 2 + 1) * (abs(factor) * 2 + 1)) >> 10;
    return factor < 0 ? -magnitude : magnitude;
}

GeRgb ge_blend_rgb(
    uint32_t mode, uint32_t fixed_a, uint32_t fixed_b,
    uint32_t source, uint32_t destination) {
    GeRgb result = {0};
    int *components[3] = {&result.r, &result.g, &result.b};
    uint32_t equation = (mode >> 8) & 7u;
    for (int channel = 0; channel < 3; channel++) {
        int source_color = color_component(source, channel);
        int destination_color = color_component(destination, channel);
        int value;
        if (equation == 3u) {
            value = source_color < destination_color
                ? source_color : destination_color;
        } else if (equation == 4u) {
            value = source_color > destination_color
                ? source_color : destination_color;
        } else if (equation == 5u) {
            value = abs(source_color - destination_color);
        } else if (equation <= 2u) {
            int source_factor = blend_factor(
                mode, channel, source, destination, fixed_a, 1);
            int destination_factor = blend_factor(
                mode >> 4, channel, source, destination, fixed_b, 0);
            int source_term = blend_term(source_color, source_factor);
            int destination_term =
                blend_term(destination_color, destination_factor);
            value = equation == 1u ? source_term - destination_term :
                    equation == 2u ? destination_term - source_term :
                    source_term + destination_term;
        } else {
            value = source_color;
        }
        *components[channel] = value;
    }
    return result;
}

uint32_t ge_logic_apply(
    uint32_t operation, uint32_t source, uint32_t destination) {
    uint32_t alpha = source & 0xff000000u;
    uint32_t source_rgb = source & 0x00ffffffu;
    uint32_t destination_rgb = destination & 0x00ffffffu;
    uint32_t result;
    switch (operation & 0xfu) {
        case 0: result = 0; break;
        case 1: result = source_rgb & destination_rgb; break;
        case 2: result = source_rgb & ~destination_rgb; break;
        case 3: result = source_rgb; break;
        case 4: result = ~source_rgb & destination_rgb; break;
        case 5: result = destination_rgb; break;
        case 6: result = source_rgb ^ destination_rgb; break;
        case 7: result = source_rgb | destination_rgb; break;
        case 8: result = ~(source_rgb | destination_rgb); break;
        case 9: result = ~(source_rgb ^ destination_rgb); break;
        case 10: result = ~destination_rgb; break;
        case 11: result = source_rgb | ~destination_rgb; break;
        case 12: result = ~source_rgb; break;
        case 13: result = ~source_rgb | destination_rgb; break;
        case 14: result = ~(source_rgb & destination_rgb); break;
        default: result = 0x00ffffffu; break;
    }
    return alpha | (result & 0x00ffffffu);
}

static void put_px_rgba_raw(
    int x, int y, int r, int g, int b, int source_alpha,
    int stencil) {
    int a = source_alpha;
    int dr, dg, db, da;
    if (!pixel_in_bounds(x, y)) return;
    read_fb_px(x, y, &dr, &dg, &db, &da);
    if ((ge.fbfmt & 3u) == 0u) da = 0;
    if (s_stat_on > 0 && s_in3d && !ge.clear && s_pxpass < 6) {
        s_pxpass++;
        fprintf(stderr, "PXPASS f=%u xy=(%d,%d) rgba=(%d,%d,%d,%d) fbp=0x%08x blend=%d/%06x\n",
                s_ge_frame, x, y, r, g, b, a, ge.fbp, ge.blend_enable, ge.blend_mode);
    }
    /* Centre-pixel composition trace: every write to (240,136), in order, shows what each
     * frame layer contributes and what finally covers the scene. */
    if (s_stat_on > 0 && x == 240 && y == 136 && s_pxc < 32) {
        s_pxc++;
        fprintf(stderr, "PXC f=%u rgba=(%d,%d,%d,%d) clear=%d in3d=%d tex=%d blend=%d/%06x fixab=%06x/%06x vtype=0x%06x fbp=0x%08x\n",
                s_ge_frame, r, g, b, a, ge.clear, s_in3d, ge.tex_enable,
                ge.blend_enable, ge.blend_mode, ge.blend_fixa, ge.blend_fixb, ge.vtype, ge.fbp);
    }
    if (ge.clear) {
        int write_color = (ge.clear_mode & 0x100) != 0;
        int write_alpha = (ge.clear_mode & 0x200) != 0;
        if (!write_color && !write_alpha) return;
        if (ge.dither_enable) {
            int d = ge.dith[y & 3][x & 3];
            r += d;
            g += d;
            b += d;
        }
        if (!write_color || !write_alpha) {
            if (!write_color) { r=dr; g=dg; b=db; }
            if (!write_alpha) a=da;
        }
    } else {
        if (ge.blend_enable) {
            uint32_t source =
                (uint32_t)clamp8(r) |
                ((uint32_t)clamp8(g) << 8) |
                ((uint32_t)clamp8(b) << 16) |
                ((uint32_t)clamp8(a) << 24);
            uint32_t destination =
                (uint32_t)dr | ((uint32_t)dg << 8) |
                ((uint32_t)db << 16) | ((uint32_t)da << 24);
            GeRgb blended = ge_blend_rgb(
                ge.blend_mode, ge.blend_fixa, ge.blend_fixb,
                source, destination);
            r = blended.r;
            g = blended.g;
            b = blended.b;
        }
        a = stencil;
        if (ge.dither_enable) {
            int d = ge.dith[y & 3][x & 3];
            r += d;
            g += d;
            b += d;
        }
        r = clamp8(r);
        g = clamp8(g);
        b = clamp8(b);
        if (ge.logic_op_enable) {
            uint32_t source =
                (uint32_t)r | ((uint32_t)g << 8) |
                ((uint32_t)b << 16) | ((uint32_t)a << 24);
            uint32_t destination =
                (uint32_t)dr | ((uint32_t)dg << 8) |
                ((uint32_t)db << 16) | ((uint32_t)da << 24);
            uint32_t logical =
                ge_logic_apply(ge.logic_op, source, destination);
            r = (int)(logical & 0xffu);
            g = (int)((logical >> 8) & 0xffu);
            b = (int)((logical >> 16) & 0xffu);
        }
        if (ge.mask_rgb || ge.mask_alpha) {
            int mr=(int)(ge.mask_rgb&0xFF), mg=(int)((ge.mask_rgb>>8)&0xFF), mb=(int)((ge.mask_rgb>>16)&0xFF);
            r=(r&~mr)|(dr&mr); g=(g&~mg)|(dg&mg); b=(b&~mb)|(db&mb);
            a=(a&~ge.mask_alpha)|(da&ge.mask_alpha);
        }
    }
    r = clamp8(r);
    g = clamp8(g);
    b = clamp8(b);
    if (ge.clear && (ge.mask_rgb || ge.mask_alpha)) {
        int mr=(int)(ge.mask_rgb&0xFF), mg=(int)((ge.mask_rgb>>8)&0xFF), mb=(int)((ge.mask_rgb>>16)&0xFF);
        r=(r&~mr)|(dr&mr); g=(g&~mg)|(dg&mg); b=(b&~mb)|(db&mb);
        a=(a&~ge.mask_alpha)|(da&ge.mask_alpha);
    }
    uint32_t c = pack_fb(r,g,b,a);
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    if ((ge.fbfmt&3)==3) MEM_W32(ge_fb_addr()+(uint32_t)(y*(int)stride+x)*4, c);
    else                 MEM_W16(ge_fb_addr()+(uint32_t)(y*(int)stride+x)*2, (uint16_t)c);
    g_ge_pixels++;
    if (s_in3d) s_stat.px3d++; else s_stat.px2d++;
    if (s_stat_on > 0) {
        if (s_in3d && r == 0 && g == 0 && b == 0) s_stat.blk3d++;
        uint32_t fba = ge_fb_addr();
        for (int i = 0; i < 4; i++) {
            if (s_stat.fbn[i] == 0 || s_stat.fbs[i] == fba) { s_stat.fbs[i] = fba; s_stat.fbn[i]++; break; }
        }
    }
    extern unsigned long g_list_writes, g_list_nonblack, g_list_clearpx;
    g_list_writes++;
    if (r | g | b) g_list_nonblack++;
    if (ge.clear) g_list_clearpx++;
}

static void put_px_rgba(int x, int y, int r, int g, int b, int a) {
    if (!pixel_in_bounds(x, y)) return;
    if (ge.clear) {
        put_px_rgba_raw(x, y, r, g, b, a, a);
        return;
    }
    uint8_t stencil = read_stencil_px(x, y);
    if (ge.stencil_test_enable) {
        uint8_t reference = (uint8_t)(ge.stencil_test >> 8);
        if (!ge_stencil_test_pass(ge.stencil_test, stencil)) {
            stencil = ge_stencil_apply(
                ge.stencil_op, ge.fbfmt, stencil, reference);
            write_stencil_px(x, y, stencil);
            return;
        }
        stencil = ge_stencil_apply(
            ge.stencil_op >> 16, ge.fbfmt, stencil, reference);
    }
    put_px_rgba_raw(x, y, r, g, b, a, stencil);
}

/* put_px_rgba with stencil and depth tests. */
static void put_px_rgba_z(int x, int y, uint16_t pz, int r, int g, int b, int a) {
    if (!pixel_in_bounds(x, y)) return;
    uint8_t stencil = read_stencil_px(x, y);
    uint8_t reference = (uint8_t)(ge.stencil_test >> 8);
    if (ge.stencil_test_enable &&
        !ge_stencil_test_pass(ge.stencil_test, stencil)) {
        stencil = ge_stencil_apply(
            ge.stencil_op, ge.fbfmt, stencil, reference);
        write_stencil_px(x, y, stencil);
        return;
    }
    if (!ztest_px(x, y, pz)) {
        if (ge.stencil_test_enable) {
            stencil = ge_stencil_apply(
                ge.stencil_op >> 8, ge.fbfmt, stencil, reference);
            write_stencil_px(x, y, stencil);
        }
        return;
    }
    zwrite_px(x, y, pz);
    if (ge.stencil_test_enable) {
        stencil = ge_stencil_apply(
            ge.stencil_op >> 16, ge.fbfmt, stencil, reference);
    }
    put_px_rgba_raw(x, y, r, g, b, a, stencil);
}

/* A rasterized vertex (GeVtx, ge_shared.h): screen position, reciprocal clip-w (for
 * persp-correct interp), UV, colour, per-vertex fog factor (1 = no fog). In transform
 * mode u/v are stored pre-multiplied by rw (texel coords over w). */
typedef GeVtx Vtx;

#define GE_BACKEND_TRI_BATCH_LIMIT (1024u * 1024u)

static Vtx *s_backend_tri_batch;
static uint32_t s_backend_tri_batch_count;
static uint32_t s_backend_tri_batch_capacity;
static int s_backend_tri_batch_active;
static int s_backend_tri_batch_perspective;

static int reserve_backend_tri_batch(uint32_t additional);
static void end_backend_tri_batch(void);

static void load_vtx(uint32_t addr, const VFmt *vf, Vtx *o) {
    float pz; read_pos(addr, vf, &o->x, &o->y, &pz);
    o->z = pz; o->rw = 1.0f; o->fog = 1.0f;
    read_tc(addr, vf, &o->u, &o->v);
    read_color(addr, vf, &o->r, &o->g, &o->b, &o->a);
}

static uint32_t index_value(uint32_t ibase, int idxfmt, int i) {
    switch (idxfmt) {
        case 1: return MEM_R8(ibase+(uint32_t)i);
        case 2: return MEM_R16(ibase+(uint32_t)i*2);
        case 3: return MEM_R32(ibase+(uint32_t)i*4);
        default: return (uint32_t)i;
    }
}

static uint32_t prim_vtx_addr(uint32_t vbase, uint32_t ibase, int idxfmt, int i, int stride) {
    uint32_t vi = idxfmt ? index_value(ibase, idxfmt, i) : (uint32_t)i;
    return vbase + vi * (uint32_t)stride;
}

static Vtx *s_through_vertex_cache;
static size_t s_through_vertex_capacity;

static Vtx *cache_through_vertices(
    uint32_t vbase, uint32_t ibase, int idxfmt,
    const VFmt *vf, int count) {
    if (count <= 0) return NULL;
    if ((size_t)count > s_through_vertex_capacity) {
        size_t capacity = s_through_vertex_capacity
            ? s_through_vertex_capacity : 256u;
        while (capacity < (size_t)count)
            capacity *= 2u;
        Vtx *grown = (Vtx *)realloc(
            s_through_vertex_cache, capacity * sizeof(*grown));
        if (!grown) return NULL;
        s_through_vertex_cache = grown;
        s_through_vertex_capacity = capacity;
    }
    for (int i = 0; i < count; i++) {
        load_vtx(
            prim_vtx_addr(vbase, ibase, idxfmt, i, vf->stride),
            vf, &s_through_vertex_cache[i]);
    }
    return s_through_vertex_cache;
}

static void load_primitive_vertex(
    const Vtx *cache, uint32_t vbase, uint32_t ibase,
    int idxfmt, int index, const VFmt *vf, Vtx *out) {
    if (cache) {
        *out = cache[index];
        return;
    }
    load_vtx(
        prim_vtx_addr(vbase, ibase, idxfmt, index, vf->stride),
        vf, out);
}

/* Programmable alpha test (ALPHATEST 0xDB: func | ref<<8 | mask<<16). */
static int alpha_test(int a) {
    if (!ge.atest_enable) return 1;
    static int noatest = -1;
    if (noatest < 0) noatest = getenv("SR_NOATEST") ? 1 : 0;
    if (noatest) return 1;
    int mask = (int)((ge.atest >> 16) & 0xFF);
    int ref  = (int)((ge.atest >> 8) & 0xFF) & mask;
    int av   = a & mask;
    int pass;
    switch (ge.atest & 7) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = (av == ref); break;
        case 3: pass = (av != ref); break;
        case 4: pass = (av <  ref); break;
        case 5: pass = (av <= ref); break;
        case 6: pass = (av >  ref); break;
        case 7: pass = (av >= ref); break;
        default: pass = 1;
    }
    if (!pass) s_stat.afail++;
    return pass;
}

/* Compute final pixel colour from interpolated texcoord+colour+fog.
 * `fog` is fixed-point 0..256 (256 = no fog). Returns 0 if the alpha test rejects. */
static int shade(float u, float fv, int fog, int vr, int vg, int vb, int va, int *or_, int *og, int *ob, int *oa) {
    int r, g, b, a;
    int dbg_ta = -1;
    if (ge.tex_enable && !ge.clear && ge.tex_addr) {
        int tr, tg, tb, ta;
        int rgba = (ge.tex_func & 0x100) != 0;
        int dbl  = (ge.tex_func & 0x10000) != 0;
        sample_tex_f(u, fv, &tr, &tg, &tb, &ta);
        dbg_ta = ta;
        switch (ge.tex_func & 7) {
            case 1: /* decal */
                if (rgba) {
                    int inva=255-ta, div=dbl?128:256;
                    r=((vr+1)*inva+(tr+1)*ta)/div; g=((vg+1)*inva+(tg+1)*ta)/div; b=((vb+1)*inva+(tb+1)*ta)/div;
                } else { r=dbl?tr*2:tr; g=dbl?tg*2:tg; b=dbl?tb*2:tb; }
                a=va; break;
            case 2: { /* blend */
                int er=ge.tex_env&0xFF, eg=(ge.tex_env>>8)&0xFF, eb=(ge.tex_env>>16)&0xFF, div=dbl?128:256;
                r=((255-tr)*vr+tr*er+255)/div; g=((255-tg)*vg+tg*eg+255)/div; b=((255-tb)*vb+tb*eb+255)/div;
                a=rgba?((va+1)*ta/256):va; break;
            }
            case 3: /* replace */
                r=dbl?tr*2:tr; g=dbl?tg*2:tg; b=dbl?tb*2:tb; a=rgba?ta:va; break;
            case 4: case 5: case 6: case 7: /* add */
                r=vr+tr; g=vg+tg; b=vb+tb;
                if (dbl){r*=2;g*=2;b*=2;}
                a=rgba?((va+1)*ta/256):va; break;
            case 0: default: /* modulate */
                r=(vr+1)*(dbl?tr*2:tr)/256; g=(vg+1)*(dbl?tg*2:tg)/256; b=(vb+1)*(dbl?tb*2:tb)/256;
                a=rgba?((va+1)*ta/256):va; break;
        }
        r=clamp8(r); g=clamp8(g); b=clamp8(b); a=clamp8(a);
    } else { r=vr; g=vg; b=vb; a=va; }
    if (!ge.clear) {
        if (!alpha_test(a)) {
            if (s_stat_on > 0 && s_in3d)
                atex_note((ge.tex_enable && ge.tex_addr) ? ge.tex_addr : 0, 0);
            if (s_stat_on > 0 && s_in3d && s_ashade < 8) {
                s_ashade++;
                /* For CLUT formats also show the sampled index byte and its raw palette entry,
                 * plus texture geometry -- distinguishes "texture really is transparent here"
                 * from "wrong texel/palette decode". */
                uint32_t rawidx = 0, rawpal = 0;
                if (ge.tex_fmt == 4 || ge.tex_fmt == 5) {
                    uint32_t stride2 = ge.tex_bufw ? ge.tex_bufw : (uint32_t)ge.tex_w;
                    uint32_t tu = (uint32_t)tex_coord((int)u, ge.tex_w, ge.tex_wrap & 1);
                    uint32_t tv = (uint32_t)tex_coord((int)fv, ge.tex_h, (ge.tex_wrap >> 8) & 1);
                    rawidx = MEM_R8(ge.tex_addr + texel_offset(
                        stride2, tu, tv, ge.tex_fmt == 4 ? 4 : 8,
                        ge.tex_swizzle));
                    if (ge.tex_fmt == 4) rawidx = (tu & 1) ? (rawidx >> 4) : (rawidx & 0xF);
                    uint32_t shift2 = (ge.clut_fmt >> 2) & 0x1F, mask2 = (ge.clut_fmt >> 8) & 0xFF;
                    uint32_t cidx = ((rawidx >> shift2) & mask2);
                    if ((ge.clut_fmt & 3) == 3) memcpy(&rawpal, &ge.clutram[(cidx * 4) & 2047], 4);
                    else { uint16_t p16; memcpy(&p16, &ge.clutram[(cidx * 2) & 2047], 2); rawpal = p16; }
                }
                fprintf(stderr, "ASHADE f=%u va=%d ta=%d out_a=%d texfunc=0x%06x texfmt=%u clutfmt=0x%06x texaddr=0x%08x atest=0x%06x uv=(%.1f,%.1f) tmm=0x%03x vtype=0x%06x tsc=(%g,%g) toff=(%g,%g) tw=%d th=%d bufw=%u swz=%d idx=0x%02x pal=0x%08x lit=%d mupd=%d ama=%d ala=%d\n",
                        s_ge_frame, va, dbg_ta, a, ge.tex_func, ge.tex_fmt, ge.clut_fmt,
                        ge.tex_addr, ge.atest, u, fv, ge.tex_map_mode, ge.vtype,
                        ge.tex_scale_u, ge.tex_scale_v, ge.tex_off_u, ge.tex_off_v,
                        ge.tex_w, ge.tex_h, ge.tex_bufw, ge.tex_swizzle, rawidx, rawpal,
                        ge.lighting_enable, ge.mat_update,
                        ge.material_alpha_set ? ge.material_alpha : -1, ge.amb_alpha);
                /* Once per window: the first 16 CLUT entries and 16 raw texels, to tell a
                 * garbled/missing palette or texture upload from a wrong-texel decode. */
                if (s_ashade == 1 && (ge.tex_fmt == 4 || ge.tex_fmt == 5)) {
                    fprintf(stderr, "ACLUT f=%u addr=0x%08x pal0..15=", s_ge_frame, ge.clut_addr);
                    for (int ci = 0; ci < 16; ci++) {
                        uint32_t pv;
                        if ((ge.clut_fmt & 3) == 3) memcpy(&pv, &ge.clutram[(ci * 4) & 2047], 4);
                        else { uint16_t p16; memcpy(&p16, &ge.clutram[(ci * 2) & 2047], 2); pv = p16; }
                        fprintf(stderr, "%08x%s", pv, ci == 15 ? "" : ",");
                    }
                    fprintf(stderr, " tex0..15=");
                    for (int ti = 0; ti < 16; ti++)
                        fprintf(stderr, "%02x%s", MEM_R8(ge.tex_addr + (uint32_t)ti), ti == 15 ? "\n" : "");
                }
            }
            return 0;
        }
        if (s_stat_on > 0 && s_in3d) {
            atex_note((ge.tex_enable && ge.tex_addr) ? ge.tex_addr : 0, 1);
            if (fog == 0) s_stat.fog0++; else if (fog >= 256) s_stat.fogfull++;
        }
        if (fog < 256) {  /* mix toward fog colour; alpha unaffected */
            int fr=(int)(ge.fog_color&0xFF), fg=(int)((ge.fog_color>>8)&0xFF), fb=(int)((ge.fog_color>>16)&0xFF);
            r = fr + (((r - fr) * fog) >> 8);
            g = fg + (((g - fg) * fog) >> 8);
            b = fb + (((b - fb) * fog) >> 8);
        }
        if (ge.color_test_enable &&
            !ge_color_test_pass(
                ge.color_test, ge.color_ref, ge.color_test_mask,
                r, g, b)) {
            return 0;
        }
    }
    *or_=r; *og=g; *ob=b; *oa=a;
    return 1;
}

/* ---- Unified triangle rasterizer ----
 * Incremental edge functions: each barycentric weight is affine in screen x/y, so it is
 * evaluated once per row and stepped per pixel; all interpolants step the same way.
 * The bounding box is pre-clipped against the screen and scissor rect.
 * persp=1 (transform-mode): back-face culling, perspective-correct UV, depth test, fog.
 * persp=0 (through-mode): affine UV; depth test only if the list enabled it. */
/* SR_PIXWHO=x,y probe pixel (shared by the triangle, sprite and clear paths). */
static int s_who_x = -2, s_who_y = -2;
static uint32_t s_who_after;
static void who_init(void) {
    if (s_who_x != -2) return;
    const char *wp = getenv("SR_PIXWHO");
    const char *after = getenv("SR_PIXWHO_AFTER");
    s_who_x = s_who_y = -1;
    if (wp) sscanf(wp, "%d,%d", &s_who_x, &s_who_y);
    if (after) s_who_after = (uint32_t)strtoul(after, NULL, 10);
}

static int who_active(void) {
    return s_ge_frame >= s_who_after;
}

static void raster_tri(const Vtx *A, const Vtx *B, const Vtx *C, int persp) {
    if (s_backend_tri_batch_active) {
        if (persp == s_backend_tri_batch_perspective &&
            reserve_backend_tri_batch(3u)) {
            s_backend_tri_batch[s_backend_tri_batch_count++] = *A;
            s_backend_tri_batch[s_backend_tri_batch_count++] = *B;
            s_backend_tri_batch[s_backend_tri_batch_count++] = *C;
            return;
        }
        end_backend_tri_batch();
    }
    /* GPU capture seam: a registered backend takes the triangle (and reproduces culling,
     * depth, shading, blending on the GPU); if it declines it has already flushed, so the
     * software path below stays correctly ordered against prior GPU work. */
    if (s_backend && s_backend->tri && s_backend->tri(A, B, C, persp)) return;
    texel_cache_begin();

    float area=(B->x-A->x)*(C->y-A->y)-(C->x-A->x)*(B->y-A->y);
    if (area == 0.0f) { if (persp) s_stat.deg3d++; return; }

    /* Back-face culling, PPSSPP model (BinManager::AddTriangle): the rasterizer only draws
     * triangles whose SUBMITTED order has positive cross product (CCW in y-down screen
     * space); the cull MODE is realised by the caller submitting the vertices in straight
     * or reversed order (and flipping that per strip-triangle parity) — see draw_prim.
     * Applies in BOTH transform and through mode (TransformUnit computes cullType before
     * the through check). */
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    if (ge.cull_enable && !ge.clear && !nocull) {
        if (area < 0.0f) { s_stat.cull++; return; }
    }
    if (persp) s_stat.tri3d++; else s_stat.tri2d++;

    int minx=(int)floorf(fminf(A->x,fminf(B->x,C->x))), maxx=(int)ceilf(fmaxf(A->x,fmaxf(B->x,C->x)));
    int miny=(int)floorf(fminf(A->y,fminf(B->y,C->y))), maxy=(int)ceilf(fmaxf(A->y,fmaxf(B->y,C->y)));
    int cx1 = ge.scis_x1 > 0 ? ge.scis_x1 : 0,   cy1 = ge.scis_y1 > 0 ? ge.scis_y1 : 0;
    int cx2 = ge.scis_x2 < 479 ? ge.scis_x2 : 479, cy2 = ge.scis_y2 < 271 ? ge.scis_y2 : 271;
    if (minx < cx1) minx = cx1;
    if (maxx > cx2) maxx = cx2;
    if (miny < cy1) miny = cy1;
    if (maxy > cy2) maxy = cy2;
    if (minx > maxx || miny > maxy) return;

    /* Coverage uses EXACT integer edge functions on coordinates snapped to the PSP's native
     * 28.4 subpixel grid, with a top-left fill rule. The previous float test (w<0 reject)
     * evaluated shared edges with different rounding in each neighbouring triangle, so dense
     * meshes both dropped pixels (cracks -> models dissolve into noise) and double-drew them
     * (blended seams). Integer edges give each edge pixel to exactly one triangle. */
    /* Hardware rounding: (int)(x*16 + 0.375) per PPSSPP ClipToScreenInternal. */
    long long ax=(long long)floorf(A->x*16.0f+0.375f), ay=(long long)floorf(A->y*16.0f+0.375f);
    long long bx=(long long)floorf(B->x*16.0f+0.375f), by=(long long)floorf(B->y*16.0f+0.375f);
    long long cxx=(long long)floorf(C->x*16.0f+0.375f), cyy=(long long)floorf(C->y*16.0f+0.375f);
    long long area2 = (bx-ax)*(cyy-ay) - (cxx-ax)*(by-ay);
    if (area2 == 0) { if (persp) s_stat.deg3d++; return; }
    const Vtx *VA=A, *VB=B, *VC=C;
    if (area2 < 0) {  /* normalise winding so the interior is the positive side */
        const Vtx *t=VB; VB=VC; VC=t;
        long long tt; tt=bx; bx=cxx; cxx=tt; tt=by; by=cyy; cyy=tt;
        area2 = -area2;
    }

    float inv = 1.0f / (float)area2;

    int textured = ge.tex_enable && !ge.clear && ge.tex_addr;
    int gouraud  = ge.shade_gouraud;
    int use_z    = persp ? ge.ztest_enable : thru_ztest();
    int use_fog  = persp && ge.fog_enable && !ge.clear;
    s_in3d = persp;

    /* Attribute deltas relative to VC: attr = VCattr + w0*(VA-VC) + w1*(VB-VC). Attributes are
     * recomputed per covered pixel from the integer weights, so identical geometry always
     * produces identical z/uv (no incremental float drift between passes). */
#define AT_SETUP(n, f) float n##_ac=(VA->f)-(VC->f), n##_bc=(VB->f)-(VC->f)
#define AT_EVAL(n, f)  ((VC->f)+w0f*n##_ac+w1f*n##_bc)
    AT_SETUP(iu, u); AT_SETUP(iv, v); AT_SETUP(irw, rw); AT_SETUP(iz, z); AT_SETUP(ifog, fog);
    AT_SETUP(ir, r); AT_SETUP(ig, g); AT_SETUP(ib, b); AT_SETUP(ia, a);

    /* Edge functions at pixel centres: e0 along VA->VB (weights VC), e1 along VB->VC
     * (weights VA), e2 along VC->VA (weights VB). Bias 0 keeps E==0 pixels only on
     * top (dy==0, dx>0) and left (dy<0) edges. */
    long long dx0=bx-ax,  dy0=by-ay;
    long long dx1=cxx-bx, dy1=cyy-by;
    long long dx2=ax-cxx, dy2=ay-cyy;
    long long bias0 = (dy0 < 0 || (dy0 == 0 && dx0 > 0)) ? 0 : -1;
    long long bias1 = (dy1 < 0 || (dy1 == 0 && dx1 > 0)) ? 0 : -1;
    long long bias2 = (dy2 < 0 || (dy2 == 0 && dx2 > 0)) ? 0 : -1;
    long long px0 = (long long)minx*16 + 8, py0 = (long long)miny*16 + 8;
    long long e0r = dx0*(py0-ay)  - dy0*(px0-ax);
    long long e1r = dx1*(py0-by)  - dy1*(px0-bx);
    long long e2r = dx2*(py0-cyy) - dy2*(px0-cxx);

    for (int y=miny; y<=maxy; y++, e0r+=dx0*16, e1r+=dx1*16, e2r+=dx2*16) {
        long long e0=e0r, e1=e1r, e2=e2r;
        for (int x=minx; x<=maxx; x++, e0-=dy0*16, e1-=dy1*16, e2-=dy2*16) {
            if ((e0+bias0) < 0 || (e1+bias1) < 0 || (e2+bias2) < 0) continue;
            float w0f = (float)e1*inv, w1f = (float)e2*inv;
            float irw = VC->rw;
            float iz = VC->z;
            float ifog = VC->fog;
            if (persp) {
                irw = AT_EVAL(irw, rw);
                iz = AT_EVAL(iz, z);
                ifog = AT_EVAL(ifog, fog);
                if (!zrange_ok(iz, 1)) {
                    if (s_stat_on > 0) s_stat.zrange++;
                    continue;
                }
            } else if (use_z) {
                iz = AT_EVAL(iz, z);
            }

            /* Clear-mode triangles: depth (bit 10) writes unconditionally, no z-test — the
             * normal path below only touches the z-buffer when z-test is enabled, so a game
             * clearing depth with triangles would leave stale z behind (mass z-rejects). */
            if (ge.clear) {
                if (ge.clear_mode & 0x400) {
                    uint32_t zi = (uint32_t)(y * (int)zbuf_stride() + x);
                    if (zi < 272u*512u) s_zbuf[zi] = clamp_z(iz);
                }
                who_init();
                if (who_active() &&
                    x == s_who_x && y == s_who_y && s_pixwho_n < 400) {
                    s_pixwho_n++;
                    fprintf(stderr, "PIXWHO f=%u CLEARTRI mode=0x%03x z=%.0f persp=%d rgba=(%d,%d,%d,%d)\n",
                            s_ge_frame, ge.clear_mode, iz, persp, C->r, C->g, C->b, C->a);
                }
                if (ge.clear_mode & 0x300)
                    put_px_rgba(x, y, C->r, C->g, C->b, C->a);
                continue;
            }

            float u = 0, fv = 0;
            if (textured) {
                float iu = AT_EVAL(iu, u);
                float iv = AT_EVAL(iv, v);
                if (persp) {
                    float inv_rw = (irw != 0.0f) ? 1.0f / irw : 1.0f;
                    u = iu * inv_rw; fv = iv * inv_rw;
                } else { u = iu; fv = iv; }
            }
            int r, g, b, a;
            if (gouraud) {
                float ir = AT_EVAL(ir, r);
                float ig = AT_EVAL(ig, g);
                float ib = AT_EVAL(ib, b);
                float ia = AT_EVAL(ia, a);
                r=(int)ir; g=(int)ig; b=(int)ib; a=(int)ia;
            }
            else         { r=C->r; g=C->g; b=C->b; a=C->a; }
            int fog = use_fog ? (int)(clampf01(ifog) * 256.0f) : 256;

            int sr,sg,sb,sa;
            if (!shade(u,fv,fog,r,g,b,a,&sr,&sg,&sb,&sa)) continue;

            /* SR_PIXWHO=x,y: log every triangle that writes the chosen pixel — identifies
             * which draw owns a misrendered region without guessing from geometry. */
            {
                who_init();
                if (who_active() &&
                    x == s_who_x && y == s_who_y && s_pixwho_n < 400) {
                    s_pixwho_n++;
                    uint32_t zidx = (uint32_t)(y * (int)zbuf_stride() + x);
                    unsigned curz = zidx < 272u*512u ? s_zbuf[zidx] : 99999u;
                    /* zpass: the verdict the depth test will actually give this pixel. */
                    int zpass = 1;
                    if (use_z && zidx < 272u*512u) {
                        uint16_t zv = clamp_z(iz);
                        switch (ge.ztest & 7) {
                            case 0: zpass = 0; break;
                            case 2: zpass = (zv == curz); break;
                            case 3: zpass = (zv != curz); break;
                            case 4: zpass = (zv <  curz); break;
                            case 5: zpass = (zv <= curz); break;
                            case 6: zpass = (zv >  curz); break;
                            case 7: zpass = (zv >= curz); break;
                            default: zpass = 1;
                        }
                    }
                    fprintf(stderr, "PIXWHO f=%u d=%u persp=%d clear=%d vtype=0x%06x tex=%d/0x%08x vcol=(%d,%d,%d,%d) uv=(%.2f,%.2f) rgba=(%d,%d,%d,%d) fog=%d z=%.0f zt=%d/%d zcur=%u zpass=%d zw=%d zr=[%u,%u] blend=%d/%06x world_t=(%.4g,%.4g,%.4g) tri=A(%.1f,%.1f,%.0f)B(%.1f,%.1f,%.0f)C(%.1f,%.1f,%.0f)\n",
                            s_ge_frame, s_rt_draw, persp, ge.clear, ge.vtype, ge.tex_enable, ge.tex_addr,
                            r, g, b, a, u, fv,
                            sr, sg, sb, sa, fog, iz,
                            use_z ? (int)(ge.ztest & 7) : -1, ge.ztest_enable, curz, zpass,
                            !ge.zwrite_disable, ge.minz, ge.maxz,
                            ge.blend_enable, ge.blend_mode,
                            ge.world[9], ge.world[10], ge.world[11],
                            A->x, A->y, A->z, B->x, B->y, B->z, C->x, C->y, C->z);
                }
            }
            if (use_z) put_px_rgba_z(x,y,clamp_z(iz),sr,sg,sb,sa);
            else       put_px_rgba(x,y,sr,sg,sb,sa);
        }
    }
#undef AT_SETUP
#undef AT_EVAL
}

/* Line rasterizer (DDA), used for prim types 1/2 in both modes. */
static void draw_line_vtx(const Vtx *A, const Vtx *B, int persp) {
    if (s_backend && s_backend->line && s_backend->line(A, B, persp)) return;
    texel_cache_begin();
    float dx=B->x-A->x, dy=B->y-A->y;
    float len=fmaxf(fabsf(dx),fabsf(dy));
    int steps=(int)len;
    if (steps<1) steps=1;
    if (steps>2048) steps=2048;
    int textured = ge.tex_enable && !ge.clear && ge.tex_addr;
    int use_z    = persp ? ge.ztest_enable : thru_ztest();
    int use_fog  = persp && ge.fog_enable && !ge.clear;
    s_in3d = persp;
    for (int i=0; i<=steps; i++) {
        float t=(float)i/(float)steps;
        int x=(int)(A->x+dx*t), y=(int)(A->y+dy*t);
        float u=0, fv=0;
        if (textured) {
            u  = A->u + (B->u - A->u)*t;
            fv = A->v + (B->v - A->v)*t;
            if (persp) {
                float rw = A->rw + (B->rw - A->rw)*t;
                if (rw != 0.0f) { u /= rw; fv /= rw; }
            }
        }
        int r=(int)(A->r+(B->r-A->r)*t), g=(int)(A->g+(B->g-A->g)*t);
        int b=(int)(A->b+(B->b-A->b)*t), a=(int)(A->a+(B->a-A->a)*t);
        if (!ge.shade_gouraud) { r=B->r; g=B->g; b=B->b; a=B->a; }
        float lz = A->z+(B->z-A->z)*t;
        if (!zrange_ok(lz, persp)) { if (s_stat_on > 0) s_stat.zrange++; continue; }
        int fog = use_fog ? (int)(clampf01(A->fog+(B->fog-A->fog)*t)*256.0f) : 256;
        int sr,sg,sb,sa;
        if (!shade(u,fv,fog,r,g,b,a,&sr,&sg,&sb,&sa)) continue;
        if (use_z) put_px_rgba_z(x,y,clamp_z(lz),sr,sg,sb,sa);
        else       put_px_rgba(x,y,sr,sg,sb,sa);
    }
}

static void draw_point_vtx(const Vtx *A, int persp) {
    if (s_backend && s_backend->point && s_backend->point(A, persp)) return;
    texel_cache_begin();
    if (!zrange_ok(A->z, persp)) { if (s_stat_on > 0) s_stat.zrange++; return; }
    int textured = ge.tex_enable && !ge.clear && ge.tex_addr;
    float u=0, fv=0;
    if (textured) {
        u=A->u; fv=A->v;
        if (persp && A->rw != 0.0f) { u/=A->rw; fv/=A->rw; }
    }
    int fog = (persp && ge.fog_enable && !ge.clear) ? (int)(clampf01(A->fog)*256.0f) : 256;
    s_in3d = persp;
    int sr,sg,sb,sa;
    if (!shade(u,fv,fog,A->r,A->g,A->b,A->a,&sr,&sg,&sb,&sa)) return;
    int use_z = persp ? ge.ztest_enable : thru_ztest();
    if (use_z) put_px_rgba_z((int)A->x,(int)A->y,clamp_z(A->z),sr,sg,sb,sa);
    else       put_px_rgba((int)A->x,(int)A->y,sr,sg,sb,sa);
}

/* ---- 3D T&L pipeline ---- */

/* Multiply 4×3 column-major matrix by vec3. Returns vec3. (World/View transforms) */
static void mat43_mul(const float *m, float ix, float iy, float iz, float *ox, float *oy, float *oz) {
    *ox = m[0]*ix + m[3]*iy + m[6]*iz + m[9];
    *oy = m[1]*ix + m[4]*iy + m[7]*iz + m[10];
    *oz = m[2]*ix + m[5]*iy + m[8]*iz + m[11];
}

/* Rotate a vec3 by the upper 3×3 of a 4×3 matrix (no translation; for normals). */
static void mat33_mul(const float *m, float ix, float iy, float iz, float *ox, float *oy, float *oz) {
    *ox = m[0]*ix + m[3]*iy + m[6]*iz;
    *oy = m[1]*ix + m[4]*iy + m[7]*iz;
    *oz = m[2]*ix + m[5]*iy + m[8]*iz;
}

/* Multiply 4×4 column-major matrix by vec3, returning vec4 (clip space). (Proj transform) */
static void mat44_mul(const float *m, float ix, float iy, float iz, float *ox, float *oy, float *oz, float *ow) {
    *ox = m[0]*ix + m[4]*iy + m[8]*iz  + m[12];
    *oy = m[1]*ix + m[5]*iy + m[9]*iz  + m[13];
    *oz = m[2]*ix + m[6]*iy + m[10]*iz + m[14];
    *ow = m[3]*ix + m[7]*iy + m[11]*iz + m[15];
}

static float c8f(uint32_t col, int chan) { return (float)((col >> (chan*8)) & 0xFF) * (1.0f/255.0f); }

/* Per-vertex lighting in world space: emissive + scene ambient, plus per-light
 * ambient/diffuse/specular for up to 4 directional/point lights (spot treated as point).
 * Material colours come from the registers or the vertex colour per MATERIALUPDATE bits. */
static void light_vertex(float wx, float wy, float wz, float nx, float ny, float nz,
                         int vr, int vg, int vb, int va, int *lr, int *lg, int *lb, int *la) {
    float vrf = vr*(1.0f/255.0f), vgf = vg*(1.0f/255.0f), vbf = vb*(1.0f/255.0f);
    /* Lit alpha = material-ambient alpha * scene-ambient alpha; diffuse/specular/emissive never
     * contribute to alpha (PPSSPP Software/Lighting.cpp). With MATERIALUPDATE bit 0 the vertex
     * colour (including its alpha) substitutes for the ambient material colour. */
    int maa = (ge.mat_update & 1) ? va : (ge.material_alpha_set ? ge.material_alpha : 255);
    *la = clamp8((maa * ge.amb_alpha + 127) / 255);
    float mar, mag, mab, mdr, mdg, mdb, msr, msg, msb;
    if (ge.mat_update & 1) { mar=vrf; mag=vgf; mab=vbf; }
    else { mar=c8f(ge.material,0); mag=c8f(ge.material,1); mab=c8f(ge.material,2); }
    if (ge.mat_update & 2) { mdr=vrf; mdg=vgf; mdb=vbf; }
    else { mdr=c8f(ge.mat_diffuse,0); mdg=c8f(ge.mat_diffuse,1); mdb=c8f(ge.mat_diffuse,2); }
    if (ge.mat_update & 4) { msr=vrf; msg=vgf; msb=vbf; }
    else { msr=c8f(ge.mat_specular,0); msg=c8f(ge.mat_specular,1); msb=c8f(ge.mat_specular,2); }

    float r = c8f(ge.mat_emissive,0) + c8f(ge.amb_color,0)*mar;
    float g = c8f(ge.mat_emissive,1) + c8f(ge.amb_color,1)*mag;
    float b = c8f(ge.mat_emissive,2) + c8f(ge.amb_color,2)*mab;

    for (int i = 0; i < 4; i++) {
        if (!ge.light_on[i]) continue;
        int comp = (int)(ge.light_type[i] & 3);
        int type = (int)((ge.light_type[i] >> 8) & 3);
        float lx = ge.light_pos[i][0], ly = ge.light_pos[i][1], lz = ge.light_pos[i][2];
        float att = 1.0f;
        if (type != 0) {  /* point/spot: light vector from vertex, distance attenuation */
            lx -= wx; ly -= wy; lz -= wz;
            float d2 = lx*lx + ly*ly + lz*lz;
            float d = sqrtf(d2);
            float den = ge.light_att[i][0] + ge.light_att[i][1]*d + ge.light_att[i][2]*d2;
            att = den > 0.0f ? clampf01(1.0f/den) : 1.0f;
            if (d > 0.0f) { lx/=d; ly/=d; lz/=d; }
        } else {          /* directional: position register holds the direction */
            float d = sqrtf(lx*lx + ly*ly + lz*lz);
            if (d > 0.0f) { lx/=d; ly/=d; lz/=d; }
        }
        float ndl = nx*lx + ny*ly + nz*lz;
        if (ndl < 0.0f) ndl = 0.0f;
        if (comp == 2 && ge.spec_coef > 0.0f) ndl = powf(ndl, ge.spec_coef);   /* powered diffuse */

        r += att*(c8f(ge.light_acol[i],0)*mar + c8f(ge.light_dcol[i],0)*mdr*ndl);
        g += att*(c8f(ge.light_acol[i],1)*mag + c8f(ge.light_dcol[i],1)*mdg*ndl);
        b += att*(c8f(ge.light_acol[i],2)*mab + c8f(ge.light_dcol[i],2)*mdb*ndl);

        if ((comp & 1) && ndl > 0.0f) {  /* specular, Blinn half-vector with eye (0,0,1) */
            float hx=lx, hy=ly, hz=lz+1.0f;
            float hd = sqrtf(hx*hx + hy*hy + hz*hz);
            if (hd > 0.0f) {
                float ndh = (nx*hx + ny*hy + nz*hz) / hd;
                if (ndh > 0.0f) {
                    float s = ge.spec_coef > 0.0f ? powf(ndh, ge.spec_coef) : ndh;
                    r += att*c8f(ge.light_scol[i],0)*msr*s;
                    g += att*c8f(ge.light_scol[i],1)*msg*s;
                    b += att*c8f(ge.light_scol[i],2)*msb*s;
                }
            }
        }
    }
    *lr = clamp8((int)(r*255.0f + 0.5f));
    *lg = clamp8((int)(g*255.0f + 0.5f));
    *lb = clamp8((int)(b*255.0f + 0.5f));
}

/* Transform a model-space vertex through the T&L pipeline and write screen-space result into *o.
 * Returns 0 if the vertex projects to non-positive clip w (behind the camera). */
/* Clip-space vertex: position before the perspective divide plus raw (un-premultiplied)
 * texel UV and lit color, so triangles crossing the near plane can be clipped with plain
 * linear interpolation and only then projected. The old pipeline rejected any vertex with
 * clipw <= 0, which discarded every triangle touching the camera plane — in close-up
 * scenes (the hangar floor/aircraft) that erased whole models. */
#define NEAR_W 1e-3f

typedef struct {
    float cx, cy, cz, cw; float u, v; float fog; int r, g, b, a;
    /* PSP primitive-acceptance flags (PPSSPP Clipper::ProcessTriangle / ClipToScreenInternal):
     * oor = projected screen coords outside the GE's 4096-grid; ozp/ozn = z/w beyond +-1. */
    int oor, ozp, ozn;
    /* Projected screen-grid coords (pre-offset) kept for SR_RTRACE decision logging; only
     * meaningful when cw != 0. */
    float sx, sy, sz;
} CVtx;

static int reserve_backend_tri_batch(uint32_t additional) {
    if (additional >
        GE_BACKEND_TRI_BATCH_LIMIT - s_backend_tri_batch_count) {
        return 0;
    }
    uint32_t required = s_backend_tri_batch_count + additional;
    if (required <= s_backend_tri_batch_capacity) return 1;
    uint32_t capacity = s_backend_tri_batch_capacity
        ? s_backend_tri_batch_capacity : 3072u;
    while (capacity < required) {
        capacity = capacity > GE_BACKEND_TRI_BATCH_LIMIT / 2u
            ? GE_BACKEND_TRI_BATCH_LIMIT : capacity * 2u;
    }
    void *memory = realloc(
        s_backend_tri_batch, (size_t)capacity * sizeof(*s_backend_tri_batch));
    if (!memory) return 0;
    s_backend_tri_batch = (Vtx *)memory;
    s_backend_tri_batch_capacity = capacity;
    return 1;
}

static void end_backend_tri_batch(void) {
    uint32_t count = s_backend_tri_batch_count;
    int perspective = s_backend_tri_batch_perspective;
    s_backend_tri_batch_active = 0;
    s_backend_tri_batch_count = 0;
    if (!count) return;
    if (s_backend && s_backend->tri_list &&
        s_backend->tri_list(
            s_backend_tri_batch, count, perspective)) {
        return;
    }
    for (uint32_t i = 0; i < count; i += 3u) {
        raster_tri(
            &s_backend_tri_batch[i],
            &s_backend_tri_batch[i + 1u],
            &s_backend_tri_batch[i + 2u],
            perspective);
    }
}

static void begin_backend_tri_batch(int perspective) {
    if (s_backend_tri_batch_active) end_backend_tri_batch();
    if (s_backend && s_backend->tri_list) {
        s_backend_tri_batch_active = 1;
        s_backend_tri_batch_perspective = perspective;
        s_backend_tri_batch_count = 0;
    }
}

typedef struct {
    float x, y, z;
    float nx, ny, nz;
    float u, v;
    int r, g, b, a;
} ModelVtx;

static void decode_model_vtx(uint32_t addr, const VFmt *vf, ModelVtx *out) {
    read_pos(addr, vf, &out->x, &out->y, &out->z);
    read_nrm(addr, vf, &out->nx, &out->ny, &out->nz);
    read_tc(addr, vf, &out->u, &out->v);
    read_color(addr, vf, &out->r, &out->g, &out->b, &out->a);
    if (vf->through || !vf->w_n) return;

    float weights[8];
    read_weights(addr, vf, weights);
    float px = 0.0f, py = 0.0f, pz = 0.0f;
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    for (int i = 0; i < vf->w_n; i++) {
        if (weights[i] == 0.0f) continue;
        float tx, ty, tz;
        mat43_mul(
            ge.bone + 12 * i, out->x, out->y, out->z,
            &tx, &ty, &tz);
        px += tx * weights[i];
        py += ty * weights[i];
        pz += tz * weights[i];
        mat33_mul(
            ge.bone + 12 * i, out->nx, out->ny, out->nz,
            &tx, &ty, &tz);
        nx += tx * weights[i];
        ny += ty * weights[i];
        nz += tz * weights[i];
    }
    out->x = px;
    out->y = py;
    out->z = pz;
    out->nx = nx;
    out->ny = ny;
    out->nz = nz;
}

/* Viewport/offset registers change per draw at most; decode them only when they do. */
typedef struct { float vp[6]; float off[2]; } ViewportCache;
static const ViewportCache *viewport_cache(void) {
    static uint32_t raw[8];
    static ViewportCache cache;
    static int valid;
    if (!valid || memcmp(raw, ge.viewport_raw, sizeof(uint32_t) * 6) != 0 ||
        raw[6] != ge.offsetx || raw[7] != ge.offsety) {
        memcpy(raw, ge.viewport_raw, sizeof(uint32_t) * 6);
        raw[6] = ge.offsetx;
        raw[7] = ge.offsety;
        for (int i = 0; i < 6; i++) cache.vp[i] = decode_float24(raw[i]);
        cache.off[0] = (float)(ge.offsetx & 0xFFFF) / 16.0f;
        cache.off[1] = (float)(ge.offsety & 0xFFFF) / 16.0f;
        valid = 1;
    }
    return &cache;
}

static void transform_model_vtx_clip(const ModelVtx *in, CVtx *o) {
    float mx = in->x, my = in->y, mz = in->z;

    /* World transform (model → world) */
    float wx, wy, wz;
    mat43_mul(ge.world, mx, my, mz, &wx, &wy, &wz);

    /* View transform (world → camera) */
    float cx, cy, cz;
    mat43_mul(ge.view, wx, wy, wz, &cx, &cy, &cz);

    /* Projection (camera → clip). The divisor is the real clip-space w from the projection
     * matrix W row: perspective matrices (sceGumPerspective) have W row (0,0,-1,0), so
     * clipw == -cz; orthographic matrices have W row (0,0,0,1), so clipw == 1 and view
     * depth must NOT divide (hangar/menu 3D and overlay quads use ortho). */
    mat44_mul(ge.proj, cx, cy, cz, &o->cx, &o->cy, &o->cz, &o->cw);

    /* Fog factor from view-space depth: 1 = no fog, 0 = full fog (PPSSPP convention).
     * Linear in cz, so interpolating the factor across a clipped edge stays exact. */
    static int nofog = -1;
    if (nofog < 0) nofog = getenv("SR_NOFOG") ? 1 : 0;
    if (ge.fog_enable && !nofog) {
        /* Inf/NaN fog coefficients are VALID on the PSP: games leave fog enabled but set
         * fog-end to inf/nan to force a constant factor (PPSSPP TransformUnit ComputeState,
         * "required for Outrun to render proper skies"). Plain float math turns these into
         * NaN, which clampf01 passes through -> full fog -> solid fog-coloured geometry. */
        float fend = ge.fog_end, fslope = ge.fog_slope;
        if (isinf(fend) || isnan(fend)) {
            int sign = signbit(fend) ? 1 : 0;
            if (signbit(fslope)) sign = !sign;   /* the multiply would flip it */
            if (fslope == 0.0f) sign = 1;        /* *0 clamps to zero regardless of sign */
            o->fog = sign ? 0.0f : 1.0f;
        } else {
            if (isinf(fslope) || isnan(fslope))
                fslope = signbit(fslope) ? -262144.0f : 262144.0f;
            o->fog = clampf01((cz + fend) * fslope);
        }
    } else o->fog = 1.0f;

    /* Texcoords. Mode 0: vertex UVs with TEXSCALE/TEXOFFSET. Mode 1: texgen matrix applied
     * to a source picked by bits 8-9 (0=model position, 1=vertex uv, 2=normalized normal,
     * 3=raw normal); scale/offset are NOT applied in this mode (PPSSPP TransformUnit).
     * Mode 2: environment map from two light directions. Result is in texel units;
     * the 1/w premultiply for perspective-correct interpolation happens at projection. */
    float u, fv;
    uint32_t tmm = ge.tex_map_mode & 3;
    if (tmm == 1) {
        float sx, sy, sz2;
        switch ((ge.tex_map_mode >> 8) & 3) {
        case 0:  sx = mx; sy = my; sz2 = mz; break;
        case 1:  sx = in->u; sy = in->v; sz2 = 0.0f; break;
        default: {
            sx = in->nx;
            sy = in->ny;
            sz2 = in->nz;
            if (((ge.tex_map_mode >> 8) & 3) == 2) {
                float nl = sqrtf(sx*sx + sy*sy + sz2*sz2);
                if (nl > 0.0f) { sx /= nl; sy /= nl; sz2 /= nl; }
            }
            break;
        }
        }
        float ts, tt, tq;
        mat43_mul(ge.tgen, sx, sy, sz2, &ts, &tt, &tq);
        if (tq == 0.0f) tq = 1.0f;   /* affine texgen has q==... (z row unused) */
        u  = (ts / tq) * (float)ge.tex_w;
        fv = (tt / tq) * (float)ge.tex_h;
    } else if (tmm == 2) {
        /* env map: s/t = (1 + dot(normalize(lightpos[lsN]), worldnormal)) / 2 */
        float wnx2, wny2, wnz2;
        mat33_mul(
            ge.world, in->nx, in->ny, in->nz,
            &wnx2, &wny2, &wnz2);
        float nl2 = sqrtf(wnx2*wnx2 + wny2*wny2 + wnz2*wnz2);
        if (nl2 > 0.0f) { wnx2 /= nl2; wny2 /= nl2; wnz2 /= nl2; }
        float st2[2];
        for (int li = 0; li < 2; li++) {
            int l = (int)((ge.tex_shade_ls >> (li * 8)) & 3);
            float lx = ge.light_pos[l][0], ly = ge.light_pos[l][1], lz = ge.light_pos[l][2];
            float ld = sqrtf(lx*lx + ly*ly + lz*lz);
            if (ld > 0.0f) { lx /= ld; ly /= ld; lz /= ld; }
            st2[li] = (1.0f + (lx*wnx2 + ly*wny2 + lz*wnz2)) * 0.5f;
        }
        u  = st2[0] * (float)ge.tex_w;
        fv = st2[1] * (float)ge.tex_h;
    } else {
        u = in->u;
        fv = in->v;
        u  = (u  * ge.tex_scale_u + ge.tex_off_u) * (float)ge.tex_w;
        fv = (fv * ge.tex_scale_v + ge.tex_off_v) * (float)ge.tex_h;
    }
    o->u = u;
    o->v = fv;

    o->r = in->r;
    o->g = in->g;
    o->b = in->b;
    o->a = in->a;

    static int nolight = -1;
    if (nolight < 0) nolight = getenv("SR_NOLIGHT") ? 1 : 0;
    if (ge.lighting_enable && !nolight) {
        float wnx, wny, wnz;
        mat33_mul(
            ge.world, in->nx, in->ny, in->nz,
            &wnx, &wny, &wnz);
        float nl = sqrtf(wnx*wnx + wny*wny + wnz*wnz);
        if (nl > 0.0f) { wnx/=nl; wny/=nl; wnz/=nl; }
        light_vertex(wx, wy, wz, wnx, wny, wnz, o->r, o->g, o->b, o->a, &o->r, &o->g, &o->b, &o->a);
    }

    /* PSP acceptance flags (PPSSPP ClipToScreenInternal + CheckOutsideZ). The hardware does
     * NOT rasterise primitives whose vertices leave the valid screen grid or the z range —
     * games count on this to hide effect passes and behind-camera geometry. */
    o->oor = o->ozp = o->ozn = 0;
    {
        const float zoutside = 1.000030517578125f;
        float zw = o->cz / o->cw;
        if (zw >= zoutside) o->ozp = 1;
        else if (-zw >= zoutside) o->ozn = 1;
        const float *vp = viewport_cache()->vp;
        float sx = o->cx * vp[0] / o->cw + vp[3];
        float sy = o->cy * vp[1] / o->cw + vp[4];
        float sz = o->cz * vp[2] / o->cw + vp[5];
        o->sx = sx; o->sy = sy; o->sz = sz;
        const float SB = 4095.0f + 15.5f / 16.0f;
        if (ge.depth_clip) {
            if (o->cz > -o->cw && (sx >= SB || sy >= SB || sx < 0.0f || sy < 0.0f)) o->oor = 1;
        } else {
            if (sx > SB || sy >= SB || sx < 0.0f || sy < 0.0f || sz < 0.0f || sz >= 65536.0f) o->oor = 1;
        }
    }
}

static void transform_vtx_clip(uint32_t addr, const VFmt *vf, CVtx *o) {
    ModelVtx model;
    decode_model_vtx(addr, vf, &model);
    transform_model_vtx_clip(&model, o);
}

static CVtx *s_transform_vertex_cache;
static CVtx **s_transform_vertex_refs;
static size_t s_transform_vertex_capacity;
static CVtx *s_indexed_transform_cache;
static uint32_t *s_indexed_transform_tags;
static size_t s_indexed_transform_capacity;
static uint32_t s_indexed_transform_generation;

static int reserve_indexed_transform_cache(size_t count) {
    if (count <= s_indexed_transform_capacity) return 1;
    size_t capacity = s_indexed_transform_capacity
        ? s_indexed_transform_capacity : 256u;
    while (capacity < count)
        capacity *= 2u;
    CVtx *vertices = (CVtx *)realloc(
        s_indexed_transform_cache, capacity * sizeof(*vertices));
    if (!vertices) return 0;
    s_indexed_transform_cache = vertices;
    uint32_t *tags = (uint32_t *)realloc(
        s_indexed_transform_tags, capacity * sizeof(*tags));
    if (!tags) return 0;
    memset(
        tags + s_indexed_transform_capacity, 0,
        (capacity - s_indexed_transform_capacity) * sizeof(*tags));
    s_indexed_transform_tags = tags;
    s_indexed_transform_capacity = capacity;
    return 1;
}

static const CVtx *const *cache_transform_vertices(
    uint32_t vbase, uint32_t ibase, int idxfmt,
    const VFmt *vf, int count) {
    if (count <= 0) return NULL;
    if ((size_t)count > s_transform_vertex_capacity) {
        size_t capacity = s_transform_vertex_capacity
            ? s_transform_vertex_capacity : 256u;
        while (capacity < (size_t)count)
            capacity *= 2u;
        CVtx *grown = (CVtx *)realloc(
            s_transform_vertex_cache, capacity * sizeof(*grown));
        if (!grown) return NULL;
        s_transform_vertex_cache = grown;
        CVtx **refs = (CVtx **)realloc(
            s_transform_vertex_refs, capacity * sizeof(*refs));
        if (!refs) return NULL;
        s_transform_vertex_refs = refs;
        s_transform_vertex_capacity = capacity;
    }
    uint32_t maximum_index = 0;
    int cache_indices = idxfmt != 0;
    if (cache_indices) {
        for (int i = 0; i < count; i++) {
            uint32_t index = index_value(ibase, idxfmt, i);
            if (index > maximum_index) maximum_index = index;
        }
        cache_indices =
            maximum_index < 65536u &&
            reserve_indexed_transform_cache(
                (size_t)maximum_index + 1u);
        if (cache_indices &&
            ++s_indexed_transform_generation == 0) {
            memset(
                s_indexed_transform_tags, 0,
                s_indexed_transform_capacity *
                    sizeof(*s_indexed_transform_tags));
            s_indexed_transform_generation = 1;
        }
    }
    uint64_t transformed = 0;
    for (int i = 0; i < count; i++) {
        if (cache_indices) {
            uint32_t index = index_value(ibase, idxfmt, i);
            if (s_indexed_transform_tags[index] !=
                s_indexed_transform_generation) {
                transform_vtx_clip(
                    vbase + index * (uint32_t)vf->stride,
                    vf, &s_indexed_transform_cache[index]);
                transformed++;
                s_indexed_transform_tags[index] =
                    s_indexed_transform_generation;
            }
            s_transform_vertex_refs[i] =
                &s_indexed_transform_cache[index];
        } else {
            transform_vtx_clip(
                prim_vtx_addr(
                    vbase, ibase, idxfmt, i, vf->stride),
                vf, &s_transform_vertex_cache[i]);
            s_transform_vertex_refs[i] =
                &s_transform_vertex_cache[i];
            transformed++;
        }
    }
    sr_perf_counter_add(
        SR_PERF_GE_TRANSFORM_UNIQUE_VERTICES, transformed);
    return (const CVtx *const *)s_transform_vertex_refs;
}

/* Perspective divide + viewport: clip-space vertex (cw > 0 required) → screen vertex. */
static void project_cvtx(const CVtx *c, Vtx *o) {
    float inv_w = 1.0f / c->cw;
    float ndcx = c->cx * inv_w, ndcy = c->cy * inv_w, ndcz = c->cz * inv_w;

    const float *vp = viewport_cache()->vp, *off = viewport_cache()->off;
    float vp_xs = vp[0], vp_ys = vp[1], vp_zs = vp[2];
    float vp_xc = vp[3], vp_yc = vp[4], vp_zc = vp[5];
    float off_x = off[0], off_y = off[1];

    o->x = ndcx * vp_xs + vp_xc - off_x;
    o->y = ndcy * vp_ys + vp_yc - off_y;
    float sz = ndcz * vp_zs + vp_zc;
    o->z = sz < 0.0f ? 0.0f : sz > 65535.0f ? 65535.0f : sz;
    o->rw = inv_w;
    o->u = c->u * inv_w;
    o->v = c->v * inv_w;
    o->fog = c->fog;
    o->r = c->r; o->g = c->g; o->b = c->b; o->a = c->a;
}

/* Compatibility wrapper for points/lines/sprites: reject behind-camera vertices. */
static int transform_vtx_3d(uint32_t addr, const VFmt *vf, Vtx *o) {
    CVtx c;
    transform_vtx_clip(addr, vf, &c);
    if (c.cw <= NEAR_W) { s_stat.nearclip++; return 0; }
    /* PSP point/sprite acceptance: out-of-range or out-of-z vertices are dropped whole
     * (PPSSPP Clipper::ProcessRect / ProcessPoint; rects do not clip against any plane). */
    if (c.oor) { s_stat.nearclip++; return 0; }
    if (!ge.depth_clip && (c.ozp || c.ozn)) { s_stat.nearclip++; return 0; }
    project_cvtx(&c, o);
    return 1;
}

static int project_cached_vtx(const CVtx *c, Vtx *out) {
    if (c->cw <= NEAR_W) {
        s_stat.nearclip++;
        return 0;
    }
    if (c->oor) {
        s_stat.nearclip++;
        return 0;
    }
    if (!ge.depth_clip && (c->ozp || c->ozn)) {
        s_stat.nearclip++;
        return 0;
    }
    project_cvtx(c, out);
    return 1;
}

static void clip_lerp_cvtx(const CVtx *a, const CVtx *b, float t, CVtx *o) {
    o->cx = a->cx + (b->cx - a->cx) * t;
    o->cy = a->cy + (b->cy - a->cy) * t;
    o->cz = a->cz + (b->cz - a->cz) * t;
    o->cw = a->cw + (b->cw - a->cw) * t;
    o->u  = a->u + (b->u - a->u) * t;
    o->v  = a->v + (b->v - a->v) * t;
    o->fog = a->fog + (b->fog - a->fog) * t;
    o->r = a->r + (int)((float)(b->r - a->r) * t);
    o->g = a->g + (int)((float)(b->g - a->g) * t);
    o->b = a->b + (int)((float)(b->b - a->b) * t);
    o->a = a->a + (int)((float)(b->a - a->a) * t);
    /* oor=2 marks a vertex CREATED by the near clip: PPSSPP's clip_interpolate re-projects
     * such vertices and re-checks the screen-grid range (clip_vtx_out_of_range below). */
    o->oor = 2; o->ozp = o->ozn = 0;
    o->sx = o->sy = o->sz = 0.0f;
}

/* PPSSPP Clipper clip_interpolate + ClipToScreenInternal<.., alwaysCheckRange=true>: a vertex
 * produced by the near-z clip is re-projected, and if its screen position leaves the 4096
 * grid the vertex is poisoned (screenpos = 0x7FFFFFFF) — every sub-triangle that references
 * it is discarded (Clipper.cpp ProcessTriangle line ~406). Without this, clipped fans from
 * geometry crossing the camera rasterize as huge wedges at z≈nearest, which then z-reject
 * everything drawn after them (bright background triangles punching through models). */
static int clip_vtx_out_of_range(CVtx *v) {
    if (v->oor != 2) return 0;        /* original vertex: transform-time verdict already passed */
    if (v->cw <= NEAR_W) return 1;    /* w ~ 0: projection blows up (PPSSPP gets inf coords) */
    float sx = v->cx * decode_float24(ge.viewport_raw[0]) / v->cw + decode_float24(ge.viewport_raw[3]);
    float sy = v->cy * decode_float24(ge.viewport_raw[1]) / v->cw + decode_float24(ge.viewport_raw[4]);
    float sz = v->cz * decode_float24(ge.viewport_raw[2]) / v->cw + decode_float24(ge.viewport_raw[5]);
    v->sx = sx; v->sy = sy; v->sz = sz;
    const float SB = 4095.0f + 15.5f / 16.0f;
    if (ge.depth_clip)
        return sx >= SB || sy >= SB || sx < 0.0f || sy < 0.0f;
    return sx > SB || sy >= SB || sx < 0.0f || sy < 0.0f || sz < 0.0f || sz >= 65536.0f;
}

/* Signed distance to the PSP's only clip plane: z >= -w (ndc z = -1). The PSP clips against
 * negative Z only, regardless of viewport (PPSSPP Clipper CLIP_POLY(CLIP_NEG_Z_BIT,0,0,1,1)). */
static float clip_d_nearz(const CVtx *v) { return v->cz + v->cw; }

/* Sutherland–Hodgman against z >= -w. in[] has n vertices (a triangle);
 * out[] receives up to n+1. Returns the clipped vertex count (0 if fully behind). */
static int clip_poly_near(const CVtx *in, int n, CVtx *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const CVtx *A = &in[i], *B = &in[(i + 1) % n];
        float da = clip_d_nearz(A), db = clip_d_nearz(B);
        int ain = da >= 0.0f, bin = db >= 0.0f;
        if (ain) out[m++] = *A;
        if (ain != bin) {
            float t = da / (da - db);
            clip_lerp_cvtx(A, B, t, &out[m++]);
        }
    }
    return m;
}

static int s_gelog = -1;
static int s_gewatch = -1, s_gewatch_after = -1, s_gewatch_limit = -1;

/* SR_GEMAT=1: dump the T&L state and the first vertex's full pipeline for the first 16
 * transform-mode PRIMs — pinpoints where 3D geometry dies (bad matrix vs bad vertex data). */
static void dump_xform_debug(uint32_t vbase, uint32_t ibase, int idxfmt, const VFmt *vf,
                             int type, int count) {
    static int on = -1, n = 0;
    if (on < 0) on = getenv("SR_GEMAT") ? 1 : 0;
    if (!on || n >= 16) return;
    n++;
    fprintf(stderr, "GEMAT frame=%u prim type=%d count=%d vtype=0x%06x vaddr=0x%08x\n",
            s_ge_frame, type, count, ge.vtype, vbase);
    fprintf(stderr, "  world:"); for (int i=0;i<12;i++) fprintf(stderr, " %.5g", ge.world[i]); fprintf(stderr, "\n");
    fprintf(stderr, "  view :"); for (int i=0;i<12;i++) fprintf(stderr, " %.5g", ge.view[i]);  fprintf(stderr, "\n");
    fprintf(stderr, "  proj :"); for (int i=0;i<16;i++) fprintf(stderr, " %.5g", ge.proj[i]);  fprintf(stderr, "\n");
    fprintf(stderr, "  vp   : xs=%.4g ys=%.4g zs=%.4g xc=%.4g yc=%.4g zc=%.4g offx=%.4g offy=%.4g\n",
            decode_float24(ge.viewport_raw[0]), decode_float24(ge.viewport_raw[1]),
            decode_float24(ge.viewport_raw[2]), decode_float24(ge.viewport_raw[3]),
            decode_float24(ge.viewport_raw[4]), decode_float24(ge.viewport_raw[5]),
            (float)(ge.offsetx & 0xFFFF) / 16.0f, (float)(ge.offsety & 0xFFFF) / 16.0f);
    uint32_t a = prim_vtx_addr(vbase, ibase, idxfmt, 0, vf->stride);
    float mx,my,mz, wx,wy,wz, cx,cy,cz, X,Y,Z,W;
    read_pos(a, vf, &mx, &my, &mz);
    mat43_mul(ge.world, mx, my, mz, &wx, &wy, &wz);
    mat43_mul(ge.view,  wx, wy, wz, &cx, &cy, &cz);
    mat44_mul(ge.proj,  cx, cy, cz, &X, &Y, &Z, &W);
    fprintf(stderr, "  v0: model=(%.4g,%.4g,%.4g) world=(%.4g,%.4g,%.4g) view=(%.4g,%.4g,%.4g) clip=(%.4g,%.4g,%.4g) clipw=%.4g%s\n",
            mx,my,mz, wx,wy,wz, cx,cy,cz, X,Y,Z, W, W <= 0.0f ? "  << NEAR-REJECTED" : "");
}

/* Fast clear-mode rectangle fill: when both colour and alpha are written the packed value is
 * constant across the rect, so rows are written directly (no per-pixel state checks). */
static void fill_rect_clear_fast(int xa, int ya, int xb, int yb, int r, int g, int b, int a) {
    int cx1 = ge.scis_x1 > 0 ? ge.scis_x1 : 0,   cy1 = ge.scis_y1 > 0 ? ge.scis_y1 : 0;
    int cx2 = ge.scis_x2 < 479 ? ge.scis_x2 : 479, cy2 = ge.scis_y2 < 271 ? ge.scis_y2 : 271;
    if (xa < cx1) xa = cx1;
    if (xb > cx2 + 1) xb = cx2 + 1;
    if (ya < cy1) ya = cy1;
    if (yb > cy2 + 1) yb = cy2 + 1;
    if (xa >= xb || ya >= yb) return;
    if (s_stat_on > 0 && xa <= 240 && 240 < xb && ya <= 136 && 136 < yb && s_pxc < 32) {
        s_pxc++;
        fprintf(stderr, "PXC f=%u CLEARFILL rect=(%d,%d)-(%d,%d) rgba=(%d,%d,%d,%d) fbp=0x%08x\n",
                s_ge_frame, xa, ya, xb, yb, r, g, b, a, ge.fbp);
    }
    uint32_t c = pack_fb(r, g, b, a);
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t fb = ge_fb_addr();
    int wide = (ge.fbfmt & 3) == 3;
    for (int y = ya; y < yb; y++) {
        uint32_t row = fb + (uint32_t)(y * (int)stride) * (wide ? 4u : 2u);
        if (wide) for (int x = xa; x < xb; x++) MEM_W32(row + (uint32_t)x*4, c);
        else      for (int x = xa; x < xb; x++) MEM_W16(row + (uint32_t)x*2, (uint16_t)c);
    }
    g_ge_pixels += (unsigned long)(xb - xa) * (unsigned long)(yb - ya);
}

/* Rasterise a sprite (axis-aligned rect from two corner vertices). Used by both modes;
 * persp=1 recovers UV from the over-w storage and depth-tests with the second vertex's z. */
static void fill_sprite(const Vtx *p0, const Vtx *p1, int persp) {
    if (s_backend && s_backend->sprite && s_backend->sprite(p0, p1, persp)) return;
    texel_cache_begin();

    int xa=(int)fminf(p0->x,p1->x), xb=(int)fmaxf(p0->x,p1->x);
    int ya=(int)fminf(p0->y,p1->y), yb=(int)fmaxf(p0->y,p1->y);

    if (ge.clear) {
        /* Clear-mode: bit 10 clears depth (to the sprite's z), bits 8/9 colour/alpha. */
        if (ge.clear_mode & 0x400)
            zbuf_fill_rect(xa, ya, xb - 1, yb - 1, (uint16_t)p1->z);
        who_init();
        if (who_active() &&
            s_who_x >= xa && s_who_x < xb &&
            s_who_y >= ya && s_who_y < yb && s_pixwho_n < 400) {
            s_pixwho_n++;
            fprintf(stderr, "PIXWHO f=%u CLEARSPR mode=0x%03x z=%.0f rect=(%d,%d)-(%d,%d) rgba=(%d,%d,%d,%d)\n",
                    s_ge_frame, ge.clear_mode, p1->z, xa, ya, xb, yb, p1->r, p1->g, p1->b, p1->a);
        }
        int wc = (ge.clear_mode & 0x100) != 0, wa = (ge.clear_mode & 0x200) != 0;
        if (!wc && !wa) return;
        if (wc && wa && !ge.dither_enable) {
            fill_rect_clear_fast(
                xa, ya, xb, yb, p1->r, p1->g, p1->b, p1->a);
            return;
        }
        for (int y=ya; y<yb; y++) for (int x=xa; x<xb; x++)
            put_px_rgba(x, y, p1->r, p1->g, p1->b, p1->a);
        return;
    }

    float u0=p0->u, v0=p0->v, u1=p1->u, v1=p1->v;
    if (persp) {  /* UV stored over w; recover texel coords */
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    if (!zrange_ok(p1->z, persp)) { if (s_stat_on > 0) s_stat.zrange++; return; }
    uint16_t pz = clamp_z(p1->z);
    int use_z = persp ? ge.ztest_enable : thru_ztest();
    int fog = (persp && ge.fog_enable) ? (int)(clampf01(p1->fog)*256.0f) : 256;
    s_in3d = persp;
    if (persp) s_stat.spr3d++; else s_stat.spr2d++;
    float uw =(xb>xa)?(u1-u0)/(float)(xb-xa):0;
    float uvv=(yb>ya)?(v1-v0)/(float)(yb-ya):0;
    int linear =
        (ge.tex_filter & 1u) != 0 ||
        ((ge.tex_filter >> 8) & 1u) != 0;
    float sample_x = linear ? 0.5f : 0.0f;
    float sample_y = linear ? 0.5f : 0.0f;
    for (int y=ya; y<yb; y++) {
        float fv = v0 + ((float)(y - ya) + sample_y) * uvv;
        for (int x=xa; x<xb; x++) {
            float u = u0 + ((float)(x - xa) + sample_x) * uw;
            int r,g,b,a;
            if (!shade(u,fv,fog,p1->r,p1->g,p1->b,p1->a,&r,&g,&b,&a)) continue;
            if (use_z) put_px_rgba_z(x,y,pz,r,g,b,a);
            else       put_px_rgba(x,y,r,g,b,a);
        }
    }
}

/* When an entire 3D primitive lands behind the near plane (all clip-w <= NEAR_W), dump the raw
 * model-space position, the resulting w's, and the full matrix state -- distinguishes "camera
 * genuinely faces away" from "our matrix state is wrong for this draw". Budgeted per stat window
 * (reset alongside the other SR_GESTAT trace budgets). */
static void near_dump(const char *what, uint32_t a0, const VFmt *vf, const CVtx *cv, int n) {
    if (s_stat_on <= 0 || s_neardump >= 3) return;
    s_neardump++;
    float mx, my, mz; read_pos(a0, vf, &mx, &my, &mz);
    fprintf(stderr, "NEARDUMP f=%u %s vtype=0x%06x raw=(%g,%g,%g) cw=(", s_ge_frame, what, ge.vtype, mx, my, mz);
    for (int i = 0; i < n; i++) fprintf(stderr, "%s%g", i ? "," : "", cv[i].cw);
    fprintf(stderr, ")\n  world=[%g %g %g; %g %g %g; %g %g %g; %g %g %g]\n",
            ge.world[0], ge.world[1], ge.world[2], ge.world[3], ge.world[4], ge.world[5],
            ge.world[6], ge.world[7], ge.world[8], ge.world[9], ge.world[10], ge.world[11]);
    fprintf(stderr, "  view =[%g %g %g; %g %g %g; %g %g %g; %g %g %g]\n",
            ge.view[0], ge.view[1], ge.view[2], ge.view[3], ge.view[4], ge.view[5],
            ge.view[6], ge.view[7], ge.view[8], ge.view[9], ge.view[10], ge.view[11]);
    fprintf(stderr, "  proj =[%g %g %g %g; %g %g %g %g; %g %g %g %g; %g %g %g %g]\n",
            ge.proj[0], ge.proj[1], ge.proj[2], ge.proj[3], ge.proj[4], ge.proj[5],
            ge.proj[6], ge.proj[7], ge.proj[8], ge.proj[9], ge.proj[10], ge.proj[11],
            ge.proj[12], ge.proj[13], ge.proj[14], ge.proj[15]);
}

static void draw_patch_cvtx_triangle(CVtx a, CVtx b, CVtx c) {
    CVtx cv[3] = {a, b, c};
    if (ge.cull_enable && !ge.clear && (ge.cull & 1u) == 0u) {
        CVtx swap = cv[0];
        cv[0] = cv[1];
        cv[1] = swap;
    }
    if (cv[0].oor || cv[1].oor || cv[2].oor) {
        s_stat.nearclip++;
        return;
    }
    if (cv[0].cw < 0.0f && cv[1].cw < 0.0f && cv[2].cw < 0.0f) {
        s_stat.nearclip++;
        return;
    }
    int positive_z = cv[0].ozp + cv[1].ozp + cv[2].ozp;
    int negative_z = cv[0].ozn + cv[1].ozn + cv[2].ozn;
    if ((!ge.depth_clip && positive_z + negative_z > 0) ||
        positive_z >= 3 || negative_z >= 3) {
        s_stat.nearclip++;
        return;
    }

    float near_distance[3] = {
        clip_d_nearz(&cv[0]),
        clip_d_nearz(&cv[1]),
        clip_d_nearz(&cv[2]),
    };
    if (near_distance[0] >= 0.0f &&
        near_distance[1] >= 0.0f &&
        near_distance[2] >= 0.0f) {
        if (cv[0].cw <= NEAR_W ||
            cv[1].cw <= NEAR_W ||
            cv[2].cw <= NEAR_W) {
            s_stat.nearclip++;
            return;
        }
        Vtx projected[3];
        project_cvtx(&cv[0], &projected[0]);
        project_cvtx(&cv[1], &projected[1]);
        project_cvtx(&cv[2], &projected[2]);
        raster_tri(&projected[0], &projected[1], &projected[2], 1);
        return;
    }
    if (near_distance[0] < 0.0f &&
        near_distance[1] < 0.0f &&
        near_distance[2] < 0.0f) {
        s_stat.nearclip++;
        return;
    }

    CVtx clipped[4];
    int count = clip_poly_near(cv, 3, clipped);
    s_stat.nearclip++;
    if (count < 3) return;
    Vtx projected[4];
    int invalid[4] = {0, 0, 0, 0};
    for (int i = 0; i < count && i < 4; i++) {
        invalid[i] =
            clipped[i].cw <= NEAR_W ||
            clip_vtx_out_of_range(&clipped[i]);
        if (!invalid[i]) project_cvtx(&clipped[i], &projected[i]);
    }
    for (int i = 2; i < count && i < 4; i++) {
        if (!invalid[0] && !invalid[i - 1] && !invalid[i]) {
            raster_tri(
                &projected[0], &projected[i - 1], &projected[i], 1);
        }
    }
}

static void draw_patch_cvtx_line(CVtx a, CVtx b) {
    if (a.oor || b.oor ||
        (a.cw < 0.0f && b.cw < 0.0f) ||
        (!ge.depth_clip && (a.ozp || a.ozn || b.ozp || b.ozn)) ||
        (a.ozp && b.ozp) || (a.ozn && b.ozn)) {
        s_stat.nearclip++;
        return;
    }
    float da = clip_d_nearz(&a);
    float db = clip_d_nearz(&b);
    if (da < 0.0f && db < 0.0f) {
        s_stat.nearclip++;
        return;
    }
    if (da < 0.0f) {
        CVtx clipped;
        clip_lerp_cvtx(&a, &b, da / (da - db), &clipped);
        a = clipped;
    } else if (db < 0.0f) {
        CVtx clipped;
        clip_lerp_cvtx(&b, &a, db / (db - da), &clipped);
        b = clipped;
    }
    if (a.cw <= NEAR_W || b.cw <= NEAR_W ||
        clip_vtx_out_of_range(&a) || clip_vtx_out_of_range(&b)) {
        s_stat.nearclip++;
        return;
    }
    Vtx projected_a, projected_b;
    project_cvtx(&a, &projected_a);
    project_cvtx(&b, &projected_b);
    draw_line_vtx(&projected_a, &projected_b, 1);
}

static void model_to_through_vtx(const ModelVtx *model, Vtx *out) {
    out->x = model->x;
    out->y = model->y;
    out->z = model->z;
    out->rw = 1.0f;
    out->u = model->u;
    out->v = model->v;
    out->fog = 1.0f;
    out->r = model->r;
    out->g = model->g;
    out->b = model->b;
    out->a = model->a;
}

static void draw_patch_triangle(
    const ModelVtx *a, const ModelVtx *b, const ModelVtx *c,
    const VFmt *vf) {
    if (vf->through) {
        Vtx va, vb, vc;
        model_to_through_vtx(a, &va);
        model_to_through_vtx(b, &vb);
        model_to_through_vtx(c, &vc);
        if (ge.cull_enable && !ge.clear && (ge.cull & 1u) == 0u) {
            Vtx swap = va;
            va = vb;
            vb = swap;
        }
        raster_tri(&va, &vb, &vc, 0);
        return;
    }
    CVtx ca, cb, cc;
    transform_model_vtx_clip(a, &ca);
    transform_model_vtx_clip(b, &cb);
    transform_model_vtx_clip(c, &cc);
    draw_patch_cvtx_triangle(ca, cb, cc);
}

static void draw_patch_line(
    const ModelVtx *a, const ModelVtx *b, const VFmt *vf) {
    if (vf->through) {
        Vtx va, vb;
        model_to_through_vtx(a, &va);
        model_to_through_vtx(b, &vb);
        draw_line_vtx(&va, &vb, 0);
        return;
    }
    CVtx ca, cb;
    transform_model_vtx_clip(a, &ca);
    transform_model_vtx_clip(b, &cb);
    draw_patch_cvtx_line(ca, cb);
}

static void draw_patch_point(const ModelVtx *point, const VFmt *vf) {
    if (vf->through) {
        Vtx projected;
        model_to_through_vtx(point, &projected);
        draw_point_vtx(&projected, 0);
        return;
    }
    CVtx clip;
    transform_model_vtx_clip(point, &clip);
    if (clip.cw <= NEAR_W || clip.oor ||
        (!ge.depth_clip && (clip.ozp || clip.ozn))) {
        s_stat.nearclip++;
        return;
    }
    Vtx projected;
    project_cvtx(&clip, &projected);
    draw_point_vtx(&projected, 1);
}

#define GE_MAX_PATCH_VERTICES 65536u

typedef struct {
    int segment;
    int k;
    int alpha[6];
} PatchAxisParam;

typedef struct {
    float position[3];
    float before[3];
    float after[3];
    float texcoord[2];
    int color[4];
} PatchColumn;

typedef struct {
    int32_t mantissa;
    int lsb_exponent;
} PatchRowTerm;

static int patch_param_256(int index, int tessellation) {
    return 2 * index <= tessellation
        ? (256 * index) / tessellation
        : 256 - (256 * (tessellation - index)) / tessellation;
}

static float patch_ge_lerp(float a, float b, int k) {
    if (k == 0) return a;
    if (k == 256) return b;
    uint32_t bits_a, bits_b;
    memcpy(&bits_a, &a, sizeof(bits_a));
    memcpy(&bits_b, &b, sizeof(bits_b));
    int exponent_a = (int)((bits_a >> 23) & 0xffu);
    int exponent_b = (int)((bits_b >> 23) & 0xffu);
    int exponent =
        exponent_a > exponent_b ? exponent_a : exponent_b;
    if (exponent == 0) return 0.0f;
    int fixed_a = 0;
    int fixed_b = 0;
    if (exponent_a != 0 && exponent - exponent_a < 16) {
        fixed_a = (int)(
            ((bits_a & 0x7fffffu) | 0x800000u) >>
            (exponent - exponent_a + 8));
        if ((bits_a & 0x80000000u) != 0) fixed_a = -fixed_a;
    }
    if (exponent_b != 0 && exponent - exponent_b < 16) {
        fixed_b = (int)(
            ((bits_b & 0x7fffffu) | 0x800000u) >>
            (exponent - exponent_b + 8));
        if ((bits_b & 0x80000000u) != 0) fixed_b = -fixed_b;
    }
    int result = fixed_a + (((fixed_b - fixed_a) * k) >> 8);
    if (exponent <= 15)
        return ldexpf((float)result, exponent - 127 - 15);
    uint32_t scale_bits = (uint32_t)(exponent - 15) << 23;
    float scale;
    memcpy(&scale, &scale_bits, sizeof(scale));
    return (float)result * scale;
}

static float patch_bezier_eval(
    const float values[4], int k, float *before, float *after) {
    float a = patch_ge_lerp(values[0], values[1], k);
    float b = patch_ge_lerp(values[1], values[2], k);
    float c = patch_ge_lerp(values[2], values[3], k);
    float left = patch_ge_lerp(a, b, k);
    float right = patch_ge_lerp(b, c, k);
    if (before) *before = left;
    if (after) *after = right;
    return patch_ge_lerp(left, right, k);
}

static int patch_color_lerp(int a, int b, int k) {
    return a + (((b - a) * k) >> 8);
}

static int patch_bezier_color(const int values[4], int k) {
    int a = patch_color_lerp(values[0], values[1], k);
    int b = patch_color_lerp(values[1], values[2], k);
    int c = patch_color_lerp(values[2], values[3], k);
    return patch_color_lerp(
        patch_color_lerp(a, b, k),
        patch_color_lerp(b, c, k), k);
}

static int patch_spline_knot(int index, int patch_count, int type) {
    if (index < 3 && (type & 1) != 0) return 0;
    if (index > patch_count + 3 && (type & 2) != 0)
        return patch_count;
    return index - 3;
}

static void patch_spline_param(
    int index, int tessellation, int patch_count, int type,
    PatchAxisParam *out) {
    out->segment = index / tessellation;
    out->k = patch_param_256(index % tessellation, tessellation);
    if (out->segment == patch_count) {
        out->segment = patch_count - 1;
        out->k = 256;
    }
    int level_index[6] = {
        out->segment + 1, out->segment + 2,
        out->segment + 3, out->segment + 2,
        out->segment + 3, out->segment + 3,
    };
    static const int level[6] = {1, 1, 1, 2, 2, 3};
    for (int i = 0; i < 6; i++) {
        int knot_left =
            patch_spline_knot(level_index[i], patch_count, type);
        int knot_right = patch_spline_knot(
            level_index[i] + 4 - level[i], patch_count, type);
        int span = knot_right - knot_left;
        int distance_left =
            256 * (out->segment - knot_left) + out->k;
        int distance_right = 256 * span - distance_left;
        out->alpha[i] = span == 0 ? 0
            : (distance_left <= distance_right
                ? distance_left / span
                : 256 - distance_right / span);
    }
}

static float patch_spline_eval(
    const float values[4], const int alpha[6],
    float *before, float *after) {
    float left0 = patch_ge_lerp(values[0], values[1], alpha[0]);
    float left1 = patch_ge_lerp(values[1], values[2], alpha[1]);
    float left2 = patch_ge_lerp(values[2], values[3], alpha[2]);
    float middle0 = patch_ge_lerp(left0, left1, alpha[3]);
    float middle1 = patch_ge_lerp(left1, left2, alpha[4]);
    if (before) *before = middle0;
    if (after) *after = middle1;
    return patch_ge_lerp(middle0, middle1, alpha[5]);
}

static int patch_spline_color(
    const int values[4], const int alpha[6]) {
    int left0 =
        patch_color_lerp(values[0], values[1], alpha[0]);
    int left1 =
        patch_color_lerp(values[1], values[2], alpha[1]);
    int left2 =
        patch_color_lerp(values[2], values[3], alpha[2]);
    return patch_color_lerp(
        patch_color_lerp(left0, left1, alpha[3]),
        patch_color_lerp(left1, left2, alpha[4]), alpha[5]);
}

static float patch_axis_eval(
    const float values[4], const PatchAxisParam *param,
    int spline, float *before, float *after) {
    return spline
        ? patch_spline_eval(
            values, param->alpha, before, after)
        : patch_bezier_eval(values, param->k, before, after);
}

static int patch_axis_color(
    const int values[4], const PatchAxisParam *param, int spline) {
    return spline
        ? patch_spline_color(values, param->alpha)
        : patch_bezier_color(values, param->k);
}

static float patch_truncate_float24(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    bits &= 0xffffff00u;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float patch_ge_add(float a, float b) {
    uint32_t bits_a, bits_b;
    memcpy(&bits_a, &a, sizeof(bits_a));
    memcpy(&bits_b, &b, sizeof(bits_b));
    int exponent_a = (int)((bits_a >> 23) & 0xffu);
    int exponent_b = (int)((bits_b >> 23) & 0xffu);
    if (exponent_a == 0 || exponent_b == 0) {
        if (exponent_a == 0 && exponent_b == 0) return 0.0f;
        return exponent_a == 0
            ? patch_truncate_float24(b)
            : patch_truncate_float24(a);
    }
    int max_exponent =
        exponent_a > exponent_b ? exponent_a : exponent_b;
    int clear_a = 8 + max_exponent - exponent_a;
    int clear_b = 8 + max_exponent - exponent_b;
    bits_a = clear_a >= 24
        ? bits_a & 0x80000000u
        : bits_a & ~((1u << clear_a) - 1u);
    bits_b = clear_b >= 24
        ? bits_b & 0x80000000u
        : bits_b & ~((1u << clear_b) - 1u);
    memcpy(&a, &bits_a, sizeof(a));
    memcpy(&b, &bits_b, sizeof(b));
    return a + b;
}

static PatchRowTerm patch_ge_product(float a, float b) {
    uint32_t bits_a, bits_b;
    memcpy(&bits_a, &a, sizeof(bits_a));
    memcpy(&bits_b, &b, sizeof(bits_b));
    int exponent_a = (int)((bits_a >> 23) & 0xffu);
    int exponent_b = (int)((bits_b >> 23) & 0xffu);
    PatchRowTerm term;
    if (exponent_a == 0 || exponent_b == 0) {
        term.mantissa = 0;
        term.lsb_exponent = INT_MIN;
        return term;
    }
    uint64_t mantissa_a =
        (uint64_t)((bits_a & 0x7fffffu) | 0x800000u);
    uint64_t mantissa_b =
        (uint64_t)((bits_b & 0x7fffffu) | 0x800000u);
    int32_t product =
        (int32_t)((mantissa_a * mantissa_b) >> 31);
    term.mantissa =
        ((bits_a ^ bits_b) & 0x80000000u) != 0
        ? -product : product;
    term.lsb_exponent =
        (exponent_a - 127) + (exponent_b - 127) - 15;
    return term;
}

static float patch_ge_row_sum(const PatchRowTerm terms[2]) {
    int lsb_exponent = terms[0].lsb_exponent >
        terms[1].lsb_exponent
        ? terms[0].lsb_exponent : terms[1].lsb_exponent;
    int32_t sum = 0;
    for (int i = 0; i < 2; i++) {
        int32_t mantissa = terms[i].mantissa;
        if (mantissa == 0) continue;
        int shift = lsb_exponent - terms[i].lsb_exponent;
        if (shift < 32) {
            sum += mantissa < 0
                ? -(-mantissa >> shift)
                : mantissa >> shift;
        }
    }
    if (sum == 0) return 0.0f;
    return patch_truncate_float24(
        ldexpf((float)sum, lsb_exponent));
}

typedef struct {
    PatchAxisParam *params;
    size_t capacity;
    int point_count;
    int tessellation;
    int edge_type;
    int count;
} PatchAxisCache;

static PatchAxisParam *get_spline_axis_params(
    PatchAxisCache *cache, int point_count,
    int tessellation, int edge_type, int *out_count) {
    int patch_count = point_count - 3;
    int count = patch_count * tessellation + 1;
    if ((size_t)count > cache->capacity) {
        size_t capacity = cache->capacity ? cache->capacity : 32u;
        while (capacity < (size_t)count)
            capacity *= 2u;
        void *memory = realloc(
            cache->params, capacity * sizeof(*cache->params));
        if (!memory) return NULL;
        cache->params = (PatchAxisParam *)memory;
        cache->capacity = capacity;
    }
    if (cache->point_count == point_count &&
        cache->tessellation == tessellation &&
        cache->edge_type == edge_type) {
        *out_count = cache->count;
        return cache->params;
    }
    for (int index = 0; index < count; index++)
        patch_spline_param(
            index, tessellation, patch_count, edge_type,
            &cache->params[index]);
    cache->point_count = point_count;
    cache->tessellation = tessellation;
    cache->edge_type = edge_type;
    cache->count = count;
    *out_count = count;
    return cache->params;
}

static void build_patch_columns(
    const ModelVtx *control, int point_stride,
    int u_segment, const PatchAxisParam *v_param, int spline,
    int has_texcoord, int needs_normal, PatchColumn columns[4]) {
    for (int column = 0; column < 4; column++) {
        for (int component = 0; component < 3; component++) {
            float values[4];
            for (int row = 0; row < 4; row++) {
                const ModelVtx *point = &control[
                    (v_param->segment + row) * point_stride +
                    u_segment + column];
                values[row] = (&point->x)[component];
            }
            columns[column].position[component] = patch_axis_eval(
                values, v_param, spline,
                needs_normal
                    ? &columns[column].before[component] : NULL,
                needs_normal
                    ? &columns[column].after[component] : NULL);
        }
        if (has_texcoord) {
            for (int component = 0; component < 2; component++) {
                float values[4];
                for (int row = 0; row < 4; row++) {
                    const ModelVtx *point = &control[
                        (v_param->segment + row) * point_stride +
                        u_segment + column];
                    values[row] = (&point->u)[component];
                }
                columns[column].texcoord[component] =
                    patch_axis_eval(
                        values, v_param, spline, NULL, NULL);
            }
        }
        for (int component = 0; component < 4; component++) {
            int values[4];
            for (int row = 0; row < 4; row++) {
                const ModelVtx *point = &control[
                    (v_param->segment + row) * point_stride +
                    u_segment + column];
                values[row] =
                    ((&point->r)[component] << 7) | 0x7f;
            }
            columns[column].color[component] =
                patch_axis_color(values, v_param, spline);
        }
    }
}

static void evaluate_patch_columns(
    const PatchColumn columns[4],
    const PatchAxisParam *u_param, int spline,
    float generated_u, float generated_v,
    int has_texcoord, int needs_normal,
    ModelVtx *out) {
    float tangent_u[3] = {0.0f, 0.0f, 0.0f};
    float tangent_v[3] = {0.0f, 0.0f, 0.0f};
    for (int component = 0; component < 3; component++) {
        float values[4];
        for (int column = 0; column < 4; column++) {
            values[column] = columns[column].position[component];
        }
        if (needs_normal) {
            float before_values[4], after_values[4];
            for (int column = 0; column < 4; column++) {
                before_values[column] =
                    columns[column].before[component];
                after_values[column] =
                    columns[column].after[component];
            }
            float before, after;
            (&out->x)[component] = patch_axis_eval(
                values, u_param, spline, &before, &after);
            tangent_u[component] =
                patch_ge_add(after, -before);
            tangent_v[component] = patch_ge_add(
                patch_axis_eval(
                    after_values, u_param, spline, NULL, NULL),
                -patch_axis_eval(
                    before_values, u_param, spline, NULL, NULL));
        } else {
            (&out->x)[component] = patch_axis_eval(
                values, u_param, spline, NULL, NULL);
        }
    }
    if (has_texcoord) {
        for (int component = 0; component < 2; component++) {
            float values[4];
            for (int column = 0; column < 4; column++)
                values[column] =
                    columns[column].texcoord[component];
            (&out->u)[component] = patch_axis_eval(
                values, u_param, spline, NULL, NULL);
        }
    } else {
        out->u = generated_u;
        out->v = generated_v;
    }
    for (int component = 0; component < 4; component++) {
        int values[4];
        for (int column = 0; column < 4; column++)
            values[column] = columns[column].color[component];
        (&out->r)[component] = clamp8(
            patch_axis_color(values, u_param, spline) >> 7);
    }

    if (needs_normal) {
        for (int component = 0; component < 3; component++) {
            int first = (component + 1) % 3;
            int second = (component + 2) % 3;
            PatchRowTerm terms[2] = {
                patch_ge_product(
                    tangent_u[first], tangent_v[second]),
                patch_ge_product(
                    -tangent_u[second], tangent_v[first]),
            };
            (&out->nx)[component] = patch_ge_row_sum(terms);
        }
    } else {
        out->nx = 0.0f;
        out->ny = 0.0f;
        out->nz = 1.0f;
    }
    if ((ge.patch_facing ^ (uint32_t)(ge.reverse_normal != 0)) != 0) {
        out->nx = -out->nx;
        out->ny = -out->ny;
        out->nz = -out->nz;
    }
}

static Vtx *s_patch_through_cache;
static CVtx *s_patch_transform_cache;
static size_t s_patch_cache_capacity;

static int reserve_patch_cache(size_t count) {
    if (count <= s_patch_cache_capacity) return 1;
    size_t capacity = s_patch_cache_capacity
        ? s_patch_cache_capacity : 256u;
    while (capacity < count)
        capacity *= 2u;
    Vtx *through = (Vtx *)realloc(
        s_patch_through_cache, capacity * sizeof(*through));
    if (!through) return 0;
    s_patch_through_cache = through;
    CVtx *transform = (CVtx *)realloc(
        s_patch_transform_cache, capacity * sizeof(*transform));
    if (!transform) return 0;
    s_patch_transform_cache = transform;
    s_patch_cache_capacity = capacity;
    return 1;
}

static void draw_patch_through_triangle(
    const Vtx *a, const Vtx *b, const Vtx *c) {
    Vtx first = *a;
    Vtx second = *b;
    if (ge.cull_enable && !ge.clear && (ge.cull & 1u) == 0u) {
        Vtx swap = first;
        first = second;
        second = swap;
    }
    raster_tri(&first, &second, c, 0);
}

static void draw_patch_grid(
    const ModelVtx *vertices, int width, int height,
    const VFmt *vf) {
    size_t count = (size_t)width * (size_t)height;
    Vtx *through = NULL;
    CVtx *transform = NULL;
    if (reserve_patch_cache(count)) {
        if (vf->through) {
            through = s_patch_through_cache;
            for (size_t i = 0; i < count; i++)
                model_to_through_vtx(&vertices[i], &through[i]);
        } else {
            transform = s_patch_transform_cache;
            for (size_t i = 0; i < count; i++)
                transform_model_vtx_clip(&vertices[i], &transform[i]);
        }
    }
    int batch_triangles = (ge.patch_primitive & 3u) == 0u;
    if (batch_triangles) begin_backend_tri_batch(!vf->through);
    for (int v = 0; v + 1 < height; v++) {
        for (int u = 0; u + 1 < width; u++) {
            int top_left_index = v * width + u;
            int top_right_index = top_left_index + 1;
            int bottom_left_index = (v + 1) * width + u;
            int bottom_right_index = bottom_left_index + 1;
            const ModelVtx *top_left = &vertices[v * width + u];
            const ModelVtx *top_right = top_left + 1;
            const ModelVtx *bottom_left =
                &vertices[(v + 1) * width + u];
            const ModelVtx *bottom_right = bottom_left + 1;
            if ((ge.patch_primitive & 3u) == 0u) {
                if (through) {
                    draw_patch_through_triangle(
                        &through[top_left_index],
                        &through[bottom_left_index],
                        &through[top_right_index]);
                    draw_patch_through_triangle(
                        &through[top_right_index],
                        &through[bottom_left_index],
                        &through[bottom_right_index]);
                } else if (transform) {
                    draw_patch_cvtx_triangle(
                        transform[top_left_index],
                        transform[bottom_left_index],
                        transform[top_right_index]);
                    draw_patch_cvtx_triangle(
                        transform[top_right_index],
                        transform[bottom_left_index],
                        transform[bottom_right_index]);
                } else {
                    draw_patch_triangle(
                        top_left, bottom_left, top_right, vf);
                    draw_patch_triangle(
                        top_right, bottom_left, bottom_right, vf);
                }
            } else if ((ge.patch_primitive & 3u) == 1u) {
                if (through) {
                    draw_line_vtx(
                        &through[top_left_index],
                        &through[bottom_left_index], 0);
                    draw_line_vtx(
                        &through[bottom_left_index],
                        &through[top_right_index], 0);
                    draw_line_vtx(
                        &through[top_right_index],
                        &through[bottom_right_index], 0);
                } else if (transform) {
                    draw_patch_cvtx_line(
                        transform[top_left_index],
                        transform[bottom_left_index]);
                    draw_patch_cvtx_line(
                        transform[bottom_left_index],
                        transform[top_right_index]);
                    draw_patch_cvtx_line(
                        transform[top_right_index],
                        transform[bottom_right_index]);
                } else {
                    draw_patch_line(top_left, bottom_left, vf);
                    draw_patch_line(bottom_left, top_right, vf);
                    draw_patch_line(top_right, bottom_right, vf);
                }
            } else {
                if (through) {
                    draw_point_vtx(&through[top_left_index], 0);
                    draw_point_vtx(&through[bottom_left_index], 0);
                    draw_point_vtx(&through[bottom_left_index], 0);
                    draw_point_vtx(&through[top_right_index], 0);
                    draw_point_vtx(&through[top_right_index], 0);
                    draw_point_vtx(&through[bottom_right_index], 0);
                } else if (transform) {
                    static const int offsets[6][2] = {
                        {0, 0}, {0, 1}, {0, 1},
                        {1, 0}, {1, 0}, {1, 1},
                    };
                    for (int i = 0; i < 6; i++) {
                        int index =
                            (v + offsets[i][1]) * width +
                            u + offsets[i][0];
                        Vtx projected;
                        if (project_cached_vtx(
                                &transform[index], &projected)) {
                            draw_point_vtx(&projected, 1);
                        }
                    }
                } else {
                    draw_patch_point(top_left, vf);
                    draw_patch_point(bottom_left, vf);
                    draw_patch_point(bottom_left, vf);
                    draw_patch_point(top_right, vf);
                    draw_patch_point(top_right, vf);
                    draw_patch_point(bottom_right, vf);
                }
            }
        }
    }
    if (batch_triangles) end_backend_tri_batch();
}

static void limit_patch_tessellation(
    int *tess_u, int *tess_v, uint64_t patch_u, uint64_t patch_v,
    int separate_patches) {
    for (;;) {
        uint64_t width = separate_patches
            ? (uint64_t)(*tess_u + 1)
            : patch_u * (uint64_t)*tess_u + 1u;
        uint64_t height = separate_patches
            ? (uint64_t)(*tess_v + 1)
            : patch_v * (uint64_t)*tess_v + 1u;
        uint64_t vertices = width * height;
        if (separate_patches) vertices *= patch_u * patch_v;
        if (vertices <= GE_MAX_PATCH_VERTICES ||
            (*tess_u <= 1 && *tess_v <= 1)) {
            return;
        }
        if (*tess_u >= *tess_v && *tess_u > 1)
            (*tess_u)--;
        else if (*tess_v > 1)
            (*tess_v)--;
    }
}

static ModelVtx *s_patch_control_cache;
static size_t s_patch_control_capacity;
static ModelVtx *s_patch_vertex_cache;
static size_t s_patch_vertex_capacity;
static PatchAxisCache s_patch_u_axis_cache;
static PatchAxisCache s_patch_v_axis_cache;

static ModelVtx *reserve_patch_model_cache(
    ModelVtx **cache, size_t *capacity, size_t count) {
    if (count <= *capacity) return *cache;
    size_t next = *capacity ? *capacity : 64u;
    while (next < count)
        next *= 2u;
    void *memory = realloc(*cache, next * sizeof(**cache));
    if (!memory) return NULL;
    *cache = (ModelVtx *)memory;
    *capacity = next;
    return *cache;
}

static ModelVtx *decode_patch_controls(
    uint32_t vbase, uint32_t ibase, int index_format,
    const VFmt *vf, int count) {
    ModelVtx *control = reserve_patch_model_cache(
        &s_patch_control_cache, &s_patch_control_capacity,
        (size_t)count);
    if (!control) return NULL;
    for (int i = 0; i < count; i++) {
        uint32_t address = prim_vtx_addr(
            vbase, ibase, index_format, i, vf->stride);
        decode_model_vtx(address, vf, &control[i]);
    }
    return control;
}

static void advance_patch_vertices(
    int count, int index_format, const VFmt *vf) {
    if (index_format)
        ge.iaddr += (uint32_t)(count << (index_format - 1));
    else
        ge.vaddr += (uint32_t)(count * vf->stride);
}

static void draw_bezier(uint32_t op) {
    int points_u = (int)(op & 0xffu);
    int points_v = (int)((op >> 8) & 0xffu);
    int count = points_u * points_v;
    VFmt vf;
    decode_vtype(ge.vtype, &vf);
    int index_format = (int)((ge.vtype >> 11) & 3u);
    uint32_t vbase = ge.vaddr;
    uint32_t ibase = ge.iaddr;
    advance_patch_vertices(count, index_format, &vf);
    int patch_u = (points_u - 1) / 3;
    int patch_v = (points_v - 1) / 3;
    if (points_u < 4 || points_v < 4 ||
        patch_u < 1 || patch_v < 1) {
        return;
    }
    ModelVtx *control = decode_patch_controls(
        vbase, ibase, index_format, &vf, count);
    if (!control) {
        fprintf(stderr, "GE: Bezier control-point allocation failed\n");
        return;
    }

    int tess_u = (int)(ge.patch_division & 0x7fu);
    int tess_v = (int)((ge.patch_division >> 8) & 0x7fu);
    if (tess_u < 1) tess_u = 1;
    if (tess_v < 1) tess_v = 1;
    limit_patch_tessellation(
        &tess_u, &tess_v, (uint64_t)patch_u, (uint64_t)patch_v, 1);
    int width = tess_u + 1;
    int height = tess_v + 1;
    uint32_t texture_map_mode = ge.tex_map_mode & 3u;
    int needs_normal =
        !vf.through &&
        (ge.lighting_enable ||
         texture_map_mode == 2u ||
         (texture_map_mode == 1u &&
          ((ge.tex_map_mode >> 8) & 3u) >= 2u));
    ModelVtx *vertices = reserve_patch_model_cache(
        &s_patch_vertex_cache, &s_patch_vertex_capacity,
        (size_t)width * (size_t)height);
    if (!vertices) {
        fprintf(stderr, "GE: Bezier tessellation allocation failed\n");
        return;
    }
    SrPerfCategory patch_previous =
        sr_perf_switch(SR_PERF_PATCH_EVAL);
    for (int pu = 0; pu < patch_u; pu++) {
        for (int pv = 0; pv < patch_v; pv++) {
            const ModelVtx *patch =
                &control[(pv * 3) * points_u + pu * 3];
            for (int v = 0; v <= tess_v; v++) {
                PatchAxisParam param_v = {0};
                param_v.k = patch_param_256(v, tess_v);
                PatchColumn columns[4];
                build_patch_columns(
                    patch, points_u, 0, &param_v, 0,
                    vf.tc_off >= 0, needs_normal, columns);
                for (int u = 0; u <= tess_u; u++) {
                    PatchAxisParam param_u = {0};
                    param_u.k = patch_param_256(u, tess_u);
                    evaluate_patch_columns(
                        columns, &param_u, 0,
                        (float)pu + (float)param_u.k / 256.0f,
                        (float)pv + (float)param_v.k / 256.0f,
                        vf.tc_off >= 0, needs_normal,
                        &vertices[v * width + u]);
                }
            }
            sr_perf_switch(SR_PERF_PATCH_DRAW);
            draw_patch_grid(vertices, width, height, &vf);
            sr_perf_switch(SR_PERF_PATCH_EVAL);
        }
    }
    sr_perf_switch(patch_previous);
}

static void draw_spline(uint32_t op) {
    int points_u = (int)(op & 0xffu);
    int points_v = (int)((op >> 8) & 0xffu);
    int count = points_u * points_v;
    VFmt vf;
    decode_vtype(ge.vtype, &vf);
    int index_format = (int)((ge.vtype >> 11) & 3u);
    uint32_t vbase = ge.vaddr;
    uint32_t ibase = ge.iaddr;
    advance_patch_vertices(count, index_format, &vf);
    int patch_u = points_u - 3;
    int patch_v = points_v - 3;
    if (patch_u < 1 || patch_v < 1) return;
    ModelVtx *control = decode_patch_controls(
        vbase, ibase, index_format, &vf, count);
    if (!control) {
        fprintf(stderr, "GE: spline control-point allocation failed\n");
        return;
    }

    int tess_u = (int)(ge.patch_division & 0x7fu);
    int tess_v = (int)((ge.patch_division >> 8) & 0x7fu);
    if (tess_u < 1) tess_u = 1;
    if (tess_v < 1) tess_v = 1;
    limit_patch_tessellation(
        &tess_u, &tess_v, (uint64_t)patch_u, (uint64_t)patch_v, 0);
    int width = 0, height = 0;
    int type_u = (int)((op >> 16) & 3u);
    int type_v = (int)((op >> 18) & 3u);
    PatchAxisParam *params_u = get_spline_axis_params(
        &s_patch_u_axis_cache,
        points_u, tess_u, type_u, &width);
    PatchAxisParam *params_v = get_spline_axis_params(
        &s_patch_v_axis_cache,
        points_v, tess_v, type_v, &height);
    if (!params_u || !params_v) {
        fprintf(stderr, "GE: spline parameter allocation failed\n");
        return;
    }
    ModelVtx *vertices = reserve_patch_model_cache(
        &s_patch_vertex_cache, &s_patch_vertex_capacity,
        (size_t)width * (size_t)height);
    if (!vertices) {
        fprintf(stderr, "GE: spline tessellation allocation failed\n");
        return;
    }
    uint32_t texture_map_mode = ge.tex_map_mode & 3u;
    int needs_normal =
        !vf.through &&
        (ge.lighting_enable ||
         texture_map_mode == 2u ||
         (texture_map_mode == 1u &&
          ((ge.tex_map_mode >> 8) & 3u) >= 2u));
    SrPerfCategory patch_previous =
        sr_perf_switch(SR_PERF_PATCH_EVAL);
    for (int v = 0; v < height; v++) {
        PatchColumn columns[4];
        int column_segment = -1;
        for (int u = 0; u < width; u++) {
            const PatchAxisParam *param_u = &params_u[u];
            const PatchAxisParam *param_v = &params_v[v];
            if (param_u->segment != column_segment) {
                column_segment = param_u->segment;
                build_patch_columns(
                    control, points_u, column_segment, param_v, 1,
                    vf.tc_off >= 0, needs_normal, columns);
            }
            evaluate_patch_columns(
                columns, param_u, 1,
                (float)param_u->segment +
                    (float)param_u->k / 256.0f,
                (float)param_v->segment +
                    (float)param_v->k / 256.0f,
                vf.tc_off >= 0, needs_normal,
                &vertices[v * width + u]);
        }
    }
    sr_perf_switch(SR_PERF_PATCH_DRAW);
    draw_patch_grid(vertices, width, height, &vf);
    sr_perf_switch(patch_previous);
}

static void draw_prim(uint32_t op) {
    int type = (op >> 16) & 7;
    int count = op & 0xFFFF;
    VFmt vf; decode_vtype(ge.vtype, &vf);
    sr_perf_counter_add(
        vf.through
            ? SR_PERF_GE_THROUGH_DRAWS
            : SR_PERF_GE_TRANSFORM_DRAWS,
        1);
    sr_perf_counter_add(
        vf.through
            ? SR_PERF_GE_THROUGH_VERTICES
            : SR_PERF_GE_TRANSFORM_VERTICES,
        (uint64_t)count);
    if (s_gelog < 0) s_gelog = getenv("SR_GEDUMP") ? 1 : 0;
    if (s_gewatch < 0) s_gewatch = getenv("SR_GEWATCH") ? 1 : 0;
    if (s_gewatch_after < 0) { const char *p=getenv("SR_GEWATCH_AFTER"); s_gewatch_after=p?atoi(p):0; }
    if (s_gewatch_limit < 0) { const char *p=getenv("SR_GEWATCH_LIMIT"); s_gewatch_limit=p?atoi(p):240; if(s_gewatch_limit<=0)s_gewatch_limit=240; }
    int watch_now = s_gewatch && s_ge_frame >= (uint32_t)s_gewatch_after;
    int idxfmt = (ge.vtype >> 11) & 3;
    if (s_gelog) {
        static int n=0, nthrough=0, ntransform=0;
        if (vf.through) nthrough++; else ntransform++;
        if (n++ < 40 || (!vf.through && ntransform < 20))
            fprintf(stderr, "GE PRIM type=%d count=%d vtype=0x%06x through=%d idx=%d tex=%d fmt=%u fbp=0x%08x vaddr=0x%08x iaddr=0x%08x [thru=%d xform=%d]\n",
                type, count, ge.vtype, vf.through, idxfmt, ge.tex_enable, ge.tex_fmt, ge.fbp, ge.vaddr, ge.iaddr, nthrough, ntransform);
    }
    uint32_t vbase = ge.vaddr, ibase = ge.iaddr;
    /* Advance vertex/index pointer (PPSSPP GPUCommon::AdvanceVerts). */
    if (idxfmt) ge.iaddr += (uint32_t)(count << (idxfmt - 1));
    else        ge.vaddr += (uint32_t)(count * vf.stride);

    /* ---- THROUGH-MODE (2D, screen coords) ---- */
    if (vf.through) {
        SrPerfCategory decode_previous =
            sr_perf_switch(SR_PERF_VERTEX_DECODE);
        const Vtx *cached = cache_through_vertices(
            vbase, ibase, idxfmt, &vf, count);
        sr_perf_switch(decode_previous);
        if (type == 0) {           /* points */
            for (int i = 0; i < count; i++) {
                Vtx a;
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i, &vf, &a);
                draw_point_vtx(&a, 0);
            }
        } else if (type == 1 || type == 2) {   /* lines / line strip */
            for (int i = 1; i < count; i++) {
                if (type == 1 && (i & 1) == 0) continue;
                Vtx a, b;
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i - 1, &vf, &a);
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i, &vf, &b);
                draw_line_vtx(&a,&b,0);
            }
        } else if (type == 3 || type == 4 || type == 5) {
            begin_backend_tri_batch(0);
            for (int i = 2; i < count; i++) {
                int i0, i1, i2;
                if      (type == 3) { if (i%3!=2) continue; i0=i-2; i1=i-1; i2=i; }
                else if (type == 4) { i0=i-2; i1=i-1; i2=i; }
                else                { i0=0;   i1=i-1; i2=i; }
                Vtx a, b, c;
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i0, &vf, &a);
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i1, &vf, &b);
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i2, &vf, &c);
                /* Same submission-order culling as transform mode (the GE culls in through
                 * mode too); strips alternate, provoking vertex stays last. */
                if (ge.cull_enable && !ge.clear) {
                    int flip = ((ge.cull & 1) == 0) ^ ((type == 4) ? (i & 1) : 0);
                    if (flip) { Vtx t = a; a = b; b = t; }
                }
                raster_tri(&a,&b,&c,0);
            }
            end_backend_tri_batch();
        } else if (type == 6) {   /* sprites */
            for (int i = 1; i < count; i += 2) {
                Vtx p0, p1;
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i - 1, &vf, &p0);
                load_primitive_vertex(
                    cached, vbase, ibase, idxfmt, i, &vf, &p1);
                if (watch_now) {
                    int xa=(int)fminf(p0.x,p1.x), xb=(int)fmaxf(p0.x,p1.x);
                    int ya=(int)fminf(p0.y,p1.y), yb=(int)fmaxf(p0.y,p1.y);
                    int area=(xb-xa)*(yb-ya);
                    if (idxfmt||area>=4096) {
                        static int wn=0;
                        if (wn++<s_gewatch_limit)
                            fprintf(stderr,"GEWATCH frame=%u spr idx=%d area=%d rect=(%d,%d)-(%d,%d) uv=(%.0f,%.0f)-(%.0f,%.0f) color0=%02x%02x%02x%02x color1=%02x%02x%02x%02x tex=%d/0x%08x %dx%d fmt=%u func=0x%06x blend=%d mode=0x%06x fix=%06x/%06x mat=%06x a=%d clear=0x%03x fbp=0x%08x vtype=0x%06x\n",
                                s_ge_frame,idxfmt,area,xa,ya,xb,yb,p0.u,p0.v,p1.u,p1.v,
                                p0.a,p0.b,p0.g,p0.r,p1.a,p1.b,p1.g,p1.r,
                                ge.tex_enable,ge.tex_addr,ge.tex_w,ge.tex_h,ge.tex_fmt,ge.tex_func,
                                ge.blend_enable,ge.blend_mode,ge.blend_fixa,ge.blend_fixb,
                                ge.material,ge.material_alpha_set?ge.material_alpha:-1,ge.clear_mode,
                                ge.fbp,ge.vtype);
                    }
                }
                if (s_gelog&&(ge.tex_enable&&!ge.clear)) { static int sn=0; if(sn++<12)
                    fprintf(stderr,"  texspr (%.0f,%.0f)-(%.0f,%.0f) uv(%.0f,%.0f)-(%.0f,%.0f) tex=0x%08x %dx%d fmt=%u bufw=%u\n",
                        p0.x,p0.y,p1.x,p1.y,p0.u,p0.v,p1.u,p1.v,ge.tex_addr,ge.tex_w,ge.tex_h,ge.tex_fmt,ge.tex_bufw); }
                fill_sprite(&p0,&p1,0);
            }
        }
        return;
    }

    /* ---- TRANSFORM-MODE (3D, full T&L) ---- */
    dump_xform_debug(vbase, ibase, idxfmt, &vf, type, count);
    if (s_rtrace < 0) {
        s_rtrace = getenv("SR_RTRACE") ? 1 : 0;
        const char *rf = getenv("SR_RTRACE_FRAMES");
        if (rf) { s_rt_fbudget = atoi(rf); if (s_rt_fbudget <= 0) s_rt_fbudget = 2; }
    }
    int rt = 0;
    if (s_rtrace) {
        if (s_ge_frame != s_rt_cur_frame) {
            s_rt_cur_frame = s_ge_frame;
            s_rt_active = s_rt_frames < s_rt_fbudget;
            if (s_rt_active) {
                s_rt_frames++;
                s_rt_draws_f = 0;
                who_init();
                fprintf(stderr, "RTRACE f=%u BEGIN fbp=0x%08x/%u fmt=%u zbp=0x%08x dclip=%u probe=(%d,%d)\n",
                        s_ge_frame, ge.fbp, ge.fbw, ge.fbfmt, ge.zbp, ge.depth_clip, s_who_x, s_who_y);
            }
        }
        rt = s_rt_active && s_rt_draws_f < 4000;
        if (rt) s_rt_draws_f++;
    }
    s_rt_draw++;
    texdump_maybe();
    if (s_stat_on > 0 && ((ge.vtype >> 9) & 3)) s_stat.skin++;   /* weighted (count field is n-1) */
    /* One line per DISTINCT transform-mode draw signature per stat window: enumerates every
     * unique 3D draw in a scene (type/count/vtype/texture), not just the first few calls. */
    if (s_stat_on > 0 && s_ge3d < 48) {
        uint32_t sig = (uint32_t)type ^ ((uint32_t)count << 3) ^ (ge.vtype << 13) ^ ge.tex_addr ^ (ge.tex_enable ? 0x80000000u : 0);
        int seen = 0;
        for (int si = 0; si < s_ge3d; si++) if (s_ge3d_sigs[si] == sig) { seen = 1; break; }
        if (!seen) {
            s_ge3d_sigs[s_ge3d++] = sig;
            fprintf(stderr, "GE3D f=%u type=%d count=%d vtype=0x%06x tex=%d/0x%08x fmt=%u lit=%d atest=%d/0x%06x blend=%d/%06x world_t=(%.4g,%.4g,%.4g) fog=%d/%g/%g/0x%06x\n",
                    s_ge_frame, type, count, ge.vtype, ge.tex_enable, ge.tex_addr, ge.tex_fmt,
                    ge.lighting_enable, ge.atest_enable, ge.atest, ge.blend_enable, ge.blend_mode,
                    ge.world[9], ge.world[10], ge.world[11],
                    ge.fog_enable, ge.fog_end, ge.fog_slope, ge.fog_color);
            if (ge.lighting_enable)
                fprintf(stderr, "GE3D+ mupd=%d amb=0x%06x/%d mat_a=0x%06x mat_d=0x%06x mat_e=0x%06x texfunc=0x%06x L0=%d/0x%03x ac=0x%06x dc=0x%06x pos=(%.3g,%.3g,%.3g) L1=%d L2=%d L3=%d\n",
                        ge.mat_update, ge.amb_color, ge.amb_alpha, ge.material, ge.mat_diffuse,
                        ge.mat_emissive, ge.tex_func, ge.light_on[0], ge.light_type[0],
                        ge.light_acol[0], ge.light_dcol[0],
                        ge.light_pos[0][0], ge.light_pos[0][1], ge.light_pos[0][2],
                        ge.light_on[1], ge.light_on[2], ge.light_on[3]);
        }
    }
    if (watch_now) {
        static int xn=0;
        if (xn++<s_gewatch_limit)
            fprintf(stderr,"GEWATCH frame=%u xform type=%d count=%d vtype=0x%06x idx=%d vaddr=0x%08x iaddr=0x%08x\n",
                s_ge_frame,type,count,ge.vtype,idxfmt,vbase,ibase);
    }

    /* SR_VDUMP=1: for the first 2 transform-mode draws of each distinct vtype, dump the raw
     * vertex bytes and every decoded attribute plus the post-T&L color, so shading bugs can be
     * separated into decode vs. transform vs. lighting without a debugger attached. */
    {
        static int vdump = -1;
        if (vdump < 0) vdump = getenv("SR_VDUMP") ? 1 : 0;
        if (vdump) {
            static uint32_t seen_vt[16]; static int seen_n[16]; static int seen_cnt = 0;
            /* Re-arm once per 60-frame stat window so later scenes (hangar aircraft, mission
             * terrain) get vertex dumps too, not just the first menu that used the vtype. */
            static uint32_t vd_win = 0;
            if (s_ge_frame / 60 != vd_win) {
                vd_win = s_ge_frame / 60;
                for (int si = 0; si < seen_cnt; si++) seen_n[si] = 0;
            }
            int slot = -1;
            for (int si = 0; si < seen_cnt; si++) if (seen_vt[si] == ge.vtype) { slot = si; break; }
            if (slot < 0 && seen_cnt < 16) { slot = seen_cnt++; seen_vt[slot] = ge.vtype; seen_n[slot] = 0; }
            if (slot >= 0 && seen_n[slot] < 2) {
                seen_n[slot]++;
                fprintf(stderr, "VDUMP f=%u type=%d count=%d vtype=0x%06x stride=%d offs[w=%d tc=%d col=%d nrm=%d pos=%d] lit=%d gouraud=%d fog=%d tmm=0x%03x\n",
                        s_ge_frame, type, count, ge.vtype, vf.stride, vf.w_n ? vf.w_off : -1,
                        vf.tc_off, vf.col_off, vf.nrm_off, vf.pos_off,
                        ge.lighting_enable, ge.shade_gouraud, ge.fog_enable, ge.tex_map_mode);
                if (vf.w_n)
                    fprintf(stderr, "  bone0=[%.3g %.3g %.3g; %.3g %.3g %.3g; %.3g %.3g %.3g; %.3g %.3g %.3g]\n",
                            ge.bone[0], ge.bone[1], ge.bone[2], ge.bone[3], ge.bone[4], ge.bone[5],
                            ge.bone[6], ge.bone[7], ge.bone[8], ge.bone[9], ge.bone[10], ge.bone[11]);
                int nv = count < 6 ? count : 6;
                for (int i = 0; i < nv; i++) {
                    uint32_t a = prim_vtx_addr(vbase, ibase, idxfmt, i, vf.stride);
                    char hex[3*32 + 1]; int hn = vf.stride < 32 ? vf.stride : 32;
                    for (int k = 0; k < hn; k++) sprintf(hex + 3*k, "%02x ", MEM_R8(a + (uint32_t)k));
                    hex[3*hn] = 0;
                    float px, py, pz, nx = 0, ny = 0, nz = 0, u = 0, tv = 0, w0 = 0;
                    int r, g, b, al;
                    read_pos(a, &vf, &px, &py, &pz);
                    read_color(a, &vf, &r, &g, &b, &al);
                    if (vf.nrm_off >= 0) read_nrm(a, &vf, &nx, &ny, &nz);
                    if (vf.tc_off >= 0) read_tc(a, &vf, &u, &tv);
                    if (vf.w_n) { float ws[8]; read_weights(a, &vf, ws); w0 = ws[0]; }
                    CVtx cv2; transform_vtx_clip(a, &vf, &cv2);
                    fprintf(stderr, "  v%d @0x%08x [%s] pos=(%.4g,%.4g,%.4g) uv=(%.3g,%.3g) col=%02x%02x%02x%02x nrm=(%.3g,%.3g,%.3g) w0=%.3g -> cw=%.4g lit=%02x%02x%02x%02x uvT=(%.3g,%.3g) fog=%.3g\n",
                            i, a, hex, px, py, pz, u, tv, r, g, b, al, nx, ny, nz, w0,
                            cv2.cw, cv2.r, cv2.g, cv2.b, cv2.a, cv2.u, cv2.v, cv2.fog);
                }
            }
        }
    }

    SrPerfCategory decode_previous =
        sr_perf_switch(SR_PERF_VERTEX_DECODE);
    const CVtx *const *cached = cache_transform_vertices(
        vbase, ibase, idxfmt, &vf, count);
    sr_perf_switch(decode_previous);
    if (type == 0) {           /* points */
        for (int i = 0; i < count; i++) {
            Vtx a;
            int visible = cached
                ? project_cached_vtx(cached[i], &a)
                : transform_vtx_3d(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i, vf.stride),
                    &vf, &a);
            if (!visible) continue;
            draw_point_vtx(&a, 1);
        }
    } else if (type == 1 || type == 2) {   /* lines / line strip */
        if (s_stat_on > 0) s_stat.line3d += (unsigned long)(count > 0 ? count - 1 : 0);
        for (int i = 1; i < count; i++) {
            if (type == 1 && (i & 1) == 0) continue;
            CVtx ca, cb;
            if (cached) {
                ca = *cached[i - 1];
                cb = *cached[i];
            } else {
                transform_vtx_clip(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i - 1, vf.stride),
                    &vf, &ca);
                transform_vtx_clip(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i, vf.stride),
                    &vf, &cb);
            }
            /* PSP line acceptance (PPSSPP Clipper::ProcessLine): same drop rules as triangles. */
            if (ca.oor || cb.oor) { s_stat.nearclip++; continue; }
            if (ca.cw < 0.0f && cb.cw < 0.0f) { s_stat.nearclip++; continue; }
            if (!ge.depth_clip && (ca.ozp + ca.ozn + cb.ozp + cb.ozn) > 0) { s_stat.nearclip++; continue; }
            if ((ca.ozp + cb.ozp) >= 2 || (ca.ozn + cb.ozn) >= 2) { s_stat.nearclip++; continue; }
            float da = clip_d_nearz(&ca), db2 = clip_d_nearz(&cb);
            if (da < 0.0f && db2 < 0.0f) {
                s_stat.nearclip++;
                CVtx both[2]; both[0] = ca; both[1] = cb;
                near_dump("line", prim_vtx_addr(vbase,ibase,idxfmt,i-1,vf.stride), &vf, both, 2);
                continue;
            }
            if (da < 0.0f)       { CVtx t; clip_lerp_cvtx(&ca,&cb, da/(da-db2), &t); ca=t; }
            else if (db2 < 0.0f) { CVtx t; clip_lerp_cvtx(&cb,&ca, db2/(db2-da), &t); cb=t; }
            if (ca.cw <= NEAR_W || cb.cw <= NEAR_W) { s_stat.nearclip++; continue; }
            /* PPSSPP ProcessLine: clip-created endpoints outside the screen grid kill the line. */
            if (clip_vtx_out_of_range(&ca) || clip_vtx_out_of_range(&cb)) { s_stat.nearclip++; continue; }
            Vtx a, b;
            project_cvtx(&ca,&a); project_cvtx(&cb,&b);
            draw_line_vtx(&a,&b,1);
        }
    } else if (type == 3 || type == 4 || type == 5) {
        /* SR_RTRACE per-draw bookkeeping: per-reason triangle counters plus a snapshot of the
         * pixel-level stat counters so the TRIDRW+ line can report this draw's pixel outcome. */
        enum { D_DRAW, D_OOR, D_WNEG, D_ZOUT, D_ZSAME, D_NEARALL, D_CWLOW, D_CLIP };
        static const char *dec_name[8] =
            { "DRAW", "drop-oor", "drop-wneg", "drop-zout", "drop-zsame", "drop-nearall", "drop-cwlow", "CLIP" };
        unsigned long dcnt[8] = {0,0,0,0,0,0,0,0};
        struct { unsigned long tri3d, cull, zfail, afail, px3d, zrange, deg3d, blk3d; } st0;
        st0.tri3d = s_stat.tri3d; st0.cull = s_stat.cull; st0.zfail = s_stat.zfail;
        st0.afail = s_stat.afail; st0.px3d = s_stat.px3d; st0.zrange = s_stat.zrange;
        st0.deg3d = s_stat.deg3d; st0.blk3d = s_stat.blk3d;
        int tlogged = 0;
        float roffx = (float)(ge.offsetx & 0xFFFF) / 16.0f;
        float roffy = (float)(ge.offsety & 0xFFFF) / 16.0f;
        if (rt) {
            fprintf(stderr, "TRIDRW f=%u d=%u fbp=0x%08x/%u zbp=0x%08x/%u type=%d n=%d vtype=0x%06x idx=%d va=0x%08x ia=0x%08x "
                    "tex=%d/0x%08x %ux%u fmt=%u bufw=%u sw=%u tfunc=0x%06x tmm=0x%03x lit=%d gou=%d "
                    "zt=%d/%d zw=%d zr=[%u,%u] at=%d/0x%06x bl=%d/%06x/%06x/%06x cull=%d/%u clr=0x%03x "
                    "dclip=%u fog=%d vp=[%.4g,%.4g,%.4g|%.4g,%.4g,%.4g] off=(%.4g,%.4g) sc=(%d,%d)-(%d,%d) "
                    "wt=(%.4g,%.4g,%.4g) vt=(%.4g,%.4g,%.4g)\n",
                    s_ge_frame, s_rt_draw, ge.fbp, ge.fbw, ge.zbp, ge.zbw, type, count, ge.vtype, idxfmt, vbase, ibase,
                    ge.tex_enable, ge.tex_addr, ge.tex_w, ge.tex_h, ge.tex_fmt, ge.tex_bufw,
                    ge.tex_swizzle, ge.tex_func, ge.tex_map_mode, ge.lighting_enable, ge.shade_gouraud,
                    ge.ztest_enable, (int)(ge.ztest & 7), !ge.zwrite_disable, ge.minz, ge.maxz,
                    ge.atest_enable, ge.atest, ge.blend_enable, ge.blend_mode, ge.blend_fixa, ge.blend_fixb,
                    ge.cull_enable, ge.cull, ge.clear_mode, ge.depth_clip, ge.fog_enable,
                    decode_float24(ge.viewport_raw[0]), decode_float24(ge.viewport_raw[1]),
                    decode_float24(ge.viewport_raw[2]), decode_float24(ge.viewport_raw[3]),
                    decode_float24(ge.viewport_raw[4]), decode_float24(ge.viewport_raw[5]),
                    roffx, roffy, ge.scis_x1, ge.scis_y1, ge.scis_x2, ge.scis_y2,
                    ge.world[9], ge.world[10], ge.world[11], ge.view[9], ge.view[10], ge.view[11]);
        }
        begin_backend_tri_batch(1);
        for (int i = 2; i < count; i++) {
            int i0, i1, i2;
            if      (type == 3) { if (i%3!=2) continue; i0=i-2; i1=i-1; i2=i; }
            else if (type == 4) { i0=i-2; i1=i-1; i2=i; }
            else                { i0=0;   i1=i-1; i2=i; }
            CVtx local[3], clip_input[3], cl[4];
            const CVtx *cv[3];
            if (cached) {
                cv[0] = cached[i0];
                cv[1] = cached[i1];
                cv[2] = cached[i2];
            } else {
                transform_vtx_clip(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i0, vf.stride),
                    &vf, &local[0]);
                transform_vtx_clip(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i1, vf.stride),
                    &vf, &local[1]);
                transform_vtx_clip(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i2, vf.stride),
                    &vf, &local[2]);
                cv[0] = &local[0];
                cv[1] = &local[1];
                cv[2] = &local[2];
            }

            /* Culling via submission order (PPSSPP TransformUnit::SendTriangle): the
             * rasterizer keeps only positive-winding triangles, so cull mode 0 (CW front)
             * reverses the order, and STRIPS flip it again on every other triangle because
             * strip triangles alternate winding (cullType ^ wind). Without the parity flip
             * every second strip triangle is culled wrongly and back faces leak through —
             * meshes dissolve into scattered triangles. The last vertex (provoking, used
             * for flat shading) stays in place. */
            if (ge.cull_enable && !ge.clear) {
                int flip = ((ge.cull & 1) == 0) ^ ((type == 4) ? (i & 1) : 0);
                if (flip) {
                    const CVtx *t = cv[0];
                    cv[0] = cv[1];
                    cv[1] = t;
                }
            }

            /* PSP triangle acceptance (PPSSPP Clipper::ProcessTriangle). The hardware DROPS
             * whole triangles rather than clipping when vertices leave the screen grid or the
             * z range; games rely on it (effect passes, behind-camera geometry). Drawing them
             * (the old behaviour) scattered runaway triangles across the scene. */
            int dec = D_DRAW;
            if (cv[0]->oor || cv[1]->oor || cv[2]->oor) dec = D_OOR;
            else if (cv[0]->cw < 0.0f && cv[1]->cw < 0.0f && cv[2]->cw < 0.0f) dec = D_WNEG;
            else {
                int zp = cv[0]->ozp + cv[1]->ozp + cv[2]->ozp;
                int zn = cv[0]->ozn + cv[1]->ozn + cv[2]->ozn;
                if (!ge.depth_clip && zp + zn > 0) dec = D_ZOUT;
                else if (zp >= 3 || zn >= 3) dec = D_ZSAME;
            }
            if (dec == D_DRAW) {
                if (clip_d_nearz(cv[0]) >= 0.0f && clip_d_nearz(cv[1]) >= 0.0f && clip_d_nearz(cv[2]) >= 0.0f) {
                    if (!(cv[0]->cw > NEAR_W && cv[1]->cw > NEAR_W && cv[2]->cw > NEAR_W)) dec = D_CWLOW;
                } else if (clip_d_nearz(cv[0]) < 0.0f && clip_d_nearz(cv[1]) < 0.0f && clip_d_nearz(cv[2]) < 0.0f) {
                    dec = D_NEARALL;
                } else {
                    dec = D_CLIP;   /* crosses the z >= -w plane: clip and fan */
                }
            }
            dcnt[dec]++;
            if (dec != D_DRAW) s_stat.nearclip++;

            /* TRIDEC: per-triangle decision log. First 12 per draw, plus EVERY triangle whose
             * screen bbox covers the SR_PIXWHO probe pixel (drawn or dropped) — the probe
             * coverage test treats any cw<=0 vertex as unbounded (could project anywhere). */
            if (rt) {
                int probe = 0;
                if (s_who_x >= 0) {
                    float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
                    int unbounded = 0;
                    for (int k = 0; k < 3; k++) {
                        if (cv[k]->cw <= 0.0f) { unbounded = 1; continue; }
                        float px = cv[k]->sx - roffx, py = cv[k]->sy - roffy;
                        if (px < bx0) bx0 = px;
                        if (px > bx1) bx1 = px;
                        if (py < by0) by0 = py;
                        if (py > by1) by1 = py;
                    }
                    if (unbounded) probe = 1;
                    else probe = (bx0 <= (float)s_who_x + 1 && bx1 >= (float)s_who_x - 1 &&
                                  by0 <= (float)s_who_y + 1 && by1 >= (float)s_who_y - 1);
                }
                if (probe || tlogged < 12) {
                    tlogged++;
                    fprintf(stderr, "TRIDEC f=%u d=%u i=%d %s%s"
                            " v0[c=(%.4g,%.4g,%.4g,%.4g) s=(%.1f,%.1f,%.0f) f=%d%d%d rgba=%d,%d,%d,%d uv=(%.3g,%.3g)]"
                            " v1[c=(%.4g,%.4g,%.4g,%.4g) s=(%.1f,%.1f,%.0f) f=%d%d%d rgba=%d,%d,%d,%d uv=(%.3g,%.3g)]"
                            " v2[c=(%.4g,%.4g,%.4g,%.4g) s=(%.1f,%.1f,%.0f) f=%d%d%d rgba=%d,%d,%d,%d uv=(%.3g,%.3g)]\n",
                            s_ge_frame, s_rt_draw, i, dec_name[dec], probe ? " PROBE" : "",
                            cv[0]->cx, cv[0]->cy, cv[0]->cz, cv[0]->cw, cv[0]->sx - roffx, cv[0]->sy - roffy, cv[0]->sz,
                            cv[0]->oor, cv[0]->ozp, cv[0]->ozn, cv[0]->r, cv[0]->g, cv[0]->b, cv[0]->a, cv[0]->u, cv[0]->v,
                            cv[1]->cx, cv[1]->cy, cv[1]->cz, cv[1]->cw, cv[1]->sx - roffx, cv[1]->sy - roffy, cv[1]->sz,
                            cv[1]->oor, cv[1]->ozp, cv[1]->ozn, cv[1]->r, cv[1]->g, cv[1]->b, cv[1]->a, cv[1]->u, cv[1]->v,
                            cv[2]->cx, cv[2]->cy, cv[2]->cz, cv[2]->cw, cv[2]->sx - roffx, cv[2]->sy - roffy, cv[2]->sz,
                            cv[2]->oor, cv[2]->ozp, cv[2]->ozn, cv[2]->r, cv[2]->g, cv[2]->b, cv[2]->a, cv[2]->u, cv[2]->v);
                }
            }

            if (dec == D_DRAW) {
                Vtx a, b, c;
                project_cvtx(cv[0],&a); project_cvtx(cv[1],&b); project_cvtx(cv[2],&c);
                raster_tri(&a,&b,&c,1);
            } else if (dec == D_CLIP) {
                clip_input[0] = *cv[0];
                clip_input[1] = *cv[1];
                clip_input[2] = *cv[2];
                int m = clip_poly_near(clip_input, 3, cl);
                if (m >= 3) {
                    /* PPSSPP discards each SUB-triangle whose vertices include a clip-created
                     * vertex that re-projects outside the screen grid (clip_vtx_out_of_range);
                     * the rest of the fan still draws. */
                    Vtx p[4];
                    int bad[4];
                    for (int k = 0; k < m && k < 4; k++) {
                        bad[k] = (cl[k].cw <= NEAR_W) || clip_vtx_out_of_range(&cl[k]);
                        if (!bad[k]) project_cvtx(&cl[k], &p[k]);
                    }
                    if (rt && tlogged <= 13) {
                        char buf[640]; int bn = 0;
                        for (int k = 0; k < m && k < 4; k++)
                            bn += snprintf(buf + bn, sizeof(buf) - (size_t)bn,
                                           " p%d[c=(%.4g,%.4g,%.4g,%.4g)->(%.1f,%.1f,%.0f) bad=%d rgba=%d,%d,%d,%d]",
                                           k, cl[k].cx, cl[k].cy, cl[k].cz, cl[k].cw,
                                           bad[k] ? -1.0f : p[k].x, bad[k] ? -1.0f : p[k].y,
                                           bad[k] ? -1.0f : p[k].z, bad[k],
                                           cl[k].r, cl[k].g, cl[k].b, cl[k].a);
                        fprintf(stderr, "TRICLP f=%u d=%u i=%d m=%d%s\n",
                                s_ge_frame, s_rt_draw, i, m, buf);
                    }
                    for (int k = 2; k < m && k < 4; k++)
                        if (!bad[0] && !bad[k-1] && !bad[k])
                            raster_tri(&p[0], &p[k-1], &p[k], 1);
                }
            }
        }
        end_backend_tri_batch();
        if (rt) {
            fprintf(stderr, "TRIDRW+ f=%u d=%u tris=%lu draw=%lu clip=%lu drop[oor=%lu wneg=%lu zout=%lu "
                    "zsame=%lu nearall=%lu cwlow=%lu] rast=%lu cull=%lu deg=%lu px=%lu zfail=%lu afail=%lu "
                    "zrange=%lu blk=%lu\n",
                    s_ge_frame, s_rt_draw,
                    dcnt[0]+dcnt[1]+dcnt[2]+dcnt[3]+dcnt[4]+dcnt[5]+dcnt[6]+dcnt[7],
                    dcnt[D_DRAW], dcnt[D_CLIP], dcnt[D_OOR], dcnt[D_WNEG], dcnt[D_ZOUT],
                    dcnt[D_ZSAME], dcnt[D_NEARALL], dcnt[D_CWLOW],
                    s_stat.tri3d - st0.tri3d, s_stat.cull - st0.cull, s_stat.deg3d - st0.deg3d,
                    s_stat.px3d - st0.px3d, s_stat.zfail - st0.zfail, s_stat.afail - st0.afail,
                    s_stat.zrange - st0.zrange, s_stat.blk3d - st0.blk3d);
        }
    } else if (type == 6) {
        /* Sprites in transform mode: two 3D corner vertices define an axis-aligned screen rect.
         * The Z/depth comes from the second vertex. */
        for (int i = 1; i < count; i += 2) {
            Vtx p0, p1;
            int first_visible = cached
                ? project_cached_vtx(cached[i - 1], &p0)
                : transform_vtx_3d(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i - 1, vf.stride),
                    &vf, &p0);
            int second_visible = cached
                ? project_cached_vtx(cached[i], &p1)
                : transform_vtx_3d(
                    prim_vtx_addr(
                        vbase, ibase, idxfmt, i, vf.stride),
                    &vf, &p1);
            if (!first_visible || !second_visible) continue;
            fill_sprite(&p0,&p1,1);
        }
    }
}

/* Unconditional framebuffer snapshot (no "best frame" gating) for SR_FBSNAP. */
static void ge_snapshot_plain(const char *path) {
    uint32_t fb = ge_fb_addr(), stride = ge.fbw ? ge.fbw : 512;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n480 272\n255\n");
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            int r, g, b, a;
            if ((ge.fbfmt & 3) == 3) unpack_color(MEM_R32(fb + (uint32_t)(y*(int)stride + x)*4), 3, &r, &g, &b, &a);
            else                     unpack_color(MEM_R16(fb + (uint32_t)(y*(int)stride + x)*2), ge.fbfmt & 3, &r, &g, &b, &a);
            unsigned char px[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* Snapshot the current GE target framebuffer to a PPM. */
static void ge_snapshot(const char *path) {
    uint32_t fb=ge_fb_addr(), stride=ge.fbw?ge.fbw:512;
    unsigned char img[272][480][3]; long nb=0;
    for (int y=0; y<272; y++) for (int x=0; x<480; x++) {
        int r,g,b,a;
        if ((ge.fbfmt&3)==3) unpack_color(MEM_R32(fb+(uint32_t)(y*(int)stride+x)*4),3,&r,&g,&b,&a);
        else                 unpack_color(MEM_R16(fb+(uint32_t)(y*(int)stride+x)*2),ge.fbfmt&3,&r,&g,&b,&a);
        img[y][x][0]=(unsigned char)r; img[y][x][1]=(unsigned char)g; img[y][x][2]=(unsigned char)b;
        if (r|g|b) nb++;
    }
    static long best=-1; if (nb<=best) return; best=nb;
    FILE *f=fopen(path,"wb"); if(!f) return;
    fprintf(f,"P6\n480 272\n255\n"); fwrite(img,1,sizeof(img),f); fclose(f);
}

static const char *ge_cmd_name(uint32_t cmd) {
    switch (cmd) {
        case 0x05: return "BEZIER";           case 0x06: return "SPLINE";
        case 0x07: return "BOUNDINGBOX";
        case 0x2A: return "BONEMATRIXNUMBER"; case 0x2B: return "BONEMATRIXDATA";
        case 0x2C: return "MORPHWEIGHT0";      case 0x2D: return "MORPHWEIGHT1";
        case 0x2E: return "MORPHWEIGHT2";      case 0x2F: return "MORPHWEIGHT3";
        case 0x30: return "MORPHWEIGHT4";      case 0x31: return "MORPHWEIGHT5";
        case 0x32: return "MORPHWEIGHT6";      case 0x33: return "MORPHWEIGHT7";
        case 0x40: return "TGENMATRIXNUMBER"; case 0x41: return "TGENMATRIXDATA";
        case 0xB2: return "TRANSFERSRC";      case 0xB3: return "TRANSFERSRCW";
        case 0xB4: return "TRANSFERDST";      case 0xB5: return "TRANSFERDSTW";
        case 0xD8: return "COLORTEST";        case 0xD9: return "COLORREF";
        case 0xDA: return "COLORTESTMASK";    case 0xE6: return "LOGICOP";
        case 0xEA: return "TRANSFERSTART";    case 0xEB: return "TRANSFERSRCPOS";
        case 0xEC: return "TRANSFERDSTPOS";   case 0xEE: return "TRANSFERSIZE";
        case 0xF0: return "VSCX";             case 0xF1: return "VSCY";
        case 0xF2: return "VSCZ";             case 0xF3: return "VTCS";
        case 0xF4: return "VTCT";             case 0xF5: return "VTCQ";
        case 0xF6: return "VCV";              case 0xF7: return "VAP";
        case 0xF8: return "VFC";              case 0xF9: return "VSCV";
        default: return NULL;
    }
}

unsigned long g_ge_unhandled_mask[8];
static void ge_note_unhandled_cmd(uint32_t cmd, uint32_t op) {
    static int on=-1;
    if (on<0) { const char *e=getenv("SR_GEUNK"); on=(e&&e[0]=='0')?0:1; }
    cmd &= 0xFF;
    unsigned long bit = 1ul << (cmd & 31);
    if (g_ge_unhandled_mask[cmd>>5] & bit) return;
    g_ge_unhandled_mask[cmd>>5] |= bit;
    if (on) {
        const char *nm=ge_cmd_name(cmd);
        fprintf(stderr,"GE: unhandled command 0x%02x (%s) data=0x%06x [reported once]\n",
                cmd, nm?nm:"unknown", op&0xFFFFFF);
    }
}

unsigned long g_ge_list_sig=0, g_ge_prim_count=0;
/* Per-list write accounting for SR_GEWATCH: shows whether a list that cleared a buffer
 * also drew non-black content into it, and which buffer it targeted. */
unsigned long g_list_writes=0, g_list_nonblack=0, g_list_clearpx=0;
/* Display-list control flow (stall, JUMP/CALL/RET, SIGNAL/FINISH/END, queueing) lives in
 * gelist.c, a port of PPSSPP's GPUCommon list manager. It calls ge_list_begin when a list
 * starts, ge_execute_op for every other command, and ge_list_end when the list finishes. */
static uint32_t s_list_addr;
static unsigned long s_list_sig, s_list_prims, s_list_start_nz, s_list_t0;
static int s_list_snap = -1;

/* GE block transfer (the "memcpy engine"): rectangle copy between guest buffers, used for
 * framebuffer effects (pause capture, blur, scene transitions). Decode masks follow PPSSPP
 * GPUState.h. With a GPU backend the source may live in a GPU image — flush("xfersrc")
 * reads overlapping targets back to guest VRAM first — and the destination may be
 * GPU-cached, so the written range is dirty-notified afterwards. */
static void ge_block_transfer(uint32_t startdata) {
    uint32_t srcBase = (ge.xf_src & 0xFFFFF0u) | ((ge.xf_srcw & 0xFF0000u) << 8);
    uint32_t dstBase = (ge.xf_dst & 0xFFFFF0u) | ((ge.xf_dstw & 0xFF0000u) << 8);
    uint32_t srcStride = ge.xf_srcw & 0x7F8u; if (srcStride > 0x400) srcStride = 0;
    uint32_t dstStride = ge.xf_dstw & 0x7F8u; if (dstStride > 0x400) dstStride = 0;
    uint32_t srcX = ge.xf_spos & 0x3FFu, srcY = (ge.xf_spos >> 10) & 0x3FFu;
    uint32_t dstX = ge.xf_dpos & 0x3FFu, dstY = (ge.xf_dpos >> 10) & 0x3FFu;
    uint32_t w = (ge.xf_size & 0x3FFu) + 1, h = ((ge.xf_size >> 10) & 0x3FFu) + 1;
    uint32_t bpp = (startdata & 1) ? 4 : 2;
    if (!srcStride || !dstStride) return;
    if (s_backend && s_backend->xfer) {
        SrPerfCategory previous = sr_perf_switch(SR_PERF_SYNC);
        int transferred = s_backend->xfer(startdata);
        sr_perf_switch(previous);
        if (transferred) return;   /* GPU-side blit */
    }
    if (s_backend && s_backend->flush) {
        SrPerfCategory previous = sr_perf_switch(SR_PERF_SYNC);
        s_backend->flush("xfersrc");
        sr_perf_switch(previous);
    }
    sr_gpu_vram_prepare_write(
        dstBase + (dstY * dstStride + dstX) * bpp,
        ((h - 1) * dstStride + w) * bpp);
    for (uint32_t y = 0; y < h; y++) {
        uint32_t so = srcBase + ((srcY + y) * srcStride + srcX) * bpp;
        uint32_t dofs = dstBase + ((dstY + y) * dstStride + dstX) * bpp;
        memmove(SR_HOST(dofs), SR_HOST(so), (size_t)w * bpp);
    }
    sr_gpu_vram_dirty(dstBase + (dstY * dstStride + dstX) * bpp,
                      ((h - 1) * dstStride + w) * bpp);
}

uint32_t ge_rel(uint32_t data) { return ge_rel_addr(data); }

void ge_list_begin(uint32_t list_addr) {
    if (!s_ge_inited) ge_state_init();
    if (s_list_snap < 0) s_list_snap = (getenv("SR_FBDUMP") || getenv("SR_GEDUMP")) ? 1 : 0;
    extern unsigned long g_tex_nonzero;
    s_list_addr = list_addr;
    g_list_writes = 0; g_list_nonblack = 0; g_list_clearpx = 0;
    s_list_start_nz = g_tex_nonzero;
    s_list_sig = 0; s_list_prims = 0;
    s_list_t0 = wall_ms();
}

void ge_list_end(void) {
    extern unsigned long g_tex_nonzero;
    if (s_backend && s_backend->flush) {
        SrPerfCategory previous = sr_perf_switch(SR_PERF_SYNC);
        s_backend->flush("listend");
        sr_perf_switch(previous);
    }
    if (s_list_snap > 0 && g_tex_nonzero > s_list_start_nz) ge_snapshot("fb_content.ppm");
    g_ge_list_sig = s_list_sig; g_ge_prim_count = s_list_prims;
    if (s_gewatch > 0 && s_ge_frame >= (uint32_t)s_gewatch_after)
        fprintf(stderr, "GELIST f=%u list=0x%08x fbp=0x%08x prims=%lu writes=%lu nonblack=%lu clearpx=%lu\n",
                s_ge_frame, s_list_addr, ge_fb_addr(), s_list_prims,
                g_list_writes, g_list_nonblack, g_list_clearpx);
    s_ge_ms_acc += wall_ms() - s_list_t0;
}

/* Executes one non-flow GE command. pc is the command's own address (ORIGIN reads it). */
void ge_execute_op(uint32_t op, uint32_t pc) {
    if (!s_ge_inited) ge_state_init();
    uint32_t cmd=op>>24, data=op&0xFFFFFF;
    s_list_sig=s_list_sig*1000003ul+op;
    {
        switch (cmd) {
            case GE_NOP: case 0xFF: break;   /* 0xFF = GE_CMD_NOP_FF (padding, PPSSPP) */
            case GE_VADDR:    ge.vaddr=ge_rel_addr(data); break;
            case GE_IADDR:    ge.iaddr=ge_rel_addr(data); break;
            case GE_BASE:     ge.base=data; break;
            case GE_OFFSETADDR: ge.offset=data<<8; break;
            case GE_ORIGIN:   ge.offset=pc; break;
            case GE_VERTEXTYPE: ge.vtype=data; break;

            case 0xF0: case 0xF1: case 0xF2: case 0xF3: case 0xF4:
            case 0xF5: case 0xF6: case 0xF8: case 0xF9:
                ge.imm[cmd - 0xF0] = data;
                break;
            case 0xF7:
                ge.imm[7] = data;
                if (data != 0)
                    ge_note_unhandled_cmd(cmd, op);
                break;

            case GE_FRAMEBUFPTR:   ge.fbp=(ge.fbp&0xFF000000)|data;
                if (s_gelog>0) fprintf(stderr,"GE FRAMEBUFPTR data=0x%06x -> fbp=0x%08x\n",data,ge.fbp);
                break;
            case GE_FRAMEBUFWIDTH: ge.fbw=data&0xFFFF; ge.fbp=(ge.fbp&0x00FFFFFF)|((data&0xFF0000)<<8);
                if (s_gelog>0) fprintf(stderr,"GE FRAMEBUFWIDTH data=0x%06x -> fbw=%u fbp=0x%08x\n",data,ge.fbw,ge.fbp);
                break;
            case GE_FRAMEBUFPIXFORMAT: ge.fbfmt=data&3; break;

            case GE_ZBUFPTR:   ge.zbp=(ge.zbp&0xFF000000)|data; break;
            case GE_ZBUFWIDTH: ge.zbw=data&0xFFFF; ge.zbp=(ge.zbp&0x00FFFFFF)|((data&0xFF0000)<<8); break;

            /* Drawing region: the scissor (which the game also sets) bounds all writes. */
            case GE_REGION1: case GE_REGION2: break;

            case GE_SCISSOR1: ge.scis_x1=data&0x3FF; ge.scis_y1=(data>>10)&0x3FF; break;
            case GE_SCISSOR2: ge.scis_x2=data&0x3FF; ge.scis_y2=(data>>10)&0x3FF; break;

            /* ---- Depth / cull state ---- */
            case GE_ZTESTENABLE:   ge.ztest_enable  = data & 1; break;
            case GE_ZWRITEDISABLE: ge.zwrite_disable= data & 1; break;
            case GE_ZTEST:         ge.ztest         = data & 7; break;
            case GE_MINZ:          ge.minz          = data & 0xFFFF; break;
            case GE_MAXZ:          ge.maxz          = data & 0xFFFF; break;
            case GE_CULLFACEENABLE:ge.cull_enable   = data & 1; break;
            case GE_CULL:          ge.cull          = data & 1; break;
            case GE_CLIPENABLE:    ge.depth_clip = data & 1; break;

            /* ---- Fragment test / blend toggles ---- */
            case GE_ALPHATESTENABLE: ge.atest_enable = data & 1; break;
            case GE_ALPHATEST:       ge.atest        = data; break;
            case GE_COLORTESTENABLE: ge.color_test_enable = data & 1; break;
            case GE_COLORTEST:       ge.color_test = data; break;
            case GE_COLORREF:        ge.color_ref = data; break;
            case GE_COLORTESTMASK:   ge.color_test_mask = data; break;
            case GE_STENCILTESTENABLE: ge.stencil_test_enable = data & 1; break;
            case GE_STENCILTEST:     ge.stencil_test = data; break;
            case GE_STENCILOP:       ge.stencil_op = data; break;
            case GE_ALPHABLENDENABLE: ge.blend_enable=data&1; break;
            case GE_FOGENABLE:  ge.fog_enable = data & 1; break;
            case GE_FOG1:       ge.fog_end   = decode_float24(data); break;
            case GE_FOG2:       ge.fog_slope = decode_float24(data); break;
            case GE_FOGCOLOR:   ge.fog_color = data; break;
            case GE_DITHERENABLE: ge.dither_enable = data & 1; break;
            case GE_DITH0: case GE_DITH1: case GE_DITH2: case GE_DITH3: {
                /* One row of the 4x4 dither matrix: 4 signed 4-bit values. */
                int row = (int)(cmd - GE_DITH0);
                for (int i = 0; i < 4; i++) {
                    int v = (int)((data >> (i*4)) & 0xF);
                    ge.dith[row][i] = (int8_t)(v >= 8 ? v - 16 : v);
                }
                break;
            }
            case GE_MASKRGB:   ge.mask_rgb   = data; break;
            case GE_MASKALPHA: ge.mask_alpha = (int)(data & 0xFF); break;
            case GE_ANTIALIASENABLE: case GE_PATCHCULLENABLE: break;
            case GE_LOGICOPENABLE: ge.logic_op_enable = data & 1; break;
            case GE_MORPHWEIGHT0: case GE_MORPHWEIGHT0 + 1:
            case GE_MORPHWEIGHT0 + 2: case GE_MORPHWEIGHT0 + 3:
            case GE_MORPHWEIGHT0 + 4: case GE_MORPHWEIGHT0 + 5:
            case GE_MORPHWEIGHT0 + 6: case GE_MORPHWEIGHT7:
                ge.morph_weight[cmd - GE_MORPHWEIGHT0] =
                    decode_float24(data);
                break;
            case GE_PATCHDIVISION: ge.patch_division = data; break;
            case GE_PATCHPRIMITIVE: ge.patch_primitive = data & 3u; break;
            case GE_PATCHFACING: ge.patch_facing = data & 1u; break;

            /* ---- Matrix capture ----
             * *MATRIXNUMBER sets the write cursor; *MATRIXDATA writes one float24 and advances.
             * World/View are 4×3 (12 elements), Proj is 4×4 (16 elements). */
            /* Bone matrices: 8 4x3 matrices in one 96-float array; NUMBER takes data&0x7F
             * (a raw cursor, can point past the end), DATA writes land only while < 96
             * (PPSSPP SoftGpu.cpp). The hangar aircraft is drawn part-by-part with one
             * rigid bone per part and single-weight vertices. */
            case 0x2A: ge.bone_num = (int)(data & 0x7F); break;    /* BONEMATRIXNUMBER */
            case 0x2B:                                             /* BONEMATRIXDATA */
                if (s_stat_on > 0) s_stat.bonew++;
                if (ge.bone_num < 96) ge.bone[ge.bone_num] = decode_float24(data);
                ge.bone_num++;
                break;
            /* Hardware semantics (PPSSPP SoftGpu.cpp): NUMBER takes the low 4 bits; DATA writes
             * advance the cursor unconditionally but only land while it is in range -- excess
             * words are DISCARDED, never wrapped. (The old wrap-to-0 let a 13th data word clobber
             * element 0: the hangar/briefing engine path uploads 13-word matrices and every one
             * of its world matrices was corrupted, throwing whole meshes behind the camera.) */
            case GE_WORLDMATRIXNUMBER: ge.world_num = (int)(data & 0xF); break;
            case GE_WORLDMATRIXDATA:
                s_stat.mw_world++;
                if (ge.world_num < 12) ge.world[ge.world_num] = decode_float24(data);
                ge.world_num++;
                break;
            case GE_VIEWMATRIXNUMBER: ge.view_num = (int)(data & 0xF); break;
            case GE_VIEWMATRIXDATA:
                s_stat.mw_view++;
                trace_mtx_write("view", ge.view_num, data);
                if (ge.view_num < 12) ge.view[ge.view_num] = decode_float24(data);
                ge.view_num++;
                break;
            case GE_PROJMATRIXNUMBER: ge.proj_num = (int)(data & 0xF); break;
            case GE_PROJMATRIXDATA:
                s_stat.mw_proj++;
                trace_mtx_write("proj", ge.proj_num, data);
                if (ge.proj_num < 16) ge.proj[ge.proj_num] = decode_float24(data);
                ge.proj_num++;
                break;

            /* ---- Viewport / offset ---- */
            case GE_VIEWPORTXSCALE:  ge.viewport_raw[0]=data; break;
            case GE_VIEWPORTYSCALE:  ge.viewport_raw[1]=data; break;
            case GE_VIEWPORTZSCALE:  ge.viewport_raw[2]=data; break;
            case GE_VIEWPORTXCENTER: ge.viewport_raw[3]=data; break;
            case GE_VIEWPORTYCENTER: ge.viewport_raw[4]=data; break;
            case GE_VIEWPORTZCENTER: ge.viewport_raw[5]=data; break;
            case GE_OFFSETX:         ge.offsetx=data; break;
            case GE_OFFSETY:         ge.offsety=data; break;

            /* ---- Shading / material / lighting ---- */
            case GE_SHADEMODE:       ge.shade_gouraud = data & 1; break;
            case GE_REVERSENORMAL:   ge.reverse_normal = data & 1; break;
            case GE_MATERIALUPDATE:  ge.mat_update = data & 7; break;
            case GE_MATERIALEMISSIVE:ge.mat_emissive = data; break;
            case GE_MATERIALAMBIENT: ge.material=data; ge.material_set=1; break;
            case GE_MATERIALDIFFUSE: ge.mat_diffuse=data;
                /* Flat-colour fallback for colourless vertices also tracks diffuse. */
                ge.material=data; ge.material_set=1; break;
            case GE_MATERIALSPECULAR:ge.mat_specular = data; break;
            case GE_MATERIALALPHA:   ge.material_alpha=data&0xFF; ge.material_alpha_set=1; break;
            case GE_MATERIALSPECULARCOEF: ge.spec_coef = decode_float24(data); break;
            case GE_AMBIENTCOLOR:    ge.amb_color = data; break;
            case GE_AMBIENTALPHA:    ge.amb_alpha = (int)(data & 0xFF); break;
            case GE_LIGHTMODE:       ge.light_mode = data & 1; break;
            case GE_LIGHTINGENABLE:  ge.lighting_enable = data & 1; break;
            case GE_LIGHTENABLE0+0: case GE_LIGHTENABLE0+1:
            case GE_LIGHTENABLE0+2: case GE_LIGHTENABLE0+3:
                ge.light_on[cmd-GE_LIGHTENABLE0] = data & 1; break;
            case GE_LIGHTTYPE0+0: case GE_LIGHTTYPE0+1:
            case GE_LIGHTTYPE0+2: case GE_LIGHTTYPE0+3:
                ge.light_type[cmd-GE_LIGHTTYPE0] = data; break;
            case GE_LX0+0:  case GE_LX0+1:  case GE_LX0+2:  case GE_LX0+3:
            case GE_LX0+4:  case GE_LX0+5:  case GE_LX0+6:  case GE_LX0+7:
            case GE_LX0+8:  case GE_LX0+9:  case GE_LX0+10: case GE_LX0+11: {
                int k=(int)(cmd-GE_LX0); ge.light_pos[k/3][k%3]=decode_float24(data); break;
            }
            case GE_LDX0+0: case GE_LDX0+1: case GE_LDX0+2: case GE_LDX0+3:
            case GE_LDX0+4: case GE_LDX0+5: case GE_LDX0+6: case GE_LDX0+7:
            case GE_LDX0+8: case GE_LDX0+9: case GE_LDX0+10:case GE_LDX0+11: {
                int k=(int)(cmd-GE_LDX0); ge.light_dir[k/3][k%3]=decode_float24(data); break;
            }
            case GE_LKA0+0: case GE_LKA0+1: case GE_LKA0+2: case GE_LKA0+3:
            case GE_LKA0+4: case GE_LKA0+5: case GE_LKA0+6: case GE_LKA0+7:
            case GE_LKA0+8: case GE_LKA0+9: case GE_LKA0+10:case GE_LKA0+11: {
                int k=(int)(cmd-GE_LKA0); ge.light_att[k/3][k%3]=decode_float24(data); break;
            }
            case GE_LKS0+0: case GE_LKS0+1: case GE_LKS0+2: case GE_LKS0+3:
                ge.light_spot_exp[cmd-GE_LKS0]=decode_float24(data); break;
            case GE_LKO0+0: case GE_LKO0+1: case GE_LKO0+2: case GE_LKO0+3:
                ge.light_spot_cut[cmd-GE_LKO0]=decode_float24(data); break;
            case GE_LAC0+0: case GE_LAC0+1: case GE_LAC0+2: case GE_LAC0+3:
            case GE_LAC0+4: case GE_LAC0+5: case GE_LAC0+6: case GE_LAC0+7:
            case GE_LAC0+8: case GE_LAC0+9: case GE_LAC0+10:case GE_LAC0+11: {
                int k=(int)(cmd-GE_LAC0), li=k/3;
                if      (k%3==0) ge.light_acol[li]=data;
                else if (k%3==1) ge.light_dcol[li]=data;
                else             ge.light_scol[li]=data;
                break;
            }

            /* ---- Texturing ---- */
            case GE_TEXTUREMAPENABLE: ge.tex_enable=data&1; break;
            case GE_TEXADDR0: case GE_TEXADDR0+1: case GE_TEXADDR0+2:
            case GE_TEXADDR0+3: case GE_TEXADDR0+4: case GE_TEXADDR0+5:
            case GE_TEXADDR0+6: case GE_TEXADDR0+7: {
                int level = (int)(cmd - GE_TEXADDR0);
                ge.tex_addr_levels[level] =
                    (ge.tex_addr_levels[level] & 0xFF000000u) |
                    (data & 0x00FFFFFFu);
                if (level == 0) ge.tex_addr = ge.tex_addr_levels[0];
                break;
            }
            case GE_TEXBUFWIDTH0: case GE_TEXBUFWIDTH0+1:
            case GE_TEXBUFWIDTH0+2: case GE_TEXBUFWIDTH0+3:
            case GE_TEXBUFWIDTH0+4: case GE_TEXBUFWIDTH0+5:
            case GE_TEXBUFWIDTH0+6: case GE_TEXBUFWIDTH0+7: {
                int level = (int)(cmd - GE_TEXBUFWIDTH0);
                ge.tex_bufw_levels[level] = data & 0xFFFFu;
                ge.tex_addr_levels[level] =
                    (ge.tex_addr_levels[level] & 0x00FFFFFFu) |
                    ((data & 0xFF0000u) << 8);
                if (level == 0) {
                    ge.tex_bufw = ge.tex_bufw_levels[0];
                    ge.tex_addr = ge.tex_addr_levels[0];
                }
                break;
            }
            case GE_TEXSIZE0: case GE_TEXSIZE0+1: case GE_TEXSIZE0+2:
            case GE_TEXSIZE0+3: case GE_TEXSIZE0+4: case GE_TEXSIZE0+5:
            case GE_TEXSIZE0+6: case GE_TEXSIZE0+7: {
                int level = (int)(cmd - GE_TEXSIZE0);
                ge.tex_width_levels[level] = 1 << (data & 0xF);
                ge.tex_height_levels[level] = 1 << ((data >> 8) & 0xF);
                if (level == 0) {
                    ge.tex_w = ge.tex_width_levels[0];
                    ge.tex_h = ge.tex_height_levels[0];
                }
                break;
            }
            case GE_TEXMODE:
                ge.tex_mode = data;
                ge.tex_swizzle = data & 1;
                ge.tex_max_level = (int)((data >> 16) & 7);
                break;
            case GE_TEXFORMAT: ge.tex_fmt=data&0xF; break;
            case GE_TEXFILTER: ge.tex_filter=data; break;
            case GE_TEXWRAP:   ge.tex_wrap=data; break;
            case GE_TEXFUNC:   ge.tex_func=data; break;
            case GE_TEXENVCOLOR: ge.tex_env=data; break;
            case GE_TEXSCALEU:  ge.tex_scale_u=decode_float24(data); break;
            case GE_TEXSCALEV:  ge.tex_scale_v=decode_float24(data); break;
            case GE_TEXOFFSETU: ge.tex_off_u=decode_float24(data); break;
            case GE_TEXOFFSETV: ge.tex_off_v=decode_float24(data); break;
            case GE_TEXMAPMODE: ge.tex_map_mode=data; break;
            case GE_TEXSHADELS: ge.tex_shade_ls=data; break;
            case GE_TGENMATRIXNUMBER: ge.tgen_num = (int)(data & 0xF); break;
            case GE_TGENMATRIXDATA:
                if (ge.tgen_num < 12) ge.tgen[ge.tgen_num] = decode_float24(data);
                ge.tgen_num++;
                break;
            case GE_TEXLEVEL: ge.tex_level=data; break;
            case GE_TEXLODSLOPE: break;
            case GE_TEXFLUSH:
                if (s_backend && s_backend->flush) {
                    SrPerfCategory previous =
                        sr_perf_switch(SR_PERF_SYNC);
                    s_backend->flush("texflush");
                    sr_perf_switch(previous);
                }
                break;
            case GE_TEXSYNC: break;
            case GE_LOADCLUT: {
                /* Copy (data&0x3F) 32-byte blocks from clut_addr into internal CLUT RAM, like
                 * the hardware (PPSSPP TextureCacheCommon::LoadClut). */
                /* CLUT sourced from VRAM may read pixels the GPU backend hasn't written back. */
                if (s_backend && s_backend->flush &&
                    (ge.clut_addr & 0x0F000000u) == 0x04000000u) {
                    SrPerfCategory previous =
                        sr_perf_switch(SR_PERF_SYNC);
                    s_backend->flush("loadclut");
                    sr_perf_switch(previous);
                }
                uint32_t bytes = (data & 0x3F) * 32;
                if (bytes > sizeof(ge.clutram)) bytes = sizeof(ge.clutram);
                for (uint32_t i = 0; i < bytes; i += 4) {
                    uint32_t w = MEM_R32(ge.clut_addr + i);
                    memcpy(&ge.clutram[i], &w, 4);
                }
                ge.clut_gen++;
                break;
            }
            case GE_CLUTADDR:      ge.clut_addr=(ge.clut_addr&0xFF000000)|(data&0x00FFFFFF); break;
            case GE_CLUTADDRUPPER: ge.clut_addr=(ge.clut_addr&0x00FFFFFF)|((data&0xFF0000)<<8); break;
            case GE_CLUTFORMAT: ge.clut_fmt=data; break;

            case GE_BLENDMODE:  ge.blend_mode=data; break;
            case GE_BLENDFIXEDA: ge.blend_fixa=data; break;
            case GE_BLENDFIXEDB: ge.blend_fixb=data; break;
            case GE_LOGICOP: ge.logic_op=data; break;
            case GE_CLEARMODE:  ge.clear_mode=data; ge.clear=data&1; break;

            /* ---- Block transfer ---- */
            case GE_TRANSFERSRC:    ge.xf_src  = data; break;
            case GE_TRANSFERSRCW:   ge.xf_srcw = data; break;
            case GE_TRANSFERDST:    ge.xf_dst  = data; break;
            case GE_TRANSFERDSTW:   ge.xf_dstw = data; break;
            case GE_TRANSFERSRCPOS: ge.xf_spos = data; break;
            case GE_TRANSFERDSTPOS: ge.xf_dpos = data; break;
            case GE_TRANSFERSIZE:   ge.xf_size = data; break;
            case GE_TRANSFERSTART:  ge_block_transfer(data); break;

            case GE_PRIM: {
                SrPerfCategory previous =
                    sr_perf_switch(SR_PERF_PRIMITIVE);
                sr_perf_counter_add(SR_PERF_GE_DRAWS, 1);
                sr_perf_counter_add(
                    SR_PERF_GE_VERTICES, op & 0xffffu);
                s_list_prims++;
                ge_debug_begin_draw(pc, op);
                draw_prim(op);
                sr_perf_switch(previous);
                break;
            }
            case GE_BEZIER: {
                SrPerfCategory previous =
                    sr_perf_switch(SR_PERF_BEZIER);
                sr_perf_counter_add(SR_PERF_GE_DRAWS, 1);
                sr_perf_counter_add(
                    SR_PERF_GE_VERTICES,
                    (op & 0xffu) * ((op >> 8) & 0xffu));
                s_list_prims++;
                ge_debug_begin_draw(pc, op);
                draw_bezier(op);
                sr_perf_switch(previous);
                break;
            }
            case GE_SPLINE: {
                SrPerfCategory previous =
                    sr_perf_switch(SR_PERF_SPLINE);
                sr_perf_counter_add(SR_PERF_GE_DRAWS, 1);
                sr_perf_counter_add(
                    SR_PERF_GE_VERTICES,
                    (op & 0xffu) * ((op >> 8) & 0xffu));
                s_list_prims++;
                ge_debug_begin_draw(pc, op);
                draw_spline(op);
                sr_perf_switch(previous);
                break;
            }
            /* The bounding-box test result is kept by gelist.c (BJUMP); nothing to draw. */
            case GE_BOUNDINGBOX: break;
            /* SIGNAL and FINISH only take effect at the END that follows (gelist.c). */
            case GE_SIGNAL: case GE_FINISH: break;
            default:
                ge_note_unhandled_cmd(cmd, op);
                break;
        }
    }
}

uint32_t ge_framebuffer(void) { return ge_fb_addr(); }
