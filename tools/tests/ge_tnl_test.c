#include "ge_tnl.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int s_failures;

static void fail(const char *name) {
    fprintf(stderr, "FAIL: %s\n", name);
    s_failures++;
}

static void check_u32(
    const char *name, uint32_t actual, uint32_t expected) {
    if (actual == expected)
        return;
    fprintf(
        stderr, "FAIL: %s: got 0x%08x, expected 0x%08x\n",
        name, actual, expected);
    s_failures++;
}

static void check_float(
    const char *name, float actual, float expected) {
    if (fabsf(actual - expected) <= 1e-6f)
        return;
    fprintf(
        stderr, "FAIL: %s: got %.9g, expected %.9g\n",
        name, actual, expected);
    s_failures++;
}

static void check_float_tolerance(
    const char *name, float actual, float expected, float tolerance) {
    if (fabsf(actual - expected) <= tolerance)
        return;
    fprintf(
        stderr, "FAIL: %s: got %.9g, expected %.9g (tolerance %.3g)\n",
        name, actual, expected, tolerance);
    s_failures++;
}

static uint32_t encode_float24(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits >> 8;
}

static void test_state_capture(void) {
    GeState source;
    GeTnlState snapshot;
    memset(&source, 0, sizeof(source));
    source.vtype = 0x00123456u;
    source.tex_enable = 1;
    source.lighting_enable = 1;
    source.reverse_normal = 1;
    source.light_mode = 1;
    source.fog_enable = 1;
    source.shade_gouraud = 1;
    source.depth_clip = 1;
    source.cull_enable = 1;
    source.clear = 1;
    source.material_set = 1;
    source.material_alpha_set = 1;
    for (uint32_t i = 0; i < 12; i++) {
        source.world[i] = (float)i + 1.0f;
        source.view[i] = (float)i + 21.0f;
        source.tgen[i] = (float)i + 41.0f;
    }
    for (uint32_t i = 0; i < 16; i++)
        source.proj[i] = (float)i + 61.0f;
    source.tex_scale_u = 1.25f;
    source.tex_scale_v = 2.5f;
    source.tex_off_u = -0.5f;
    source.tex_off_v = 0.75f;
    for (uint32_t i = 0; i < 6; i++)
        source.viewport_raw[i] = encode_float24((float)i - 2.0f);
    source.offsetx = 24;
    source.offsety = 52;
    source.scis_x1 = 3;
    source.scis_y1 = 4;
    source.scis_x2 = 477;
    source.scis_y2 = 267;
    source.tex_w = 256;
    source.tex_h = 128;
    source.tex_map_mode = 0x102u;
    source.tex_shade_ls = 0x301u;
    source.minz = 17;
    source.maxz = 65000;
    source.cull = 1;
    source.fog_end = -3.0f;
    source.fog_slope = 0.25f;
    source.fog_color = 0x00112233u;
    source.material = 0x00445566u;
    source.material_alpha = 0x77;
    source.mat_emissive = 0x00010203u;
    source.mat_diffuse = 0x00040506u;
    source.mat_specular = 0x00070809u;
    source.mat_update = 5;
    source.amb_color = 0x000a0b0cu;
    source.amb_alpha = 0xdd;
    source.spec_coef = 7.5f;
    for (uint32_t i = 0; i < 4; i++) {
        source.light_on[i] = (int)(i & 1u);
        source.light_type[i] = 0x100u + i;
        source.light_spot_exp[i] = (float)i + 0.5f;
        source.light_spot_cut[i] = (float)i + 0.75f;
        source.light_acol[i] = 0x10u + i;
        source.light_dcol[i] = 0x20u + i;
        source.light_scol[i] = 0x30u + i;
        for (uint32_t j = 0; j < 3; j++) {
            source.light_pos[i][j] = (float)(i * 10u + j);
            source.light_dir[i][j] = (float)(i * 20u + j);
            source.light_att[i][j] = (float)(i * 30u + j);
        }
    }

    uint32_t vertex_flags =
        GE_TNL_VERTEX_NORMAL | GE_TNL_VERTEX_TEXCOORD |
        GE_TNL_VERTEX_COLOR | GE_TNL_VERTEX_SKINNED |
        GE_TNL_VERTEX_MORPHED;
    if (!ge_tnl_state_capture(&source, vertex_flags, &snapshot)) {
        fail("valid transform state rejected");
        return;
    }
    check_u32("ABI version", snapshot.abi_version, GE_TNL_ABI_VERSION);
    check_u32("source vertex type", snapshot.source_vtype, source.vtype);
    check_u32("vertex flags", snapshot.vertex_flags, vertex_flags);
    check_u32(
        "state flags", snapshot.flags,
        GE_TNL_STATE_TEXTURE | GE_TNL_STATE_LIGHTING |
        GE_TNL_STATE_REVERSE_NORMAL |
        GE_TNL_STATE_SEPARATE_SPECULAR | GE_TNL_STATE_FOG |
        GE_TNL_STATE_GOURAUD | GE_TNL_STATE_DEPTH_CLIP |
        GE_TNL_STATE_CULL | GE_TNL_STATE_CLEAR |
        GE_TNL_STATE_MATERIAL_COLOR_SET |
        GE_TNL_STATE_MATERIAL_ALPHA_SET);
    if (memcmp(snapshot.world, source.world, sizeof(snapshot.world)) ||
        memcmp(snapshot.view, source.view, sizeof(snapshot.view)) ||
        memcmp(
            snapshot.projection, source.proj,
            sizeof(snapshot.projection)) ||
        memcmp(snapshot.texture, source.tgen, sizeof(snapshot.texture))) {
        fail("transform matrices not captured");
    }
    check_float("texture scale u", snapshot.texture_scale[0], 1.25f);
    check_float("texture offset v", snapshot.texture_offset[1], 0.75f);
    check_float("viewport x scale", snapshot.viewport_scale[0], -2.0f);
    check_float("viewport z center", snapshot.viewport_center[2], 3.0f);
    check_float("viewport x offset", snapshot.viewport_offset[0], 1.5f);
    check_float("viewport y offset", snapshot.viewport_offset[1], 3.25f);
    check_u32("scissor left", (uint32_t)snapshot.scissor[0], 3);
    check_u32("scissor bottom", (uint32_t)snapshot.scissor[3], 267);
    check_u32("texture width", snapshot.texture_width, 256);
    check_u32("texture height", snapshot.texture_height, 128);
    check_u32("minimum depth", snapshot.minimum_depth, 17);
    check_u32("maximum depth", snapshot.maximum_depth, 65000);
    check_u32("material alpha", snapshot.material_alpha, 0x77);
    check_float(
        "specular coefficient", snapshot.specular_coefficient, 7.5f);
    check_u32("light enabled", snapshot.lights[1].enabled, 1);
    check_u32("light type", snapshot.lights[3].type, 0x103);
    check_float(
        "light direction", snapshot.lights[2].direction[1], 41.0f);
    check_u32(
        "light specular color", snapshot.lights[3].specular_color, 0x33);

    source.world[0] = 999.0f;
    source.light_pos[1][0] = 999.0f;
    check_float("snapshot matrix immutability", snapshot.world[0], 1.0f);
    check_float(
        "snapshot light immutability", snapshot.lights[1].position[0],
        10.0f);
}

static void identity_state(GeTnlState *state) {
    memset(state, 0, sizeof(*state));
    state->abi_version = GE_TNL_ABI_VERSION;
    state->flags = GE_TNL_STATE_GOURAUD;
    state->world[0] = 1.0f;
    state->world[4] = 1.0f;
    state->world[8] = 1.0f;
    state->view[0] = 1.0f;
    state->view[4] = 1.0f;
    state->view[8] = 1.0f;
    state->projection[0] = 1.0f;
    state->projection[5] = 1.0f;
    state->projection[10] = 1.0f;
    state->projection[15] = 1.0f;
    state->viewport_scale[0] = 240.0f;
    state->viewport_scale[1] = -136.0f;
    state->viewport_scale[2] = 32767.5f;
    state->viewport_center[0] = 240.0f;
    state->viewport_center[1] = 136.0f;
    state->viewport_center[2] = 32767.5f;
}

static void compare_reference_vertex(
    const char *name, const GeTnlState *state,
    const GeTnlVertex *vertex, GeTnlReferenceVertex *cpu_out) {
    GeTnlGpuState gpu_state;
    GeTnlReferenceVertex cpu;
    GeTnlReferenceVertex gpu;
    char field[96];
    ge_tnl_gpu_state_build(state, &gpu_state);
    if (!ge_tnl_reference_transform(state, vertex, &cpu)) {
        snprintf(field, sizeof(field), "%s CPU reference rejected", name);
        fail(field);
        return;
    }
    if (!ge_tnl_gpu_reference_transform(&gpu_state, vertex, &gpu)) {
        snprintf(field, sizeof(field), "%s GPU reference rejected", name);
        fail(field);
        return;
    }
    for (uint32_t i = 0; i < 3; i++) {
        snprintf(field, sizeof(field), "%s screen[%u]", name, i);
        check_float_tolerance(field, gpu.screen[i], cpu.screen[i], 2e-5f);
    }
    for (uint32_t i = 0; i < 2; i++) {
        snprintf(field, sizeof(field), "%s uv[%u]", name, i);
        check_float_tolerance(field, gpu.uv[i], cpu.uv[i], 2e-5f);
    }
    snprintf(field, sizeof(field), "%s reciprocal w", name);
    check_float_tolerance(
        field, gpu.reciprocal_w, cpu.reciprocal_w, 2e-6f);
    snprintf(field, sizeof(field), "%s fog", name);
    check_float_tolerance(field, gpu.fog, cpu.fog, 2e-6f);
    for (uint32_t i = 0; i < 4; i++) {
        snprintf(field, sizeof(field), "%s color[%u]", name, i);
        check_float_tolerance(field, gpu.color[i], cpu.color[i], 2e-5f);
    }
    if (cpu_out) *cpu_out = cpu;
}

static void test_gpu_state_layout(void) {
    GeTnlState state;
    GeTnlGpuState gpu;
    identity_state(&state);
    for (uint32_t i = 0; i < 12; i++) {
        state.world[i] = (float)i + 1.0f;
        state.view[i] = (float)i + 21.0f;
        state.texture[i] = (float)i + 41.0f;
    }
    for (uint32_t i = 0; i < 16; i++)
        state.projection[i] = (float)i + 61.0f;
    state.flags |= GE_TNL_STATE_TEXTURE | GE_TNL_STATE_LIGHTING;
    state.texture_scale[0] = 2.0f;
    state.texture_scale[1] = 3.0f;
    state.texture_offset[0] = 4.0f;
    state.texture_offset[1] = 5.0f;
    state.texture_width = 128;
    state.texture_height = 64;
    state.lights[2].enabled = 1;
    state.lights[2].type = 0x102u;
    state.lights[2].direction[1] = 7.0f;
    state.lights[2].spot_exponent = 8.0f;
    state.lights[2].spot_cutoff = 9.0f;
    state.lights[2].specular_color = 0x00112233u;
    ge_tnl_gpu_state_build(&state, &gpu);

    check_u32(
        "packed GPU state size", (uint32_t)sizeof(GeTnlGpuState), 608);
    check_u32(
        "packed view offset", (uint32_t)offsetof(GeTnlGpuState, view), 48);
    check_u32(
        "packed projection offset",
        (uint32_t)offsetof(GeTnlGpuState, projection), 96);
    check_u32(
        "packed texture params offset",
        (uint32_t)offsetof(GeTnlGpuState, texture_params), 208);
    check_u32(
        "packed config offset",
        (uint32_t)offsetof(GeTnlGpuState, config), 288);
    check_u32(
        "packed lights offset",
        (uint32_t)offsetof(GeTnlGpuState, light_position), 352);
    check_u32(
        "packed light colors offset",
        (uint32_t)offsetof(GeTnlGpuState, light_colors), 544);
    check_float("packed world transpose 0", gpu.world[0][0], 1.0f);
    check_float("packed world transpose 1", gpu.world[0][1], 4.0f);
    check_float("packed world translation", gpu.world[2][3], 12.0f);
    check_float(
        "packed projection transpose", gpu.projection[1][2], 70.0f);
    check_float("packed texture width", gpu.viewport_offset_size[2], 128.0f);
    check_u32("packed enabled lights", gpu.material_config[2], 1u << 2);
    check_u32("packed light type", gpu.light_colors[2][3], 0x102u);
    check_float(
        "packed spot exponent", gpu.light_direction[2][3], 8.0f);
    check_float(
        "packed spot cutoff", gpu.light_attenuation[2][3], 9.0f);
}

static void test_transform_differential(void) {
    GeTnlState state;
    GeTnlVertex vertex;
    GeTnlReferenceVertex output;
    identity_state(&state);
    memset(&vertex, 0, sizeof(vertex));
    vertex.position[0] = 0.25f;
    vertex.position[1] = -0.5f;
    vertex.position[2] = 0.75f;
    vertex.normal[0] = 0.25f;
    vertex.normal[1] = 0.5f;
    vertex.normal[2] = 1.0f;
    vertex.texcoord[0] = 0.2f;
    vertex.texcoord[1] = 0.75f;
    vertex.color[0] = 31;
    vertex.color[1] = 97;
    vertex.color[2] = 193;
    vertex.color[3] = 211;

    state.world[0] = 2.0f;
    state.world[4] = 0.75f;
    state.world[8] = 1.5f;
    state.world[9] = 0.125f;
    state.world[10] = -0.25f;
    state.world[11] = 0.5f;
    state.view[9] = -0.25f;
    state.view[10] = 0.125f;
    state.view[11] = -0.5f;
    state.projection[0] = 0.8f;
    state.projection[5] = 1.1f;
    state.projection[10] = 0.5f;
    state.projection[11] = 0.2f;
    state.viewport_scale[0] = 160.0f;
    state.viewport_scale[1] = -120.0f;
    state.viewport_scale[2] = 40000.0f;
    state.viewport_center[0] = 200.0f;
    state.viewport_center[1] = 120.0f;
    state.viewport_center[2] = 32000.0f;
    state.viewport_offset[0] = 1.5f;
    state.viewport_offset[1] = 2.25f;
    state.texture_width = 256;
    state.texture_height = 128;
    state.texture_scale[0] = 2.0f;
    state.texture_scale[1] = -0.5f;
    state.texture_offset[0] = 0.125f;
    state.texture_offset[1] = 1.0f;
    compare_reference_vertex(
        "matrix and direct UV", &state, &vertex, &output);
    check_float_tolerance(
        "matrix reference reciprocal w", output.reciprocal_w,
        1.0f / 1.225f, 2e-6f);
    check_float_tolerance(
        "direct UV reference u", output.uv[0],
        (0.2f * 2.0f + 0.125f) * 256.0f / 1.225f, 2e-5f);

    state.texture[0] = 1.5f;
    state.texture[4] = 0.5f;
    state.texture[8] = 0.25f;
    state.texture[9] = 0.1f;
    state.texture[1] = -0.25f;
    state.texture[5] = 1.25f;
    state.texture[10] = 0.2f;
    state.texture[2] = 0.1f;
    state.texture[6] = 0.2f;
    state.texture[8] = 0.3f;
    state.texture[11] = 1.0f;
    for (uint32_t source = 0; source < 4; source++) {
        char name[48];
        state.texture_map_mode = 1u | (source << 8u);
        snprintf(name, sizeof(name), "texture matrix source %u", source);
        compare_reference_vertex(name, &state, &vertex, NULL);
    }

    state.texture_map_mode = 2u;
    state.texture_shade_lights = 0u | (1u << 8u);
    state.lights[0].position[2] = 2.0f;
    state.lights[1].position[0] = 1.0f;
    state.lights[1].position[1] = 1.0f;
    compare_reference_vertex(
        "environment texture mapping", &state, &vertex, NULL);

    state.texture_map_mode = 0u;
    state.flags |=
        GE_TNL_STATE_LIGHTING | GE_TNL_STATE_MATERIAL_ALPHA_SET;
    state.material_color = 0x00406080u;
    state.material_alpha = 173;
    state.material_emissive = 0x00010203u;
    state.material_diffuse = 0x00c08040u;
    state.material_specular = 0x006090c0u;
    state.ambient_color = 0x00203040u;
    state.ambient_alpha = 201;
    state.specular_coefficient = 3.5f;
    state.lights[0].enabled = 1;
    state.lights[0].type = 0u;
    state.lights[0].position[0] = 0.25f;
    state.lights[0].position[1] = 0.5f;
    state.lights[0].position[2] = 1.0f;
    state.lights[0].ambient_color = 0x00102030u;
    state.lights[0].diffuse_color = 0x00a08060u;
    state.lights[1].enabled = 1;
    state.lights[1].type = 0x101u;
    state.lights[1].position[0] = 2.0f;
    state.lights[1].position[1] = 1.0f;
    state.lights[1].position[2] = 3.0f;
    state.lights[1].attenuation[0] = 0.5f;
    state.lights[1].attenuation[1] = 0.25f;
    state.lights[1].attenuation[2] = 0.125f;
    state.lights[1].diffuse_color = 0x0080a0c0u;
    state.lights[1].specular_color = 0x00d0b090u;
    state.lights[2].enabled = 1;
    state.lights[2].type = 2u;
    state.lights[2].position[2] = 1.0f;
    state.lights[2].diffuse_color = 0x004080c0u;
    for (uint32_t update = 0; update < 8; update++) {
        char name[48];
        state.material_update = update;
        snprintf(name, sizeof(name), "lighting material update %u", update);
        compare_reference_vertex(name, &state, &vertex, &output);
    }
    if (output.color[0] < 0.0f || output.color[0] > 1.0f ||
        output.color[3] < 0.0f || output.color[3] > 1.0f) {
        fail("lighting output not clamped");
    }
}

static void test_fog_depth_and_indexed_reuse(void) {
    GeTnlState state;
    GeTnlVertex vertices[3];
    GeTnlReferenceVertex output;
    uint16_t indices[6] = {0, 1, 0, 0, 1, 2};
    identity_state(&state);
    memset(vertices, 0, sizeof(vertices));
    for (uint32_t i = 0; i < 3; i++) {
        vertices[i].position[0] = (float)i * 0.1f;
        vertices[i].position[2] = 0.5f;
        vertices[i].normal[2] = 1.0f;
        vertices[i].color[0] = (uint8_t)(30u + i);
        vertices[i].color[1] = (uint8_t)(60u + i);
        vertices[i].color[2] = (uint8_t)(90u + i);
        vertices[i].color[3] = 255;
    }
    state.flags |= GE_TNL_STATE_FOG;
    state.fog_end = 0.25f;
    state.fog_slope = 1.0f;
    compare_reference_vertex(
        "finite fog", &state, &vertices[0], &output);
    check_float("finite fog value", output.fog, 0.75f);

    state.fog_end = INFINITY;
    state.fog_slope = 1.0f;
    compare_reference_vertex(
        "positive infinite fog end", &state, &vertices[0], &output);
    check_float("positive infinite fog value", output.fog, 1.0f);
    state.fog_end = -INFINITY;
    compare_reference_vertex(
        "negative infinite fog end", &state, &vertices[0], &output);
    check_float("negative infinite fog value", output.fog, 0.0f);
    state.fog_end = NAN;
    state.fog_slope = 0.0f;
    compare_reference_vertex(
        "NaN fog end", &state, &vertices[0], &output);
    check_float("NaN fog end value", output.fog, 0.0f);
    state.fog_end = 0.25f;
    state.fog_slope = INFINITY;
    compare_reference_vertex(
        "infinite fog slope", &state, &vertices[0], &output);
    check_float("infinite fog slope value", output.fog, 1.0f);

    state.flags &= ~GE_TNL_STATE_FOG;
    state.viewport_scale[2] = 100000.0f;
    state.viewport_center[2] = 60000.0f;
    compare_reference_vertex(
        "maximum depth clamp", &state, &vertices[0], &output);
    check_float("maximum depth value", output.screen[2], 65535.0f);
    state.viewport_scale[2] = -100000.0f;
    state.viewport_center[2] = 0.0f;
    compare_reference_vertex(
        "minimum depth clamp", &state, &vertices[0], &output);
    check_float("minimum depth value", output.screen[2], 0.0f);

    GeTnlGpuState gpu_state;
    GeTnlReferenceVertex first;
    GeTnlReferenceVertex reused;
    ge_tnl_gpu_state_build(&state, &gpu_state);
    if (!ge_tnl_gpu_reference_transform(
            &gpu_state, &vertices[indices[0]], &first) ||
        !ge_tnl_gpu_reference_transform(
            &gpu_state, &vertices[indices[2]], &reused)) {
        fail("indexed reference transform rejected");
    } else if (memcmp(&first, &reused, sizeof(first)) != 0) {
        fail("reused canonical index changed transform output");
    }
    state.viewport_scale[2] = 32767.5f;
    state.viewport_center[2] = 32767.5f;
    GeTnlDraw draw = {
        vertices, indices, &state, 3, 6, GE_TNL_TRIANGLES, 0,
    };
    check_u32(
        "indexed reuse remains eligible",
        ge_tnl_classify(&draw), GE_TNL_ELIGIBLE);
}

static void test_eligibility(void) {
    GeTnlVertex vertices[3];
    GeTnlState state;
    uint16_t indices[3] = {0, 1, 2};
    memset(vertices, 0, sizeof(vertices));
    identity_state(&state);
    vertices[0].position[0] = -0.5f;
    vertices[1].position[0] = 0.5f;
    vertices[2].position[1] = 0.5f;
    GeTnlDraw draw = {
        vertices, indices, &state, 3, 3, GE_TNL_TRIANGLES, 0,
    };
    check_u32(
        "eligible triangle draw", ge_tnl_classify(&draw),
        GE_TNL_ELIGIBLE);
    draw.primitive = GE_TNL_TRIANGLE_STRIP;
    check_u32(
        "eligible triangle strip", ge_tnl_classify(&draw),
        GE_TNL_ELIGIBLE);
    draw.primitive = GE_TNL_TRIANGLE_FAN;
    check_u32(
        "eligible triangle fan", ge_tnl_classify(&draw),
        GE_TNL_ELIGIBLE);
    draw.primitive = GE_TNL_POINTS;
    check_u32(
        "eligible point draw", ge_tnl_classify(&draw),
        GE_TNL_ELIGIBLE);
    vertices[2].position[2] = -2.0f;
    check_u32(
        "unsafe point draw declines to CPU",
        ge_tnl_classify(&draw), GE_TNL_DECLINE_CLIP);
    vertices[2].position[2] = 0.0f;
    draw.primitive = GE_TNL_SPRITES;
    draw.index_count = 2;
    check_u32(
        "eligible sprite draw", ge_tnl_classify(&draw),
        GE_TNL_ELIGIBLE);
    vertices[1].position[2] = -2.0f;
    check_u32(
        "unsafe sprite draw declines to CPU",
        ge_tnl_classify(&draw), GE_TNL_DECLINE_CLIP);
    vertices[1].position[2] = 0.0f;
    draw.index_count = 3;
    draw.primitive = GE_TNL_LINES;
    draw.index_count = 2;
    check_u32(
        "line draw retains CPU path", ge_tnl_classify(&draw),
        GE_TNL_DECLINE_PRIMITIVE);
    draw.primitive = GE_TNL_LINE_STRIP;
    check_u32(
        "line strip retains CPU path", ge_tnl_classify(&draw),
        GE_TNL_DECLINE_PRIMITIVE);
    draw.index_count = 3;
    draw.primitive = GE_TNL_TRIANGLES;
    state.flags |= GE_TNL_STATE_CLEAR;
    check_u32(
        "clear draw declined", ge_tnl_classify(&draw),
        GE_TNL_DECLINE_CLEAR);
    state.flags &= ~GE_TNL_STATE_CLEAR;
    state.flags &= ~GE_TNL_STATE_GOURAUD;
    check_u32(
        "flat draw declined", ge_tnl_classify(&draw),
        GE_TNL_DECLINE_FLAT);
    state.flags |= GE_TNL_STATE_GOURAUD;
    vertices[2].position[2] = -2.0f;
    check_u32(
        "near-clipped draw requires preprocessing",
        ge_tnl_classify(&draw), GE_TNL_ELIGIBLE_PREPROCESS);
    vertices[2].position[2] = 0.0f;
    indices[2] = 3;
    check_u32(
        "out-of-range index declined", ge_tnl_classify(&draw),
        GE_TNL_DECLINE_INVALID);
}

static void test_preprocess_reference(void) {
    GeTnlState state;
    GeTnlVertex vertices[3];
    GeTnlClipVertex clip[3];
    GeTnlScreenVertex screen[6];
    identity_state(&state);
    state.flags |= GE_TNL_STATE_DEPTH_CLIP;
    state.texture_scale[0] = 1.0f;
    state.texture_scale[1] = 1.0f;
    state.texture_width = 1;
    state.texture_height = 1;
    memset(vertices, 0, sizeof(vertices));
    vertices[0].position[0] = -0.5f;
    vertices[0].position[1] = -0.5f;
    vertices[1].position[0] = 0.5f;
    vertices[1].position[1] = -0.5f;
    vertices[2].position[1] = 0.5f;
    for (uint32_t i = 0; i < 3; i++) {
        vertices[i].normal[2] = 1.0f;
        vertices[i].texcoord[0] = (float)i;
        vertices[i].texcoord[1] = (float)i * 0.25f;
        vertices[i].color[0] = (uint8_t)(1u + 99u * i);
        vertices[i].color[1] = (uint8_t)(20u + 10u * i);
        vertices[i].color[2] = (uint8_t)(40u + 20u * i);
        vertices[i].color[3] = 255;
        if (!ge_tnl_reference_clip_transform(
                &state, &vertices[i], &clip[i])) {
            fail("safe clip transform");
        }
    }

    uint32_t count =
        ge_tnl_reference_preprocess_triangle(&state, clip, screen);
    check_u32("safe preprocess vertex count", count, 3);
    for (uint32_t i = 0; i < 3; i++) {
        GeTnlReferenceVertex expected;
        char name[96];
        if (!ge_tnl_reference_transform(
                &state, &vertices[i], &expected)) {
            fail("safe projected reference");
            continue;
        }
        for (uint32_t component = 0; component < 3; component++) {
            snprintf(
                name, sizeof(name), "safe screen %u.%u",
                i, component);
            check_float(
                name, screen[i].position[component],
                expected.screen[component]);
        }
        snprintf(name, sizeof(name), "safe reciprocal w %u", i);
        check_float(
            name, screen[i].position[3], expected.reciprocal_w);
        for (uint32_t component = 0; component < 2; component++) {
            snprintf(
                name, sizeof(name), "safe texcoord %u.%u",
                i, component);
            check_float(
                name, screen[i].texcoord[component],
                expected.uv[component]);
        }
    }

    vertices[0].position[2] = -2.0f;
    for (uint32_t i = 0; i < 3; i++) {
        if (!ge_tnl_reference_clip_transform(
                &state, &vertices[i], &clip[i])) {
            fail("near clip transform");
        }
    }
    count = ge_tnl_reference_preprocess_triangle(
        &state, clip, screen);
    check_u32("near-clipped quad vertex count", count, 6);
    check_float("near intersection depth", screen[0].position[2], 0.0f);
    check_float("near intersection u", screen[0].texcoord[0], 0.5f);
    check_u32(
        "near intersection red", screen[0].color & 255u, 50u);
    check_u32(
        "near fan shared vertex",
        memcmp(&screen[0], &screen[3], sizeof(screen[0])), 0);

    vertices[0].position[0] = 100.0f;
    if (!ge_tnl_reference_clip_transform(
            &state, &vertices[0], &clip[0])) {
        fail("created-range clip transform");
    }
    count = ge_tnl_reference_preprocess_triangle(
        &state, clip, screen);
    check_u32("out-of-range created vertices dropped", count, 0);

    vertices[0].position[0] = -0.5f;
    for (uint32_t i = 0; i < 3; i++) {
        vertices[i].position[2] = -2.0f;
        if (!ge_tnl_reference_clip_transform(
                &state, &vertices[i], &clip[i])) {
            fail("behind-near clip transform");
        }
    }
    count = ge_tnl_reference_preprocess_triangle(
        &state, clip, screen);
    check_u32("fully behind triangle dropped", count, 0);

    vertices[0].position[0] = 100.0f;
    vertices[0].position[2] = 0.0f;
    vertices[1].position[2] = 0.0f;
    vertices[2].position[2] = 0.0f;
    for (uint32_t i = 0; i < 3; i++) {
        if (!ge_tnl_reference_clip_transform(
                &state, &vertices[i], &clip[i])) {
            fail("original-range clip transform");
        }
    }
    count = ge_tnl_reference_preprocess_triangle(
        &state, clip, screen);
    check_u32("out-of-range original vertex drops triangle", count, 0);

    vertices[0].position[0] = -0.5f;
    vertices[0].position[2] = 2.0f;
    if (!ge_tnl_reference_clip_transform(
            &state, &vertices[0], &clip[0])) {
        fail("depth-range clip transform");
    }
    state.flags &= ~GE_TNL_STATE_DEPTH_CLIP;
    count = ge_tnl_reference_preprocess_triangle(
        &state, clip, screen);
    check_u32("disabled depth clip drops any outside vertex", count, 0);

    state.flags |= GE_TNL_STATE_DEPTH_CLIP;
    for (uint32_t i = 0; i < 3; i++) {
        vertices[i].position[2] = 2.0f;
        if (!ge_tnl_reference_clip_transform(
                &state, &vertices[i], &clip[i])) {
            fail("same-depth clip transform");
        }
    }
    count = ge_tnl_reference_preprocess_triangle(
        &state, clip, screen);
    check_u32("same positive depth side drops triangle", count, 0);
}

static void test_point_expansion_reference(void) {
    GeTnlState state;
    GeTnlVertex vertex;
    GeTnlScreenVertex output[6];
    identity_state(&state);
    state.texture_width = 1;
    state.texture_height = 1;
    state.texture_scale[0] = 1.0f;
    state.texture_scale[1] = 1.0f;
    memset(&vertex, 0, sizeof(vertex));
    vertex.position[0] = 0.25f;
    vertex.position[1] = -0.25f;
    vertex.texcoord[0] = 0.5f;
    vertex.texcoord[1] = 0.75f;
    vertex.color[0] = 10;
    vertex.color[1] = 20;
    vertex.color[2] = 30;
    vertex.color[3] = 40;
    if (!ge_tnl_reference_expand_point(&state, &vertex, output)) {
        fail("point expansion reference rejected");
        return;
    }
    static const float expected_xy[6][2] = {
        {300.0f, 170.0f},
        {301.0f, 170.0f},
        {300.0f, 171.0f},
        {301.0f, 170.0f},
        {301.0f, 171.0f},
        {300.0f, 171.0f},
    };
    for (uint32_t i = 0; i < 6; i++) {
        char name[96];
        snprintf(name, sizeof(name), "point corner %u x", i);
        check_float(name, output[i].position[0], expected_xy[i][0]);
        snprintf(name, sizeof(name), "point corner %u y", i);
        check_float(name, output[i].position[1], expected_xy[i][1]);
        snprintf(name, sizeof(name), "point corner %u u", i);
        check_float(name, output[i].texcoord[0], 0.5f);
        snprintf(name, sizeof(name), "point corner %u color", i);
        check_u32(name, output[i].color, 0x281e140au);
    }
}

static void test_sprite_expansion_reference(void) {
    GeTnlState state;
    GeTnlVertex controls[2];
    GeTnlScreenVertex output[6];
    identity_state(&state);
    state.viewport_scale[0] = 4.0f;
    state.viewport_scale[1] = 4.0f;
    state.viewport_center[0] = 10.0f;
    state.viewport_center[1] = 10.0f;
    state.texture_width = 1;
    state.texture_height = 1;
    state.texture_scale[0] = 1.0f;
    state.texture_scale[1] = 1.0f;
    memset(controls, 0, sizeof(controls));
    controls[0].position[0] = -0.5f;
    controls[0].position[1] = -0.5f;
    controls[0].texcoord[0] = 2.0f;
    controls[0].texcoord[1] = 4.0f;
    controls[1].position[0] = 0.5f;
    controls[1].position[1] = 0.5f;
    controls[1].texcoord[0] = 10.0f;
    controls[1].texcoord[1] = 12.0f;
    controls[1].color[0] = 10;
    controls[1].color[1] = 20;
    controls[1].color[2] = 30;
    controls[1].color[3] = 40;
    if (!ge_tnl_reference_expand_sprite(
            &state, controls, 0, output)) {
        fail("nearest sprite expansion rejected");
        return;
    }
    static const float expected_xy[6][2] = {
        {8.0f, 8.0f}, {12.0f, 8.0f}, {8.0f, 12.0f},
        {12.0f, 8.0f}, {12.0f, 12.0f}, {8.0f, 12.0f},
    };
    static const float nearest_uv[6][2] = {
        {1.0f, 3.0f}, {9.0f, 3.0f}, {1.0f, 11.0f},
        {9.0f, 3.0f}, {9.0f, 11.0f}, {1.0f, 11.0f},
    };
    for (uint32_t i = 0; i < 6; i++) {
        char name[96];
        snprintf(name, sizeof(name), "sprite corner %u x", i);
        check_float(name, output[i].position[0], expected_xy[i][0]);
        snprintf(name, sizeof(name), "sprite corner %u y", i);
        check_float(name, output[i].position[1], expected_xy[i][1]);
        snprintf(name, sizeof(name), "sprite corner %u u", i);
        check_float(name, output[i].texcoord[0], nearest_uv[i][0]);
        snprintf(name, sizeof(name), "sprite corner %u v", i);
        check_float(name, output[i].texcoord[1], nearest_uv[i][1]);
        snprintf(name, sizeof(name), "sprite corner %u rw", i);
        check_float(name, output[i].position[3], 1.0f);
        snprintf(name, sizeof(name), "sprite corner %u color", i);
        check_u32(name, output[i].color, 0x281e140au);
    }
    if (!ge_tnl_reference_expand_sprite(
            &state, controls, 1, output)) {
        fail("linear sprite expansion rejected");
        return;
    }
    check_float("linear sprite low u", output[0].texcoord[0], 2.0f);
    check_float("linear sprite low v", output[0].texcoord[1], 4.0f);
    check_float("linear sprite high u", output[4].texcoord[0], 10.0f);
    check_float("linear sprite high v", output[4].texcoord[1], 12.0f);
}

static void test_validation_abi(void) {
    GeTnlReferenceVertex reference;
    GeTnlValidationVertex expected;
    GeTnlValidationVertex actual;
    uint32_t mismatch = UINT32_MAX;
    memset(&reference, 0, sizeof(reference));
    reference.screen[0] = 12.5f;
    reference.screen[1] = -3.25f;
    reference.screen[2] = 1024.0f;
    reference.reciprocal_w = 0.5f;
    reference.uv[0] = 4.0f;
    reference.uv[1] = 8.0f;
    reference.fog = 0.75f;
    reference.color[0] = 1.0f;
    reference.color[1] = 128.0f / 255.0f;
    reference.color[2] = 0.25f;
    reference.color[3] = 1.0f;
    ge_tnl_validation_pack(&reference, &expected);
    actual = expected;

    check_u32(
        "validation vertex size",
        (uint32_t)sizeof(GeTnlValidationVertex), 48);
    check_u32(
        "validation UV offset",
        (uint32_t)offsetof(GeTnlValidationVertex, uv_fog), 16);
    check_u32(
        "validation color offset",
        (uint32_t)offsetof(GeTnlValidationVertex, color), 32);
    check_u32(
        "clip vertex size",
        (uint32_t)sizeof(GeTnlClipVertex), 48);
    check_u32(
        "clip color offset",
        (uint32_t)offsetof(GeTnlClipVertex, color), 32);
    check_u32(
        "screen vertex size",
        (uint32_t)sizeof(GeTnlScreenVertex), 32);
    check_u32(
        "screen color offset",
        (uint32_t)offsetof(GeTnlScreenVertex, color), 28);
    check_float(
        "validation packed reciprocal W",
        expected.screen_reciprocal_w[3], reference.reciprocal_w);
    check_float(
        "validation packed fog", expected.uv_fog[2], reference.fog);
    if (!ge_tnl_validation_compare(&expected, &actual, &mismatch))
        fail("identical validation output rejected");
    if (!ge_tnl_validation_compare_exact(&expected, &actual, &mismatch))
        fail("identical exact validation output rejected");

    actual.uv_fog[0] = nextafterf(actual.uv_fog[0], INFINITY);
    if (!ge_tnl_validation_compare(&expected, &actual, &mismatch))
        fail("one-ULP validation difference rejected");
    if (ge_tnl_validation_compare_exact(&expected, &actual, &mismatch))
        fail("one-ULP exact validation difference accepted");
    check_u32("exact validation mismatch component", mismatch, 4);
    actual.uv_fog[0] += 0.125f;
    if (ge_tnl_validation_compare(&expected, &actual, &mismatch))
        fail("large validation difference accepted");
    check_u32("validation mismatch component", mismatch, 4);

    actual = expected;
    actual.color[0] -= 1.0f / 255.0f;
    if (ge_tnl_validation_compare(&expected, &actual, &mismatch))
        fail("adjacent quantized color accepted");
    check_u32("validation color component", mismatch, 8);

    actual = expected;
    actual.uv_fog[3] = 1.0f;
    if (!ge_tnl_validation_compare_exact(&expected, &actual, &mismatch))
        fail("validation padding difference rejected");
}

static void test_abi_shape(void) {
    GeTnlVertex vertices[1];
    uint16_t indices[1] = {0};
    GeState source;
    GeTnlState state;
    memset(vertices, 0, sizeof(vertices));
    memset(&source, 0, sizeof(source));
    memset(&state, 0, sizeof(state));
    GeTnlDraw draw = {
        vertices, indices, &state, 1, 1, GE_TNL_TRIANGLES, 0,
    };
    check_u32("canonical vertex size", (uint32_t)sizeof(GeTnlVertex), 36);
    check_u32("draw vertex count", draw.vertex_count, 1);
    check_u32("draw index count", draw.index_count, 1);
    check_u32("draw primitive", draw.primitive, GE_TNL_TRIANGLES);
    if (ge_tnl_state_capture(NULL, 0, &state))
        fail("null source accepted");
    if (ge_tnl_state_capture(&source, 0, NULL))
        fail("null destination accepted");
}

int main(void) {
    test_state_capture();
    test_gpu_state_layout();
    test_transform_differential();
    test_fog_depth_and_indexed_reuse();
    test_eligibility();
    test_preprocess_reference();
    test_point_expansion_reference();
    test_sprite_expansion_reference();
    test_validation_abi();
    test_abi_shape();
    if (s_failures) {
        fprintf(stderr, "%d transform ABI test(s) failed\n", s_failures);
        return 1;
    }
    puts("transform ABI tests passed");
    return 0;
}
