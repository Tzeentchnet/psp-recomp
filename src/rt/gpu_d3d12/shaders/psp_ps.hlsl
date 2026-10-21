/* PSP GE destination-aware pixel shader. Fragment tests, depth/stencil, blending,
 * dithering, logic operations, and write masks follow the software GE ordering. */

Texture2D<float4> psp_texture : register(t0);
Buffer<uint> psp_clut : register(t1);
Texture2D<float4> destination_color : register(t2);
Texture2D<float> destination_depth : register(t3);
SamplerState psp_sampler : register(s0);

cbuffer PSPConstants : register(b0) {
    int4 cfg;       /* x=texfunc|flags<<8, y=alpha test, z=minz, w=maxz */
    float4 texenv;  /* rgb=GE_TEXENVCOLOR / 255, w=internal resolution scale */
    float4 fogcol;  /* rgb=GE_FOGCOLOR / 255 */
    float4 texsize; /* xy=dimensions, zw=render-target sub-rect texel offset */
    int4 color_test_state; /* x=func, y=reference RGB, z=mask RGB */
    int4 stencil_state;    /* x=test, y=ops, z=write mask, w=fb format */
    int4 dither_rows;      /* four packed signed-nibble rows */
    int4 blend_state;      /* x=enabled, y=mode, z=FIXA, w=FIXB */
    int4 fragment_state;   /* x=write mask, y=logic, z=clear, w=depth */
    int4 scissor_state;    /* applied as dynamic raster state by the CPU */
};

struct PSInput {
    float4 position              : SV_Position;
    noperspective float2 uv      : TEXCOORD0;
    noperspective float  rw      : TEXCOORD1;
    noperspective float  fog     : TEXCOORD2;
    noperspective float4 color   : COLOR0;
};

struct PSOutput {
    float4 color : SV_Target0;
    float depth : SV_Depth;
};

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

int stencil_operation(int operation, int format, int old_value, int reference) {
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

uint pack_native(int4 color, int format) {
    color = clamp(color, int4(0, 0, 0, 0), int4(255, 255, 255, 255));
    if (format == 0) {
        return (uint)(color.r >> 3) |
               ((uint)(color.g >> 2) << 5) |
               ((uint)(color.b >> 3) << 11);
    }
    if (format == 1) {
        return (uint)(color.r >> 3) |
               ((uint)(color.g >> 3) << 5) |
               ((uint)(color.b >> 3) << 10) |
               ((uint)(color.a >> 7) << 15);
    }
    if (format == 2) {
        return (uint)(color.r >> 4) |
               ((uint)(color.g >> 4) << 4) |
               ((uint)(color.b >> 4) << 8) |
               ((uint)(color.a >> 4) << 12);
    }
    return (uint)color.r | ((uint)color.g << 8) |
           ((uint)color.b << 16) | ((uint)color.a << 24);
}

int4 unpack_native(uint pixel, int format) {
    if (format == 0) {
        int red = (int)(pixel & 31);
        int green = (int)((pixel >> 5) & 63);
        int blue = (int)((pixel >> 11) & 31);
        return int4(
            (red << 3) | (red >> 2),
            (green << 2) | (green >> 4),
            (blue << 3) | (blue >> 2), 255);
    }
    if (format == 1) {
        int red = (int)(pixel & 31);
        int green = (int)((pixel >> 5) & 31);
        int blue = (int)((pixel >> 10) & 31);
        return int4(
            (red << 3) | (red >> 2),
            (green << 3) | (green >> 2),
            (blue << 3) | (blue >> 2),
            (pixel & 32768) != 0 ? 255 : 0);
    }
    if (format == 2) {
        int4 value = int4(
            (int)(pixel & 15), (int)((pixel >> 4) & 15),
            (int)((pixel >> 8) & 15), (int)((pixel >> 12) & 15));
        return (value << 4) | value;
    }
    return int4(
        (int)(pixel & 255), (int)((pixel >> 8) & 255),
        (int)((pixel >> 16) & 255), (int)((pixel >> 24) & 255));
}

int feedback_coordinate(int value, int size, bool clamp_value) {
    if (clamp_value) return clamp(value, 0, size - 1);
    int wrapped = value % size;
    return wrapped < 0 ? wrapped + size : wrapped;
}

float4 feedback_texel(int2 coordinate) {
    int2 dimensions = max((int2)texsize.xy, int2(1, 1));
    int render_scale = max((int)(texenv.w + 0.5), 1);
    uint clut_format = (uint)color_test_state.w;
    coordinate.x = feedback_coordinate(
        coordinate.x, dimensions.x, (clut_format & (1u << 30)) != 0);
    coordinate.y = feedback_coordinate(
        coordinate.y, dimensions.y, (clut_format & (1u << 31)) != 0);
    coordinate += (int2)texsize.zw;
    coordinate =
        coordinate * render_scale + render_scale / 2;

    float4 source = psp_texture.Load(int3(coordinate, 0));
    uint4 bytes = (uint4)(source * 255.0 + 0.5);
    uint index = bytes.r | (bytes.g << 8) |
                 (bytes.b << 16) | (bytes.a << 24);
    uint shift = (clut_format >> 2) & 31;
    uint mask = (clut_format >> 8) & 255;
    uint start = ((clut_format >> 16) & 31) << 4;
    uint format = clut_format & 3;
    uint start_mask = format == 3 ? 255 : 511;
    index = ((index >> shift) & mask) | (start & start_mask);

    uint color;
    if (format == 3) {
        color = psp_clut[index & 511];
    } else {
        index &= 1023;
        uint packed = psp_clut[index >> 1];
        color = (packed >> ((index & 1) * 16)) & 65535;
    }
    return (float4)unpack_native(color, (int)format);
}

float4 sample_feedback(float2 uv, bool nearest) {
    if (nearest) return feedback_texel((int2)floor(uv));
    float2 position = uv - 0.5;
    int2 base = (int2)floor(position);
    float2 weight = position - (float2)base;
    float4 upper = lerp(
        feedback_texel(base),
        feedback_texel(base + int2(1, 0)),
        weight.x);
    float4 lower = lerp(
        feedback_texel(base + int2(0, 1)),
        feedback_texel(base + int2(1, 1)),
        weight.x);
    return lerp(upper, lower, weight.y);
}

int factor_value(
    int factor, int channel, int4 source, int4 destination,
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

int3 blend_rgb(int4 source, int4 destination) {
    int mode = blend_state.y;
    int equation = (mode >> 8) & 7;
    if (equation == 3) return min(source.rgb, destination.rgb);
    if (equation == 4) return max(source.rgb, destination.rgb);
    if (equation == 5) return abs(source.rgb - destination.rgb);
    if (equation > 2) return source.rgb;

    int3 result;
    [unroll]
    for (int channel = 0; channel < 3; channel++) {
        int source_factor = factor_value(
            mode & 15, channel, source, destination,
            blend_state.z, true);
        int destination_factor = factor_value(
            (mode >> 4) & 15, channel, source, destination,
            blend_state.w, false);
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
    source &= 0x00ffffff;
    destination &= 0x00ffffff;
    if (operation == 0) return 0;
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
    return 0x00ffffff;
}

int dither_value(int2 pixel) {
    int row = dither_rows[pixel.y & 3];
    int value = (row >> ((pixel.x & 3) * 4)) & 15;
    return value >= 8 ? value - 16 : value;
}

int4 apply_write_mask(
    int4 color, int4 old_color, uint write_mask, int format) {
    uint native_mask = pack_native(int4(
        (int)(write_mask & 255), (int)((write_mask >> 8) & 255),
        (int)((write_mask >> 16) & 255), (int)((write_mask >> 24) & 255)),
        format);
    uint old_native = pack_native(old_color, format);
    uint new_native = pack_native(color, format);
    return unpack_native(
        (new_native & ~native_mask) | (old_native & native_mask),
        format);
}

int4 shade_source(
    PSInput input, bool front_facing,
    out int source_depth, out int flags) {
    flags = cfg.x >> 8;
    source_depth = (int)(input.position.z * 65535.0 + 0.5);

    if ((flags & 4096) != 0 && !front_facing) {
        discard;
    }
    if ((flags & 16) != 0 &&
        (source_depth < cfg.z || source_depth > cfg.w)) {
        discard;
    }

    float4 vertex = input.color * 255.0;
    float4 shaded = vertex;

    if ((flags & 32) == 0) {
        if ((flags & 1) != 0) {
            float rw = max(abs(input.rw), 1e-20);
            float2 uv = input.uv / rw;
            if ((flags & 64) != 0) uv += float2(0.5, 0.5);
            uv += texsize.zw;

            float4 texture_color =
                (flags & 16384) != 0
                    ? sample_feedback(uv, (flags & 64) != 0)
                    : psp_texture.Sample(
                          psp_sampler, uv / texsize.xy) * 255.0;
            int texture_function = cfg.x & 7;
            bool texture_rgba = (flags & 2) != 0;
            float color_scale = (flags & 4) != 0 ? 2.0 : 1.0;
            float3 rgb;
            float alpha;

            if (texture_function == 1) {
                rgb = texture_rgba
                    ? lerp(vertex.rgb, texture_color.rgb,
                           texture_color.a * (1.0 / 255.0)) * color_scale
                    : texture_color.rgb * color_scale;
                alpha = vertex.a;
            } else if (texture_function == 2) {
                rgb = lerp(
                    vertex.rgb, texenv.rgb * 255.0,
                    texture_color.rgb * (1.0 / 255.0)) * color_scale;
                alpha = texture_rgba
                    ? vertex.a * texture_color.a * (1.0 / 255.0)
                    : vertex.a;
            } else if (texture_function == 3) {
                rgb = texture_color.rgb * color_scale;
                alpha = texture_rgba ? texture_color.a : vertex.a;
            } else if (texture_function >= 4) {
                rgb = (vertex.rgb + texture_color.rgb) * color_scale;
                alpha = texture_rgba
                    ? vertex.a * texture_color.a * (1.0 / 255.0)
                    : vertex.a;
            } else {
                rgb = vertex.rgb * texture_color.rgb *
                      (1.0 / 255.0) * color_scale;
                alpha = texture_rgba
                    ? vertex.a * texture_color.a * (1.0 / 255.0)
                    : vertex.a;
            }
            shaded = float4(
                min(rgb, float3(255.0, 255.0, 255.0)),
                min(alpha, 255.0));
        }

        int alpha_test = cfg.y;
        int alpha_mask = (alpha_test >> 16) & 255;
        int alpha_reference = ((alpha_test >> 8) & 255) & alpha_mask;
        int alpha_value = ((int)(shaded.a + 0.5)) & alpha_mask;
        if (!compare_value(
                alpha_test & 7, alpha_value, alpha_reference)) {
            discard;
        }

        if ((flags & 8) != 0) {
            shaded.rgb = lerp(
                fogcol.rgb * 255.0, shaded.rgb, saturate(input.fog));
        }

        if ((flags & 512) != 0) {
            int3 rgb = (int3)(shaded.rgb + 0.5);
            int packed = rgb.r | (rgb.g << 8) | (rgb.b << 16);
            int masked = packed & color_test_state.z;
            int reference = color_test_state.y & color_test_state.z;
            if (!compare_value(
                    color_test_state.x, masked, reference)) {
                discard;
            }
        }
    }

    return clamp(
        (int4)(shaded + 0.5),
        int4(0, 0, 0, 0), int4(255, 255, 255, 255));
}

float4 main_direct(
    PSInput input, bool front_facing : SV_IsFrontFace) : SV_Target0 {
    int source_depth;
    int flags;
    int4 result = shade_source(
        input, front_facing, source_depth, flags);
    int render_scale = max((int)(texenv.w + 0.5), 1);
    int2 pixel = (int2)input.position.xy / render_scale;
    int format = stencil_state.w & 3;
    if ((flags & 1024) != 0)
        result.rgb += dither_value(pixel);
    result = clamp(
        result, int4(0, 0, 0, 0),
        int4(255, 255, 255, 255));
    result = unpack_native(pack_native(result, format), format);
    return (float4)result * (1.0 / 255.0);
}

PSOutput main(PSInput input, bool front_facing : SV_IsFrontFace) {
    int source_depth;
    int flags;
    int4 source = shade_source(
        input, front_facing, source_depth, flags);
    int render_scale = max((int)(texenv.w + 0.5), 1);
    int2 render_pixel = (int2)input.position.xy;
    int2 pixel = render_pixel / render_scale;
    int format = stencil_state.w & 3;
    if ((flags & 8192) != 0) {
        int4 result = source;
        if ((flags & 1024) != 0)
            result.rgb += dither_value(pixel);
        result = clamp(
            result, int4(0, 0, 0, 0),
            int4(255, 255, 255, 255));
        result = unpack_native(pack_native(result, format), format);
        PSOutput output;
        output.color = (float4)result * (1.0 / 255.0);
        output.depth = (float)source_depth * (1.0 / 65535.0);
        return output;
    }
    int4 old_storage = (int4)(
        destination_color.Load(int3(render_pixel, 0)) * 255.0 + 0.5);
    int4 destination = old_storage;
    if (format == 0) destination.a = 0;
    int old_depth = (int)(
        destination_depth.Load(int3(render_pixel, 0)) * 65535.0 + 0.5);

    int4 result = source;
    int output_depth = old_depth;
    uint write_mask = (uint)fragment_state.x;

    if ((flags & 32) != 0) {
        if ((flags & 1024) != 0)
            result.rgb += dither_value(pixel);
        result = clamp(
            result, int4(0, 0, 0, 0), int4(255, 255, 255, 255));
        if ((fragment_state.z & 0x100) == 0)
            result.rgb = old_storage.rgb;
        if ((fragment_state.z & 0x200) == 0)
            result.a = old_storage.a;
        if ((fragment_state.z & 0x400) != 0)
            output_depth = source_depth;
    } else {
        int stencil = format == 0 ? 0 : destination.a;
        int stencil_function = stencil_state.x & 7;
        int stencil_reference = (stencil_state.x >> 8) & 255;
        int stencil_mask = (stencil_state.x >> 16) & 255;
        bool stencil_pass = (flags & 2048) == 0 ||
            compare_value(
                stencil_function,
                stencil_reference & stencil_mask,
                stencil & stencil_mask);
        bool color_pass = stencil_pass;
        if (!stencil_pass) {
            stencil = stencil_operation(
                stencil_state.y & 7, format, stencil, stencil_reference);
        } else {
            int depth_state = fragment_state.w;
            bool depth_enabled = (depth_state & 1) != 0;
            bool depth_pass = !depth_enabled ||
                compare_value(
                    (depth_state >> 8) & 7,
                    source_depth, old_depth);
            if (!depth_pass) {
                stencil = stencil_operation(
                    (stencil_state.y >> 8) & 7,
                    format, stencil, stencil_reference);
                color_pass = false;
            } else {
                stencil = stencil_operation(
                    (stencil_state.y >> 16) & 7,
                    format, stencil, stencil_reference);
                if (depth_enabled && (depth_state & 0x10000) != 0)
                    output_depth = source_depth;
            }
        }

        if (color_pass) {
            result.rgb = blend_state.x != 0
                ? blend_rgb(source, destination) : source.rgb;
            result.a = stencil;
            if ((flags & 1024) != 0)
                result.rgb += dither_value(pixel);
            result = clamp(
                result, int4(0, 0, 0, 0),
                int4(255, 255, 255, 255));
            if ((fragment_state.y & 1) != 0) {
                uint logical = logic_rgb(
                    (fragment_state.y >> 8) & 15,
                    (uint)result.r | ((uint)result.g << 8) |
                        ((uint)result.b << 16),
                    (uint)destination.r |
                        ((uint)destination.g << 8) |
                        ((uint)destination.b << 16));
                result.rgb = int3(
                    (int)(logical & 255),
                    (int)((logical >> 8) & 255),
                    (int)((logical >> 16) & 255));
            }
        } else {
            result = old_storage;
            result.a = stencil;
        }

        if ((flags & 2048) == 0)
            write_mask |= 0xff000000;
    }

    result = apply_write_mask(result, old_storage, write_mask, format);
    PSOutput output;
    output.color = (float4)result * (1.0 / 255.0);
    output.depth = (float)output_depth * (1.0 / 65535.0);
    return output;
}
