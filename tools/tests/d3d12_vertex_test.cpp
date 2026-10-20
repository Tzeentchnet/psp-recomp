#include "../../src/rt/gpu_d3d12/device.h"
#include "../../src/rt/gpu_d3d12/framebuffer.h"
#include "../../src/rt/gpu_d3d12/present.h"
#include "../../src/rt/gpu_d3d12/vertex.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_UPLOAD_SIZE (8u * 1024u * 1024u)
#define TEST_GPU_BASE UINT64_C(0x10000000)

static GeState s_state;
static uint8_t s_upload[TEST_UPLOAD_SIZE];
static UINT64 s_upload_cursor;
static int s_failures;

extern "C" GeState *ge_state_ptr(void) {
    return &s_state;
}

bool ged3d12_texture_capture(GeD3D12TextureBinding *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->handle = GE_D3D12_TEXTURE_HANDLE_NONE;
    out->framebuffer.handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    ged3d12_texture_sampler_build(&s_state, &out->sampler);
    return true;
}

bool ged3d12_framebuffer_capture(GeD3D12FramebufferBinding *out) {
    if (!out) return false;
    out->color_handle = 1;
    out->depth_handle = 2;
    return true;
}

bool ged3d12_upload_alloc(
    UINT64 size, UINT64 alignment, GeD3D12UploadSlice *out) {
    UINT64 offset = (s_upload_cursor + alignment - 1) & ~(alignment - 1);
    if (!out || !size || !alignment || offset > TEST_UPLOAD_SIZE ||
        size > TEST_UPLOAD_SIZE - offset) {
        return false;
    }
    s_upload_cursor = offset + size;
    out->cpu = s_upload + offset;
    out->gpu = TEST_GPU_BASE + offset;
    out->offset = offset;
    out->size = size;
    return true;
}

ID3D12GraphicsCommandList *ged3d12_command_list(void) {
    return nullptr;
}

static void reset_upload(void) {
    memset(s_upload, 0xcd, sizeof(s_upload));
    s_upload_cursor = 3;
}

static void fail(const char *message) {
    fprintf(stderr, "FAIL: %s\n", message);
    s_failures++;
}

static void check_u32(const char *what, uint32_t got, uint32_t expected) {
    if (got != expected) {
        fprintf(stderr, "FAIL: %s: got 0x%08x, expected 0x%08x\n",
                what, got, expected);
        s_failures++;
    }
}

static void check_u64(const char *what, uint64_t got, uint64_t expected) {
    if (got != expected) {
        fprintf(stderr, "FAIL: %s: got 0x%016llx, expected 0x%016llx\n",
                what, (unsigned long long)got, (unsigned long long)expected);
        s_failures++;
    }
}

static void check_float(const char *what, float got, float expected) {
    if (fabsf(got - expected) > 1e-6f) {
        fprintf(stderr, "FAIL: %s: got %.9g, expected %.9g\n",
                what, got, expected);
        s_failures++;
    }
}

static const GeD3D12Vertex *uploaded_vertices(const GeD3D12VertexFrame *frame) {
    UINT64 offset = frame->buffer.BufferLocation - TEST_GPU_BASE;
    return reinterpret_cast<const GeD3D12Vertex *>(s_upload + offset);
}

static GeVtx vertex(
    float x, float y, float z, float rw, float u, float v, float fog,
    int r, int g, int b, int a) {
    GeVtx out = {x, y, z, rw, u, v, fog, r, g, b, a};
    return out;
}

static void test_present_viewports(void) {
    GeD3D12PresentViewport layout;
    ged3d12_present_viewport(0, 0, &layout);
    check_float("zero viewport width", layout.viewport.Width, 0.0f);
    check_float("zero viewport height", layout.viewport.Height, 0.0f);
    check_u32("zero scissor right", (uint32_t)layout.scissor.right, 0);
    check_u32("zero scissor bottom", (uint32_t)layout.scissor.bottom, 0);

    ged3d12_present_viewport(960, 544, &layout);
    check_float("2x viewport x", layout.viewport.TopLeftX, 0.0f);
    check_float("2x viewport y", layout.viewport.TopLeftY, 0.0f);
    check_float("2x viewport width", layout.viewport.Width, 960.0f);
    check_float("2x viewport height", layout.viewport.Height, 544.0f);
    check_u32("2x scissor right", (uint32_t)layout.scissor.right, 960);
    check_u32("2x scissor bottom", (uint32_t)layout.scissor.bottom, 544);

    ged3d12_present_viewport(1000, 600, &layout);
    float letterbox_height = 1000.0f * (272.0f / 480.0f);
    float letterbox_y = (600.0f - letterbox_height) * 0.5f;
    check_float("letterbox x", layout.viewport.TopLeftX, 0.0f);
    check_float("letterbox y", layout.viewport.TopLeftY, letterbox_y);
    check_float("letterbox width", layout.viewport.Width, 1000.0f);
    check_float("letterbox height", layout.viewport.Height, letterbox_height);
    check_u32("letterbox scissor top", (uint32_t)layout.scissor.top, 16);
    check_u32("letterbox scissor bottom", (uint32_t)layout.scissor.bottom, 584);

    ged3d12_present_viewport(600, 1000, &layout);
    check_float("portrait x", layout.viewport.TopLeftX, 0.0f);
    check_float("portrait y", layout.viewport.TopLeftY, 330.0f);
    check_float("portrait width", layout.viewport.Width, 600.0f);
    check_float("portrait height", layout.viewport.Height, 340.0f);
    check_u32("portrait scissor top", (uint32_t)layout.scissor.top, 330);
    check_u32("portrait scissor bottom", (uint32_t)layout.scissor.bottom, 670);

    ged3d12_present_viewport(1920, 1080, &layout);
    float pillarbox_width = 1080.0f * (480.0f / 272.0f);
    float pillarbox_x = (1920.0f - pillarbox_width) * 0.5f;
    check_float("pillarbox x", layout.viewport.TopLeftX, pillarbox_x);
    check_float("pillarbox y", layout.viewport.TopLeftY, 0.0f);
    check_float("pillarbox width", layout.viewport.Width, pillarbox_width);
    check_float("pillarbox height", layout.viewport.Height, 1080.0f);
    check_u32("pillarbox scissor left", (uint32_t)layout.scissor.left, 7);
    check_u32("pillarbox scissor right", (uint32_t)layout.scissor.right, 1913);
}

static void test_shader_constants(void) {
    GeState state = {};
    state.minz = 17;
    state.maxz = 65000;
    state.tex_enable = 1;
    state.tex_addr = 0x04010000u;
    state.tex_w = 128;
    state.tex_h = 64;
    state.tex_func = 4u | 0x100u | 0x10000u;
    state.tex_filter = 0;
    state.tex_env = 0x00332211u;
    state.fog_enable = 1;
    state.fog_color = 0x00665544u;
    state.atest_enable = 1;
    state.atest = 6u | (0x7fu << 8) | (0xf0u << 16);
    state.blend_enable = 1;
    state.blend_mode = 6;
    state.color_test_enable = 1;
    state.color_test = 2;
    state.color_ref = 0x00a1b2c3u;
    state.color_test_mask = 0x00f0ff0fu;
    state.stencil_test_enable = 1;
    state.stencil_test = 5u | (0x66u << 8) | (0xf0u << 16);
    state.stencil_op = 1u | (2u << 8) | (3u << 16);
    state.mask_alpha = 0x0f;
    state.fbfmt = 2;
    state.dither_enable = 1;
    state.dith[0][0] = -4;
    state.dith[0][1] = -1;
    state.dith[0][2] = 0;
    state.dith[0][3] = 7;

    GeD3D12TextureBinding texture = {};
    texture.handle = GE_D3D12_TEXTURE_HANDLE_NONE;
    texture.framebuffer.handle = 9;
    texture.framebuffer.x = 7;
    texture.framebuffer.y = 11;
    GeD3D12ShaderConstants constants = {};
    ged3d12_shader_constants_build(
        &state, 1, 0, &texture, &constants);
    uint32_t flags = (uint32_t)constants.config[0] >> 8;
    check_u32(
        "shader texture function",
        (uint32_t)constants.config[0] & 7u, 4);
    check_u32(
        "shader feature flags", flags,
        GE_D3D12_SHADER_TEXTURE |
        GE_D3D12_SHADER_TEXTURE_RGBA |
        GE_D3D12_SHADER_COLOR_DOUBLE |
        GE_D3D12_SHADER_FOG |
        GE_D3D12_SHADER_DEPTH_RANGE |
        GE_D3D12_SHADER_NEAREST |
        GE_D3D12_SHADER_DOUBLE_SOURCE_ALPHA |
        GE_D3D12_SHADER_COLOR_TEST |
        GE_D3D12_SHADER_DITHER |
        GE_D3D12_SHADER_STENCIL);
    check_u32(
        "shader alpha test", (uint32_t)constants.config[1],
        state.atest);
    check_u32(
        "shader minimum depth", (uint32_t)constants.config[2], 17);
    check_u32(
        "shader maximum depth", (uint32_t)constants.config[3], 65000);
    check_float(
        "shader texture environment red",
        constants.texture_environment[0], 0x11 / 255.0f);
    check_float(
        "shader fog blue", constants.fog_color[2], 0x66 / 255.0f);
    check_float("shader texture width", constants.texture_size[0], 128.0f);
    check_float("shader texture height", constants.texture_size[1], 64.0f);
    check_float("shader alias x", constants.texture_size[2], 7.0f);
    check_float("shader alias y", constants.texture_size[3], 11.0f);
    check_u32("shader color function", constants.color_test[0], 2);
    check_u32(
        "shader color reference", constants.color_test[1], 0x00a1b2c3u);
    check_u32(
        "shader color mask", constants.color_test[2], 0x00f0ff0fu);
    check_u32(
        "shader stencil test", constants.stencil_test[0],
        state.stencil_test);
    check_u32(
        "shader stencil operations", constants.stencil_test[1],
        state.stencil_op);
    check_u32("shader stencil write mask", constants.stencil_test[2], 0x0f);
    check_u32("shader stencil format", constants.stencil_test[3], 2);
    check_u32("shader dither row", constants.dither[0], 0x000070fcu);

    state.clear = 1;
    state.clear_mode = 0x700;
    ged3d12_shader_constants_build(
        &state, 1, 1, &texture, &constants);
    flags = (uint32_t)constants.config[0] >> 8;
    check_u32(
        "clear shader flags", flags,
        GE_D3D12_SHADER_CLEAR | GE_D3D12_SHADER_DITHER);
    check_u32(
        "clear alpha test disabled",
        (uint32_t)constants.config[1], 1);
    check_u32(
        "clear color test disabled", constants.color_test[0], 1);
    check_u32(
        "clear stencil test disabled", constants.stencil_test[0], 1);
}

static void test_pipeline_keys(void) {
    GeD3D12PipelineKey base;
    GeD3D12PipelineKey key;
    memset(&s_state, 0, sizeof(s_state));
    ged3d12_pipeline_key_build(&s_state, 0, &base);
    check_u32("default blend disabled", base.blend_enable, 0);
    check_u32("default source blend", base.source_blend, D3D12_BLEND_ONE);
    check_u32("default destination blend", base.destination_blend, D3D12_BLEND_ZERO);
    check_u32(
        "default color mask", base.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_RED |
        D3D12_COLOR_WRITE_ENABLE_GREEN |
        D3D12_COLOR_WRITE_ENABLE_BLUE);
    check_u32("default depth disabled", base.depth_enable, 0);
    check_u32("default culling", base.cull_mode, D3D12_CULL_MODE_NONE);
    check_u32("default render-target format", base.render_target_format,
              DXGI_FORMAT_B8G8R8A8_UNORM);
    check_u32("default depth format", base.depth_stencil_format,
              DXGI_FORMAT_UNKNOWN);

    uint64_t base_hash = ged3d12_pipeline_key_hash(&base);
    s_state.tex_addr = 0x04012340u;
    s_state.tex_func = 0x10103u;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u64("shader-only state keeps PSO hash",
              ged3d12_pipeline_key_hash(&key), base_hash);
    if (memcmp(&base, &key, sizeof(key)) != 0)
        fail("shader-only state changed the PSO key");

    s_state.blend_enable = 1;
    s_state.blend_mode = 2u | (3u << 4) | (1u << 8);
    s_state.ztest_enable = 1;
    s_state.ztest = 6;
    s_state.cull_enable = 1;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32("blend enabled", key.blend_enable, 1);
    check_u32("source alpha blend", key.source_blend, D3D12_BLEND_SRC_ALPHA);
    check_u32("inverse source alpha blend", key.destination_blend,
              D3D12_BLEND_INV_SRC_ALPHA);
    check_u32("subtract blend operation", key.blend_operation,
              D3D12_BLEND_OP_SUBTRACT);
    check_u32("depth enabled", key.depth_enable, 1);
    check_u32("depth write enabled", key.depth_write, 1);
    check_u32("depth comparison", key.depth_function,
              D3D12_COMPARISON_FUNC_GREATER);
    check_u32("depth format", key.depth_stencil_format,
              DXGI_FORMAT_D16_UNORM);
    check_u32("triangle culling", key.cull_mode, D3D12_CULL_MODE_BACK);
    if (ged3d12_pipeline_key_hash(&key) == base_hash)
        fail("pipeline hash ignored relevant state");

    GeD3D12PipelineKey sprite;
    ged3d12_pipeline_key_build(&s_state, 1, &sprite);
    check_u32("sprite culling disabled", sprite.cull_mode, D3D12_CULL_MODE_NONE);

    s_state.clear = 1;
    s_state.clear_mode = 0x500;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32("clear disables blend", key.blend_enable, 0);
    check_u32("clear color mask", key.color_write_mask,
              D3D12_COLOR_WRITE_ENABLE_RED |
              D3D12_COLOR_WRITE_ENABLE_GREEN |
              D3D12_COLOR_WRITE_ENABLE_BLUE);
    check_u32("clear depth enabled", key.depth_enable, 1);
    check_u32("clear depth write", key.depth_write, 1);
    check_u32("clear depth function", key.depth_function,
              D3D12_COMPARISON_FUNC_ALWAYS);
    check_u32("clear culling disabled", key.cull_mode, D3D12_CULL_MODE_NONE);

    memset(&s_state, 0, sizeof(s_state));
    s_state.stencil_test_enable = 1;
    s_state.stencil_test = 1;
    s_state.stencil_op = 2u << 16;
    s_state.fbfmt = 3;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32(
        "stencil replace enables alpha write", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_ALL);
    s_state.stencil_op = 0;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32(
        "stencil keep preserves alpha", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_RED |
        D3D12_COLOR_WRITE_ENABLE_GREEN |
        D3D12_COLOR_WRITE_ENABLE_BLUE);
    s_state.stencil_test = 2;
    s_state.stencil_op = 2u << 16;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32(
        "stencil compare preserves alpha", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_RED |
        D3D12_COLOR_WRITE_ENABLE_GREEN |
        D3D12_COLOR_WRITE_ENABLE_BLUE);
    s_state.stencil_test = 1;
    s_state.mask_alpha = 0x0f;
    s_state.fbfmt = 3;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32(
        "partial 8888 stencil mask preserves alpha", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_RED |
        D3D12_COLOR_WRITE_ENABLE_GREEN |
        D3D12_COLOR_WRITE_ENABLE_BLUE);
    s_state.fbfmt = 2;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32(
        "irrelevant 4444 stencil mask writes alpha", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_ALL);

    memset(&s_state, 0, sizeof(s_state));
    s_state.blend_enable = 1;
    s_state.blend_mode = 10u | (10u << 4);
    s_state.blend_fixa = 0x010101u;
    s_state.blend_fixb = 0xfefefeu;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32("fixed source factor", key.source_blend,
              D3D12_BLEND_BLEND_FACTOR);
    check_u32("bright conflicting destination factor", key.destination_blend,
              D3D12_BLEND_ONE);
    s_state.blend_fixb = 0x010000u;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32("dark conflicting destination factor", key.destination_blend,
              D3D12_BLEND_ZERO);
}

static void test_texture_keys(void) {
    GeD3D12TextureKey base;
    GeD3D12TextureKey changed;
    GeD3D12TextureSampler sampler;
    memset(&s_state, 0, sizeof(s_state));
    s_state.tex_enable = 1;
    s_state.tex_addr = 0x04010000u;
    s_state.tex_bufw = 8;
    s_state.tex_w = 8;
    s_state.tex_h = 8;
    s_state.tex_fmt = 4;
    s_state.tex_filter = 4;
    s_state.tex_max_level = 2;
    s_state.tex_mode = 0x100;
    s_state.clut_fmt = 3u | (0xffu << 8);
    s_state.clutram[4] = 0x55;
    s_state.tex_addr_levels[1] = 0x04010100u;
    s_state.tex_bufw_levels[1] = 4;
    s_state.tex_width_levels[1] = 4;
    s_state.tex_height_levels[1] = 4;
    s_state.tex_addr_levels[2] = 0x04010200u;
    s_state.tex_bufw_levels[2] = 2;
    s_state.tex_width_levels[2] = 2;
    s_state.tex_height_levels[2] = 2;
    ged3d12_texture_key_build(&s_state, &base);
    check_u32("texture key format", base.format, 4);
    check_u32("texture key mip count", base.mip_count, 3);
    check_u32("texture key base address", base.address[0], 0x04010000u);
    check_u32("texture key mip address", base.address[2], 0x04010200u);
    check_u32("texture key mip width", base.width[1], 4);
    check_u32("texture key aligned stride", base.stride[0], 32);
    check_u32("texture key separate CLUT", base.clut_shared, 0);
    if (!base.clut_hash) fail("texture key omitted CLUT identity");
    check_u64("texture CLUT4 byte size",
              ged3d12_texture_key_level_bytes(&base, 0), 128);
    if (!ged3d12_texture_key_overlaps(&base, 0x04010010u, 4))
        fail("texture key missed base-level invalidation");
    if (!ged3d12_texture_key_overlaps(&base, 0x04010200u, 1))
        fail("texture key missed mip-level invalidation");
    if (ged3d12_texture_key_overlaps(&base, 0x04011000u, 4))
        fail("texture key reported disjoint invalidation");

    changed = base;
    s_state.tex_wrap = 0x101;
    s_state.tex_filter = 5;
    ged3d12_texture_key_build(&s_state, &changed);
    if (memcmp(&base, &changed, sizeof(base)) != 0)
        fail("sampler-only state changed texture resource key");

    uint64_t base_hash = ged3d12_texture_key_hash(&base);
    s_state.clutram[4] ^= 1;
    ged3d12_texture_key_build(&s_state, &changed);
    if (ged3d12_texture_key_hash(&changed) == base_hash)
        fail("texture resource hash ignored CLUT contents");

    s_state.tex_fmt = 8;
    s_state.tex_filter = 0;
    s_state.tex_swizzle = 0;
    s_state.tex_bufw = 8;
    s_state.tex_w = 8;
    s_state.tex_h = 8;
    ged3d12_texture_key_build(&s_state, &changed);
    check_u64("texture DXT1 key byte size",
              ged3d12_texture_key_level_bytes(&changed, 0), 32);

    s_state.tex_w = 1;
    s_state.tex_h = 1;
    s_state.tex_max_level = 7;
    s_state.tex_filter = 4;
    for (int level = 1; level < GE_D3D12_TEXTURE_LEVELS; level++) {
        s_state.tex_addr_levels[level] =
            0x04010000u + (uint32_t)level * 0x100u;
        s_state.tex_bufw_levels[level] = 4;
        s_state.tex_width_levels[level] = 1;
        s_state.tex_height_levels[level] = 1;
    }
    ged3d12_texture_key_build(&s_state, &changed);
    check_u32("texture legal mip chain cap", changed.mip_count, 1);

    s_state.tex_fmt = 5;
    s_state.tex_swizzle = 1;
    s_state.tex_bufw = 16;
    s_state.tex_w = 16;
    s_state.tex_h = 1;
    ged3d12_texture_key_build(&s_state, &changed);
    check_u64("texture swizzled key byte size",
              ged3d12_texture_key_level_bytes(&changed, 0), 128);

    s_state.tex_filter = 0x107;
    s_state.tex_wrap = 0x101;
    s_state.tex_level = 0xf80000u;
    s_state.tex_max_level = 2;
    ged3d12_texture_sampler_build(&s_state, &sampler);
    check_u32("texture sampler min linear", sampler.min_linear, 1);
    check_u32("texture sampler mag linear", sampler.mag_linear, 1);
    check_u32("texture sampler mip linear", sampler.mip_linear, 1);
    check_u32("texture sampler mip enabled", sampler.mip_enabled, 1);
    check_u32("texture sampler clamp u", sampler.clamp_u, 1);
    check_u32("texture sampler clamp v", sampler.clamp_v, 1);
    check_u32("texture sampler signed bias",
              (uint32_t)(int32_t)sampler.lod_bias_16,
              (uint32_t)(int32_t)(int8_t)0xf8);
}

static void test_framebuffer_keys(void) {
    memset(&s_state, 0, sizeof(s_state));
    s_state.fbp = 0x00100000u;
    s_state.fbfmt = 3;
    s_state.zbp = 0x00188000u;
    GeD3D12FramebufferKey source = {};
    if (!ged3d12_framebuffer_key_build(&s_state, &source)) {
        fail("default framebuffer key rejected");
        return;
    }
    check_u32("framebuffer address", source.address, 0x04100000u);
    check_u32("framebuffer default stride", source.stride, 512);
    check_u32("framebuffer depth address", source.depth_address, 0x04188000u);
    check_u32("framebuffer default depth stride", source.depth_stride, 512);
    check_u64("framebuffer byte range",
              ged3d12_framebuffer_key_bytes(&source),
              UINT64_C(512) * 272u * 4u);
    check_u32("framebuffer 565 red decode",
              ged3d12_framebuffer_decode_pixel(0x001fu, 0),
              0xffff0000u);
    check_u32("framebuffer 565 blue decode",
              ged3d12_framebuffer_decode_pixel(0xf800u, 0),
              0xff0000ffu);
    check_u32("framebuffer 8888 channel decode",
              ged3d12_framebuffer_decode_pixel(0x44332211u, 3),
              0x44112233u);
    check_u32("framebuffer 8888 channel encode",
              ged3d12_framebuffer_encode_pixel(0x44112233u, 3),
              0x44332211u);
    for (uint32_t format = 0; format < 3; format++) {
        static const uint32_t pixels[] = {
            0xffffu, 0x8421u, 0x1234u,
        };
        uint32_t raw = pixels[format];
        check_u32("framebuffer 16-bit round trip",
                  ged3d12_framebuffer_encode_pixel(
                      ged3d12_framebuffer_decode_pixel(raw, format),
                      format),
                  raw);
    }
    if (!ged3d12_framebuffer_key_overlaps(
            &source, 0x44100020u, 16u)) {
        fail("framebuffer alias address did not overlap");
    }
    if (ged3d12_framebuffer_key_overlaps(
            &source, 0x04190000u, 16u)) {
        fail("disjoint framebuffer range overlapped");
    }
    s_state.fbw = 480;
    GeD3D12FramebufferKey narrow = {};
    if (!ged3d12_framebuffer_key_build(&s_state, &narrow))
        fail("480-pixel framebuffer key rejected");
    check_u32("framebuffer explicit stride", narrow.stride, 480);
    check_u32("framebuffer inherited depth stride",
              narrow.depth_stride, 480);
    s_state.fbw = 0;
    s_state.fbp = 0x001f0000u;
    GeD3D12FramebufferKey outside = {};
    if (ged3d12_framebuffer_key_build(&s_state, &outside))
        fail("framebuffer outside PSP VRAM accepted");
    s_state.fbp = 0x00100000u;

    GeD3D12FramebufferAlias alias = {};
    uint32_t alias_address = source.address + (2u * source.stride + 8u) * 4u;
    if (!ged3d12_framebuffer_alias(
            &source, alias_address, source.stride, 64, 32, 3, false,
            &alias)) {
        fail("valid framebuffer texture alias rejected");
    }
    check_u32("framebuffer alias x", alias.x, 8);
    check_u32("framebuffer alias y", alias.y, 2);
    if (ged3d12_framebuffer_alias(
            &source, alias_address, source.stride, 64, 32, 2, false,
            &alias)) {
        fail("format-reinterpreted framebuffer aliased directly");
    }
    if (ged3d12_framebuffer_alias(
            &source, alias_address, source.stride, 64, 32, 3, true,
            &alias)) {
        fail("swizzled framebuffer aliased directly");
    }

    s_state.xf_src = 0x001000u;
    s_state.xf_srcw = 0x040000u | 512u;
    s_state.xf_dst = 0x002000u;
    s_state.xf_dstw = 0x040000u | 512u;
    s_state.xf_spos = 3u | (4u << 10);
    s_state.xf_dpos = 5u | (6u << 10);
    s_state.xf_size = 6u | (7u << 10);
    GeD3D12Transfer transfer = {};
    if (!ged3d12_transfer_build(&s_state, 1, &transfer)) {
        fail("valid framebuffer transfer rejected");
        return;
    }
    check_u32("transfer source address", transfer.source_address, 0x04001000u);
    check_u32("transfer destination address",
              transfer.destination_address, 0x04002000u);
    check_u32("transfer width", transfer.width, 7);
    check_u32("transfer height", transfer.height, 8);
    check_u32("transfer bytes per pixel", transfer.bytes_per_pixel, 4);

    source.address = transfer.source_address;
    source.stride = transfer.source_stride;
    GeD3D12FramebufferKey destination = source;
    destination.address = transfer.destination_address;
    D3D12_BOX source_box = {};
    UINT destination_x = 0;
    UINT destination_y = 0;
    if (!ged3d12_transfer_coordinates(
            &transfer, &source, &destination, &source_box,
            &destination_x, &destination_y)) {
        fail("compatible framebuffer transfer coordinates rejected");
    }
    check_u32("transfer source left", source_box.left, 3);
    check_u32("transfer source top", source_box.top, 4);
    check_u32("transfer source right", source_box.right, 10);
    check_u32("transfer source bottom", source_box.bottom, 12);
    check_u32("transfer destination x", destination_x, 5);
    check_u32("transfer destination y", destination_y, 6);
    destination.format = 2;
    if (ged3d12_transfer_coordinates(
            &transfer, &source, &destination, &source_box,
            &destination_x, &destination_y)) {
        fail("format-reinterpreted transfer used an image copy");
    }
}

static void test_triangle(void) {
    reset_upload();
    memset(&s_state, 0, sizeof(s_state));
    s_state.shade_gouraud = 0;
    GeVtx a = vertex(1.1f, 2.2f, 3.0f, 0.25f, 4.0f, 5.0f, 0.5f,
                     1, 2, 3, 4);
    GeVtx b = vertex(6.3f, 7.4f, 8.0f, 0.5f, 9.0f, 10.0f, 0.75f,
                     5, 6, 7, 8);
    GeVtx c = vertex(11.5f, 12.6f, 13.0f, 1.0f, 14.0f, 15.0f, 1.0f,
                     300, -1, 128, 256);
    check_u32("triangle hook remains software", ged3d12_vertex_capture_tri(&a, &b, &c, 1), 0);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("triangle upload failed");
        return;
    }
    check_u32("triangle vertex count", frame.vertex_count, 3);
    check_u32("triangle batch count", frame.batch_count, 1);
    check_u32("triangle primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE], 1);
    check_u32("triangle stride", frame.buffer.StrideInBytes, 32);
    check_u32("triangle upload bytes", frame.buffer.SizeInBytes, 96);
    check_u64("triangle upload alignment", frame.buffer.BufferLocation, TEST_GPU_BASE + 16);
    if (!frame.hash) fail("triangle hash was zero");
    check_u32("triangle batch first", frame.batches[0].first, 0);
    check_u32("triangle batch vertices", frame.batches[0].count, 3);
    check_u32("triangle batch kind", frame.batches[0].primitive,
              GE_D3D12_PRIMITIVE_TRIANGLE);
    check_u32("triangle perspective", frame.batches[0].perspective, 1);

    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_float("triangle snapped x", vertices[0].x, 1.0625f);
    check_float("triangle snapped y", vertices[0].y, 2.1875f);
    check_float("triangle z", vertices[0].z, 3.0f);
    check_float("triangle reciprocal w", vertices[0].rw, 0.25f);
    check_float("triangle u", vertices[0].u, 4.0f);
    check_float("triangle fog", vertices[0].fog, 0.5f);
    check_u32("triangle flat color A", vertices[0].rgba, 0xff8000ffu);
    check_u32("triangle flat color B", vertices[1].rgba, 0xff8000ffu);
    check_u32("triangle flat color C", vertices[2].rgba, 0xff8000ffu);

    reset_upload();
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("empty upload failed");
    } else {
        check_u32("empty upload vertex count", frame.vertex_count, 0);
        check_u32("empty upload batch count", frame.batch_count, 0);
    }
}

static void test_sprites(void) {
    reset_upload();
    GeVtx p0 = vertex(10.9f, 20.9f, 2.0f, 0.5f, 2.0f, 3.0f, 0.25f,
                      1, 2, 3, 4);
    GeVtx p1 = vertex(14.2f, 24.7f, 9.0f, 1.5f, 12.0f, 15.0f, 0.75f,
                      10, 20, 30, 40);
    ged3d12_vertex_capture_sprite(&p0, &p1, 1);

    GeVtx q0 = vertex(30.2f, 40.8f, 4.0f, 0.25f, 2.0f, 6.0f, 0.2f,
                      5, 6, 7, 8);
    GeVtx q1 = vertex(34.9f, 44.1f, 5.0f, 2.0f, 10.0f, 14.0f, 0.4f,
                      50, 60, 70, 80);
    ged3d12_vertex_capture_sprite(&q0, &q1, 0);
    GeVtx empty = q0;
    empty.x = q0.x + 0.1f;
    empty.y = q0.y + 0.1f;
    ged3d12_vertex_capture_sprite(&q0, &empty, 0);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("sprite upload failed");
        return;
    }
    check_u32("sprite vertex count", frame.vertex_count, 12);
    check_u32("sprite batch count", frame.batch_count, 2);
    check_u32("sprite primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_SPRITE], 2);
    check_u32("perspective sprite flag", frame.batches[0].perspective, 1);
    check_u32("through sprite flag", frame.batches[1].perspective, 0);
    check_u32("through sprite batch first", frame.batches[1].first, 6);

    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_float("perspective sprite xa", vertices[0].x, 10.0f);
    check_float("perspective sprite ya", vertices[0].y, 20.0f);
    check_float("perspective sprite xb", vertices[1].x, 14.0f);
    check_float("perspective sprite yb", vertices[2].y, 24.0f);
    check_float("perspective sprite ua", vertices[0].u, 3.5f);
    check_float("perspective sprite ub", vertices[1].u, 7.5f);
    check_float("perspective sprite va", vertices[0].v, 5.5f);
    check_float("perspective sprite vb", vertices[2].v, 9.5f);
    check_float("perspective sprite z from p1", vertices[0].z, 9.0f);
    check_float("perspective sprite rw", vertices[0].rw, 1.0f);
    check_u32("perspective sprite color", vertices[0].rgba, 0x281e140au);

    check_float("through sprite ua", vertices[6].u, 1.0f);
    check_float("through sprite ub", vertices[7].u, 9.0f);
    check_float("through sprite va", vertices[6].v, 5.0f);
    check_float("through sprite vb", vertices[8].v, 13.0f);
}

static void test_lines_and_points(void) {
    reset_upload();
    s_state.shade_gouraud = 0;
    GeVtx a = vertex(0.0f, 0.0f, 1.0f, 0.5f, 2.0f, 3.0f, 0.5f,
                     1, 2, 3, 4);
    GeVtx b = vertex(2.0f, 0.0f, 4.0f, 1.0f, 5.0f, 6.0f, 0.75f,
                     10, 20, 30, 40);
    GeVtx p = vertex(4.8f, 8.9f, 7.0f, 1.0f, 8.0f, 9.0f, 1.0f,
                     50, 60, 70, 80);
    ged3d12_vertex_capture_line(&a, &b, 1);
    ged3d12_vertex_capture_point(&p, 0);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("line/point upload failed");
        return;
    }
    check_u32("line/point vertex count", frame.vertex_count, 12);
    check_u32("line/point batch count", frame.batch_count, 2);
    check_u32("line primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_LINE], 1);
    check_u32("point primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_POINT], 1);
    check_u32("point batch first", frame.batches[1].first, 6);
    check_u32("point batch kind", frame.batches[1].primitive,
              GE_D3D12_PRIMITIVE_POINT);

    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_float("line first x", vertices[0].x, 0.0f);
    check_float("line first y", vertices[0].y, 0.0f);
    check_float("line far x", vertices[2].x, 3.0f);
    check_u32("line flat color A", vertices[0].rgba, 0x281e140au);
    check_u32("line color B", vertices[2].rgba, 0x281e140au);
    check_float("point floor x", vertices[6].x, 4.0f);
    check_float("point floor y", vertices[6].y, 8.0f);
    check_float("point max x", vertices[10].x, 5.0f);
    check_float("point max y", vertices[10].y, 9.0f);
}

static void test_storage_growth(void) {
    reset_upload();
    s_state.shade_gouraud = 1;
    GeVtx a = vertex(1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 1.0f,
                     1, 2, 3, 4);
    GeVtx b = a;
    GeVtx c = a;
    for (uint32_t i = 0; i < 3000; i++)
        ged3d12_vertex_capture_tri(&a, &b, &c, 1);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("grown storage upload failed");
        return;
    }
    check_u32("grown vertex count", frame.vertex_count, 9000);
    check_u32("grown batch count", frame.batch_count, 3000);
    check_u32("grown upload size", frame.buffer.SizeInBytes, 9000 * 32);
}

int main(void) {
    if (!ged3d12_vertex_capture_init()) {
        fail("vertex capture initialization failed");
    } else {
        test_present_viewports();
        test_shader_constants();
        test_pipeline_keys();
        test_texture_keys();
        test_framebuffer_keys();
        test_triangle();
        test_sprites();
        test_lines_and_points();
        test_storage_growth();
        ged3d12_vertex_capture_shutdown();
    }

    if (s_failures) {
        fprintf(stderr, "D3D12 vertex tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("D3D12 vertex tests passed\n");
    return 0;
}
