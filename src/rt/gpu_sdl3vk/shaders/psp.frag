#version 450

layout(location = 0) noperspective in vec2 v_uv;
layout(location = 1) noperspective in float v_rw;
layout(location = 2) noperspective in float v_fog;
layout(location = 3) noperspective in vec4 v_color;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D u_texture;
layout(set = 1, binding = 0) uniform sampler2D u_destination_color;
layout(set = 1, binding = 1) uniform sampler2D u_destination_depth;

layout(push_constant) uniform PC {
    ivec4 config;
    uvec4 texture_state;
    ivec4 color_test_state;
    ivec4 stencil_state;
    ivec4 dither_rows;
    ivec4 blend_state;
    ivec4 fragment_state;
} pc;

const int FLAG_TEXTURE = 1;
const int FLAG_TEXTURE_RGBA = 2;
const int FLAG_COLOR_DOUBLE = 4;
const int FLAG_FOG = 8;
const int FLAG_DEPTH_RANGE = 16;
const int FLAG_CLEAR = 32;
const int FLAG_NEAREST = 64;
const int FLAG_COLOR_TEST = 512;
const int FLAG_DITHER = 1024;
const int FLAG_STENCIL = 2048;
const int FLAG_DIRECT = 8192;
const int FLAG_PSP_SAMPLER = 32768;
const int FLAG_FRAMEBUFFER = 65536;
const int FLAG_CLAMP_U = 131072;
const int FLAG_CLAMP_V = 262144;

bool compare_value(int function, int value, int reference) {
    if (function == 0) return false;
    if (function == 1) return true;
    if (function == 2) return value == reference;
    if (function == 3) return value != reference;
    if (function == 4) return value < reference;
    if (function == 5) return value <= reference;
    if (function == 6) return value > reference;
    return value >= reference;
}

int stencil_operation(
    int operation, int format, int old_value, int reference) {
    if (operation == 1) return 0;
    if (operation == 2) return reference;
    if (operation == 3) return old_value ^ 255;
    if (operation == 4) {
        if (format == 0) return 0;
        if (format == 1) return 255;
        if (format == 2)
            return old_value < 240 ? old_value + 16 : old_value;
        return min(old_value + 1, 255);
    }
    if (operation == 5) {
        if (format == 0 || format == 1) return 0;
        if (format == 2)
            return old_value >= 16 ? old_value - 16 : old_value;
        return max(old_value - 1, 0);
    }
    return old_value;
}

uint pack_native(ivec4 color, int format) {
    color = clamp(color, ivec4(0), ivec4(255));
    if (format == 0) {
        return uint(color.r >> 3) |
               (uint(color.g >> 2) << 5) |
               (uint(color.b >> 3) << 11);
    }
    if (format == 1) {
        return uint(color.r >> 3) |
               (uint(color.g >> 3) << 5) |
               (uint(color.b >> 3) << 10) |
               (uint(color.a >> 7) << 15);
    }
    if (format == 2) {
        return uint(color.r >> 4) |
               (uint(color.g >> 4) << 4) |
               (uint(color.b >> 4) << 8) |
               (uint(color.a >> 4) << 12);
    }
    return uint(color.r) |
           (uint(color.g) << 8) |
           (uint(color.b) << 16) |
           (uint(color.a) << 24);
}

ivec4 unpack_native(uint pixel, int format) {
    if (format == 0) {
        int red = int(pixel & 31u);
        int green = int((pixel >> 5) & 63u);
        int blue = int((pixel >> 11) & 31u);
        return ivec4(
            (red << 3) | (red >> 2),
            (green << 2) | (green >> 4),
            (blue << 3) | (blue >> 2), 255);
    }
    if (format == 1) {
        int red = int(pixel & 31u);
        int green = int((pixel >> 5) & 31u);
        int blue = int((pixel >> 10) & 31u);
        return ivec4(
            (red << 3) | (red >> 2),
            (green << 3) | (green >> 2),
            (blue << 3) | (blue >> 2),
            (pixel & 32768u) != 0u ? 255 : 0);
    }
    if (format == 2) {
        ivec4 value = ivec4(
            int(pixel & 15u), int((pixel >> 4) & 15u),
            int((pixel >> 8) & 15u), int((pixel >> 12) & 15u));
        return (value << 4) | value;
    }
    return ivec4(
        int(pixel & 255u), int((pixel >> 8) & 255u),
        int((pixel >> 16) & 255u), int((pixel >> 24) & 255u));
}

ivec2 texture_dimensions_psp() {
    return max(
        ivec2(
            int(pc.texture_state.z & 65535u),
            int(pc.texture_state.z >> 16)),
        ivec2(1));
}

ivec2 texture_offset_psp() {
    return ivec2(
        int(pc.texture_state.w & 65535u),
        int(pc.texture_state.w >> 16));
}

int wrap_coordinate(int value, int size, bool clamp_value) {
    if (clamp_value) return clamp(value, 0, size - 1);
    int wrapped = value % size;
    return wrapped < 0 ? wrapped + size : wrapped;
}

ivec4 texture_texel(ivec2 coordinate, int flags) {
    ivec2 dimensions = texture_dimensions_psp();
    coordinate.x = wrap_coordinate(
        coordinate.x, dimensions.x, (flags & FLAG_CLAMP_U) != 0);
    coordinate.y = wrap_coordinate(
        coordinate.y, dimensions.y, (flags & FLAG_CLAMP_V) != 0);
    coordinate += texture_offset_psp();
    return ivec4(
        texelFetch(u_texture, coordinate, 0) * 255.0 + 0.5);
}

ivec4 psp_lerp(ivec4 first, ivec4 second, int weight) {
    return first + (((second - first) * weight) >> 8);
}

vec4 sample_texture_psp(vec2 uv, bool nearest, int flags) {
    if (nearest)
        return vec4(texture_texel(ivec2(floor(uv)), flags));
    vec2 position = uv - 0.5;
    ivec2 base = ivec2(floor(position));
    ivec2 weight = clamp(
        ivec2((position - vec2(base)) * 256.0),
        ivec2(0), ivec2(256));
    ivec4 upper = psp_lerp(
        texture_texel(base, flags),
        texture_texel(base + ivec2(1, 0), flags),
        weight.x);
    ivec4 lower = psp_lerp(
        texture_texel(base + ivec2(0, 1), flags),
        texture_texel(base + ivec2(1, 1), flags),
        weight.x);
    return vec4(psp_lerp(upper, lower, weight.y));
}

ivec3 unpack_rgb(uint packed) {
    return ivec3(
        int(packed & 255u),
        int((packed >> 8) & 255u),
        int((packed >> 16) & 255u));
}

ivec4 combine_texture_psp(
    ivec4 vertex, ivec4 texture_color,
    int texture_function, bool texture_rgba, bool color_double) {
    ivec4 result = vertex;
    int divisor = color_double ? 128 : 256;
    if (texture_function == 1) {
        if (texture_rgba) {
            int inverse_alpha = 255 - texture_color.a;
            result.rgb =
                ((vertex.rgb + 1) * inverse_alpha +
                 (texture_color.rgb + 1) * texture_color.a) /
                divisor;
        } else {
            result.rgb = color_double
                ? texture_color.rgb * 2 : texture_color.rgb;
        }
    } else if (texture_function == 2) {
        ivec3 environment = unpack_rgb(pc.texture_state.x);
        result.rgb =
            ((255 - texture_color.rgb) * vertex.rgb +
             texture_color.rgb * environment + 255) /
            divisor;
        result.a = texture_rgba
            ? (vertex.a + 1) * texture_color.a / 256
            : vertex.a;
    } else if (texture_function == 3) {
        result.rgb = color_double
            ? texture_color.rgb * 2 : texture_color.rgb;
        result.a = texture_rgba ? texture_color.a : vertex.a;
    } else if (texture_function >= 4) {
        result.rgb = vertex.rgb + texture_color.rgb;
        if (color_double) result.rgb *= 2;
        result.a = texture_rgba
            ? (vertex.a + 1) * texture_color.a / 256
            : vertex.a;
    } else {
        result.rgb =
            (vertex.rgb + 1) *
            (color_double ? texture_color.rgb * 2 : texture_color.rgb) /
            256;
        result.a = texture_rgba
            ? (vertex.a + 1) * texture_color.a / 256
            : vertex.a;
    }
    return clamp(result, ivec4(0), ivec4(255));
}

int factor_value(
    int factor, int channel, ivec4 source, ivec4 destination,
    int fixed_color, bool source_side) {
    int other = source_side ? destination[channel] : source[channel];
    if (factor == 0) return other;
    if (factor == 1) return 255 - other;
    if (factor == 2) return source.a;
    if (factor == 3) return 255 - source.a;
    if (factor == 4) return destination.a;
    if (factor == 5) return 255 - destination.a;
    if (factor == 6) return source.a * 2;
    if (factor == 7) return 255 - source.a * 2;
    if (factor == 8) return destination.a * 2;
    if (factor == 9) return 255 - destination.a * 2;
    return (fixed_color >> (channel * 8)) & 255;
}

int blend_term(int color, int factor) {
    int magnitude =
        ((color * 2 + 1) * (abs(factor) * 2 + 1)) >> 10;
    return factor < 0 ? -magnitude : magnitude;
}

ivec3 blend_rgb(ivec4 source, ivec4 destination) {
    int mode = pc.blend_state.y;
    int equation = (mode >> 8) & 7;
    if (equation == 3) return min(source.rgb, destination.rgb);
    if (equation == 4) return max(source.rgb, destination.rgb);
    if (equation == 5) return abs(source.rgb - destination.rgb);
    if (equation > 2) return source.rgb;

    ivec3 result;
    for (int channel = 0; channel < 3; channel++) {
        int source_factor = factor_value(
            mode & 15, channel, source, destination,
            pc.blend_state.z, true);
        int destination_factor = factor_value(
            (mode >> 4) & 15, channel, source, destination,
            pc.blend_state.w, false);
        int source_term = blend_term(source[channel], source_factor);
        int destination_term =
            blend_term(destination[channel], destination_factor);
        result[channel] =
            equation == 1 ? source_term - destination_term :
            equation == 2 ? destination_term - source_term :
            source_term + destination_term;
    }
    return result;
}

uint logic_rgb(int operation, uint source, uint destination) {
    source &= 0x00ffffffu;
    destination &= 0x00ffffffu;
    if (operation == 0) return 0u;
    if (operation == 1) return source & destination;
    if (operation == 2) return source & ~destination;
    if (operation == 3) return source;
    if (operation == 4) return ~source & destination;
    if (operation == 5) return destination;
    if (operation == 6) return source ^ destination;
    if (operation == 7) return source | destination;
    if (operation == 8) return ~(source | destination);
    if (operation == 9) return ~(source ^ destination);
    if (operation == 10) return ~destination;
    if (operation == 11) return source | ~destination;
    if (operation == 12) return ~source;
    if (operation == 13) return ~source | destination;
    if (operation == 14) return ~(source & destination);
    return 0x00ffffffu;
}

int dither_value(ivec2 pixel) {
    int row = pc.dither_rows[pixel.y & 3];
    int value = (row >> ((pixel.x & 3) * 4)) & 15;
    return value >= 8 ? value - 16 : value;
}

ivec4 apply_write_mask(
    ivec4 color, ivec4 old_color, uint write_mask, int format) {
    uint native_mask = pack_native(
        ivec4(
            int(write_mask & 255u),
            int((write_mask >> 8) & 255u),
            int((write_mask >> 16) & 255u),
            int((write_mask >> 24) & 255u)),
        format);
    uint old_native = pack_native(old_color, format);
    uint new_native = pack_native(color, format);
    return unpack_native(
        (new_native & ~native_mask) | (old_native & native_mask),
        format);
}

ivec4 shade_source(out int source_depth, out int flags) {
    flags = pc.config.x >> 8;
    source_depth = int(gl_FragCoord.z * 65535.0 + 0.5);
    if ((flags & FLAG_DEPTH_RANGE) != 0 &&
        (source_depth < pc.config.z ||
         source_depth > pc.config.w)) {
        discard;
    }

    vec4 vertex = v_color * 255.0;
    vec4 shaded = vertex;
    if ((flags & FLAG_CLEAR) == 0) {
        if ((flags & FLAG_TEXTURE) != 0) {
            float rw = max(abs(v_rw), 1e-20);
            vec2 uv = v_uv / rw;
            if ((flags & FLAG_NEAREST) != 0)
                uv += vec2(0.5);
            vec2 shifted_uv = uv + vec2(texture_offset_psp());
            bool nearest = (flags & FLAG_NEAREST) != 0;
            bool quantized = (flags & FLAG_PSP_SAMPLER) != 0;
            vec4 texture_color = quantized
                ? sample_texture_psp(uv, nearest, flags)
                : texture(
                      u_texture,
                      shifted_uv / vec2(texture_dimensions_psp())) *
                  255.0;
            int texture_function = pc.config.x & 7;
            bool texture_rgba =
                (flags & FLAG_TEXTURE_RGBA) != 0;
            float color_scale =
                (flags & FLAG_COLOR_DOUBLE) != 0 ? 2.0 : 1.0;
            if (quantized) {
                shaded = vec4(combine_texture_psp(
                    ivec4(vertex), ivec4(texture_color),
                    texture_function, texture_rgba,
                    (flags & FLAG_COLOR_DOUBLE) != 0));
            } else {
                vec3 rgb;
                float alpha;
                if (texture_function == 1) {
                    rgb = texture_rgba
                        ? mix(
                              vertex.rgb, texture_color.rgb,
                              texture_color.a * (1.0 / 255.0)) *
                          color_scale
                        : texture_color.rgb * color_scale;
                    alpha = vertex.a;
                } else if (texture_function == 2) {
                    rgb = mix(
                              vertex.rgb,
                              vec3(unpack_rgb(pc.texture_state.x)),
                              texture_color.rgb * (1.0 / 255.0)) *
                          color_scale;
                    alpha = texture_rgba
                        ? vertex.a * texture_color.a *
                          (1.0 / 255.0)
                        : vertex.a;
                } else if (texture_function == 3) {
                    rgb = texture_color.rgb * color_scale;
                    alpha = texture_rgba
                        ? texture_color.a : vertex.a;
                } else if (texture_function >= 4) {
                    rgb =
                        (vertex.rgb + texture_color.rgb) *
                        color_scale;
                    alpha = texture_rgba
                        ? vertex.a * texture_color.a *
                          (1.0 / 255.0)
                        : vertex.a;
                } else {
                    rgb = vertex.rgb * texture_color.rgb *
                          (1.0 / 255.0) * color_scale;
                    alpha = texture_rgba
                        ? vertex.a * texture_color.a *
                          (1.0 / 255.0)
                        : vertex.a;
                }
                shaded = vec4(
                    min(rgb, vec3(255.0)), min(alpha, 255.0));
            }
        }

        int alpha_test = pc.config.y;
        int alpha_mask = (alpha_test >> 16) & 255;
        int alpha_reference =
            ((alpha_test >> 8) & 255) & alpha_mask;
        int alpha_value = int(shaded.a + 0.5) & alpha_mask;
        if (!compare_value(
                alpha_test & 7, alpha_value, alpha_reference)) {
            discard;
        }

        if ((flags & FLAG_FOG) != 0) {
            if ((flags & FLAG_PSP_SAMPLER) != 0) {
                int fog = int(clamp(v_fog, 0.0, 1.0) * 256.0);
                ivec3 fog_color =
                    unpack_rgb(pc.texture_state.y);
                ivec3 source = ivec3(shaded.rgb);
                shaded.rgb = vec3(
                    fog_color +
                    (((source - fog_color) * fog) >> 8));
            } else {
                shaded.rgb = mix(
                    vec3(unpack_rgb(pc.texture_state.y)),
                    shaded.rgb, clamp(v_fog, 0.0, 1.0));
            }
        }

        if ((flags & FLAG_COLOR_TEST) != 0) {
            ivec3 rgb = ivec3(shaded.rgb + 0.5);
            int packed = rgb.r | (rgb.g << 8) | (rgb.b << 16);
            int masked = packed & pc.color_test_state.z;
            int reference =
                pc.color_test_state.y & pc.color_test_state.z;
            if (!compare_value(
                    pc.color_test_state.x, masked, reference)) {
                discard;
            }
        }
    }

    return clamp(
        ivec4(shaded + 0.5), ivec4(0), ivec4(255));
}

void main() {
    int source_depth;
    int flags;
    ivec4 source = shade_source(source_depth, flags);
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int format = pc.stencil_state.w & 3;

    if ((flags & FLAG_DIRECT) != 0) {
        ivec4 result = source;
        if ((flags & FLAG_DITHER) != 0)
            result.rgb += dither_value(pixel);
        if ((flags & FLAG_STENCIL) != 0) {
            int stencil_reference =
                (pc.stencil_state.x >> 8) & 255;
            result.a = stencil_operation(
                (pc.stencil_state.y >> 16) & 7,
                format, 0, stencil_reference);
        }
        result = clamp(result, ivec4(0), ivec4(255));
        result = unpack_native(pack_native(result, format), format);
        o_color = vec4(result) * (1.0 / 255.0);
        gl_FragDepth = gl_FragCoord.z;
        return;
    }

    ivec4 old_storage = ivec4(
        texelFetch(u_destination_color, pixel, 0) * 255.0 + 0.5);
    ivec4 destination = old_storage;
    if (format == 0) destination.a = 0;
    int old_depth = int(
        texelFetch(u_destination_depth, pixel, 0).r *
        65535.0 + 0.5);

    ivec4 result = source;
    int output_depth = old_depth;
    uint write_mask = uint(pc.fragment_state.x);

    if ((flags & FLAG_CLEAR) != 0) {
        if ((flags & FLAG_DITHER) != 0)
            result.rgb += dither_value(pixel);
        result = clamp(result, ivec4(0), ivec4(255));
        if ((pc.fragment_state.z & 0x100) == 0)
            result.rgb = old_storage.rgb;
        if ((pc.fragment_state.z & 0x200) == 0)
            result.a = old_storage.a;
        if ((pc.fragment_state.z & 0x400) != 0)
            output_depth = source_depth;
    } else {
        int stencil = format == 0 ? 0 : destination.a;
        int stencil_function = pc.stencil_state.x & 7;
        int stencil_reference =
            (pc.stencil_state.x >> 8) & 255;
        int stencil_mask =
            (pc.stencil_state.x >> 16) & 255;
        bool stencil_pass = (flags & FLAG_STENCIL) == 0 ||
            compare_value(
                stencil_function,
                stencil_reference & stencil_mask,
                stencil & stencil_mask);
        bool color_pass = stencil_pass;
        if (!stencil_pass) {
            stencil = stencil_operation(
                pc.stencil_state.y & 7, format,
                stencil, stencil_reference);
        } else {
            int depth_state = pc.fragment_state.w;
            bool depth_enabled = (depth_state & 1) != 0;
            bool depth_pass = !depth_enabled ||
                compare_value(
                    (depth_state >> 8) & 7,
                    source_depth, old_depth);
            if (!depth_pass) {
                stencil = stencil_operation(
                    (pc.stencil_state.y >> 8) & 7,
                    format, stencil, stencil_reference);
                color_pass = false;
            } else {
                stencil = stencil_operation(
                    (pc.stencil_state.y >> 16) & 7,
                    format, stencil, stencil_reference);
                if (depth_enabled &&
                    (depth_state & 0x10000) != 0) {
                    output_depth = source_depth;
                }
            }
        }

        if (color_pass) {
            result.rgb = pc.blend_state.x != 0
                ? blend_rgb(source, destination)
                : source.rgb;
            result.a = stencil;
            if ((flags & FLAG_DITHER) != 0)
                result.rgb += dither_value(pixel);
            result = clamp(result, ivec4(0), ivec4(255));
            if ((pc.fragment_state.y & 1) != 0) {
                uint logical = logic_rgb(
                    (pc.fragment_state.y >> 8) & 15,
                    uint(result.r) |
                        (uint(result.g) << 8) |
                        (uint(result.b) << 16),
                    uint(destination.r) |
                        (uint(destination.g) << 8) |
                        (uint(destination.b) << 16));
                result.rgb = ivec3(
                    int(logical & 255u),
                    int((logical >> 8) & 255u),
                    int((logical >> 16) & 255u));
            }
        } else {
            result = old_storage;
            result.a = stencil;
        }

        if ((flags & FLAG_STENCIL) == 0)
            write_mask |= 0xff000000u;
    }

    result = apply_write_mask(
        result, old_storage, write_mask, format);
    o_color = vec4(result) * (1.0 / 255.0);
    gl_FragDepth = float(output_depth) * (1.0 / 65535.0);
}
