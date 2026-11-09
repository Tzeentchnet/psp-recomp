#version 450

#if !defined(TNL_EXPAND_POINTS) && \
    !defined(TNL_EXPAND_SPRITES) && \
    !defined(TNL_EXPAND_THROUGH_SPRITES)
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_color;
#endif

layout(location = 0) noperspective out vec2 v_uv;
layout(location = 1) noperspective out float v_rw;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out vec4 v_color;

struct TnlState {
    vec4 world[3];
    vec4 view[3];
    vec4 projection[4];
    vec4 texture_matrix[3];
    vec4 texture_params;
    vec4 viewport_scale;
    vec4 viewport_center;
    vec4 viewport_offset_size;
    vec4 fog_material;
    uvec4 config;
    uvec4 depth_material;
    uvec4 material_colors;
    uvec4 material_config;
    vec4 light_position[4];
    vec4 light_direction[4];
    vec4 light_attenuation[4];
    uvec4 light_colors[4];
};

layout(std430, set = 3, binding = 0) readonly buffer TnlStates {
    TnlState values[];
} tnl_states;

#if defined(TNL_EXPAND_POINTS) || \
    defined(TNL_EXPAND_SPRITES) || \
    defined(TNL_EXPAND_THROUGH_SPRITES)
layout(std430, set = 3, binding = 2) readonly buffer TnlVertices {
    uint words[];
} tnl_vertices;

layout(std430, set = 3, binding = 3) readonly buffer TnlIndices {
    uint values[];
} tnl_indices;
#endif

#ifdef TNL_VALIDATE
struct TnlValidationVertex {
    vec4 screen_reciprocal_w;
    vec4 uv_fog;
    vec4 color;
};

layout(std430, set = 3, binding = 1) writeonly buffer TnlValidation {
    TnlValidationVertex values[];
} tnl_validation;
#endif

layout(push_constant) uniform PC {
    ivec4 config;
    uvec4 texture_state;
    ivec4 color_test_state;
    ivec4 stencil_state;
    ivec4 dither_rows;
    ivec4 blend_state;
    ivec4 fragment_state;
    uvec4 tnl;
} pc;

const uint STATE_LIGHTING = 2u;
const uint STATE_FOG = 16u;
const uint STATE_MATERIAL_ALPHA_SET = 1024u;

float ordered_dot4(vec4 row, vec4 value) {
    precise float result = row.x * value.x;
    result = result + row.y * value.y;
    result = result + row.z * value.z;
    result = result + row.w * value.w;
    return result;
}

vec3 transform43(vec4 rows[3], vec3 value, float w) {
    vec4 input_value = vec4(value, w);
    return vec3(
        ordered_dot4(rows[0], input_value),
        ordered_dot4(rows[1], input_value),
        ordered_dot4(rows[2], input_value));
}

vec4 transform44(vec4 rows[4], vec3 value) {
    vec4 input_value = vec4(value, 1.0);
    return vec4(
        ordered_dot4(rows[0], input_value),
        ordered_dot4(rows[1], input_value),
        ordered_dot4(rows[2], input_value),
        ordered_dot4(rows[3], input_value));
}

vec3 unpack_rgb(uint color) {
    return vec3(
        float(color & 255u),
        float((color >> 8u) & 255u),
        float((color >> 16u) & 255u)) * (1.0 / 255.0);
}

vec4 light_vertex(
    TnlState state, vec3 world_position, vec3 world_normal,
    vec4 vertex_color) {
    uint material_update = state.material_config.x;
    vec3 ambient_material = (material_update & 1u) != 0u
        ? vertex_color.rgb : unpack_rgb(state.depth_material.z);
    vec3 diffuse_material = (material_update & 2u) != 0u
        ? vertex_color.rgb : unpack_rgb(state.material_colors.y);
    vec3 specular_material = (material_update & 4u) != 0u
        ? vertex_color.rgb : unpack_rgb(state.material_colors.z);
    vec3 result =
        unpack_rgb(state.material_colors.x) +
        unpack_rgb(state.material_colors.w) * ambient_material;

    for (uint i = 0u; i < 4u; i++) {
        if ((state.material_config.z & (1u << i)) == 0u)
            continue;
        uint light_type = state.light_colors[i].w;
        uint computation = light_type & 3u;
        uint type = (light_type >> 8u) & 3u;
        vec3 light_vector = state.light_position[i].xyz;
        float attenuation = 1.0;
        if (type != 0u) {
            light_vector -= world_position;
            float distance_squared = dot(light_vector, light_vector);
            float distance_value = sqrt(distance_squared);
            float denominator =
                state.light_attenuation[i].x +
                state.light_attenuation[i].y * distance_value +
                state.light_attenuation[i].z * distance_squared;
            attenuation = denominator > 0.0
                ? clamp(1.0 / denominator, 0.0, 1.0) : 1.0;
            if (distance_value > 0.0)
                light_vector /= distance_value;
        } else {
            float distance_value = length(light_vector);
            if (distance_value > 0.0)
                light_vector /= distance_value;
        }

        float diffuse = max(dot(world_normal, light_vector), 0.0);
        if (computation == 2u && state.fog_material.z > 0.0)
            diffuse = pow(diffuse, state.fog_material.z);
        result += attenuation * (
            unpack_rgb(state.light_colors[i].x) * ambient_material +
            unpack_rgb(state.light_colors[i].y) *
                diffuse_material * diffuse);

        if ((computation & 1u) != 0u && diffuse > 0.0) {
            vec3 half_vector =
                vec3(light_vector.xy, light_vector.z + 1.0);
            float half_length = length(half_vector);
            if (half_length > 0.0) {
                float normal_half =
                    dot(world_normal, half_vector) / half_length;
                if (normal_half > 0.0) {
                    float specular = state.fog_material.z > 0.0
                        ? pow(normal_half, state.fog_material.z)
                        : normal_half;
                    result += attenuation *
                        unpack_rgb(state.light_colors[i].z) *
                        specular_material * specular;
                }
            }
        }
    }

    uint vertex_alpha =
        uint(floor(vertex_color.a * 255.0 + 0.5));
    uint material_alpha = (material_update & 1u) != 0u
        ? vertex_alpha
        : ((state.config.x & STATE_MATERIAL_ALPHA_SET) != 0u
            ? state.depth_material.w : 255u);
    uint alpha =
        (material_alpha * state.material_config.y + 127u) / 255u;
    vec3 rounded =
        floor(clamp(result, 0.0, 1.0) * 255.0 + 0.5) *
        (1.0 / 255.0);
    return vec4(rounded, float(min(alpha, 255u)) * (1.0 / 255.0));
}

float fog_factor(TnlState state, float view_z) {
    float fog_end = state.fog_material.x;
    float fog_slope = state.fog_material.y;
    if (isinf(fog_end) || isnan(fog_end)) {
        bool negative =
            (floatBitsToUint(fog_end) & 0x80000000u) != 0u;
        if ((floatBitsToUint(fog_slope) & 0x80000000u) != 0u)
            negative = !negative;
        if (fog_slope == 0.0)
            negative = true;
        return negative ? 0.0 : 1.0;
    }
    if (isinf(fog_slope) || isnan(fog_slope))
        fog_slope =
            (floatBitsToUint(fog_slope) & 0x80000000u) != 0u
                ? -262144.0 : 262144.0;
    return clamp((view_z + fog_end) * fog_slope, 0.0, 1.0);
}

struct CanonicalVertex {
    vec3 position;
    vec3 normal;
    vec2 uv;
    vec4 color;
};

struct TransformedVertex {
    vec3 screen;
    vec2 uv;
    float reciprocal_w;
    float fog;
    vec4 color;
};

#if defined(TNL_EXPAND_POINTS) || \
    defined(TNL_EXPAND_SPRITES) || \
    defined(TNL_EXPAND_THROUGH_SPRITES)
CanonicalVertex load_canonical(uint source_vertex) {
    uint source_word = source_vertex * 9u;
    CanonicalVertex vertex;
    vertex.position = vec3(
        uintBitsToFloat(tnl_vertices.words[source_word]),
        uintBitsToFloat(tnl_vertices.words[source_word + 1u]),
        uintBitsToFloat(tnl_vertices.words[source_word + 2u]));
    vertex.normal = vec3(
        uintBitsToFloat(tnl_vertices.words[source_word + 3u]),
        uintBitsToFloat(tnl_vertices.words[source_word + 4u]),
        uintBitsToFloat(tnl_vertices.words[source_word + 5u]));
    vertex.uv = vec2(
        uintBitsToFloat(tnl_vertices.words[source_word + 6u]),
        uintBitsToFloat(tnl_vertices.words[source_word + 7u]));
    uint packed_color = tnl_vertices.words[source_word + 8u];
    vertex.color = vec4(
        float(packed_color & 255u),
        float((packed_color >> 8u) & 255u),
        float((packed_color >> 16u) & 255u),
        float(packed_color >> 24u)) * (1.0 / 255.0);
    return vertex;
}
#endif

TransformedVertex transform_vertex(
    TnlState state, CanonicalVertex vertex) {
    precise vec3 world_position =
        transform43(state.world, vertex.position, 1.0);
    precise vec3 view_position =
        transform43(state.view, world_position, 1.0);
    precise vec4 clip_position =
        transform44(state.projection, view_position);
    precise vec3 world_normal =
        transform43(state.world, vertex.normal, 0.0);
    float normal_length = length(world_normal);
    if (normal_length > 0.0)
        world_normal /= normal_length;

    uint texture_mode = state.config.z & 3u;
    precise vec2 uv;
    if (texture_mode == 1u) {
        uint source_mode = (state.config.z >> 8u) & 3u;
        vec3 source;
        if (source_mode == 0u)
            source = vertex.position;
        else if (source_mode == 1u)
            source = vec3(vertex.uv, 0.0);
        else {
            source = vertex.normal;
            if (source_mode == 2u) {
                float source_length = length(source);
                if (source_length > 0.0)
                    source /= source_length;
            }
        }
        precise vec3 generated =
            transform43(state.texture_matrix, source, 1.0);
        float q = generated.z == 0.0 ? 1.0 : generated.z;
        uv = generated.xy / q * state.viewport_offset_size.zw;
    } else if (texture_mode == 2u) {
        vec2 environment;
        for (uint i = 0u; i < 2u; i++) {
            uint light_index =
                (state.config.w >> (i * 8u)) & 3u;
            vec3 direction =
                state.light_position[light_index].xyz;
            float direction_length = length(direction);
            if (direction_length > 0.0)
                direction /= direction_length;
            environment[i] =
                (1.0 + dot(direction, world_normal)) * 0.5;
        }
        uv = environment * state.viewport_offset_size.zw;
    } else {
        uv = (vertex.uv * state.texture_params.xy +
            state.texture_params.zw) *
            state.viewport_offset_size.zw;
    }

    precise float inverse_w = 1.0 / clip_position.w;
    precise vec3 screen =
        clip_position.xyz * inverse_w * state.viewport_scale.xyz +
        state.viewport_center.xyz;
    screen.xy -= state.viewport_offset_size.xy;
    screen.xy = floor(screen.xy * 16.0 + 0.375) * (1.0 / 16.0);
    screen.z = clamp(screen.z, 0.0, 65535.0);
    TransformedVertex output_vertex;
    output_vertex.screen = screen;
    precise vec2 uv_over_w = uv * inverse_w;
    output_vertex.uv = uv_over_w;
    output_vertex.reciprocal_w = inverse_w;
    output_vertex.fog = (state.config.x & STATE_FOG) != 0u
        ? fog_factor(state, view_position.z) : 1.0;
    output_vertex.color =
        (state.config.x & STATE_LIGHTING) != 0u
            ? light_vertex(
                state, world_position, world_normal, vertex.color)
            : vertex.color;
    return output_vertex;
}

void main() {
#ifndef TNL_EXPAND_THROUGH_SPRITES
    TnlState state = tnl_states.values[pc.tnl.x];
#endif
    TransformedVertex output_vertex;
#if defined(TNL_EXPAND_POINTS) || \
    defined(TNL_EXPAND_SPRITES) || \
    defined(TNL_EXPAND_THROUGH_SPRITES)
    uint expansion_base = pc.tnl.w & 0x7fffffffu;
    uint expanded_vertex =
        uint(gl_VertexIndex) - expansion_base;
    const vec2 corners[6] = vec2[](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
        vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
#endif
#ifdef TNL_EXPAND_POINTS
    uint source_vertex =
        tnl_indices.values[pc.tnl.z + expanded_vertex / 6u];
    output_vertex =
        transform_vertex(state, load_canonical(source_vertex));
    output_vertex.screen.xy =
        floor(output_vertex.screen.xy) +
        corners[expanded_vertex % 6u];
#elif defined(TNL_EXPAND_SPRITES)
    uint control_base =
        pc.tnl.z + (expanded_vertex / 6u) * 2u;
    TransformedVertex first = transform_vertex(
        state, load_canonical(tnl_indices.values[control_base]));
    TransformedVertex second = transform_vertex(
        state, load_canonical(tnl_indices.values[control_base + 1u]));
    float xa = floor(min(first.screen.x, second.screen.x));
    float xb = floor(max(first.screen.x, second.screen.x));
    float ya = floor(min(first.screen.y, second.screen.y));
    float yb = floor(max(first.screen.y, second.screen.y));
    vec2 corner = corners[expanded_vertex % 6u];
    output_vertex = second;
    output_vertex.screen.x = corner.x != 0.0 ? xb : xa;
    output_vertex.screen.y = corner.y != 0.0 ? yb : ya;
    output_vertex.reciprocal_w = 1.0;
    if (xb > xa && yb > ya) {
        vec2 first_uv = first.uv / first.reciprocal_w;
        vec2 second_uv = second.uv / second.reciprocal_w;
        vec2 texel_step =
            (second_uv - first_uv) / vec2(xb - xa, yb - ya);
        bool linear_filter = (pc.tnl.w & 0x80000000u) != 0u;
        vec2 low_uv = linear_filter
            ? first_uv : first_uv - 0.5 * texel_step;
        output_vertex.uv = mix(
            low_uv, linear_filter ? second_uv
                : second_uv - 0.5 * texel_step,
            corner);
    } else {
        output_vertex.screen.xy = vec2(xa, ya);
        output_vertex.uv = second.uv / second.reciprocal_w;
    }
#elif defined(TNL_EXPAND_THROUGH_SPRITES)
    uint control_base = (expanded_vertex / 6u) * 2u;
    CanonicalVertex first = load_canonical(control_base);
    CanonicalVertex second = load_canonical(control_base + 1u);
    float xa = floor(min(first.position.x, second.position.x));
    float xb = floor(max(first.position.x, second.position.x));
    float ya = floor(min(first.position.y, second.position.y));
    float yb = floor(max(first.position.y, second.position.y));
    vec2 corner = corners[expanded_vertex % 6u];
    output_vertex.screen = vec3(
        corner.x != 0.0 ? xb : xa,
        corner.y != 0.0 ? yb : ya,
        second.position.z);
    output_vertex.reciprocal_w = 1.0;
    output_vertex.fog = 1.0;
    output_vertex.color = second.color;
    if (xb > xa && yb > ya) {
        vec2 texel_step =
            (second.uv - first.uv) / vec2(xb - xa, yb - ya);
        bool linear_filter = (pc.tnl.w & 0x80000000u) != 0u;
        vec2 low_uv = linear_filter
            ? first.uv : first.uv - 0.5 * texel_step;
        output_vertex.uv = mix(
            low_uv, linear_filter ? second.uv
                : second.uv - 0.5 * texel_step,
            corner);
    } else {
        output_vertex.screen.xy = vec2(xa, ya);
        output_vertex.uv = second.uv;
    }
#else
    CanonicalVertex vertex;
    vertex.position = in_position;
    vertex.normal = in_normal;
    vertex.uv = in_uv;
    vertex.color = vec4(in_color) * (1.0 / 255.0);
    output_vertex = transform_vertex(state, vertex);
#endif
    gl_Position = vec4(
        output_vertex.screen.x * (1.0 / 256.0) - 1.0,
        output_vertex.screen.y * (1.0 / 136.0) - 1.0,
        output_vertex.screen.z * (1.0 / 65535.0),
        1.0);
    v_uv = output_vertex.uv;
    v_rw = output_vertex.reciprocal_w;
    v_fog = output_vertex.fog;
    v_color = output_vertex.color;
#ifdef TNL_VALIDATE
    if (pc.tnl.y != 0u) {
        TnlValidationVertex validation;
        validation.screen_reciprocal_w =
            vec4(
                output_vertex.screen,
                output_vertex.reciprocal_w);
        validation.uv_fog =
            vec4(output_vertex.uv, output_vertex.fog, 0.0);
        validation.color = output_vertex.color;
        tnl_validation.values[uint(gl_VertexIndex)] = validation;
    }
#endif
}
