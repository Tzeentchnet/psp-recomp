#ifndef SR_GE_TNL_H
#define SR_GE_TNL_H

#include "ge_shared.h"

#include <math.h>
#include <string.h>

typedef enum GeTnlEligibility {
    GE_TNL_ELIGIBLE = 0,
    GE_TNL_ELIGIBLE_PREPROCESS,
    GE_TNL_DECLINE_INVALID,
    GE_TNL_DECLINE_PRIMITIVE,
    GE_TNL_DECLINE_CLEAR,
    GE_TNL_DECLINE_FLAT,
    GE_TNL_DECLINE_CLIP,
} GeTnlEligibility;

typedef struct GeTnlGpuState {
    float world[3][4];
    float view[3][4];
    float projection[4][4];
    float texture[3][4];
    float texture_params[4];
    float viewport_scale[4];
    float viewport_center[4];
    float viewport_offset_size[4];
    float fog_material[4];
    uint32_t config[4];
    uint32_t depth_material[4];
    uint32_t material_colors[4];
    uint32_t material_config[4];
    float light_position[4][4];
    float light_direction[4][4];
    float light_attenuation[4][4];
    uint32_t light_colors[4][4];
} GeTnlGpuState;

typedef struct GeTnlValidationVertex {
    float screen_reciprocal_w[4];
    float uv_fog[4];
    float color[4];
} GeTnlValidationVertex;

/* GPU preprocessing transforms canonical vertices once into this clip-space
 * form, then processes each triangle into zero, one, or two screen triangles. */
typedef struct GeTnlClipVertex {
    float clip[4];
    float uv_fog[4];
    uint32_t color[4];
} GeTnlClipVertex;

/* Matches the 32-byte screen-space vertex consumed by both render backends. */
typedef struct GeTnlScreenVertex {
    float position[4];
    float texcoord[2];
    float fog;
    uint32_t color;
} GeTnlScreenVertex;

#if defined(__cplusplus)
static_assert(sizeof(GeTnlVertex) == 36, "canonical T&L vertex ABI changed");
static_assert(sizeof(GeTnlGpuState) == 608, "packed T&L state ABI changed");
static_assert(sizeof(GeTnlValidationVertex) == 48, "T&L validation ABI changed");
static_assert(sizeof(GeTnlClipVertex) == 48, "T&L clip vertex ABI changed");
static_assert(sizeof(GeTnlScreenVertex) == 32, "T&L screen vertex ABI changed");
#else
_Static_assert(sizeof(GeTnlVertex) == 36, "canonical T&L vertex ABI changed");
_Static_assert(sizeof(GeTnlGpuState) == 608, "packed T&L state ABI changed");
_Static_assert(sizeof(GeTnlValidationVertex) == 48, "T&L validation ABI changed");
_Static_assert(sizeof(GeTnlClipVertex) == 48, "T&L clip vertex ABI changed");
_Static_assert(sizeof(GeTnlScreenVertex) == 32, "T&L screen vertex ABI changed");
#endif

static inline float ge_tnl_decode_float24(uint32_t value) {
    uint32_t bits = value << 8;
    float decoded;
    memcpy(&decoded, &bits, sizeof(decoded));
    return decoded;
}

static inline int ge_tnl_state_capture(
    const GeState *source, uint32_t vertex_flags, GeTnlState *out) {
    if (!source || !out)
        return 0;

    GeTnlState snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.abi_version = GE_TNL_ABI_VERSION;
    snapshot.vertex_flags = vertex_flags;
    snapshot.source_vtype = source->vtype;
    if (source->tex_enable) snapshot.flags |= GE_TNL_STATE_TEXTURE;
    if (source->lighting_enable) snapshot.flags |= GE_TNL_STATE_LIGHTING;
    if (source->reverse_normal)
        snapshot.flags |= GE_TNL_STATE_REVERSE_NORMAL;
    if (source->light_mode)
        snapshot.flags |= GE_TNL_STATE_SEPARATE_SPECULAR;
    if (source->fog_enable) snapshot.flags |= GE_TNL_STATE_FOG;
    if (source->shade_gouraud) snapshot.flags |= GE_TNL_STATE_GOURAUD;
    if (source->depth_clip) snapshot.flags |= GE_TNL_STATE_DEPTH_CLIP;
    if (source->cull_enable) snapshot.flags |= GE_TNL_STATE_CULL;
    if (source->clear) snapshot.flags |= GE_TNL_STATE_CLEAR;
    if (source->material_set)
        snapshot.flags |= GE_TNL_STATE_MATERIAL_COLOR_SET;
    if (source->material_alpha_set)
        snapshot.flags |= GE_TNL_STATE_MATERIAL_ALPHA_SET;

    memcpy(snapshot.world, source->world, sizeof(snapshot.world));
    memcpy(snapshot.view, source->view, sizeof(snapshot.view));
    memcpy(
        snapshot.projection, source->proj, sizeof(snapshot.projection));
    memcpy(snapshot.texture, source->tgen, sizeof(snapshot.texture));
    snapshot.texture_scale[0] = source->tex_scale_u;
    snapshot.texture_scale[1] = source->tex_scale_v;
    snapshot.texture_offset[0] = source->tex_off_u;
    snapshot.texture_offset[1] = source->tex_off_v;
    for (uint32_t i = 0; i < 3; i++) {
        snapshot.viewport_scale[i] =
            ge_tnl_decode_float24(source->viewport_raw[i]);
        snapshot.viewport_center[i] =
            ge_tnl_decode_float24(source->viewport_raw[i + 3u]);
    }
    snapshot.viewport_offset[0] =
        (float)(source->offsetx & 0xffffu) * (1.0f / 16.0f);
    snapshot.viewport_offset[1] =
        (float)(source->offsety & 0xffffu) * (1.0f / 16.0f);
    snapshot.scissor[0] = source->scis_x1;
    snapshot.scissor[1] = source->scis_y1;
    snapshot.scissor[2] = source->scis_x2;
    snapshot.scissor[3] = source->scis_y2;
    snapshot.texture_width =
        source->tex_w > 0 ? (uint32_t)source->tex_w : 0;
    snapshot.texture_height =
        source->tex_h > 0 ? (uint32_t)source->tex_h : 0;
    snapshot.texture_map_mode = source->tex_map_mode;
    snapshot.texture_shade_lights = source->tex_shade_ls;
    snapshot.minimum_depth = source->minz;
    snapshot.maximum_depth = source->maxz;
    snapshot.cull_mode = source->cull;
    snapshot.fog_end = source->fog_end;
    snapshot.fog_slope = source->fog_slope;
    snapshot.fog_color = source->fog_color;
    snapshot.material_color = source->material;
    snapshot.material_alpha = (uint32_t)source->material_alpha;
    snapshot.material_emissive = source->mat_emissive;
    snapshot.material_diffuse = source->mat_diffuse;
    snapshot.material_specular = source->mat_specular;
    snapshot.material_update = source->mat_update;
    snapshot.ambient_color = source->amb_color;
    snapshot.ambient_alpha = (uint32_t)source->amb_alpha;
    snapshot.specular_coefficient = source->spec_coef;
    for (uint32_t i = 0; i < 4; i++) {
        GeTnlLight *light = &snapshot.lights[i];
        light->enabled = source->light_on[i] ? 1u : 0u;
        light->type = source->light_type[i];
        memcpy(
            light->position, source->light_pos[i],
            sizeof(light->position));
        memcpy(
            light->direction, source->light_dir[i],
            sizeof(light->direction));
        memcpy(
            light->attenuation, source->light_att[i],
            sizeof(light->attenuation));
        light->spot_exponent = source->light_spot_exp[i];
        light->spot_cutoff = source->light_spot_cut[i];
        light->ambient_color = source->light_acol[i];
        light->diffuse_color = source->light_dcol[i];
        light->specular_color = source->light_scol[i];
    }

    *out = snapshot;
    return 1;
}

static inline void ge_tnl_gpu_matrix43(
    float output[3][4], const float input[12]) {
    for (uint32_t row = 0; row < 3; row++) {
        output[row][0] = input[row];
        output[row][1] = input[row + 3u];
        output[row][2] = input[row + 6u];
        output[row][3] = input[row + 9u];
    }
}

static inline void ge_tnl_gpu_matrix44(
    float output[4][4], const float input[16]) {
    for (uint32_t row = 0; row < 4; row++) {
        output[row][0] = input[row];
        output[row][1] = input[row + 4u];
        output[row][2] = input[row + 8u];
        output[row][3] = input[row + 12u];
    }
}

static inline void ge_tnl_gpu_state_build(
    const GeTnlState *state, GeTnlGpuState *out) {
    uint32_t enabled_lights = 0;
    memset(out, 0, sizeof(*out));
    if (!state) return;
    ge_tnl_gpu_matrix43(out->world, state->world);
    ge_tnl_gpu_matrix43(out->view, state->view);
    ge_tnl_gpu_matrix44(out->projection, state->projection);
    ge_tnl_gpu_matrix43(out->texture, state->texture);
    out->texture_params[0] = state->texture_scale[0];
    out->texture_params[1] = state->texture_scale[1];
    out->texture_params[2] = state->texture_offset[0];
    out->texture_params[3] = state->texture_offset[1];
    memcpy(
        out->viewport_scale, state->viewport_scale,
        sizeof(state->viewport_scale));
    memcpy(
        out->viewport_center, state->viewport_center,
        sizeof(state->viewport_center));
    out->viewport_offset_size[0] = state->viewport_offset[0];
    out->viewport_offset_size[1] = state->viewport_offset[1];
    out->viewport_offset_size[2] = (float)state->texture_width;
    out->viewport_offset_size[3] = (float)state->texture_height;
    out->fog_material[0] = state->fog_end;
    out->fog_material[1] = state->fog_slope;
    out->fog_material[2] = state->specular_coefficient;
    out->config[0] = state->flags;
    out->config[1] = state->vertex_flags;
    out->config[2] = state->texture_map_mode;
    out->config[3] = state->texture_shade_lights;
    out->depth_material[0] = state->minimum_depth;
    out->depth_material[1] = state->maximum_depth;
    out->depth_material[2] = state->material_color;
    out->depth_material[3] = state->material_alpha;
    out->material_colors[0] = state->material_emissive;
    out->material_colors[1] = state->material_diffuse;
    out->material_colors[2] = state->material_specular;
    out->material_colors[3] = state->ambient_color;
    out->material_config[0] = state->material_update;
    out->material_config[1] = state->ambient_alpha;
    for (uint32_t i = 0; i < 4; i++) {
        const GeTnlLight *light = &state->lights[i];
        if (light->enabled) enabled_lights |= 1u << i;
        memcpy(
            out->light_position[i], light->position,
            sizeof(light->position));
        memcpy(
            out->light_direction[i], light->direction,
            sizeof(light->direction));
        out->light_direction[i][3] = light->spot_exponent;
        memcpy(
            out->light_attenuation[i], light->attenuation,
            sizeof(light->attenuation));
        out->light_attenuation[i][3] = light->spot_cutoff;
        out->light_colors[i][0] = light->ambient_color;
        out->light_colors[i][1] = light->diffuse_color;
        out->light_colors[i][2] = light->specular_color;
        out->light_colors[i][3] = light->type;
    }
    out->material_config[2] = enabled_lights;
}

static inline void ge_tnl_mat43(
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

static inline void ge_tnl_mat44(
    const float matrix[16], const float input[3], float output[4]) {
    output[0] =
        matrix[0] * input[0] + matrix[4] * input[1] +
        matrix[8] * input[2] + matrix[12];
    output[1] =
        matrix[1] * input[0] + matrix[5] * input[1] +
        matrix[9] * input[2] + matrix[13];
    output[2] =
        matrix[2] * input[0] + matrix[6] * input[1] +
        matrix[10] * input[2] + matrix[14];
    output[3] =
        matrix[3] * input[0] + matrix[7] * input[1] +
        matrix[11] * input[2] + matrix[15];
}

typedef struct GeTnlReferenceVertex {
    float screen[3];
    float uv[2];
    float reciprocal_w;
    float fog;
    float color[4];
} GeTnlReferenceVertex;

static inline void ge_tnl_validation_pack(
    const GeTnlReferenceVertex *source, GeTnlValidationVertex *out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->screen_reciprocal_w, source->screen, sizeof(source->screen));
    out->screen_reciprocal_w[3] = source->reciprocal_w;
    memcpy(out->uv_fog, source->uv, sizeof(source->uv));
    out->uv_fog[2] = source->fog;
    memcpy(out->color, source->color, sizeof(source->color));
}

static inline uint32_t ge_tnl_validation_ordered_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits & UINT32_C(0x80000000)
        ? ~bits : bits | UINT32_C(0x80000000);
}

static inline int ge_tnl_validation_compare(
    const GeTnlValidationVertex *expected,
    const GeTnlValidationVertex *actual,
    uint32_t *mismatch_component) {
    float expected_values[12];
    float actual_values[12];
    memcpy(expected_values, expected, sizeof(expected_values));
    memcpy(actual_values, actual, sizeof(actual_values));
    for (uint32_t i = 0; i < 12; i++) {
        float lhs = expected_values[i];
        float rhs = actual_values[i];
        if (lhs == rhs)
            continue;
        if (!isfinite(lhs) || !isfinite(rhs)) {
            if (mismatch_component) *mismatch_component = i;
            return 0;
        }
        float difference = fabsf(lhs - rhs);
        if (i == 7u || i >= 8u) {
            if (difference <= 1e-6f)
                continue;
        } else {
            uint32_t lhs_ordered =
                ge_tnl_validation_ordered_float(lhs);
            uint32_t rhs_ordered =
                ge_tnl_validation_ordered_float(rhs);
            uint32_t ulps = lhs_ordered > rhs_ordered
                ? lhs_ordered - rhs_ordered
                : rhs_ordered - lhs_ordered;
            if (difference <= 1e-6f || ulps <= 64u)
                continue;
        }
        if (mismatch_component) *mismatch_component = i;
        return 0;
    }
    return 1;
}

static inline int ge_tnl_validation_compare_exact(
    const GeTnlValidationVertex *expected,
    const GeTnlValidationVertex *actual,
    uint32_t *mismatch_component) {
    float expected_values[12];
    float actual_values[12];
    memcpy(expected_values, expected, sizeof(expected_values));
    memcpy(actual_values, actual, sizeof(actual_values));
    for (uint32_t i = 0; i < 12; i++) {
        if (i == 7u || expected_values[i] == actual_values[i])
            continue;
        if (mismatch_component) *mismatch_component = i;
        return 0;
    }
    return 1;
}

static inline float ge_tnl_clamp01(float value) {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

static inline float ge_tnl_color_channel(
    uint32_t color, uint32_t channel) {
    return (float)((color >> (channel * 8u)) & 255u) *
        (1.0f / 255.0f);
}

static inline float ge_tnl_length3(const float value[3]) {
    return sqrtf(
        value[0] * value[0] + value[1] * value[1] +
        value[2] * value[2]);
}

static inline void ge_tnl_normalize3(float value[3]) {
    float length = ge_tnl_length3(value);
    if (length > 0.0f) {
        value[0] /= length;
        value[1] /= length;
        value[2] /= length;
    }
}

static inline void ge_tnl_mat33(
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

static inline void ge_tnl_gpu_mat43(
    const float matrix[3][4], const float input[3], float w,
    float output[3]) {
    for (uint32_t row = 0; row < 3; row++) {
        output[row] =
            matrix[row][0] * input[0] +
            matrix[row][1] * input[1] +
            matrix[row][2] * input[2] +
            matrix[row][3] * w;
    }
}

static inline void ge_tnl_gpu_mat44(
    const float matrix[4][4], const float input[3], float output[4]) {
    for (uint32_t row = 0; row < 4; row++) {
        output[row] =
            matrix[row][0] * input[0] +
            matrix[row][1] * input[1] +
            matrix[row][2] * input[2] +
            matrix[row][3];
    }
}

static inline float ge_tnl_reference_fog(
    float fog_end, float fog_slope, float view_z) {
    if (isinf(fog_end) || isnan(fog_end)) {
        int negative = signbit(fog_end) ? 1 : 0;
        if (signbit(fog_slope)) negative = !negative;
        if (fog_slope == 0.0f) negative = 1;
        return negative ? 0.0f : 1.0f;
    }
    if (isinf(fog_slope) || isnan(fog_slope))
        fog_slope = signbit(fog_slope) ? -262144.0f : 262144.0f;
    return ge_tnl_clamp01((view_z + fog_end) * fog_slope);
}

static inline void ge_tnl_reference_light(
    const GeTnlState *state, const GeTnlVertex *vertex,
    const float world_position[3], const float world_normal[3],
    float output[4]) {
    float vertex_color[4];
    for (uint32_t i = 0; i < 4; i++)
        vertex_color[i] = (float)vertex->color[i] * (1.0f / 255.0f);
    uint32_t material_update = state->material_update;
    float ambient_material[3];
    float diffuse_material[3];
    float specular_material[3];
    for (uint32_t i = 0; i < 3; i++) {
        ambient_material[i] = material_update & 1u
            ? vertex_color[i]
            : ge_tnl_color_channel(state->material_color, i);
        diffuse_material[i] = material_update & 2u
            ? vertex_color[i]
            : ge_tnl_color_channel(state->material_diffuse, i);
        specular_material[i] = material_update & 4u
            ? vertex_color[i]
            : ge_tnl_color_channel(state->material_specular, i);
        output[i] =
            ge_tnl_color_channel(state->material_emissive, i) +
            ge_tnl_color_channel(state->ambient_color, i) *
                ambient_material[i];
    }
    for (uint32_t light_index = 0; light_index < 4; light_index++) {
        const GeTnlLight *light = &state->lights[light_index];
        if (!light->enabled) continue;
        uint32_t computation = light->type & 3u;
        uint32_t type = (light->type >> 8u) & 3u;
        float light_vector[3] = {
            light->position[0], light->position[1],
            light->position[2],
        };
        float attenuation = 1.0f;
        if (type != 0u) {
            for (uint32_t i = 0; i < 3; i++)
                light_vector[i] -= world_position[i];
            float distance_squared =
                light_vector[0] * light_vector[0] +
                light_vector[1] * light_vector[1] +
                light_vector[2] * light_vector[2];
            float distance = sqrtf(distance_squared);
            float denominator =
                light->attenuation[0] +
                light->attenuation[1] * distance +
                light->attenuation[2] * distance_squared;
            attenuation = denominator > 0.0f
                ? ge_tnl_clamp01(1.0f / denominator) : 1.0f;
            if (distance > 0.0f) {
                for (uint32_t i = 0; i < 3; i++)
                    light_vector[i] /= distance;
            }
        } else {
            ge_tnl_normalize3(light_vector);
        }
        float diffuse =
            world_normal[0] * light_vector[0] +
            world_normal[1] * light_vector[1] +
            world_normal[2] * light_vector[2];
        if (diffuse < 0.0f) diffuse = 0.0f;
        if (computation == 2u && state->specular_coefficient > 0.0f)
            diffuse = powf(diffuse, state->specular_coefficient);
        for (uint32_t i = 0; i < 3; i++) {
            output[i] += attenuation * (
                ge_tnl_color_channel(light->ambient_color, i) *
                    ambient_material[i] +
                ge_tnl_color_channel(light->diffuse_color, i) *
                    diffuse_material[i] * diffuse);
        }
        if ((computation & 1u) != 0u && diffuse > 0.0f) {
            float half_vector[3] = {
                light_vector[0], light_vector[1],
                light_vector[2] + 1.0f,
            };
            float half_length = ge_tnl_length3(half_vector);
            if (half_length > 0.0f) {
                float normal_half =
                    (world_normal[0] * half_vector[0] +
                     world_normal[1] * half_vector[1] +
                     world_normal[2] * half_vector[2]) /
                    half_length;
                if (normal_half > 0.0f) {
                    float specular =
                        state->specular_coefficient > 0.0f
                            ? powf(
                                normal_half,
                                state->specular_coefficient)
                            : normal_half;
                    for (uint32_t i = 0; i < 3; i++) {
                        output[i] += attenuation *
                            ge_tnl_color_channel(
                                light->specular_color, i) *
                            specular_material[i] * specular;
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < 3; i++) {
        output[i] =
            floorf(ge_tnl_clamp01(output[i]) * 255.0f + 0.5f) *
            (1.0f / 255.0f);
    }
    uint32_t material_alpha = material_update & 1u
        ? vertex->color[3]
        : (state->flags & GE_TNL_STATE_MATERIAL_ALPHA_SET
            ? state->material_alpha : 255u);
    uint32_t alpha =
        (material_alpha * state->ambient_alpha + 127u) / 255u;
    output[3] = (float)(alpha > 255u ? 255u : alpha) *
        (1.0f / 255.0f);
}

static inline void ge_tnl_gpu_reference_light(
    const GeTnlGpuState *state, const GeTnlVertex *vertex,
    const float world_position[3], const float world_normal[3],
    float output[4]) {
    float vertex_color[4];
    for (uint32_t i = 0; i < 4; i++)
        vertex_color[i] = (float)vertex->color[i] * (1.0f / 255.0f);
    uint32_t material_update = state->material_config[0];
    float ambient_material[3];
    float diffuse_material[3];
    float specular_material[3];
    for (uint32_t i = 0; i < 3; i++) {
        ambient_material[i] = material_update & 1u
            ? vertex_color[i]
            : ge_tnl_color_channel(state->depth_material[2], i);
        diffuse_material[i] = material_update & 2u
            ? vertex_color[i]
            : ge_tnl_color_channel(state->material_colors[1], i);
        specular_material[i] = material_update & 4u
            ? vertex_color[i]
            : ge_tnl_color_channel(state->material_colors[2], i);
        output[i] =
            ge_tnl_color_channel(state->material_colors[0], i) +
            ge_tnl_color_channel(state->material_colors[3], i) *
                ambient_material[i];
    }
    for (uint32_t light_index = 0; light_index < 4; light_index++) {
        if ((state->material_config[2] & (1u << light_index)) == 0u)
            continue;
        uint32_t light_type = state->light_colors[light_index][3];
        uint32_t computation = light_type & 3u;
        uint32_t type = (light_type >> 8u) & 3u;
        float light_vector[3] = {
            state->light_position[light_index][0],
            state->light_position[light_index][1],
            state->light_position[light_index][2],
        };
        float attenuation = 1.0f;
        if (type != 0u) {
            for (uint32_t i = 0; i < 3; i++)
                light_vector[i] -= world_position[i];
            float distance_squared =
                light_vector[0] * light_vector[0] +
                light_vector[1] * light_vector[1] +
                light_vector[2] * light_vector[2];
            float distance = sqrtf(distance_squared);
            float denominator =
                state->light_attenuation[light_index][0] +
                state->light_attenuation[light_index][1] * distance +
                state->light_attenuation[light_index][2] *
                    distance_squared;
            attenuation = denominator > 0.0f
                ? ge_tnl_clamp01(1.0f / denominator) : 1.0f;
            if (distance > 0.0f) {
                for (uint32_t i = 0; i < 3; i++)
                    light_vector[i] /= distance;
            }
        } else {
            ge_tnl_normalize3(light_vector);
        }
        float diffuse =
            world_normal[0] * light_vector[0] +
            world_normal[1] * light_vector[1] +
            world_normal[2] * light_vector[2];
        if (diffuse < 0.0f) diffuse = 0.0f;
        if (computation == 2u && state->fog_material[2] > 0.0f)
            diffuse = powf(diffuse, state->fog_material[2]);
        for (uint32_t i = 0; i < 3; i++) {
            output[i] += attenuation * (
                ge_tnl_color_channel(
                    state->light_colors[light_index][0], i) *
                    ambient_material[i] +
                ge_tnl_color_channel(
                    state->light_colors[light_index][1], i) *
                    diffuse_material[i] * diffuse);
        }
        if ((computation & 1u) != 0u && diffuse > 0.0f) {
            float half_vector[3] = {
                light_vector[0], light_vector[1],
                light_vector[2] + 1.0f,
            };
            float half_length = ge_tnl_length3(half_vector);
            if (half_length > 0.0f) {
                float normal_half =
                    (world_normal[0] * half_vector[0] +
                     world_normal[1] * half_vector[1] +
                     world_normal[2] * half_vector[2]) /
                    half_length;
                if (normal_half > 0.0f) {
                    float specular = state->fog_material[2] > 0.0f
                        ? powf(normal_half, state->fog_material[2])
                        : normal_half;
                    for (uint32_t i = 0; i < 3; i++) {
                        output[i] += attenuation *
                            ge_tnl_color_channel(
                                state->light_colors[light_index][2], i) *
                            specular_material[i] * specular;
                    }
                }
            }
        }
    }
    for (uint32_t i = 0; i < 3; i++) {
        output[i] =
            floorf(ge_tnl_clamp01(output[i]) * 255.0f + 0.5f) *
            (1.0f / 255.0f);
    }
    uint32_t material_alpha = material_update & 1u
        ? vertex->color[3]
        : (state->config[0] & GE_TNL_STATE_MATERIAL_ALPHA_SET
            ? state->depth_material[3] : 255u);
    uint32_t alpha =
        (material_alpha * state->material_config[1] + 127u) / 255u;
    output[3] = (float)(alpha > 255u ? 255u : alpha) *
        (1.0f / 255.0f);
}

static inline int ge_tnl_reference_transform(
    const GeTnlState *state, const GeTnlVertex *vertex,
    GeTnlReferenceVertex *out) {
    if (!state || !vertex || !out) return 0;
    float world_position[3];
    float view_position[3];
    float clip_position[4];
    float world_normal[3];
    ge_tnl_mat43(state->world, vertex->position, world_position);
    ge_tnl_mat43(state->view, world_position, view_position);
    ge_tnl_mat44(state->projection, view_position, clip_position);
    if (clip_position[3] == 0.0f) return 0;
    ge_tnl_mat33(state->world, vertex->normal, world_normal);
    ge_tnl_normalize3(world_normal);

    uint32_t texture_mode = state->texture_map_mode & 3u;
    float uv[2];
    if (texture_mode == 1u) {
        uint32_t source_mode =
            (state->texture_map_mode >> 8u) & 3u;
        float source[3];
        if (source_mode == 0u) {
            memcpy(source, vertex->position, sizeof(source));
        } else if (source_mode == 1u) {
            source[0] = vertex->texcoord[0];
            source[1] = vertex->texcoord[1];
            source[2] = 0.0f;
        } else {
            memcpy(source, vertex->normal, sizeof(source));
            if (source_mode == 2u) ge_tnl_normalize3(source);
        }
        float generated[3];
        ge_tnl_mat43(state->texture, source, generated);
        float q = generated[2] == 0.0f ? 1.0f : generated[2];
        uv[0] = generated[0] / q * (float)state->texture_width;
        uv[1] = generated[1] / q * (float)state->texture_height;
    } else if (texture_mode == 2u) {
        for (uint32_t i = 0; i < 2; i++) {
            uint32_t light_index =
                (state->texture_shade_lights >> (i * 8u)) & 3u;
            float direction[3];
            memcpy(
                direction, state->lights[light_index].position,
                sizeof(direction));
            ge_tnl_normalize3(direction);
            uv[i] = (1.0f +
                direction[0] * world_normal[0] +
                direction[1] * world_normal[1] +
                direction[2] * world_normal[2]) * 0.5f;
        }
        uv[0] *= (float)state->texture_width;
        uv[1] *= (float)state->texture_height;
    } else {
        uv[0] =
            (vertex->texcoord[0] * state->texture_scale[0] +
             state->texture_offset[0]) *
            (float)state->texture_width;
        uv[1] =
            (vertex->texcoord[1] * state->texture_scale[1] +
             state->texture_offset[1]) *
            (float)state->texture_height;
    }

    float inverse_w = 1.0f / clip_position[3];
    for (uint32_t i = 0; i < 3; i++) {
        out->screen[i] =
            clip_position[i] * inverse_w * state->viewport_scale[i] +
            state->viewport_center[i];
    }
    out->screen[0] -= state->viewport_offset[0];
    out->screen[1] -= state->viewport_offset[1];
    out->screen[0] =
        floorf(out->screen[0] * 16.0f + 0.375f) * (1.0f / 16.0f);
    out->screen[1] =
        floorf(out->screen[1] * 16.0f + 0.375f) * (1.0f / 16.0f);
    if (out->screen[2] < 0.0f) out->screen[2] = 0.0f;
    if (out->screen[2] > 65535.0f) out->screen[2] = 65535.0f;
    out->uv[0] = uv[0] * inverse_w;
    out->uv[1] = uv[1] * inverse_w;
    out->reciprocal_w = inverse_w;
    out->fog = state->flags & GE_TNL_STATE_FOG
        ? ge_tnl_reference_fog(
            state->fog_end, state->fog_slope, view_position[2])
        : 1.0f;
    if (state->flags & GE_TNL_STATE_LIGHTING) {
        ge_tnl_reference_light(
            state, vertex, world_position, world_normal, out->color);
    } else {
        for (uint32_t i = 0; i < 4; i++)
            out->color[i] =
                (float)vertex->color[i] * (1.0f / 255.0f);
    }
    return 1;
}

static inline int ge_tnl_reference_clip_transform(
    const GeTnlState *state, const GeTnlVertex *vertex,
    GeTnlClipVertex *out) {
    if (!state || !vertex || !out) return 0;
    GeTnlReferenceVertex reference;
    if (!ge_tnl_reference_transform(state, vertex, &reference))
        return 0;
    float world[3];
    float view[3];
    ge_tnl_mat43(state->world, vertex->position, world);
    ge_tnl_mat43(state->view, world, view);
    ge_tnl_mat44(state->projection, view, out->clip);
    if (!isfinite(out->clip[0]) || !isfinite(out->clip[1]) ||
        !isfinite(out->clip[2]) || !isfinite(out->clip[3])) {
        return 0;
    }
    out->uv_fog[0] = reference.uv[0] / reference.reciprocal_w;
    out->uv_fog[1] = reference.uv[1] / reference.reciprocal_w;
    out->uv_fog[2] = reference.fog;
    out->uv_fog[3] = 0.0f;
    for (uint32_t i = 0; i < 4; i++) {
        float channel = floorf(reference.color[i] * 255.0f + 0.5f);
        out->color[i] = channel < 0.0f ? 0u
            : channel > 255.0f ? 255u : (uint32_t)channel;
    }
    return 1;
}

static inline float ge_tnl_clip_near_distance(
    const GeTnlClipVertex *vertex) {
    return vertex->clip[2] + vertex->clip[3];
}

static inline int ge_tnl_clip_original_out_of_range(
    const GeTnlState *state, const GeTnlClipVertex *vertex) {
    float inverse_w = 1.0f / vertex->clip[3];
    float screen_x =
        vertex->clip[0] * state->viewport_scale[0] * inverse_w +
        state->viewport_center[0];
    float screen_y =
        vertex->clip[1] * state->viewport_scale[1] * inverse_w +
        state->viewport_center[1];
    float screen_z =
        vertex->clip[2] * state->viewport_scale[2] * inverse_w +
        state->viewport_center[2];
    const float screen_bound = 4095.0f + 15.5f / 16.0f;
    if (state->flags & GE_TNL_STATE_DEPTH_CLIP) {
        return vertex->clip[2] > -vertex->clip[3] &&
            (screen_x >= screen_bound || screen_y >= screen_bound ||
             screen_x < 0.0f || screen_y < 0.0f);
    }
    return screen_x > screen_bound || screen_y >= screen_bound ||
        screen_x < 0.0f || screen_y < 0.0f ||
        screen_z < 0.0f || screen_z >= 65536.0f;
}

static inline int ge_tnl_clip_created_out_of_range(
    const GeTnlState *state, const GeTnlClipVertex *vertex) {
    const float near_w = 1e-3f;
    if (vertex->clip[3] <= near_w) return 1;
    float inverse_w = 1.0f / vertex->clip[3];
    float screen_x =
        vertex->clip[0] * state->viewport_scale[0] * inverse_w +
        state->viewport_center[0];
    float screen_y =
        vertex->clip[1] * state->viewport_scale[1] * inverse_w +
        state->viewport_center[1];
    float screen_z =
        vertex->clip[2] * state->viewport_scale[2] * inverse_w +
        state->viewport_center[2];
    const float screen_bound = 4095.0f + 15.5f / 16.0f;
    if (state->flags & GE_TNL_STATE_DEPTH_CLIP) {
        return screen_x >= screen_bound || screen_y >= screen_bound ||
            screen_x < 0.0f || screen_y < 0.0f;
    }
    return screen_x > screen_bound || screen_y >= screen_bound ||
        screen_x < 0.0f || screen_y < 0.0f ||
        screen_z < 0.0f || screen_z >= 65536.0f;
}

static inline void ge_tnl_clip_lerp(
    const GeTnlClipVertex *first, const GeTnlClipVertex *second,
    float amount, GeTnlClipVertex *out) {
    for (uint32_t i = 0; i < 4; i++) {
        out->clip[i] =
            first->clip[i] +
            (second->clip[i] - first->clip[i]) * amount;
        out->uv_fog[i] =
            first->uv_fog[i] +
            (second->uv_fog[i] - first->uv_fog[i]) * amount;
        int32_t difference =
            (int32_t)second->color[i] - (int32_t)first->color[i];
        out->color[i] = (uint32_t)(
            (int32_t)first->color[i] +
            (int32_t)((float)difference * amount));
    }
}

static inline int ge_tnl_clip_project(
    const GeTnlState *state, const GeTnlClipVertex *vertex,
    GeTnlScreenVertex *out) {
    if (!state || !vertex || !out || vertex->clip[3] == 0.0f)
        return 0;
    float inverse_w = 1.0f / vertex->clip[3];
    out->position[0] =
        vertex->clip[0] * state->viewport_scale[0] * inverse_w +
        state->viewport_center[0] - state->viewport_offset[0];
    out->position[1] =
        vertex->clip[1] * state->viewport_scale[1] * inverse_w +
        state->viewport_center[1] - state->viewport_offset[1];
    out->position[2] =
        vertex->clip[2] * state->viewport_scale[2] * inverse_w +
        state->viewport_center[2];
    out->position[0] =
        floorf(out->position[0] * 16.0f + 0.375f) *
        (1.0f / 16.0f);
    out->position[1] =
        floorf(out->position[1] * 16.0f + 0.375f) *
        (1.0f / 16.0f);
    if (out->position[2] < 0.0f) out->position[2] = 0.0f;
    if (out->position[2] > 65535.0f) out->position[2] = 65535.0f;
    out->position[3] = inverse_w;
    out->texcoord[0] = vertex->uv_fog[0] * inverse_w;
    out->texcoord[1] = vertex->uv_fog[1] * inverse_w;
    out->fog = vertex->uv_fog[2];
    out->color =
        (vertex->color[0] & 255u) |
        ((vertex->color[1] & 255u) << 8u) |
        ((vertex->color[2] & 255u) << 16u) |
        ((vertex->color[3] & 255u) << 24u);
    return 1;
}

/* Returns 0, 3, or 6 screen vertices. Submission order must already include
 * strip parity and PSP cull-mode reversal, as the canonical index builder does. */
static inline uint32_t ge_tnl_reference_preprocess_triangle(
    const GeTnlState *state, const GeTnlClipVertex input[3],
    GeTnlScreenVertex output[6]) {
    if (!state || !input || !output) return 0;
    int outside_positive[3];
    int outside_negative[3];
    int all_negative_w = 1;
    int any_out_of_range = 0;
    for (uint32_t i = 0; i < 3; i++) {
        if (!isfinite(input[i].clip[0]) ||
            !isfinite(input[i].clip[1]) ||
            !isfinite(input[i].clip[2]) ||
            !isfinite(input[i].clip[3])) {
            return 0;
        }
        float depth = input[i].clip[2] / input[i].clip[3];
        outside_positive[i] = depth >= 1.000030517578125f;
        outside_negative[i] = -depth >= 1.000030517578125f;
        if (input[i].clip[3] >= 0.0f) all_negative_w = 0;
        if (ge_tnl_clip_original_out_of_range(state, &input[i]))
            any_out_of_range = 1;
    }
    if (any_out_of_range || all_negative_w) return 0;
    int positive_count =
        outside_positive[0] + outside_positive[1] +
        outside_positive[2];
    int negative_count =
        outside_negative[0] + outside_negative[1] +
        outside_negative[2];
    if (!(state->flags & GE_TNL_STATE_DEPTH_CLIP) &&
        positive_count + negative_count > 0) {
        return 0;
    }
    if (positive_count == 3 || negative_count == 3) return 0;

    float distance[3] = {
        ge_tnl_clip_near_distance(&input[0]),
        ge_tnl_clip_near_distance(&input[1]),
        ge_tnl_clip_near_distance(&input[2]),
    };
    if (distance[0] >= 0.0f && distance[1] >= 0.0f &&
        distance[2] >= 0.0f) {
        const float near_w = 1e-3f;
        if (input[0].clip[3] <= near_w ||
            input[1].clip[3] <= near_w ||
            input[2].clip[3] <= near_w) {
            return 0;
        }
        for (uint32_t i = 0; i < 3; i++)
            if (!ge_tnl_clip_project(state, &input[i], &output[i]))
                return 0;
        return 3;
    }
    if (distance[0] < 0.0f && distance[1] < 0.0f &&
        distance[2] < 0.0f) {
        return 0;
    }

    GeTnlClipVertex clipped[4];
    uint8_t created[4];
    uint32_t clipped_count = 0;
    for (uint32_t i = 0; i < 3; i++) {
        uint32_t next = (i + 1u) % 3u;
        int first_inside = distance[i] >= 0.0f;
        int second_inside = distance[next] >= 0.0f;
        if (first_inside) {
            clipped[clipped_count] = input[i];
            created[clipped_count++] = 0;
        }
        if (first_inside != second_inside) {
            float amount =
                distance[i] / (distance[i] - distance[next]);
            ge_tnl_clip_lerp(
                &input[i], &input[next], amount,
                &clipped[clipped_count]);
            created[clipped_count++] = 1;
        }
    }
    if (clipped_count < 3 || clipped_count > 4) return 0;

    uint8_t bad[4] = {0, 0, 0, 0};
    const float near_w = 1e-3f;
    for (uint32_t i = 0; i < clipped_count; i++) {
        bad[i] =
            clipped[i].clip[3] <= near_w ||
            (created[i] &&
             ge_tnl_clip_created_out_of_range(state, &clipped[i]));
    }
    uint32_t output_count = 0;
    for (uint32_t i = 2; i < clipped_count; i++) {
        if (bad[0] || bad[i - 1u] || bad[i]) continue;
        if (!ge_tnl_clip_project(
                state, &clipped[0], &output[output_count]) ||
            !ge_tnl_clip_project(
                state, &clipped[i - 1u], &output[output_count + 1u]) ||
            !ge_tnl_clip_project(
                state, &clipped[i], &output[output_count + 2u])) {
            return 0;
        }
        output_count += 3u;
    }
    return output_count;
}

static inline int ge_tnl_gpu_reference_transform(
    const GeTnlGpuState *state, const GeTnlVertex *vertex,
    GeTnlReferenceVertex *out) {
    if (!state || !vertex || !out) return 0;
    float world_position[3];
    float view_position[3];
    float clip_position[4];
    float world_normal[3];
    ge_tnl_gpu_mat43(
        state->world, vertex->position, 1.0f, world_position);
    ge_tnl_gpu_mat43(
        state->view, world_position, 1.0f, view_position);
    ge_tnl_gpu_mat44(
        state->projection, view_position, clip_position);
    if (clip_position[3] == 0.0f) return 0;
    ge_tnl_gpu_mat43(
        state->world, vertex->normal, 0.0f, world_normal);
    ge_tnl_normalize3(world_normal);

    uint32_t texture_mode = state->config[2] & 3u;
    float uv[2];
    if (texture_mode == 1u) {
        uint32_t source_mode = (state->config[2] >> 8u) & 3u;
        float source[3];
        if (source_mode == 0u) {
            memcpy(source, vertex->position, sizeof(source));
        } else if (source_mode == 1u) {
            source[0] = vertex->texcoord[0];
            source[1] = vertex->texcoord[1];
            source[2] = 0.0f;
        } else {
            memcpy(source, vertex->normal, sizeof(source));
            if (source_mode == 2u) ge_tnl_normalize3(source);
        }
        float generated[3];
        ge_tnl_gpu_mat43(
            state->texture, source, 1.0f, generated);
        float q = generated[2] == 0.0f ? 1.0f : generated[2];
        uv[0] =
            generated[0] / q * state->viewport_offset_size[2];
        uv[1] =
            generated[1] / q * state->viewport_offset_size[3];
    } else if (texture_mode == 2u) {
        for (uint32_t i = 0; i < 2; i++) {
            uint32_t light_index =
                (state->config[3] >> (i * 8u)) & 3u;
            float direction[3] = {
                state->light_position[light_index][0],
                state->light_position[light_index][1],
                state->light_position[light_index][2],
            };
            ge_tnl_normalize3(direction);
            uv[i] = (1.0f +
                direction[0] * world_normal[0] +
                direction[1] * world_normal[1] +
                direction[2] * world_normal[2]) * 0.5f;
        }
        uv[0] *= state->viewport_offset_size[2];
        uv[1] *= state->viewport_offset_size[3];
    } else {
        uv[0] =
            (vertex->texcoord[0] * state->texture_params[0] +
             state->texture_params[2]) *
            state->viewport_offset_size[2];
        uv[1] =
            (vertex->texcoord[1] * state->texture_params[1] +
             state->texture_params[3]) *
            state->viewport_offset_size[3];
    }

    float inverse_w = 1.0f / clip_position[3];
    for (uint32_t i = 0; i < 3; i++) {
        out->screen[i] =
            clip_position[i] * inverse_w * state->viewport_scale[i] +
            state->viewport_center[i];
    }
    out->screen[0] -= state->viewport_offset_size[0];
    out->screen[1] -= state->viewport_offset_size[1];
    out->screen[0] =
        floorf(out->screen[0] * 16.0f + 0.375f) * (1.0f / 16.0f);
    out->screen[1] =
        floorf(out->screen[1] * 16.0f + 0.375f) * (1.0f / 16.0f);
    if (out->screen[2] < 0.0f) out->screen[2] = 0.0f;
    if (out->screen[2] > 65535.0f) out->screen[2] = 65535.0f;
    out->uv[0] = uv[0] * inverse_w;
    out->uv[1] = uv[1] * inverse_w;
    out->reciprocal_w = inverse_w;
    out->fog = state->config[0] & GE_TNL_STATE_FOG
        ? ge_tnl_reference_fog(
            state->fog_material[0], state->fog_material[1],
            view_position[2])
        : 1.0f;
    if (state->config[0] & GE_TNL_STATE_LIGHTING) {
        ge_tnl_gpu_reference_light(
            state, vertex, world_position, world_normal, out->color);
    } else {
        for (uint32_t i = 0; i < 4; i++)
            out->color[i] =
                (float)vertex->color[i] * (1.0f / 255.0f);
    }
    return 1;
}

static inline GeTnlEligibility ge_tnl_vertex_classify(
    const GeTnlState *state, const GeTnlVertex *vertex) {
    float world[3];
    float view[3];
    float clip[4];
    ge_tnl_mat43(state->world, vertex->position, world);
    ge_tnl_mat43(state->view, world, view);
    ge_tnl_mat44(state->projection, view, clip);
    for (uint32_t i = 0; i < 4; i++)
        if (!isfinite(clip[i])) return GE_TNL_DECLINE_CLIP;
    if (clip[3] == 0.0f) return GE_TNL_DECLINE_CLIP;
    if (clip[3] <= 1e-3f || clip[2] + clip[3] < 0.0f)
        return GE_TNL_ELIGIBLE_PREPROCESS;
    float inverse_w = 1.0f / clip[3];
    float depth = clip[2] * inverse_w;
    if (depth <= -1.000030517578125f ||
        depth >= 1.000030517578125f) {
        return GE_TNL_ELIGIBLE_PREPROCESS;
    }
    float screen_x =
        clip[0] * state->viewport_scale[0] * inverse_w +
        state->viewport_center[0];
    float screen_y =
        clip[1] * state->viewport_scale[1] * inverse_w +
        state->viewport_center[1];
    float screen_z =
        clip[2] * state->viewport_scale[2] * inverse_w +
        state->viewport_center[2];
    const float screen_bound = 4095.0f + 15.5f / 16.0f;
    if (!isfinite(screen_x) || !isfinite(screen_y) ||
        !isfinite(screen_z) || screen_x < 0.0f ||
        screen_y < 0.0f || screen_x >= screen_bound ||
        screen_y >= screen_bound) {
        return GE_TNL_ELIGIBLE_PREPROCESS;
    }
    if (!(state->flags & GE_TNL_STATE_DEPTH_CLIP) &&
        (screen_z < 0.0f || screen_z >= 65536.0f)) {
        return GE_TNL_ELIGIBLE_PREPROCESS;
    }
    return GE_TNL_ELIGIBLE;
}

static inline GeTnlEligibility ge_tnl_classify(
    const GeTnlDraw *draw) {
    if (!draw || !draw->vertices || !draw->indices || !draw->state ||
        draw->state->abi_version != GE_TNL_ABI_VERSION ||
        !draw->vertex_count || draw->vertex_count > 65535u ||
        !draw->index_count) {
        return GE_TNL_DECLINE_INVALID;
    }
    int points = draw->primitive == GE_TNL_POINTS;
    int sprites = draw->primitive == GE_TNL_SPRITES;
    int triangles =
        draw->primitive == GE_TNL_TRIANGLES ||
        draw->primitive == GE_TNL_TRIANGLE_STRIP ||
        draw->primitive == GE_TNL_TRIANGLE_FAN;
    if (!points && !sprites && !triangles) {
        return GE_TNL_DECLINE_PRIMITIVE;
    }
    if (triangles && draw->index_count % 3u) {
        return GE_TNL_DECLINE_INVALID;
    }
    if (sprites && draw->index_count % 2u) {
        return GE_TNL_DECLINE_INVALID;
    }
    if (draw->state->flags & GE_TNL_STATE_CLEAR)
        return GE_TNL_DECLINE_CLEAR;
    if (triangles &&
        !(draw->state->flags & GE_TNL_STATE_GOURAUD))
        return GE_TNL_DECLINE_FLAT;
    for (uint32_t i = 0; i < draw->index_count; i++)
        if (draw->indices[i] >= draw->vertex_count)
            return GE_TNL_DECLINE_INVALID;
    GeTnlEligibility result = GE_TNL_ELIGIBLE;
    for (uint32_t i = 0; i < draw->vertex_count; i++) {
        GeTnlEligibility vertex_result = ge_tnl_vertex_classify(
            draw->state, &draw->vertices[i]);
        if (vertex_result == GE_TNL_DECLINE_CLIP)
            return vertex_result;
        if (vertex_result == GE_TNL_ELIGIBLE_PREPROCESS) {
            if (points || sprites) return GE_TNL_DECLINE_CLIP;
            result = vertex_result;
        }
    }
    return result;
}

static inline int ge_tnl_reference_expand_point(
    const GeTnlState *state, const GeTnlVertex *vertex,
    GeTnlScreenVertex output[6]) {
    if (!state || !vertex || !output) return 0;
    GeTnlClipVertex clip;
    GeTnlScreenVertex projected;
    if (!ge_tnl_reference_clip_transform(state, vertex, &clip) ||
        clip.clip[3] <= 1e-3f ||
        !ge_tnl_clip_project(state, &clip, &projected)) {
        return 0;
    }
    static const uint8_t offsets[6][2] = {
        {0, 0}, {1, 0}, {0, 1},
        {1, 0}, {1, 1}, {0, 1},
    };
    float x = floorf(projected.position[0]);
    float y = floorf(projected.position[1]);
    for (uint32_t i = 0; i < 6; i++) {
        output[i] = projected;
        output[i].position[0] = x + (float)offsets[i][0];
        output[i].position[1] = y + (float)offsets[i][1];
    }
    return 1;
}

static inline int ge_tnl_reference_expand_sprite(
    const GeTnlState *state, const GeTnlVertex controls[2],
    int linear_filter, GeTnlScreenVertex output[6]) {
    if (!state || !controls || !output) return 0;
    GeTnlClipVertex clip[2];
    GeTnlScreenVertex projected[2];
    for (uint32_t i = 0; i < 2; i++) {
        if (!ge_tnl_reference_clip_transform(
                state, &controls[i], &clip[i]) ||
            clip[i].clip[3] <= 1e-3f ||
            !ge_tnl_clip_project(
                state, &clip[i], &projected[i])) {
            return 0;
        }
    }
    float xa = floorf(fminf(
        projected[0].position[0], projected[1].position[0]));
    float xb = floorf(fmaxf(
        projected[0].position[0], projected[1].position[0]));
    float ya = floorf(fminf(
        projected[0].position[1], projected[1].position[1]));
    float yb = floorf(fmaxf(
        projected[0].position[1], projected[1].position[1]));
    if (xb <= xa || yb <= ya) return 0;

    float u0 =
        projected[0].texcoord[0] / projected[0].position[3];
    float v0 =
        projected[0].texcoord[1] / projected[0].position[3];
    float u1 =
        projected[1].texcoord[0] / projected[1].position[3];
    float v1 =
        projected[1].texcoord[1] / projected[1].position[3];
    float texel_width = (u1 - u0) / (xb - xa);
    float texel_height = (v1 - v0) / (yb - ya);
    float ua = linear_filter ? u0 : u0 - 0.5f * texel_width;
    float ub = linear_filter ? u1 : u1 - 0.5f * texel_width;
    float va = linear_filter ? v0 : v0 - 0.5f * texel_height;
    float vb = linear_filter ? v1 : v1 - 0.5f * texel_height;
    static const uint8_t corners[6][2] = {
        {0, 0}, {1, 0}, {0, 1},
        {1, 0}, {1, 1}, {0, 1},
    };
    for (uint32_t i = 0; i < 6; i++) {
        output[i] = projected[1];
        output[i].position[0] = corners[i][0] ? xb : xa;
        output[i].position[1] = corners[i][1] ? yb : ya;
        output[i].position[3] = 1.0f;
        output[i].texcoord[0] = corners[i][0] ? ub : ua;
        output[i].texcoord[1] = corners[i][1] ? vb : va;
    }
    return 1;
}

static inline int ge_tnl_reference_expand_through_sprite(
    const GeTnlVertex controls[2], int linear_filter,
    GeTnlScreenVertex output[6]) {
    if (!controls || !output) return 0;
    float xa = floorf(fminf(
        controls[0].position[0], controls[1].position[0]));
    float xb = floorf(fmaxf(
        controls[0].position[0], controls[1].position[0]));
    float ya = floorf(fminf(
        controls[0].position[1], controls[1].position[1]));
    float yb = floorf(fmaxf(
        controls[0].position[1], controls[1].position[1]));
    if (xb <= xa || yb <= ya) return 0;

    float texel_width =
        (controls[1].texcoord[0] - controls[0].texcoord[0]) /
        (xb - xa);
    float texel_height =
        (controls[1].texcoord[1] - controls[0].texcoord[1]) /
        (yb - ya);
    float ua = linear_filter
        ? controls[0].texcoord[0]
        : controls[0].texcoord[0] - 0.5f * texel_width;
    float ub = linear_filter
        ? controls[1].texcoord[0]
        : controls[1].texcoord[0] - 0.5f * texel_width;
    float va = linear_filter
        ? controls[0].texcoord[1]
        : controls[0].texcoord[1] - 0.5f * texel_height;
    float vb = linear_filter
        ? controls[1].texcoord[1]
        : controls[1].texcoord[1] - 0.5f * texel_height;
    uint32_t color =
        (uint32_t)controls[1].color[0] |
        ((uint32_t)controls[1].color[1] << 8u) |
        ((uint32_t)controls[1].color[2] << 16u) |
        ((uint32_t)controls[1].color[3] << 24u);
    static const uint8_t corners[6][2] = {
        {0, 0}, {1, 0}, {0, 1},
        {1, 0}, {1, 1}, {0, 1},
    };
    for (uint32_t i = 0; i < 6; i++) {
        output[i].position[0] = corners[i][0] ? xb : xa;
        output[i].position[1] = corners[i][1] ? yb : ya;
        output[i].position[2] = controls[1].position[2];
        output[i].position[3] = 1.0f;
        output[i].texcoord[0] = corners[i][0] ? ub : ua;
        output[i].texcoord[1] = corners[i][1] ? vb : va;
        output[i].fog = 1.0f;
        output[i].color = color;
    }
    return 1;
}

#endif
