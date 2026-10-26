#include "../../src/rt/gpu_d3d12/device.h"
#include "../../src/rt/gpu_d3d12/framebuffer.h"
#include "../../src/rt/gpu_d3d12/present.h"
#include "../../src/rt/gpu_d3d12/vertex.h"
#include "../../src/rt/perf_stats.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_UPLOAD_SIZE (8u * 1024u * 1024u)
#define TEST_GPU_BASE UINT64_C(0x10000000)

static GeState s_state;
static GeDebugContext s_debug_context;
static uint8_t s_upload[TEST_UPLOAD_SIZE];
static UINT64 s_upload_cursor;
static uint32_t s_texture_captures;
static uint32_t s_framebuffer_captures;
static uint32_t s_render_scale = 1;
static int s_failures;

extern "C" GeState *ge_state_ptr(void) {
    return &s_state;
}

extern "C" GeDebugContext ge_debug_context(void) {
    return s_debug_context;
}

bool ged3d12_texture_capture(GeD3D12TextureBinding *out) {
    if (!out) return false;
    s_texture_captures++;
    memset(out, 0, sizeof(*out));
    out->handle = GE_D3D12_TEXTURE_HANDLE_NONE;
    out->framebuffer.handle = GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    ged3d12_texture_sampler_build(&s_state, &out->sampler);
    return true;
}

bool ged3d12_framebuffer_capture(GeD3D12FramebufferBinding *out) {
    if (!out) return false;
    s_framebuffer_captures++;
    memset(out, 0, sizeof(*out));
    out->color_handle = 1;
    out->depth_handle = 2;
    return true;
}

extern "C" SrPerfCategory sr_perf_switch(SrPerfCategory category) {
    return category;
}

extern "C" volatile uint32_t sr_perf_alternate_mask = 0;

uint32_t ged3d12_command_list_generation(void) {
    return 1;
}

uint32_t ged3d12_framebuffer_scale(void) {
    return s_render_scale;
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

    GeD3D12CaptureLayout capture;
    ged3d12_present_capture_layout(3, false, &capture);
    check_u32("logical capture width", capture.width, 480);
    check_u32("logical capture height", capture.height, 272);
    check_u32("logical capture step", capture.source_step, 3);
    check_u32("logical capture offset", capture.sample_offset, 1);
    ged3d12_present_capture_layout(3, true, &capture);
    check_u32("internal capture width", capture.width, 1440);
    check_u32("internal capture height", capture.height, 816);
    check_u32("internal capture step", capture.source_step, 1);
    check_u32("internal capture offset", capture.sample_offset, 0);
    check_u32(
        "565 capture color",
        ged3d12_present_capture_rgb(0x00ffffffu, 0),
        0x00f8fcf8u);
    check_u32(
        "5551 capture color",
        ged3d12_present_capture_rgb(0x00ffffffu, 1),
        0x00f8f8f8u);
    check_u32(
        "4444 capture color",
        ged3d12_present_capture_rgb(0x00ffffffu, 2),
        0x00f0f0f0u);
    check_u32(
        "8888 capture color",
        ged3d12_present_capture_rgb(0x00123456u, 3),
        0x00123456u);
}

static void test_shader_constants(void) {
    check_u32(
        "native shading rate",
        ged3d12_shader_shading_rate(
            1, D3D12_VARIABLE_SHADING_RATE_TIER_2, true),
        D3D12_SHADING_RATE_1X1);
    check_u32(
        "unsupported shading rate",
        ged3d12_shader_shading_rate(
            4, D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED, true),
        D3D12_SHADING_RATE_1X1);
    check_u32(
        "2x shading rate",
        ged3d12_shader_shading_rate(
            2, D3D12_VARIABLE_SHADING_RATE_TIER_1, false),
        D3D12_SHADING_RATE_2X2);
    check_u32(
        "4x fallback shading rate",
        ged3d12_shader_shading_rate(
            4, D3D12_VARIABLE_SHADING_RATE_TIER_1, false),
        D3D12_SHADING_RATE_2X2);
    check_u32(
        "4x additional shading rate",
        ged3d12_shader_shading_rate(
            4, D3D12_VARIABLE_SHADING_RATE_TIER_2, true),
        D3D12_SHADING_RATE_4X4);

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
    state.blend_mode = 6u | (9u << 4) | (2u << 8);
    state.blend_fixa = 0x00112233u;
    state.blend_fixb = 0x00445566u;
    state.logic_op_enable = 1;
    state.logic_op = 6;
    state.color_test_enable = 1;
    state.color_test = 2;
    state.color_ref = 0x00a1b2c3u;
    state.color_test_mask = 0x00f0ff0fu;
    state.stencil_test_enable = 1;
    state.stencil_test = 5u | (0x66u << 8) | (0xf0u << 16);
    state.stencil_op = 1u | (2u << 8) | (3u << 16);
    state.mask_rgb = 0x00a1b2c3u;
    state.mask_alpha = 0x0f;
    state.fbfmt = 2;
    state.ztest_enable = 1;
    state.ztest = 6;
    state.scis_x1 = 3;
    state.scis_y1 = 5;
    state.scis_x2 = 200;
    state.scis_y2 = 150;
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
        &state, 1, 0, 1, &texture, &constants);
    GeD3D12ShaderConstantCache cache = {};
    if (!ged3d12_shader_constants_cache_update(&cache, &constants))
        fail("constant cache rejected initial state");
    if (ged3d12_shader_constants_cache_update(&cache, &constants))
        fail("constant cache missed identical state");
    GeD3D12ShaderConstants changed_constants = constants;
    changed_constants.config[3]++;
    if (!ged3d12_shader_constants_cache_update(
            &cache, &changed_constants)) {
        fail("constant cache missed changed state");
    }
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
        GE_D3D12_SHADER_COLOR_TEST |
        GE_D3D12_SHADER_DITHER |
        GE_D3D12_SHADER_STENCIL |
        GE_D3D12_SHADER_PSP_SAMPLER |
        GE_D3D12_SHADER_FRAMEBUFFER);
    check_u32(
        "framebuffer texture shader flag",
        flags & GE_D3D12_SHADER_FRAMEBUFFER,
        GE_D3D12_SHADER_FRAMEBUFFER);
    GeD3D12TextureBinding decoded_texture = texture;
    decoded_texture.framebuffer.handle =
        GE_D3D12_FRAMEBUFFER_HANDLE_NONE;
    GeD3D12ShaderConstants decoded_constants = {};
    ged3d12_shader_constants_build(
        &state, 1, 0, 1, &decoded_texture, &decoded_constants);
    check_u32(
        "decoded texture framebuffer flag",
        ((uint32_t)decoded_constants.config[0] >> 8) &
            GE_D3D12_SHADER_FRAMEBUFFER,
        0);
    _putenv_s("SR_GPU_PSP_SAMPLER", "0");
    GeD3D12ShaderConstants hardware_sampler_constants = {};
    ged3d12_shader_constants_build(
        &state, 1, 0, 1, &texture, &hardware_sampler_constants);
    check_u32(
        "hardware sampler override",
        ((uint32_t)hardware_sampler_constants.config[0] >> 8) &
            GE_D3D12_SHADER_PSP_SAMPLER,
        0);
    _putenv_s("SR_GPU_PSP_SAMPLER", "");
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
        "shader render scale",
        constants.texture_environment[3], 1.0f);
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
    GeD3D12TextureBinding feedback_texture = texture;
    feedback_texture.flags =
        GE_D3D12_TEXTURE_FRAMEBUFFER_CLUT |
        (3u << GE_D3D12_TEXTURE_FRAMEBUFFER_FORMAT_SHIFT);
    feedback_texture.clut_handle = 3;
    feedback_texture.sampler.clamp_u = 1;
    state.clut_fmt = 3u | (8u << 2) | (0x7fu << 8) | (4u << 16);
    GeD3D12ShaderConstants feedback_constants = {};
    ged3d12_shader_constants_build(
        &state, 1, 0, 1, &feedback_texture, &feedback_constants);
    check_u32(
        "framebuffer CLUT shader flag",
        ((uint32_t)feedback_constants.config[0] >> 8) &
            GE_D3D12_SHADER_FRAMEBUFFER_CLUT,
        GE_D3D12_SHADER_FRAMEBUFFER_CLUT);
    check_u32(
        "framebuffer CLUT state", feedback_constants.color_test[3],
        state.clut_fmt |
            (3u << GE_D3D12_SHADER_FRAMEBUFFER_FORMAT_SHIFT) |
            (1u << 30));
    for (uint32_t format = 0; format < 4; format++) {
        feedback_texture.flags =
            GE_D3D12_TEXTURE_FRAMEBUFFER_CLUT |
            (uint8_t)(
                format <<
                GE_D3D12_TEXTURE_FRAMEBUFFER_FORMAT_SHIFT);
        ged3d12_shader_constants_build(
            &state, 1, 0, 1, &feedback_texture, &feedback_constants);
        check_u32(
            "framebuffer source format state",
            feedback_constants.color_test[3] &
                GE_D3D12_SHADER_FRAMEBUFFER_FORMAT_MASK,
            format << GE_D3D12_SHADER_FRAMEBUFFER_FORMAT_SHIFT);
    }
    check_u32(
        "shader stencil test", constants.stencil_test[0],
        state.stencil_test);
    check_u32(
        "shader stencil operations", constants.stencil_test[1],
        state.stencil_op);
    check_u32("shader stencil write mask", constants.stencil_test[2], 0x0f);
    check_u32("shader stencil format", constants.stencil_test[3], 2);
    check_u32("shader dither row", constants.dither[0], 0x000070fcu);
    check_u32("shader blend enable", constants.blend[0], 1);
    check_u32("shader blend mode", constants.blend[1], state.blend_mode);
    check_u32("shader fixed source", constants.blend[2], 0x00112233u);
    check_u32("shader fixed destination", constants.blend[3], 0x00445566u);
    check_u32("shader write mask", constants.fragment[0], 0x0fa1b2c3u);
    check_u32("shader logic state", constants.fragment[1], 0x00000601u);
    check_u32("shader depth state", constants.fragment[3], 0x00010601u);
    check_u32("shader scissor left", (uint32_t)constants.scissor[0], 3);
    check_u32("shader scissor top", (uint32_t)constants.scissor[1], 5);
    check_u32("shader scissor right", (uint32_t)constants.scissor[2], 201);
    check_u32("shader scissor bottom", (uint32_t)constants.scissor[3], 151);

    s_render_scale = 4;
    ged3d12_shader_constants_build(
        &state, 1, 0, 1, &texture, &constants);
    uint32_t scaled_flags =
        (uint32_t)constants.config[0] >> 8;
    check_float(
        "scaled shader render scale",
        constants.texture_environment[3], 4.0f);
    check_u32(
        "scaled framebuffer PSP sampler",
        scaled_flags & GE_D3D12_SHADER_PSP_SAMPLER,
        GE_D3D12_SHADER_PSP_SAMPLER);
    check_u32(
        "scaled framebuffer shader flag",
        scaled_flags & GE_D3D12_SHADER_FRAMEBUFFER,
        GE_D3D12_SHADER_FRAMEBUFFER);
    GeD3D12ShaderConstants scaled_decoded_constants = {};
    ged3d12_shader_constants_build(
        &state, 1, 0, 1,
        &decoded_texture, &scaled_decoded_constants);
    check_u32(
        "scaled decoded texture PSP sampler",
        ((uint32_t)scaled_decoded_constants.config[0] >> 8) &
            GE_D3D12_SHADER_PSP_SAMPLER,
        GE_D3D12_SHADER_PSP_SAMPLER);
    check_u32(
        "scaled shader scissor left",
        (uint32_t)constants.scissor[0], 12);
    check_u32(
        "scaled shader scissor top",
        (uint32_t)constants.scissor[1], 20);
    check_u32(
        "scaled shader scissor right",
        (uint32_t)constants.scissor[2], 804);
    check_u32(
        "scaled shader scissor bottom",
        (uint32_t)constants.scissor[3], 604);
    s_render_scale = 1;

    state.scis_x1 = 600;
    state.scis_y1 = 300;
    state.scis_x2 = 2;
    state.scis_y2 = 4;
    ged3d12_shader_constants_build(
        &state, 1, 0, 1, &texture, &constants);
    check_u32(
        "empty scissor left", (uint32_t)constants.scissor[0], 512);
    check_u32(
        "empty scissor top", (uint32_t)constants.scissor[1], 272);
    check_u32(
        "empty scissor right", (uint32_t)constants.scissor[2], 512);
    check_u32(
        "empty scissor bottom", (uint32_t)constants.scissor[3], 272);

    state.clear = 1;
    state.clear_mode = 0x700;
    ged3d12_shader_constants_build(
        &state, 1, 1, 0, &texture, &constants);
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
    check_u32("clear blend disabled", constants.blend[0], 0);
    check_u32("clear logic disabled", constants.fragment[1], 0x00000600u);
    check_u32("clear mode state", constants.fragment[2], 0x700);
}

static void test_feedback_bounds(void) {
    GeD3D12TextureBinding texture = {};
    texture.framebuffer.handle = 1;
    texture.framebuffer.x = 7;
    texture.framebuffer.y = 11;
    texture.width = 128;
    texture.height = 64;
    texture.sampler.clamp_u = 1;
    texture.sampler.clamp_v = 1;
    GeD3D12Vertex vertices[3] = {};
    vertices[0].u = 10.0f;
    vertices[0].v = 5.0f;
    vertices[0].rw = 1.0f;
    vertices[1].u = 60.0f;
    vertices[1].v = 40.0f;
    vertices[1].rw = 2.0f;
    vertices[2].u = 20.0f;
    vertices[2].v = 20.0f;
    vertices[2].rw = 1.0f;
    ged3d12_vertex_feedback_bounds(&texture, vertices, 3);
    check_u32(
        "feedback snapshot left",
        texture.framebuffer.snapshot_left, 17);
    check_u32(
        "feedback snapshot top",
        texture.framebuffer.snapshot_top, 16);
    check_u32(
        "feedback snapshot right",
        texture.framebuffer.snapshot_right, 38);
    check_u32(
        "feedback snapshot bottom",
        texture.framebuffer.snapshot_bottom, 32);

    texture.sampler.clamp_u = 0;
    vertices[0].u = -1.0f;
    vertices[1].u = 4.0f;
    vertices[2].u = 8.0f;
    ged3d12_vertex_feedback_bounds(&texture, vertices, 3);
    check_u32(
        "wrapped feedback snapshot left",
        texture.framebuffer.snapshot_left, 7);
    check_u32(
        "wrapped feedback snapshot right",
        texture.framebuffer.snapshot_right, 135);

    GeD3D12FramebufferBinding framebuffer = {};
    vertices[0].x = -2.0f;
    vertices[0].y = 4.0f;
    vertices[1].x = 30.25f;
    vertices[1].y = 40.75f;
    vertices[2].x = 20.0f;
    vertices[2].y = 10.0f;
    ged3d12_vertex_draw_bounds(&framebuffer, vertices, 3);
    check_u32("draw bound left", framebuffer.draw_left, 0);
    check_u32("draw bound top", framebuffer.draw_top, 4);
    check_u32("draw bound right", framebuffer.draw_right, 31);
    check_u32("draw bound bottom", framebuffer.draw_bottom, 41);
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
        "direct RGB color mask", base.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_RED |
        D3D12_COLOR_WRITE_ENABLE_GREEN |
        D3D12_COLOR_WRITE_ENABLE_BLUE);
    check_u32("direct depth disabled", base.depth_enable, 0);
    check_u32("direct depth write disabled", base.depth_write, 0);
    check_u32(
        "direct default depth comparison", base.depth_function,
        D3D12_COMPARISON_FUNC_NEVER);
    check_u32("default culling", base.cull_mode, D3D12_CULL_MODE_NONE);
    check_u32("default render-target format", base.render_target_format,
              DXGI_FORMAT_B8G8R8A8_UNORM);
    check_u32("default depth format", base.depth_stencil_format,
              DXGI_FORMAT_D16_UNORM);

    uint64_t base_hash = ged3d12_pipeline_key_hash(&base);
    GeD3D12PipelineKey uber;
    ged3d12_pipeline_uber_key(&uber);
    if (memcmp(&base, &uber, sizeof(base)) == 0)
        fail("direct pipeline key collapsed into ubershader key");
    if (ged3d12_pipeline_uber_compatible(&base))
        fail("direct pipeline key accepted as ubershader-compatible");
    s_state.tex_addr = 0x04012340u;
    s_state.tex_func = 0x10103u;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u64("shader-only state keeps PSO hash",
              ged3d12_pipeline_key_hash(&key), base_hash);
    if (memcmp(&base, &key, sizeof(key)) != 0)
        fail("shader-only state changed the PSO key");

    GeState alpha_blend = {};
    alpha_blend.fbfmt = 3;
    alpha_blend.blend_enable = 1;
    alpha_blend.blend_mode = 2u | (3u << 4);
    alpha_blend.mask_alpha = 0xff;
    ged3d12_pipeline_key_build(&alpha_blend, 0, &key);
    check_u32("direct alpha blend enabled", key.blend_enable, 1);
    check_u32(
        "direct alpha source factor",
        key.source_blend, D3D12_BLEND_SRC_ALPHA);
    check_u32(
        "direct alpha destination factor",
        key.destination_blend, D3D12_BLEND_INV_SRC_ALPHA);
    check_u32(
        "direct alpha operation",
        key.blend_operation, D3D12_BLEND_OP_ADD);
    if (ged3d12_pipeline_uber_compatible(&key))
        fail("direct alpha blend accepted as ubershader-compatible");
    GeD3D12TextureBinding no_alpha_texture = {};
    GeD3D12ShaderConstants alpha_constants = {};
    ged3d12_shader_constants_build(
        &alpha_blend, 1, 0, 1,
        &no_alpha_texture, &alpha_constants);
    check_u32(
        "direct alpha shader flag",
        ((uint32_t)alpha_constants.config[0] >> 8) &
            GE_D3D12_SHADER_DIRECT,
        GE_D3D12_SHADER_DIRECT);

    GeState direct_stencil = {};
    direct_stencil.fbfmt = 1;
    direct_stencil.ztest_enable = 1;
    direct_stencil.ztest = 7;
    direct_stencil.stencil_test_enable = 1;
    direct_stencil.stencil_test =
        1u | (1u << 8) | (0xffu << 16);
    direct_stencil.stencil_op = 4u << 16;
    if (!ged3d12_shader_direct_compatible(&direct_stencil))
        fail("constant stencil write rejected from direct path");
    if (!ged3d12_shader_direct_stencil_write(&direct_stencil))
        fail("constant stencil write was not exposed to the pipeline");
    ged3d12_pipeline_key_build(&direct_stencil, 0, &key);
    check_u32(
        "direct stencil RGBA color mask", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_ALL);
    check_u32(
        "direct stencil depth function", key.depth_function,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    GeD3D12ShaderConstants direct_stencil_constants = {};
    ged3d12_shader_constants_build(
        &direct_stencil, 1, 0, 1, &no_alpha_texture,
        &direct_stencil_constants);
    check_u32(
        "direct stencil shader flags",
        ((uint32_t)direct_stencil_constants.config[0] >> 8) &
            (GE_D3D12_SHADER_STENCIL | GE_D3D12_SHADER_DIRECT),
        GE_D3D12_SHADER_STENCIL | GE_D3D12_SHADER_DIRECT);

    direct_stencil.stencil_op = 1u << 8;
    if (ged3d12_shader_direct_compatible(&direct_stencil))
        fail("depth-fail stencil mutation accepted by direct path");
    direct_stencil.stencil_op = 4u << 16;
    direct_stencil.mask_alpha = 0xff;
    if (ged3d12_shader_direct_compatible(&direct_stencil))
        fail("masked stencil mutation accepted by direct path");
    direct_stencil.mask_alpha = 0;
    direct_stencil.stencil_op = 3u << 16;
    if (ged3d12_shader_direct_compatible(&direct_stencil))
        fail("destination-dependent stencil mutation accepted by direct path");
    direct_stencil.stencil_op = 4u << 16;
    direct_stencil.blend_enable = 1;
    direct_stencil.blend_mode = 2u | (3u << 4);
    if (ged3d12_shader_direct_compatible(&direct_stencil))
        fail("source-alpha blend accepted with a direct stencil write");

    s_state.blend_enable = 1;
    s_state.blend_mode = 2u | (3u << 4) | (1u << 8);
    s_state.ztest_enable = 1;
    s_state.ztest = 6;
    s_state.stencil_test_enable = 1;
    s_state.stencil_test = 2u | (0x55u << 8) | (0xf0u << 16);
    s_state.stencil_op = 1u | (3u << 8) | (5u << 16);
    s_state.logic_op_enable = 1;
    s_state.logic_op = 6;
    s_state.mask_rgb = 0x123456u;
    s_state.mask_alpha = 0x78;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u64(
        "destination fragment state uses manual PSO",
        ged3d12_pipeline_key_hash(&key),
        ged3d12_pipeline_key_hash(&uber));
    if (memcmp(&uber, &key, sizeof(key)) != 0)
        fail("destination fragment state missed the manual PSO");

    s_state.cull_enable = 1;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32("triangle culling", key.cull_mode, D3D12_CULL_MODE_BACK);
    if (ged3d12_pipeline_key_hash(&key) == base_hash)
        fail("pipeline hash ignored relevant state");
    if (!ged3d12_pipeline_uber_compatible(&key))
        fail("cull pipeline key is not ubershader-compatible");

    GeState cull_state = {};
    cull_state.cull_enable = 1;
    GeD3D12TextureBinding no_texture = {};
    GeD3D12ShaderConstants cull_constants = {};
    ged3d12_shader_constants_build(
        &cull_state, 1, 0, 1, &no_texture, &cull_constants);
    check_u32(
        "ubershader triangle culling",
        (uint32_t)cull_constants.config[0] >> 8,
        GE_D3D12_SHADER_DEPTH_RANGE |
        GE_D3D12_SHADER_CULL_BACK |
        GE_D3D12_SHADER_DIRECT);
    ged3d12_shader_constants_build(
        &cull_state, 1, 0, 0, &no_texture, &cull_constants);
    check_u32(
        "ubershader non-triangle culling",
        (uint32_t)cull_constants.config[0] >> 8,
        GE_D3D12_SHADER_DEPTH_RANGE |
        GE_D3D12_SHADER_DIRECT);

    GeD3D12PipelineKey sprite;
    ged3d12_pipeline_key_build(&s_state, 1, &sprite);
    check_u32("sprite culling disabled", sprite.cull_mode, D3D12_CULL_MODE_NONE);

    GeD3D12PipelineKey incompatible = base;
    incompatible.render_target_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (ged3d12_pipeline_uber_compatible(&incompatible))
        fail("incompatible render-target key accepted by ubershader");

    s_state.clear = 1;
    s_state.clear_mode = 0x500;
    ged3d12_pipeline_key_build(&s_state, 0, &key);
    check_u32("clear disables blend", key.blend_enable, 0);
    check_u32(
        "clear writes fetched result", key.color_write_mask,
        D3D12_COLOR_WRITE_ENABLE_ALL);
    check_u32("clear depth enabled", key.depth_enable, 1);
    check_u32("clear depth write", key.depth_write, 1);
    check_u32("clear depth function", key.depth_function,
              D3D12_COMPARISON_FUNC_ALWAYS);
    check_u32("clear culling disabled", key.cull_mode, D3D12_CULL_MODE_NONE);
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
    if (!ged3d12_framebuffer_alias(
            &source, source.address, source.stride, 512, 512, 3, false,
            &alias)) {
        fail("power-of-two framebuffer texture alias rejected");
    }
    check_u32("full framebuffer alias x", alias.x, 0);
    check_u32("full framebuffer alias y", alias.y, 0);
    if (ged3d12_framebuffer_alias(
            &source, alias_address, source.stride, 64, 32, 2, false,
            &alias)) {
        fail("format-reinterpreted framebuffer aliased directly");
    }
    if (!ged3d12_framebuffer_feedback_alias(
            &source, alias_address, source.stride, 64, 32, 7, false,
            &alias)) {
        fail("CLUT32 framebuffer feedback alias rejected");
    }
    check_u32("framebuffer feedback alias x", alias.x, 8);
    check_u32("framebuffer feedback alias y", alias.y, 2);
    if (ged3d12_framebuffer_feedback_alias(
            &source, alias_address, source.stride, 64, 32, 6, false,
            &alias)) {
        fail("non-CLUT32 framebuffer feedback alias accepted");
    }
    for (uint32_t format = 0; format < 3; format++) {
        GeD3D12FramebufferKey source16 = source;
        source16.format = (uint8_t)format;
        uint32_t address16 =
            source16.address + (2u * source16.stride + 8u) * 2u;
        if (!ged3d12_framebuffer_feedback_alias(
                &source16, address16, source16.stride,
                64, 32, 6, false, &alias)) {
            fail("CLUT16 framebuffer feedback alias rejected");
        }
        check_u32("CLUT16 framebuffer feedback alias x", alias.x, 8);
        check_u32("CLUT16 framebuffer feedback alias y", alias.y, 2);
        if (ged3d12_framebuffer_feedback_alias(
                &source16, address16, source16.stride,
                64, 32, 7, false, &alias)) {
            fail("CLUT32 view of 16-bit framebuffer accepted");
        }
    }
    GeD3D12FramebufferKey source16 = source;
    source16.format = 1;
    uint32_t address16 =
        source16.address + (2u * source16.stride + 8u) * 2u;
    if (ged3d12_framebuffer_feedback_alias(
            &source16, address16, source16.stride + 1u,
            64, 32, 6, false, &alias)) {
        fail("CLUT16 framebuffer feedback with bad stride accepted");
    }
    if (ged3d12_framebuffer_feedback_alias(
            &source16, address16, source16.stride,
            64, 32, 6, true, &alias)) {
        fail("swizzled CLUT16 framebuffer feedback accepted");
    }
    if (ged3d12_framebuffer_feedback_alias(
            &source16, source16.address + 1u, source16.stride,
            64, 32, 6, false, &alias)) {
        fail("misaligned CLUT16 framebuffer feedback accepted");
    }
    if (ged3d12_framebuffer_feedback_alias(
            &source16, source16.address, source16.stride,
            source16.stride + 1u, 32, 6, false, &alias)) {
        fail("out-of-bounds CLUT16 framebuffer feedback accepted");
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
    if (!ged3d12_transfer_scale_coordinates(
            4, &source_box, &destination_x, &destination_y)) {
        fail("valid scaled framebuffer transfer rejected");
    }
    check_u32("scaled transfer source left", source_box.left, 12);
    check_u32("scaled transfer source top", source_box.top, 16);
    check_u32("scaled transfer source right", source_box.right, 40);
    check_u32("scaled transfer source bottom", source_box.bottom, 48);
    check_u32("scaled transfer destination x", destination_x, 20);
    check_u32("scaled transfer destination y", destination_y, 24);
    if (ged3d12_transfer_scale_coordinates(
            0, &source_box, &destination_x, &destination_y)) {
        fail("zero framebuffer transfer scale accepted");
    }
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
    s_debug_context.list_id = 7;
    s_debug_context.list_addr = 0x08123400u;
    s_debug_context.draw_serial = 19;
    s_debug_context.draw_pc = 0x08123440u;
    s_debug_context.draw_op = 0x04030003u;
    s_state.shade_gouraud = 0;
    GeVtx a = vertex(1.1f, 2.2f, 3.0f, 0.25f, 4.0f, 5.0f, 0.5f,
                     1, 2, 3, 4);
    GeVtx b = vertex(6.3f, 7.4f, 8.0f, 0.5f, 9.0f, 10.0f, 0.75f,
                     5, 6, 7, 8);
    GeVtx c = vertex(11.5f, 12.6f, 13.0f, 1.0f, 14.0f, 15.0f, 1.0f,
                     300, -1, 128, 256);
    check_u32("triangle hook is GPU-owned", ged3d12_vertex_capture_tri(&a, &b, &c, 1), 1);

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
    if (frame.hash) fail("triangle upload eagerly computed a hash");
    if (!ged3d12_vertex_hash(frame.vertices, frame.vertex_count))
        fail("triangle hash was zero");
    check_u32("triangle batch first", frame.batches[0].first, 0);
    check_u32("triangle batch vertices", frame.batches[0].count, 3);
    check_u32("triangle batch kind", frame.batches[0].primitive,
              GE_D3D12_PRIMITIVE_TRIANGLE);
    check_u32("triangle perspective", frame.batches[0].perspective, 1);
    check_u32("triangle list id", frame.batches[0].list_id, 7);
    check_u32(
        "triangle list address", frame.batches[0].list_addr, 0x08123400u);
    check_u32("triangle draw serial", frame.batches[0].draw_serial, 19);
    check_u32("triangle draw pc", frame.batches[0].draw_pc, 0x08123440u);
    check_u32("triangle draw op", frame.batches[0].draw_op, 0x04030003u);

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

    reset_upload();
    GeVtx triangles[6] = {a, b, c, c, b, a};
    check_u32(
        "triangle-list hook is GPU-owned",
        ged3d12_vertex_capture_tri_list(triangles, 6, 1), 1);
    check_u32(
        "invalid triangle-list hook falls back",
        ged3d12_vertex_capture_tri_list(triangles, 5, 1), 0);
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("triangle-list upload failed");
    } else {
        check_u32(
            "triangle-list vertex count", frame.vertex_count, 6);
        check_u32(
            "triangle-list batch count", frame.batch_count, 1);
        check_u32(
            "triangle-list primitive count",
            frame.primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE], 2);
        const GeD3D12Vertex *list_vertices =
            uploaded_vertices(&frame);
        check_u32(
            "triangle-list first flat color",
            list_vertices[0].rgba, 0xff8000ffu);
        check_u32(
            "triangle-list second flat color",
            list_vertices[3].rgba, 0x04030201u);
    }
}

static void test_sprites(void) {
    reset_upload();
    GeVtx p0 = vertex(10.9f, 20.9f, 2.0f, 0.5f, 2.0f, 3.0f, 0.25f,
                      1, 2, 3, 4);
    GeVtx p1 = vertex(14.2f, 24.7f, 9.0f, 1.5f, 12.0f, 15.0f, 0.75f,
                      10, 20, 30, 40);
    check_u32(
        "perspective sprite hook is GPU-owned",
        ged3d12_vertex_capture_sprite(&p0, &p1, 1), 1);

    GeVtx q0 = vertex(30.2f, 40.8f, 4.0f, 0.25f, 2.0f, 6.0f, 0.2f,
                      5, 6, 7, 8);
    GeVtx q1 = vertex(34.9f, 44.1f, 5.0f, 2.0f, 10.0f, 14.0f, 0.4f,
                      50, 60, 70, 80);
    s_state.tex_filter = 1;
    check_u32(
        "through sprite hook is GPU-owned",
        ged3d12_vertex_capture_sprite(&q0, &q1, 0), 1);
    GeVtx empty = q0;
    empty.x = q0.x + 0.1f;
    empty.y = q0.y + 0.1f;
    check_u32(
        "empty sprite hook falls back",
        ged3d12_vertex_capture_sprite(&q0, &empty, 0), 0);

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

    check_float("linear through sprite ua", vertices[6].u, 2.0f);
    check_float("linear through sprite ub", vertices[7].u, 10.0f);
    check_float("linear through sprite va", vertices[6].v, 6.0f);
    check_float("linear through sprite vb", vertices[8].v, 14.0f);
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
    check_u32(
        "line hook is GPU-owned",
        ged3d12_vertex_capture_line(&a, &b, 1), 1);
    check_u32(
        "point hook is GPU-owned",
        ged3d12_vertex_capture_point(&p, 0), 1);

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

static void test_draw_batching(void) {
    reset_upload();
    memset(&s_state, 0, sizeof(s_state));
    s_state.shade_gouraud = 1;
    s_debug_context.list_id = 11;
    s_debug_context.list_addr = 0x08110000u;
    s_debug_context.draw_serial = 40;
    s_debug_context.draw_pc = 0x08110100u;
    s_debug_context.draw_op = 0x04030006u;
    GeVtx a = vertex(1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 1.0f,
                     1, 2, 3, 4);
    GeVtx b = a;
    GeVtx c = a;
    uint32_t texture_captures = s_texture_captures;
    uint32_t framebuffer_captures = s_framebuffer_captures;
    ged3d12_vertex_capture_tri(&a, &b, &c, 1);
    ged3d12_vertex_capture_tri(&a, &b, &c, 1);
    s_debug_context.draw_serial++;
    s_debug_context.draw_pc += 4;
    ged3d12_vertex_capture_tri(&a, &b, &c, 1);

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("draw batching upload failed");
        return;
    }
    check_u32("batched draw vertex count", frame.vertex_count, 9);
    check_u32("batched draw batch count", frame.batch_count, 2);
    check_u32("batched first draw vertices", frame.batches[0].count, 6);
    check_u32("batched second draw first", frame.batches[1].first, 6);
    check_u32("batched primitive count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE], 3);
    check_u32("batched texture captures",
              s_texture_captures - texture_captures, 2);
    check_u32("batched framebuffer captures",
              s_framebuffer_captures - framebuffer_captures, 2);
}

static void test_storage_growth(void) {
    reset_upload();
    s_state.shade_gouraud = 1;
    GeVtx a = vertex(1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 1.0f,
                     1, 2, 3, 4);
    GeVtx b = a;
    GeVtx c = a;
    for (uint32_t i = 0; i < 80001; i++) {
        s_debug_context.draw_serial = i;
        ged3d12_vertex_capture_tri(&a, &b, &c, 1);
    }

    GeD3D12VertexFrame frame;
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("grown storage upload failed");
        return;
    }
    check_u32("grown vertex count", frame.vertex_count, 240003);
    check_u32("grown batch count", frame.batch_count, 80001);
    check_u32(
        "grown upload size", frame.buffer.SizeInBytes, 240003 * 32);
}

static void test_indexed_triangles(void) {
    reset_upload();
    memset(&s_state, 0, sizeof(s_state));
    s_state.shade_gouraud = 1;
    GeVtx table[4] = {
        vertex(1.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 0.5f, 10, 20, 30, 40),
        vertex(5.0f, 2.0f, 3.0f, 1.0f, 4.0f, 5.0f, 0.5f, 50, 60, 70, 80),
        vertex(1.0f, 6.0f, 3.0f, 1.0f, 4.0f, 5.0f, 0.5f, 90, 100, 110, 120),
        vertex(5.0f, 6.0f, 3.0f, 1.0f, 4.0f, 5.0f, 0.5f, 130, 140, 150, 160),
    };
    const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
    const uint16_t bad[3] = {0, 1, 4};
    check_u32("indexed hook is GPU-owned",
              ged3d12_vertex_capture_tri_indexed(table, 4, indices, 6, 1), 1);
    check_u32("indexed out-of-range index falls back",
              ged3d12_vertex_capture_tri_indexed(table, 4, bad, 3, 1), 0);
    check_u32("indexed partial triangle falls back",
              ged3d12_vertex_capture_tri_indexed(table, 4, indices, 5, 1), 0);
    check_u32("expanded hook after indexed",
              ged3d12_vertex_capture_tri_list(table, 3, 1), 1);
    GeD3D12VertexFrame frame = {};
    if (!ged3d12_vertex_upload_pending(&frame)) {
        fail("indexed upload failed");
        return;
    }
    check_u32("indexed batch count", frame.batch_count, 2);
    check_u32("indexed batch flag", frame.batches[0].flags, GE_D3D12_BATCH_INDEXED);
    check_u32("indexed batch index count", frame.batches[0].count, 6);
    check_u32("indexed batch table count", frame.batches[0].table_count, 4);
    check_u32("indexed batch index offset", frame.batches[0].index_first, 0);
    check_u32("expanded batch not indexed", frame.batches[1].flags, 0);
    check_u32("expanded batch is not merged", frame.batches[1].count, 3);
    check_u32("indexed vertex total", frame.vertex_count, 7);
    check_u32("indexed triangle count",
              frame.primitive_count[GE_D3D12_PRIMITIVE_TRIANGLE], 3);
    const GeD3D12Vertex *vertices = uploaded_vertices(&frame);
    check_u32("indexed table color", vertices[3].rgba, 0xa0968c82u);
    check_u32("indexed upload reached the ring", frame.index_base != 0, 1);
}

int main(void) {
    if (!ged3d12_vertex_capture_init()) {
        fail("vertex capture initialization failed");
    } else {
        test_present_viewports();
        test_shader_constants();
        test_feedback_bounds();
        test_pipeline_keys();
        test_texture_keys();
        test_framebuffer_keys();
        test_triangle();
        test_sprites();
        test_lines_and_points();
        test_draw_batching();
        test_storage_growth();
        test_indexed_triangles();
        ged3d12_vertex_capture_shutdown();
    }

    if (s_failures) {
        fprintf(stderr, "D3D12 vertex tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("D3D12 vertex tests passed\n");
    return 0;
}
