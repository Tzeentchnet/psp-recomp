/* PSP GE vertex stage. Screen-space CPU output and canonical model-space GPU
 * T&L share one raw vertex root SRV. */

ByteAddressBuffer ge_vertices : register(t0, space1);

struct TnlState {
    float4 world[3];
    float4 view[3];
    float4 projection[4];
    float4 texture_matrix[3];
    float4 texture_params;
    float4 viewport_scale;
    float4 viewport_center;
    float4 viewport_offset_size;
    float4 fog_material;
    uint4 config;
    uint4 depth_material;
    uint4 material_colors;
    uint4 material_config;
    float4 light_position[4];
    float4 light_direction[4];
    float4 light_attenuation[4];
    uint4 light_colors[4];
};

StructuredBuffer<TnlState> ge_tnl_states : register(t1, space1);

#ifdef TNL_VALIDATE
struct TnlValidationVertex {
    float4 screen_reciprocal_w;
    float4 uv_fog;
    float4 color;
};

RWStructuredBuffer<TnlValidationVertex> ge_tnl_validation
    : register(u0, space1);
#endif

cbuffer PSPConstants : register(b0) {
    uint4 ignored_pixel_state[10];
    uint4 tnl_state;
};

struct VSOutput {
    float4 position              : SV_Position;
    noperspective float2 uv      : TEXCOORD0;
    noperspective float  rw      : TEXCOORD1;
    noperspective float  fog     : TEXCOORD2;
    noperspective float4 color   : COLOR0;
};

static const uint STATE_LIGHTING = 2u;
static const uint STATE_FOG = 16u;
static const uint STATE_MATERIAL_ALPHA_SET = 1024u;

float3 transform43(
    float4 row0, float4 row1, float4 row2,
    float3 value, float w) {
    float4 input_value = float4(value, w);
    return float3(
        dot(row0, input_value),
        dot(row1, input_value),
        dot(row2, input_value));
}

float4 transform44(
    float4 row0, float4 row1, float4 row2, float4 row3,
    float3 value) {
    float4 input_value = float4(value, 1.0);
    return float4(
        dot(row0, input_value),
        dot(row1, input_value),
        dot(row2, input_value),
        dot(row3, input_value));
}

float3 unpack_rgb(uint color) {
    return float3(
        color & 255u,
        (color >> 8u) & 255u,
        (color >> 16u) & 255u) * (1.0 / 255.0);
}

float4 light_vertex(
    TnlState state, float3 world_position, float3 world_normal,
    float4 vertex_color) {
    uint material_update = state.material_config.x;
    float3 ambient_material = (material_update & 1u) != 0u
        ? vertex_color.rgb : unpack_rgb(state.depth_material.z);
    float3 diffuse_material = (material_update & 2u) != 0u
        ? vertex_color.rgb : unpack_rgb(state.material_colors.y);
    float3 specular_material = (material_update & 4u) != 0u
        ? vertex_color.rgb : unpack_rgb(state.material_colors.z);
    float3 result =
        unpack_rgb(state.material_colors.x) +
        unpack_rgb(state.material_colors.w) * ambient_material;

    [unroll]
    for (uint i = 0u; i < 4u; i++) {
        if ((state.material_config.z & (1u << i)) == 0u)
            continue;
        uint light_type = state.light_colors[i].w;
        uint computation = light_type & 3u;
        uint type = (light_type >> 8u) & 3u;
        float3 light_vector = state.light_position[i].xyz;
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
            float3 half_vector =
                float3(light_vector.xy, light_vector.z + 1.0);
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
        (uint)floor(vertex_color.a * 255.0 + 0.5);
    uint material_alpha = (material_update & 1u) != 0u
        ? vertex_alpha
        : ((state.config.x & STATE_MATERIAL_ALPHA_SET) != 0u
            ? state.depth_material.w : 255u);
    uint alpha =
        (material_alpha * state.material_config.y + 127u) / 255u;
    float3 rounded =
        floor(clamp(
            result, float3(0.0, 0.0, 0.0),
            float3(1.0, 1.0, 1.0)) * 255.0 + 0.5) *
        (1.0 / 255.0);
    return float4(
        rounded, float(min(alpha, 255u)) * (1.0 / 255.0));
}

float fog_factor(TnlState state, float view_z) {
    float fog_end = state.fog_material.x;
    float fog_slope = state.fog_material.y;
    if (isinf(fog_end) || isnan(fog_end)) {
        bool negative = (asuint(fog_end) & 0x80000000u) != 0u;
        if ((asuint(fog_slope) & 0x80000000u) != 0u)
            negative = !negative;
        if (fog_slope == 0.0)
            negative = true;
        return negative ? 0.0 : 1.0;
    }
    if (isinf(fog_slope) || isnan(fog_slope))
        fog_slope = (asuint(fog_slope) & 0x80000000u) != 0u
            ? -262144.0 : 262144.0;
    return clamp((view_z + fog_end) * fog_slope, 0.0, 1.0);
}

VSOutput screen_vertex(uint vertex_id) {
    uint byte_offset = vertex_id << 5;
    uint4 position_bits = ge_vertices.Load4(byte_offset);
    uint4 attributes = ge_vertices.Load4(byte_offset + 16);
    float4 input_position = asfloat(position_bits);
    float2 input_uv = asfloat(attributes.xy);
    float input_fog = asfloat(attributes.z);
    uint packed_color = attributes.w;
    float4 input_color = float4(
        packed_color & 255u,
        (packed_color >> 8u) & 255u,
        (packed_color >> 16u) & 255u,
        packed_color >> 24u) * (1.0 / 255.0);

    VSOutput output;
    output.position = float4(
        input_position.x * (1.0 / 256.0) - 1.0,
        1.0 - input_position.y * (1.0 / 136.0),
        input_position.z * (1.0 / 65535.0),
        1.0);
    output.uv = input_uv;
    output.rw = input_position.w;
    output.fog = input_fog;
    output.color = input_color;
    return output;
}

VSOutput transformed_vertex(uint vertex_id) {
    uint byte_offset = vertex_id * 36u;
    float3 input_position =
        asfloat(ge_vertices.Load3(byte_offset));
    float3 input_normal =
        asfloat(ge_vertices.Load3(byte_offset + 12u));
    float2 input_uv =
        asfloat(ge_vertices.Load2(byte_offset + 24u));
    uint packed_color = ge_vertices.Load(byte_offset + 32u);
    float4 input_color = float4(
        packed_color & 255u,
        (packed_color >> 8u) & 255u,
        (packed_color >> 16u) & 255u,
        packed_color >> 24u) * (1.0 / 255.0);

    TnlState state = ge_tnl_states[tnl_state.x];
    float3 world_position = transform43(
        state.world[0], state.world[1], state.world[2],
        input_position, 1.0);
    float3 view_position = transform43(
        state.view[0], state.view[1], state.view[2],
        world_position, 1.0);
    float4 clip_position = transform44(
        state.projection[0], state.projection[1],
        state.projection[2], state.projection[3],
        view_position);
    float3 world_normal = transform43(
        state.world[0], state.world[1], state.world[2],
        input_normal, 0.0);
    float normal_length = length(world_normal);
    if (normal_length > 0.0)
        world_normal /= normal_length;

    uint texture_mode = state.config.z & 3u;
    float2 uv;
    if (texture_mode == 1u) {
        uint source_mode = (state.config.z >> 8u) & 3u;
        float3 source;
        if (source_mode == 0u)
            source = input_position;
        else if (source_mode == 1u)
            source = float3(input_uv, 0.0);
        else {
            source = input_normal;
            if (source_mode == 2u) {
                float source_length = length(source);
                if (source_length > 0.0)
                    source /= source_length;
            }
        }
        float3 generated = transform43(
            state.texture_matrix[0], state.texture_matrix[1],
            state.texture_matrix[2], source, 1.0);
        float q = generated.z == 0.0 ? 1.0 : generated.z;
        uv = generated.xy / q * state.viewport_offset_size.zw;
    } else if (texture_mode == 2u) {
        float2 environment;
        [unroll]
        for (uint i = 0u; i < 2u; i++) {
            uint light_index =
                (state.config.w >> (i * 8u)) & 3u;
            float3 direction =
                state.light_position[light_index].xyz;
            float direction_length = length(direction);
            if (direction_length > 0.0)
                direction /= direction_length;
            environment[i] =
                (1.0 + dot(direction, world_normal)) * 0.5;
        }
        uv = environment * state.viewport_offset_size.zw;
    } else {
        uv = (input_uv * state.texture_params.xy +
            state.texture_params.zw) *
            state.viewport_offset_size.zw;
    }

    float inverse_w = 1.0 / clip_position.w;
    float3 screen =
        clip_position.xyz * inverse_w * state.viewport_scale.xyz +
        state.viewport_center.xyz;
    screen.xy -= state.viewport_offset_size.xy;
    screen.xy =
        floor(screen.xy * 16.0 + 0.375) * (1.0 / 16.0);
    screen.z = clamp(screen.z, 0.0, 65535.0);

    VSOutput output;
    output.position = float4(
        screen.x * (1.0 / 256.0) - 1.0,
        1.0 - screen.y * (1.0 / 136.0),
        screen.z * (1.0 / 65535.0),
        1.0);
    float2 output_uv = uv * inverse_w;
    float output_fog = (state.config.x & STATE_FOG) != 0u
        ? fog_factor(state, view_position.z) : 1.0;
    float4 output_color = (state.config.x & STATE_LIGHTING) != 0u
        ? light_vertex(
            state, world_position, world_normal, input_color)
        : input_color;
    output.uv = output_uv;
    output.rw = inverse_w;
    output.fog = output_fog;
    output.color = output_color;
#ifdef TNL_VALIDATE
    if (tnl_state.z != 0u) {
        TnlValidationVertex validation;
        validation.screen_reciprocal_w =
            float4(screen, inverse_w);
        validation.uv_fog =
            float4(output_uv, output_fog, 0.0);
        validation.color = output_color;
        ge_tnl_validation[tnl_state.w + vertex_id] = validation;
    }
#endif
    return output;
}

VSOutput main(uint vertex_id : SV_VertexID) {
    if (tnl_state.y != 0u) {
        return transformed_vertex(vertex_id);
    }
    return screen_vertex(vertex_id);
}
