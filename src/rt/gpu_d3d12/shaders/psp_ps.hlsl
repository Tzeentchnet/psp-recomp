/* PSP GE pixel-stage uber-shader. Mirrors ge.c shade() + zrange_ok():
 * texture functions, alpha test, fog, depth-range discard, clear mode, and the
 * doubled-source-alpha factors that cannot be expressed directly by D3D12 blending.
 * Blending, color masks, depth/stencil, and raster state remain PSO state. */

Texture2D<float4> psp_texture : register(t0);
SamplerState psp_sampler : register(s0);

cbuffer PSPConstants : register(b0) {
    int4 cfg;       /* x=texfunc|flags<<8, y=alpha test, z=minz, w=maxz */
    float4 texenv;  /* rgb=GE_TEXENVCOLOR / 255 */
    float4 fogcol;  /* rgb=GE_FOGCOLOR / 255 */
    float4 texsize; /* xy=dimensions, zw=render-target sub-rect texel offset */
    int4 color_test_state; /* x=func, y=reference RGB, z=mask RGB */
    int4 stencil_state;    /* x=test, y=ops, z=write mask, w=fb format */
    int4 dither_rows;      /* four packed signed-nibble rows */
};

struct PSInput {
    float4 position              : SV_Position;
    noperspective float2 uv      : TEXCOORD0;
    noperspective float  rw      : TEXCOORD1;
    noperspective float  fog     : TEXCOORD2;
    noperspective float4 color   : COLOR0;
};

float4 main(PSInput input) : SV_Target0 {
    int flags = cfg.x >> 8;

    /* Transform-mode depth range is a PSP kill test, not a clamp. */
    if ((flags & 16) != 0) {
        int z16 = (int)(input.position.z * 65535.0 + 0.5);
        if (z16 < cfg.z || z16 > cfg.w) discard;
    }

    float4 vertex = input.color * 255.0;
    float4 color = vertex;

    if ((flags & 32) == 0) {
        if ((flags & 1) != 0) {
            float rw = max(abs(input.rw), 1e-20);
            float2 uv = input.uv / rw;
            if ((flags & 64) != 0) uv += float2(0.5, 0.5);
            uv += texsize.zw;

            float4 texture_color =
                psp_texture.Sample(psp_sampler, uv / texsize.xy) * 255.0;
            int texture_function = cfg.x & 7;
            bool texture_rgba = (flags & 2) != 0;
            float color_scale = (flags & 4) != 0 ? 2.0 : 1.0;
            float3 rgb;
            float alpha;

            if (texture_function == 1) {
                if (texture_rgba) {
                    rgb = lerp(
                        vertex.rgb, texture_color.rgb,
                        texture_color.a * (1.0 / 255.0)) * color_scale;
                } else {
                    rgb = texture_color.rgb * color_scale;
                }
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
            color = float4(
                min(rgb, float3(255.0, 255.0, 255.0)),
                min(alpha, 255.0));
        }

        int alpha_test = cfg.y;
        int alpha_function = alpha_test & 7;
        if (alpha_function != 1) {
            int alpha_mask = (alpha_test >> 16) & 0xff;
            int alpha_reference = ((alpha_test >> 8) & 0xff) & alpha_mask;
            int alpha_value = ((int)(color.a + 0.5)) & alpha_mask;
            bool pass;
            if (alpha_function == 0) {
                pass = false;
            } else if (alpha_function == 2) {
                pass = alpha_value == alpha_reference;
            } else if (alpha_function == 3) {
                pass = alpha_value != alpha_reference;
            } else if (alpha_function == 4) {
                pass = alpha_value < alpha_reference;
            } else if (alpha_function == 5) {
                pass = alpha_value <= alpha_reference;
            } else if (alpha_function == 6) {
                pass = alpha_value > alpha_reference;
            } else {
                pass = alpha_value >= alpha_reference;
            }
            if (!pass) discard;
        }

        if ((flags & 8) != 0) {
            float fog = saturate(input.fog);
            color.rgb = lerp(fogcol.rgb * 255.0, color.rgb, fog);
        }

        if ((flags & 512) != 0) {
            int3 rgb = (int3)(color.rgb + 0.5);
            int packed = rgb.r | (rgb.g << 8) | (rgb.b << 16);
            int masked = packed & color_test_state.z;
            int reference =
                color_test_state.y & color_test_state.z;
            bool pass =
                color_test_state.x == 1 ||
                (color_test_state.x == 2 && masked == reference) ||
                (color_test_state.x == 3 && masked != reference);
            if (!pass) discard;
        }

        if ((flags & 128) != 0) {
            color.rgb *= saturate(color.a * (2.0 / 255.0));
        }
        if ((flags & 256) != 0) {
            color.rgb *= saturate(1.0 - color.a * (2.0 / 255.0));
        }
    }

    if ((flags & 1024) != 0) {
        int row = dither_rows[(int)input.position.y & 3];
        int value =
            (row >> (((int)input.position.x & 3) * 4)) & 15;
        if (value >= 8) value -= 16;
        color.rgb = clamp(
            color.rgb + (float)value,
            float3(0.0, 0.0, 0.0),
            float3(255.0, 255.0, 255.0));
    }

    if ((flags & 2048) != 0) {
        int stencil_pass = (stencil_state.y >> 16) & 7;
        if ((stencil_state.x & 7) == 1) {
            if (stencil_pass == 1) {
                color.a = 0.0;
            } else if (stencil_pass == 2) {
                color.a = (float)(
                    ((stencil_state.x >> 8) & 255) &
                    ((stencil_state.x >> 16) & 255));
            }
        }
    }

    return color * (1.0 / 255.0);
}
