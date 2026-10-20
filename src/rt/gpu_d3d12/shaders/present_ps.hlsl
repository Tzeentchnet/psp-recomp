Texture2D<float4> display_texture : register(t0);
SamplerState display_sampler : register(s0);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {
    return display_texture.Sample(display_sampler, uv);
}
