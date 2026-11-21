#define _POSIX_C_SOURCE 200809L

#include "ge_tnl.h"
#include "perf_stats.h"
#include "platform.h"
#include "recomp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    TEST_RAM_SIZE = 0x20000,
    TEST_VERTEX_ADDRESS = SR_RAM_BASE + 0x1000,
    TEST_TEXTURE_ADDRESS = SR_RAM_BASE + 0x8000,
    GE_COMMAND_PRIM = 0x04,
};

uint8_t *g_mem;
uint8_t g_scratchpad[SR_SCRATCHPAD_SIZE];
volatile uint32_t sr_perf_alternate_mask;

static int s_failures;
static int s_oor_accesses;
static int s_transform_accept = 1;
static uint32_t s_transform_calls;
static uint32_t s_triangle_calls;
static uint32_t s_point_calls;
static uint32_t s_sprite_calls;
static uint32_t s_line_calls;
static uint32_t s_draw_flags;
static uint32_t s_primitive;
static GeTnlVertex s_vertices[16];
static uint16_t s_indices[48];
static GeTnlState s_state;
static uint32_t s_vertex_count;
static uint32_t s_index_count;
static uint64_t s_perf_counters[SR_PERF_COUNTER_COUNT];
static GeVtx s_triangle[3];
static GeVtx s_points[3];
static GeVtx s_sprite[2];
static GeVtx s_line[2];

void sr_oor(uint32_t address, uint32_t value, int store) {
    (void)value;
    (void)store;
    fprintf(stderr, "FAIL: out-of-range guest access at 0x%08x\n", address);
    s_oor_accesses++;
}

int sr_perf_enabled(void) {
    return 0;
}

SrPerfCategory sr_perf_switch(SrPerfCategory category) {
    return category;
}

void sr_perf_counter_add(SrPerfCounter counter, uint64_t value) {
    if (counter >= 0 && counter < SR_PERF_COUNTER_COUNT)
        s_perf_counters[counter] += value;
}

uint64_t sr_time_now(void) {
    return 0;
}

uint64_t sr_time_frequency(void) {
    return 1;
}

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

static uint32_t encode_float24(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits >> 8;
}

static void put_u16(uint8_t *destination, size_t offset, uint16_t value) {
    memcpy(destination + offset, &value, sizeof(value));
}

static void put_u32(uint8_t *destination, size_t offset, uint32_t value) {
    memcpy(destination + offset, &value, sizeof(value));
}

static void put_float(uint8_t *destination, size_t offset, float value) {
    memcpy(destination + offset, &value, sizeof(value));
}

static void put_vec2(
    uint8_t *destination, size_t offset, const float value[2]) {
    put_float(destination, offset + 0, value[0]);
    put_float(destination, offset + 4, value[1]);
}

static void put_vec3(
    uint8_t *destination, size_t offset, const float value[3]) {
    put_float(destination, offset + 0, value[0]);
    put_float(destination, offset + 4, value[1]);
    put_float(destination, offset + 8, value[2]);
}

static void set_identity43(float matrix[12]) {
    memset(matrix, 0, 12 * sizeof(*matrix));
    matrix[0] = 1.0f;
    matrix[4] = 1.0f;
    matrix[8] = 1.0f;
}

static void set_identity44(float matrix[16]) {
    memset(matrix, 0, 16 * sizeof(*matrix));
    matrix[0] = 1.0f;
    matrix[5] = 1.0f;
    matrix[10] = 1.0f;
    matrix[15] = 1.0f;
}

static void transform_position(
    const float matrix[12], const float input[3], float output[3]) {
    output[0] =
        matrix[0] * input[0] + matrix[3] * input[1] +
        matrix[6] * input[2] + matrix[9];
    output[1] =
        matrix[1] * input[0] + matrix[4] * input[1] +
        matrix[7] * input[2] + matrix[10];
    output[2] =
        matrix[2] * input[0] + matrix[5] * input[1] +
        matrix[8] * input[2] + matrix[11];
}

static void transform_normal(
    const float matrix[12], const float input[3], float output[3]) {
    output[0] =
        matrix[0] * input[0] + matrix[3] * input[1] +
        matrix[6] * input[2];
    output[1] =
        matrix[1] * input[0] + matrix[4] * input[1] +
        matrix[7] * input[2];
    output[2] =
        matrix[2] * input[0] + matrix[5] * input[1] +
        matrix[8] * input[2];
}

static void check_vec3(
    const char *name, const float actual[3], const float expected[3]) {
    char field[96];
    for (uint32_t i = 0; i < 3; i++) {
        snprintf(field, sizeof(field), "%s[%u]", name, i);
        check_float(field, actual[i], expected[i]);
    }
}

static void reset_capture(void) {
    s_transform_calls = 0;
    s_triangle_calls = 0;
    s_point_calls = 0;
    s_sprite_calls = 0;
    s_line_calls = 0;
    s_vertex_count = 0;
    s_index_count = 0;
    s_draw_flags = 0;
    s_primitive = UINT32_MAX;
    memset(s_vertices, 0, sizeof(s_vertices));
    memset(s_indices, 0, sizeof(s_indices));
    memset(&s_state, 0, sizeof(s_state));
    memset(s_triangle, 0, sizeof(s_triangle));
    memset(s_points, 0, sizeof(s_points));
    memset(s_sprite, 0, sizeof(s_sprite));
    memset(s_line, 0, sizeof(s_line));
}

static int capture_transform(const GeTnlDraw *draw) {
    s_transform_calls++;
    if (!draw || draw->vertex_count > 16 || draw->index_count > 48) {
        fail("transform capture bounds");
        return 0;
    }
    s_vertex_count = draw->vertex_count;
    s_index_count = draw->index_count;
    memcpy(
        s_vertices, draw->vertices,
        draw->vertex_count * sizeof(*draw->vertices));
    memcpy(
        s_indices, draw->indices,
        draw->index_count * sizeof(*draw->indices));
    s_state = *draw->state;
    s_draw_flags = draw->flags;
    s_primitive = draw->primitive;
    return s_transform_accept;
}

static int capture_triangle(
    const GeVtx *a, const GeVtx *b, const GeVtx *c, int perspective) {
    s_triangle_calls++;
    if (!perspective)
        fail("fallback triangle perspective");
    s_triangle[0] = *a;
    s_triangle[1] = *b;
    s_triangle[2] = *c;
    return 1;
}

static int capture_point(const GeVtx *point, int perspective) {
    if (s_point_calls >= 3) {
        fail("point capture bounds");
        return 0;
    }
    if (!perspective)
        fail("fallback point perspective");
    s_points[s_point_calls++] = *point;
    return 1;
}

static int capture_sprite(
    const GeVtx *first, const GeVtx *second, int perspective) {
    s_sprite_calls++;
    if (!perspective)
        fail("fallback sprite perspective");
    s_sprite[0] = *first;
    s_sprite[1] = *second;
    return 1;
}

static int capture_line(
    const GeVtx *first, const GeVtx *second, int perspective) {
    s_line_calls++;
    if (!perspective)
        fail("fallback line perspective");
    s_line[0] = *first;
    s_line[1] = *second;
    return 1;
}

static const GeBackend s_backend = {
    .name = "tnl-decode-test",
    .tri = capture_triangle,
    .point = capture_point,
    .sprite = capture_sprite,
    .line = capture_line,
    .transform_draw = capture_transform,
};

static void configure_state(uint32_t vertex_type) {
    GeState *state = ge_state_ptr();
    memset(state, 0, sizeof(*state));
    state->vaddr = TEST_VERTEX_ADDRESS;
    state->vtype = vertex_type;
    state->shade_gouraud = 1;
    state->depth_clip = 1;
    state->scis_x2 = 255;
    state->scis_y2 = 255;
    state->maxz = 65535;
    state->tex_scale_u = 1.0f;
    state->tex_scale_v = 1.0f;
    state->tex_w = 1;
    state->tex_h = 1;
    state->amb_alpha = 255;
    set_identity43(state->world);
    set_identity43(state->view);
    set_identity44(state->proj);
    state->viewport_raw[0] = encode_float24(64.0f);
    state->viewport_raw[1] = encode_float24(64.0f);
    state->viewport_raw[2] = encode_float24(32768.0f);
    state->viewport_raw[3] = encode_float24(128.0f);
    state->viewport_raw[4] = encode_float24(128.0f);
    state->viewport_raw[5] = encode_float24(32768.0f);
}

static void draw_triangle(void) {
    ge_execute_op(
        (GE_COMMAND_PRIM << 24) | (GE_TNL_TRIANGLES << 16) | 3u,
        SR_RAM_BASE);
}

static void draw_points(uint32_t count) {
    ge_execute_op(
        (GE_COMMAND_PRIM << 24) |
            (GE_TNL_POINTS << 16) | count,
        SR_RAM_BASE);
}

static void draw_sprites(uint32_t count) {
    ge_execute_op(
        (GE_COMMAND_PRIM << 24) |
            (GE_TNL_SPRITES << 16) | count,
        SR_RAM_BASE);
}

static void draw_lines(uint32_t primitive, uint32_t count) {
    ge_execute_op(
        (GE_COMMAND_PRIM << 24) |
            (primitive << 16) | count,
        SR_RAM_BASE);
}

static void set_rotation_bone(float matrix[12], float tx, float ty) {
    memset(matrix, 0, 12 * sizeof(*matrix));
    matrix[0] = 0.0f;
    matrix[1] = 1.0f;
    matrix[3] = -1.0f;
    matrix[4] = 0.0f;
    matrix[8] = 1.0f;
    matrix[9] = tx;
    matrix[10] = ty;
}

static void test_single_u8_weight(void) {
    const uint32_t vertex_type =
        (1u << 9) | (3u << 5) | (3u << 7);
    const float positions[3][3] = {
        {-0.5f, -0.5f, 0.0f},
        {0.5f, -0.5f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    const float normals[3][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 1.0f},
    };
    const uint8_t raw_weights[3] = {128, 64, 32};
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(vertices, 0, 3 * 28);
    for (uint32_t i = 0; i < 3; i++) {
        uint8_t *vertex = vertices + i * 28;
        vertex[0] = raw_weights[i];
        put_vec3(vertex, 4, normals[i]);
        put_vec3(vertex, 16, positions[i]);
    }

    configure_state(vertex_type);
    GeState *state = ge_state_ptr();
    set_rotation_bone(state->bone, 0.125f, -0.25f);
    reset_capture();
    s_transform_accept = 1;
    draw_triangle();

    check_u32("u8 transform calls", s_transform_calls, 1);
    check_u32("u8 fallback calls", s_triangle_calls, 0);
    check_u32("u8 canonical vertices", s_vertex_count, 3);
    check_u32("u8 submitted indices", s_index_count, 3);
    check_u32(
        "u8 flags", s_state.vertex_flags,
        GE_TNL_VERTEX_NORMAL | GE_TNL_VERTEX_SKINNED);
    for (uint32_t i = 0; i < 3; i++) {
        float transformed_position[3];
        float transformed_normal[3];
        float expected_position[3];
        float expected_normal[3];
        float weight = (float)raw_weights[i] * (1.0f / 128.0f);
        transform_position(state->bone, positions[i], transformed_position);
        transform_normal(state->bone, normals[i], transformed_normal);
        for (uint32_t component = 0; component < 3; component++) {
            expected_position[component] =
                transformed_position[component] * weight;
            expected_normal[component] =
                transformed_normal[component] * weight;
        }
        char name[64];
        snprintf(name, sizeof(name), "u8 position %u", i);
        check_vec3(name, s_vertices[i].position, expected_position);
        snprintf(name, sizeof(name), "u8 normal %u", i);
        check_vec3(name, s_vertices[i].normal, expected_normal);
    }
}

static void test_two_u16_weights(void) {
    const uint32_t vertex_type =
        (2u << 9) | (1u << 14) | (3u << 5) | (3u << 7);
    const float positions[3][3] = {
        {-0.5f, -0.25f, 0.0f},
        {0.5f, -0.25f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    const float normals[3][3] = {
        {0.25f, 0.75f, 0.5f},
        {0.5f, 0.25f, 0.75f},
        {0.75f, 0.5f, 0.25f},
    };
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(vertices, 0, 3 * 28);
    for (uint32_t i = 0; i < 3; i++) {
        uint8_t *vertex = vertices + i * 28;
        put_u16(vertex, 0, 8192);
        put_u16(vertex, 2, 24576);
        put_vec3(vertex, 4, normals[i]);
        put_vec3(vertex, 16, positions[i]);
    }

    configure_state(vertex_type);
    GeState *state = ge_state_ptr();
    set_identity43(state->bone);
    state->bone[9] = 0.25f;
    set_rotation_bone(state->bone + 12, -0.125f, 0.125f);
    reset_capture();
    s_transform_accept = 1;
    draw_triangle();

    check_u32("u16 transform calls", s_transform_calls, 1);
    check_u32("u16 fallback calls", s_triangle_calls, 0);
    check_u32(
        "u16 flags", s_state.vertex_flags,
        GE_TNL_VERTEX_NORMAL | GE_TNL_VERTEX_SKINNED);
    for (uint32_t i = 0; i < 3; i++) {
        float bone_position[2][3];
        float bone_normal[2][3];
        float expected_position[3];
        float expected_normal[3];
        transform_position(state->bone, positions[i], bone_position[0]);
        transform_position(state->bone + 12, positions[i], bone_position[1]);
        transform_normal(state->bone, normals[i], bone_normal[0]);
        transform_normal(state->bone + 12, normals[i], bone_normal[1]);
        for (uint32_t component = 0; component < 3; component++) {
            expected_position[component] =
                bone_position[0][component] * 0.25f +
                bone_position[1][component] * 0.75f;
            expected_normal[component] =
                bone_normal[0][component] * 0.25f +
                bone_normal[1][component] * 0.75f;
        }
        char name[64];
        snprintf(name, sizeof(name), "u16 position %u", i);
        check_vec3(name, s_vertices[i].position, expected_position);
        snprintf(name, sizeof(name), "u16 normal %u", i);
        check_vec3(name, s_vertices[i].normal, expected_normal);
    }
}

static void write_morph_target(
    uint8_t *target, const float weights[2], const float texcoord[2],
    const uint8_t color[4], const float normal[3],
    const float position[3]) {
    put_float(target, 0, weights[0]);
    put_float(target, 4, weights[1]);
    put_vec2(target, 8, texcoord);
    put_u32(
        target, 16,
        (uint32_t)color[0] | ((uint32_t)color[1] << 8) |
        ((uint32_t)color[2] << 16) | ((uint32_t)color[3] << 24));
    put_vec3(target, 20, normal);
    put_vec3(target, 32, position);
}

static void prepare_float_morph_vertices(
    float expected_positions[3][3], float expected_normals[3][3],
    float expected_texcoords[3][2], uint8_t expected_colors[3][4]) {
    const float base_positions[3][3] = {
        {-0.5f, -0.5f, 0.0f},
        {0.5f, -0.5f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    const float position_delta[3] = {0.25f, 0.25f, 0.0f};
    const float target_normals[2][3] = {
        {0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f},
    };
    const float target_weights[2][2] = {
        {1.0f, 0.0f},
        {0.0f, 1.0f},
    };
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(vertices, 0, 3 * 88);
    for (uint32_t i = 0; i < 3; i++) {
        float target_positions[2][3];
        float target_texcoords[2][2] = {
            {(float)i * 0.25f, 0.25f},
            {0.5f + (float)i * 0.125f, 0.75f},
        };
        uint8_t target_colors[2][4] = {
            {(uint8_t)(40 + i * 10), 80, 120, 160},
            {200, (uint8_t)(100 + i * 10), 40, 240},
        };
        for (uint32_t component = 0; component < 3; component++) {
            target_positions[0][component] = base_positions[i][component];
            target_positions[1][component] =
                base_positions[i][component] + position_delta[component];
        }
        uint8_t *vertex = vertices + i * 88;
        for (uint32_t morph = 0; morph < 2; morph++) {
            write_morph_target(
                vertex + morph * 44, target_weights[morph],
                target_texcoords[morph], target_colors[morph],
                target_normals[morph], target_positions[morph]);
        }

        float morphed_position[3];
        float morphed_normal[3];
        float bone_position[2][3];
        float bone_normal[2][3];
        for (uint32_t component = 0; component < 3; component++) {
            morphed_position[component] =
                target_positions[0][component] * 0.25f +
                target_positions[1][component] * 0.75f;
            morphed_normal[component] =
                target_normals[0][component] * 0.25f +
                target_normals[1][component] * 0.75f;
        }
        const GeState *state = ge_state_ptr();
        transform_position(state->bone, morphed_position, bone_position[0]);
        transform_position(
            state->bone + 12, morphed_position, bone_position[1]);
        transform_normal(state->bone, morphed_normal, bone_normal[0]);
        transform_normal(
            state->bone + 12, morphed_normal, bone_normal[1]);
        for (uint32_t component = 0; component < 3; component++) {
            expected_positions[i][component] =
                bone_position[0][component] * 0.25f +
                bone_position[1][component] * 0.75f;
            expected_normals[i][component] =
                bone_normal[0][component] * 0.25f +
                bone_normal[1][component] * 0.75f;
        }
        for (uint32_t component = 0; component < 2; component++) {
            expected_texcoords[i][component] =
                target_texcoords[0][component] * 0.25f +
                target_texcoords[1][component] * 0.75f;
        }
        for (uint32_t component = 0; component < 4; component++) {
            expected_colors[i][component] = (uint8_t)(
                (float)target_colors[0][component] * 0.25f +
                (float)target_colors[1][component] * 0.75f);
        }
    }
}

static uint32_t configure_float_morph_state(void) {
    const uint32_t vertex_type =
        3u | (7u << 2) | (3u << 5) | (3u << 7) |
        (3u << 9) | (1u << 14) | (1u << 18);
    configure_state(vertex_type);
    GeState *state = ge_state_ptr();
    state->tex_enable = 1;
    state->morph_weight[0] = 0.25f;
    state->morph_weight[1] = 0.75f;
    set_identity43(state->bone);
    state->bone[9] = 0.125f;
    set_rotation_bone(state->bone + 12, -0.125f, 0.125f);
    return vertex_type;
}

static void test_float_morph_skin_and_fallback(void) {
    float expected_positions[3][3];
    float expected_normals[3][3];
    float expected_texcoords[3][2];
    uint8_t expected_colors[3][4];
    configure_float_morph_state();
    prepare_float_morph_vertices(
        expected_positions, expected_normals,
        expected_texcoords, expected_colors);
    reset_capture();
    s_transform_accept = 1;
    draw_triangle();

    check_u32("morph transform calls", s_transform_calls, 1);
    check_u32("morph fallback calls", s_triangle_calls, 0);
    check_u32(
        "morph flags", s_state.vertex_flags,
        GE_TNL_VERTEX_NORMAL | GE_TNL_VERTEX_TEXCOORD |
        GE_TNL_VERTEX_COLOR | GE_TNL_VERTEX_SKINNED |
        GE_TNL_VERTEX_MORPHED);
    for (uint32_t i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "morph position %u", i);
        check_vec3(name, s_vertices[i].position, expected_positions[i]);
        snprintf(name, sizeof(name), "morph normal %u", i);
        check_vec3(name, s_vertices[i].normal, expected_normals[i]);
        for (uint32_t component = 0; component < 2; component++) {
            snprintf(
                name, sizeof(name), "morph texcoord %u.%u",
                i, component);
            check_float(
                name, s_vertices[i].texcoord[component],
                expected_texcoords[i][component]);
        }
        for (uint32_t component = 0; component < 4; component++) {
            snprintf(
                name, sizeof(name), "morph color %u.%u",
                i, component);
            check_u32(
                name, s_vertices[i].color[component],
                expected_colors[i][component]);
        }
    }

    configure_float_morph_state();
    reset_capture();
    s_transform_accept = 0;
    draw_triangle();
    check_u32("decline transform calls", s_transform_calls, 1);
    check_u32("decline CPU triangle calls", s_triangle_calls, 1);
    GeVtx declined_triangle[3];
    memcpy(declined_triangle, s_triangle, sizeof(declined_triangle));
    for (uint32_t i = 0; i < 3; i++) {
        GeTnlReferenceVertex reference;
        if (!ge_tnl_reference_transform(
                &s_state, &s_vertices[i], &reference)) {
            fail("morph canonical reference transform");
            continue;
        }
        char name[64];
        snprintf(name, sizeof(name), "fallback screen x %u", i);
        check_float(name, declined_triangle[i].x, reference.screen[0]);
        snprintf(name, sizeof(name), "fallback screen y %u", i);
        check_float(name, declined_triangle[i].y, reference.screen[1]);
        snprintf(name, sizeof(name), "fallback screen z %u", i);
        check_float(name, declined_triangle[i].z, reference.screen[2]);
        snprintf(name, sizeof(name), "fallback reciprocal w %u", i);
        check_float(
            name, declined_triangle[i].rw, reference.reciprocal_w);
        snprintf(name, sizeof(name), "fallback u %u", i);
        check_float(name, declined_triangle[i].u, reference.uv[0]);
        snprintf(name, sizeof(name), "fallback v %u", i);
        check_float(name, declined_triangle[i].v, reference.uv[1]);
        snprintf(name, sizeof(name), "fallback fog %u", i);
        check_float(name, declined_triangle[i].fog, reference.fog);
    }

    configure_float_morph_state();
    reset_capture();
    sr_perf_alternate_mask = 1u << SR_PATH_GPU_TNL;
    draw_triangle();
    sr_perf_alternate_mask = 0;
    check_u32("disabled transform calls", s_transform_calls, 0);
    check_u32("disabled CPU triangle calls", s_triangle_calls, 1);
    if (memcmp(declined_triangle, s_triangle, sizeof(declined_triangle)) != 0)
        fail("backend decline preserves exact CPU fallback");
}

static void test_near_clip_preprocess_route(void) {
    const uint32_t vertex_type = 3u << 7;
    const float positions[3][3] = {
        {-0.5f, -0.5f, -2.0f},
        {0.5f, -0.5f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(vertices, 0, 3 * 12);
    for (uint32_t i = 0; i < 3; i++)
        put_vec3(vertices + i * 12, 0, positions[i]);

    configure_state(vertex_type);
    reset_capture();
    s_transform_accept = 0;
    draw_triangle();
    check_u32("near clip transform calls", s_transform_calls, 1);
    check_u32(
        "near clip preprocess flag", s_draw_flags,
        GE_TNL_DRAW_PREPROCESS);
    check_u32("near clip fallback triangles", s_triangle_calls, 2);

    configure_state(vertex_type);
    reset_capture();
    s_transform_accept = 1;
    draw_triangle();
    check_u32("near clip accepted transform", s_transform_calls, 1);
    check_u32("near clip accepted CPU triangles", s_triangle_calls, 0);
}

static void test_point_expansion_route(void) {
    const uint32_t vertex_type = 3u << 7;
    const float positions[3][3] = {
        {-0.5f, 0.0f, 0.0f},
        {0.0f, 0.25f, 0.0f},
        {0.5f, -0.25f, 0.0f},
    };
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(s_perf_counters, 0, sizeof(s_perf_counters));
    memset(vertices, 0, 3 * 12);
    for (uint32_t i = 0; i < 3; i++)
        put_vec3(vertices + i * 12, 0, positions[i]);

    configure_state(vertex_type);
    reset_capture();
    s_transform_accept = 1;
    draw_points(3);
    check_u32("point transform calls", s_transform_calls, 1);
    check_u32("point primitive", s_primitive, GE_TNL_POINTS);
    check_u32("point canonical vertices", s_vertex_count, 3);
    check_u32("point control indices", s_index_count, 3);
    check_u32("point CPU fallback count", s_point_calls, 0);
    for (uint32_t i = 0; i < 3; i++) {
        char name[64];
        snprintf(name, sizeof(name), "point control index %u", i);
        check_u32(name, s_indices[i], i);
    }

    const uint32_t index_address = TEST_VERTEX_ADDRESS + 0x100u;
    uint8_t *point_indices = SR_HOST(index_address);
    point_indices[0] = 2;
    point_indices[1] = 0;
    point_indices[2] = 2;
    configure_state(vertex_type | (1u << 11));
    ge_state_ptr()->iaddr = index_address;
    reset_capture();
    s_transform_accept = 1;
    draw_points(3);
    check_u32("indexed point transform calls", s_transform_calls, 1);
    check_u32("indexed point canonical vertices", s_vertex_count, 2);
    check_u32("indexed point control count", s_index_count, 3);
    check_u32("indexed point first control", s_indices[0], 0);
    check_u32("indexed point second control", s_indices[1], 1);
    check_u32("indexed point reused control", s_indices[2], 0);

    configure_state(vertex_type);
    reset_capture();
    s_transform_accept = 0;
    draw_points(3);
    check_u32("declined point transform calls", s_transform_calls, 1);
    check_u32("declined point CPU fallback count", s_point_calls, 3);
    check_float("declined point first x", s_points[0].x, 96.0f);
    check_float("declined point first y", s_points[0].y, 128.0f);

    configure_state(vertex_type);
    ge_state_ptr()->proj[15] = 0.0005f;
    reset_capture();
    s_transform_accept = 1;
    draw_points(3);
    check_u32("unsafe point bypasses backend", s_transform_calls, 0);
    check_u32("unsafe point CPU visible count", s_point_calls, 0);
    check_u32(
        "point workload draws",
        (uint32_t)s_perf_counters[SR_PERF_GE_POINT_DRAWS], 4);
    check_u32(
        "point workload count",
        (uint32_t)s_perf_counters[SR_PERF_GE_POINTS], 12);
}

static void test_sprite_expansion_route(void) {
    const uint32_t vertex_type = 3u << 7;
    const float positions[3][3] = {
        {-0.5f, -0.25f, 0.0f},
        {0.5f, 0.25f, 0.0f},
        {100.0f, 100.0f, 100.0f},
    };
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(s_perf_counters, 0, sizeof(s_perf_counters));
    memset(vertices, 0, 3 * 12);
    for (uint32_t i = 0; i < 3; i++)
        put_vec3(vertices + i * 12, 0, positions[i]);

    configure_state(vertex_type);
    ge_state_ptr()->tex_filter = 1u;
    reset_capture();
    s_transform_accept = 1;
    draw_sprites(3);
    check_u32("sprite transform calls", s_transform_calls, 1);
    check_u32("sprite primitive", s_primitive, GE_TNL_SPRITES);
    check_u32("sprite canonical controls", s_vertex_count, 2);
    check_u32("sprite control indices", s_index_count, 2);
    check_u32(
        "sprite linear filter flag", s_draw_flags,
        GE_TNL_DRAW_SPRITE_LINEAR_FILTER);
    check_u32("sprite CPU fallback count", s_sprite_calls, 0);

    configure_state(vertex_type);
    reset_capture();
    s_transform_accept = 0;
    draw_sprites(2);
    check_u32("declined sprite transform calls", s_transform_calls, 1);
    check_u32("declined sprite CPU fallback count", s_sprite_calls, 1);
    check_float("declined sprite first x", s_sprite[0].x, 96.0f);
    check_float("declined sprite first y", s_sprite[0].y, 112.0f);
    check_float("declined sprite second x", s_sprite[1].x, 160.0f);
    check_float("declined sprite second y", s_sprite[1].y, 144.0f);

    configure_state(vertex_type);
    ge_state_ptr()->proj[15] = 0.0005f;
    reset_capture();
    s_transform_accept = 1;
    draw_sprites(2);
    check_u32("unsafe sprite bypasses backend", s_transform_calls, 0);
    check_u32("unsafe sprite CPU visible count", s_sprite_calls, 0);
    check_u32(
        "sprite workload draws",
        (uint32_t)s_perf_counters[SR_PERF_GE_SPRITE_DRAWS], 3);
    check_u32(
        "sprite workload count",
        (uint32_t)s_perf_counters[SR_PERF_GE_SPRITES], 3);
}

static void test_line_tnl_fallback(void) {
    const uint32_t vertex_type = 3u << 7;
    const float positions[3][3] = {
        {-0.5f, -0.25f, 0.0f},
        {0.5f, 0.25f, 0.0f},
        {0.0f, 0.5f, 0.0f},
    };
    uint8_t *vertices = SR_HOST(TEST_VERTEX_ADDRESS);
    memset(s_perf_counters, 0, sizeof(s_perf_counters));
    memset(vertices, 0, 3 * 12);
    for (uint32_t i = 0; i < 3; i++)
        put_vec3(vertices + i * 12, 0, positions[i]);

    configure_state(vertex_type);
    reset_capture();
    s_transform_accept = 1;
    draw_lines(GE_TNL_LINES, 2);
    check_u32("line bypasses transform backend", s_transform_calls, 0);
    check_u32("line uses projected CPU fallback", s_line_calls, 1);
    check_float("line fallback first x", s_line[0].x, 96.0f);
    check_float("line fallback first y", s_line[0].y, 112.0f);
    check_float("line fallback second x", s_line[1].x, 160.0f);
    check_float("line fallback second y", s_line[1].y, 144.0f);

    configure_state(vertex_type);
    reset_capture();
    draw_lines(GE_TNL_LINE_STRIP, 3);
    check_u32(
        "line strip bypasses transform backend",
        s_transform_calls, 0);
    check_u32(
        "line strip uses projected CPU fallback",
        s_line_calls, 2);
    check_u32(
        "line workload draws",
        (uint32_t)s_perf_counters[SR_PERF_GE_LINE_DRAWS], 2);
    check_u32(
        "line workload segments",
        (uint32_t)s_perf_counters[SR_PERF_GE_LINE_SEGMENTS], 3);
}

static void test_texture_dxt_decode(void) {
    uint8_t block[16] = {0};
    block[0] = 0xe4u;
    block[4] = 0x00u;
    block[5] = 0xf8u;
    block[6] = 0x1fu;
    block[7] = 0x00u;
    check_u32(
        "DXT1 color selector zero",
        ge_texture_dxt_decode(block, 8, 0, 0),
        0xff0000f8u);
    check_u32(
        "DXT1 color selector one",
        ge_texture_dxt_decode(block, 8, 1, 0),
        0xfff80000u);
    check_u32(
        "DXT1 color selector two",
        ge_texture_dxt_decode(block, 8, 2, 0),
        0xff5200a5u);
    check_u32(
        "DXT1 color selector three",
        ge_texture_dxt_decode(block, 8, 3, 0),
        0xffa50052u);

    block[4] = 0x1fu;
    block[5] = 0x00u;
    block[6] = 0xe0u;
    block[7] = 0x07u;
    check_u32(
        "DXT1 three-color average",
        ge_texture_dxt_decode(block, 8, 2, 0),
        0xff7c7e00u);
    check_u32(
        "DXT1 transparent selector",
        ge_texture_dxt_decode(block, 8, 3, 0), 0);

    memset(block, 0, sizeof(block));
    block[4] = 0x00u;
    block[5] = 0xf8u;
    block[6] = 0x1fu;
    block[8] = 0x0au;
    check_u32(
        "DXT3 high-nibble alpha",
        ge_texture_dxt_decode(block, 9, 0, 0),
        0xa00000f8u);

    memset(block, 0, sizeof(block));
    block[4] = 0x00u;
    block[5] = 0xf8u;
    block[6] = 0x1fu;
    uint64_t alpha_data =
        ((uint64_t)0u << 0) |
        ((uint64_t)1u << 3) |
        ((uint64_t)2u << 6) |
        ((uint64_t)7u << 9);
    for (uint32_t i = 0; i < 6; i++)
        block[8 + i] = (uint8_t)(alpha_data >> (i * 8));
    block[14] = 240;
    block[15] = 16;
    check_u32(
        "DXT5 descending endpoint zero",
        ge_texture_dxt_decode(block, 10, 0, 0),
        0xf00000f8u);
    check_u32(
        "DXT5 descending endpoint one",
        ge_texture_dxt_decode(block, 10, 1, 0),
        0x100000f8u);
    check_u32(
        "DXT5 descending interpolation",
        ge_texture_dxt_decode(block, 10, 2, 0),
        0xd00000f8u);
    check_u32(
        "DXT5 descending final interpolation",
        ge_texture_dxt_decode(block, 10, 3, 0),
        0x300000f8u);

    alpha_data =
        ((uint64_t)2u << 0) |
        ((uint64_t)5u << 3) |
        ((uint64_t)6u << 6) |
        ((uint64_t)7u << 9);
    for (uint32_t i = 0; i < 6; i++)
        block[8 + i] = (uint8_t)(alpha_data >> (i * 8));
    block[14] = 16;
    block[15] = 241;
    check_u32(
        "DXT5 ascending first interpolation",
        ge_texture_dxt_decode(block, 10, 0, 0),
        0x3d0000f8u);
    check_u32(
        "DXT5 ascending final interpolation",
        ge_texture_dxt_decode(block, 10, 1, 0),
        0xc40000f8u);
    check_u32(
        "DXT5 ascending zero",
        ge_texture_dxt_decode(block, 10, 2, 0),
        0x000000f8u);
    check_u32(
        "DXT5 ascending maximum",
        ge_texture_dxt_decode(block, 10, 3, 0),
        0xff0000f8u);
}

static void test_texture_snapshot_immutability(void) {
    static const uint32_t direct_before[4] = {
        0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
    };
    static const uint32_t direct_after[4] = {
        0x01020304u, 0x05060708u, 0x090a0b0cu, 0x0d0e0f10u,
    };
    uint32_t decoded[4] = {0};
    GeState *state;
    GeTexSnapshot *snapshot;
    GeTextureRawJob raw_job;
    uint32_t byte_count;
    const uint8_t *payload;
    uint32_t clut_test =
        (1u << 2) | (0x3fu << 8) | (1u << 16);
    check_u32(
        "CLUT4 mip offset",
        ge_texture_clut_index(
            0xfeu, 4, 2, clut_test, 0x100u),
        95);
    check_u32(
        "CLUT8 mip offset",
        ge_texture_clut_index(
            0xfeu, 5, 1, clut_test | 1u, 0x100u),
        319);
    check_u32(
        "CLUT8 RGBA8888 has no mip offset",
        ge_texture_clut_index(
            0xfeu, 5, 1, clut_test | 3u, 0x100u),
        63);
    check_u32(
        "shared CLUT has no mip offset",
        ge_texture_clut_index(
            0xfeu, 4, 2, clut_test, 0),
        63);

    configure_state(0);
    state = ge_state_ptr();
    state->tex_addr = TEST_TEXTURE_ADDRESS;
    state->tex_bufw = 4;
    state->tex_w = 4;
    state->tex_h = 1;
    state->tex_fmt = 3;
    state->tex_addr_levels[1] = TEST_TEXTURE_ADDRESS + 0x100;
    state->tex_bufw_levels[1] = 4;
    state->tex_width_levels[1] = 2;
    state->tex_height_levels[1] = 1;
    memcpy(
        SR_HOST(TEST_TEXTURE_ADDRESS),
        direct_before, sizeof(direct_before));
    memcpy(
        SR_HOST(TEST_TEXTURE_ADDRESS + 0x100),
        direct_after, sizeof(direct_after));
    snapshot = ge_tex_snapshot_capture(2);
    if (!snapshot) {
        fail("direct texture snapshot capture");
        return;
    }
    payload = ge_tex_snapshot_source_data(
        snapshot, 0, &byte_count);
    if (!payload ||
        byte_count != sizeof(direct_before) ||
        memcmp(payload, direct_before, sizeof(direct_before)) != 0) {
        fail("direct raw texture payload");
    }
    if (!ge_tex_snapshot_build_raw_job(
            snapshot, 0, 32, 0, 7, 4, 1, &raw_job)) {
        fail("direct raw texture job");
    } else {
        check_u32(
            "raw texture ABI version",
            raw_job.abi_version,
            GE_TEXTURE_RAW_JOB_ABI_VERSION);
        check_u32("direct raw flags", raw_job.flags, 0);
        check_u32("direct raw format", raw_job.format, 3);
        check_u32("direct raw mip", raw_job.mip_level, 0);
        check_u32(
            "direct raw source offset",
            raw_job.source_byte_offset, 32);
        check_u32(
            "direct raw source bytes",
            raw_job.source_byte_count, 16);
        check_u32(
            "direct raw source stride",
            raw_job.source_stride, 4);
        check_u32(
            "direct raw source width",
            raw_job.source_width, 4);
        check_u32(
            "direct raw source height",
            raw_job.source_height, 1);
        check_u32(
            "direct raw destination offset",
            raw_job.destination_pixel_offset, 7);
        check_u32(
            "direct raw destination width",
            raw_job.destination_width, 4);
        check_u32(
            "direct raw destination height",
            raw_job.destination_height, 1);
    }
    if (!ge_tex_snapshot_build_raw_job(
            snapshot, 1, 128, 0, 20, 2, 1, &raw_job)) {
        fail("mip raw texture job");
    } else {
        check_u32("raw texture mip level", raw_job.mip_level, 1);
        check_u32(
            "raw texture mip source width",
            raw_job.source_width, 2);
        check_u32(
            "raw texture mip source bytes",
            raw_job.source_byte_count, 16);
    }
    memset(&raw_job, 0xff, sizeof(raw_job));
    if (ge_tex_snapshot_build_raw_job(
            snapshot, 0, UINT32_MAX, 0, 0, 4, 1,
            &raw_job)) {
        fail("raw texture source overflow accepted");
    }
    check_u32(
        "invalid raw texture job is cleared",
        raw_job.abi_version, 0);
    if (ge_tex_snapshot_build_raw_job(
            snapshot, 0, 0, 0, UINT32_MAX, 4, 1,
            &raw_job)) {
        fail("raw texture destination overflow accepted");
    }
    memcpy(
        SR_HOST(TEST_TEXTURE_ADDRESS),
        direct_after, sizeof(direct_after));
    ge_tex_snapshot_decode_level(snapshot, decoded, 0);
    for (uint32_t i = 0; i < 4; i++)
        check_u32(
            "direct texture snapshot remains immutable",
            decoded[i], direct_before[i]);
    memset(decoded, 0, sizeof(decoded));
    ge_decode_tex_rgba(decoded);
    for (uint32_t i = 0; i < 4; i++)
        check_u32(
            "live direct texture sees mutation",
            decoded[i], direct_after[i]);
    ge_tex_snapshot_free(snapshot);

    static const uint32_t palette[5] = {
        0xff000000u, 0xff112233u, 0xff445566u,
        0xff778899u, 0xffaabbccu,
    };
    configure_state(0);
    state = ge_state_ptr();
    state->tex_addr = TEST_TEXTURE_ADDRESS;
    state->tex_bufw = 2;
    state->tex_w = 2;
    state->tex_h = 1;
    state->tex_fmt = 4;
    state->clut_fmt = 3u | (0xffu << 8);
    memcpy(state->clutram, palette, sizeof(palette));
    *(uint8_t *)SR_HOST(TEST_TEXTURE_ADDRESS) = 0x21u;
    snapshot = ge_tex_snapshot_capture(1);
    if (!snapshot) {
        fail("indexed texture snapshot capture");
        return;
    }
    payload = ge_tex_snapshot_source_data(
        snapshot, 0, &byte_count);
    if (!payload || byte_count != 16 || payload[0] != 0x21u)
        fail("indexed raw texture payload");
    payload = ge_tex_snapshot_clut_data(
        snapshot, &byte_count);
    if (!payload ||
        byte_count != GE_TEXTURE_RAW_CLUT_BYTES ||
        memcmp(payload, palette, sizeof(palette)) != 0) {
        fail("raw texture CLUT payload");
    }
    if (!ge_tex_snapshot_build_raw_job(
            snapshot, 0, 64, 128, 5, 2, 1, &raw_job)) {
        fail("indexed raw texture job");
    } else {
        check_u32(
            "indexed raw flags", raw_job.flags,
            GE_TEXTURE_RAW_INDEXED |
                GE_TEXTURE_RAW_CLUT_SHARED);
        check_u32("indexed raw format", raw_job.format, 4);
        check_u32(
            "indexed raw source bytes",
            raw_job.source_byte_count, 16);
        check_u32(
            "indexed raw source stride",
            raw_job.source_stride, 32);
        check_u32(
            "indexed raw CLUT offset",
            raw_job.clut_byte_offset, 128);
        check_u32(
            "indexed raw CLUT bytes",
            raw_job.clut_byte_count,
            GE_TEXTURE_RAW_CLUT_BYTES);
        check_u32(
            "indexed raw CLUT format",
            raw_job.clut_format, state->clut_fmt);
        check_u32(
            "indexed raw texture mode",
            raw_job.texture_mode, state->tex_mode);
    }
    if (ge_tex_snapshot_build_raw_job(
            snapshot, 0, 0, UINT32_MAX, 0, 2, 1,
            &raw_job)) {
        fail("raw texture CLUT overflow accepted");
    }
    memset(state->clutram, 0, sizeof(state->clutram));
    memcpy(state->clutram, palette, sizeof(palette));
    *(uint8_t *)SR_HOST(TEST_TEXTURE_ADDRESS) = 0x43u;
    state->clutram[4] ^= 0xffu;
    state->clutram[8] ^= 0xffu;
    memset(decoded, 0, sizeof(decoded));
    ge_tex_snapshot_decode_level(snapshot, decoded, 0);
    check_u32(
        "snapshot retains indexed texel zero",
        decoded[0], palette[1]);
    check_u32(
        "snapshot retains indexed texel one",
        decoded[1], palette[2]);
    memset(decoded, 0, sizeof(decoded));
    ge_decode_tex_rgba(decoded);
    check_u32(
        "live indexed texture sees source mutation zero",
        decoded[0], palette[3]);
    check_u32(
        "live indexed texture sees source mutation one",
        decoded[1], palette[4]);
    ge_tex_snapshot_free(snapshot);

    configure_state(0);
    state = ge_state_ptr();
    state->tex_addr = TEST_TEXTURE_ADDRESS;
    state->tex_bufw = 4;
    state->tex_w = 4;
    state->tex_h = 4;
    state->tex_fmt = 8;
    state->tex_swizzle = 1;
    memset(SR_HOST(TEST_TEXTURE_ADDRESS), 0, 8);
    snapshot = ge_tex_snapshot_capture(1);
    if (!snapshot) {
        fail("compressed texture snapshot capture");
        return;
    }
    if (!ge_tex_snapshot_build_raw_job(
            snapshot, 0, 0, 0, 0, 4, 4,
            &raw_job)) {
        fail("compressed raw texture job");
    } else {
        check_u32(
            "compressed raw flags", raw_job.flags,
            GE_TEXTURE_RAW_SWIZZLED |
                GE_TEXTURE_RAW_BLOCK_COMPRESSED);
        check_u32(
            "compressed raw source bytes",
            raw_job.source_byte_count, 8);
        check_u32(
            "compressed raw CLUT bytes",
            raw_job.clut_byte_count, 0);
    }
    ge_tex_snapshot_free(snapshot);
}

static int enable_tnl(void) {
#ifdef _WIN32
    return _putenv_s("SR_GPU_TNL", "1") == 0;
#else
    return setenv("SR_GPU_TNL", "1", 1) == 0;
#endif
}

int main(void) {
    if (!enable_tnl()) {
        fprintf(stderr, "FAIL: could not set SR_GPU_TNL\n");
        return 1;
    }
    g_mem = calloc(1, TEST_RAM_SIZE);
    if (!g_mem) {
        fprintf(stderr, "FAIL: could not allocate guest RAM\n");
        return 1;
    }

    ge_execute_op(0, SR_RAM_BASE);
    ge_set_backend(&s_backend);
    test_single_u8_weight();
    test_two_u16_weights();
    test_float_morph_skin_and_fallback();
    test_near_clip_preprocess_route();
    test_point_expansion_route();
    test_sprite_expansion_route();
    test_line_tnl_fallback();
    test_texture_dxt_decode();
    test_texture_snapshot_immutability();
    ge_set_backend(NULL);

    free(g_mem);
    g_mem = NULL;
    if (s_oor_accesses)
        s_failures += s_oor_accesses;
    if (s_failures) {
        fprintf(stderr, "GE T&L decode tests: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("GE T&L decode tests passed\n");
    return 0;
}
