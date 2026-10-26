cbuffer SmaaConstants : register(b0) {
    float4 smaa_rt_metrics;
};

#define SMAA_RT_METRICS smaa_rt_metrics
#define SMAA_PRESET_HIGH 1
#define SMAA_CUSTOM_SL 1

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);

#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) \
    tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) \
    tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) \
    tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(LinearSampler, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(PointSampler, coord)
#define SMAASampleOffset(tex, coord, offset) \
    tex.Sample(LinearSampler, coord, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#include "../../../../third_party/smaa/SMAA.hlsl"

Texture2D<float4> input0 : register(t0);
Texture2D<float4> input1 : register(t1);
Texture2D<float4> input2 : register(t2);

void fullscreen_vertex(
    uint vertex_id, out float4 position, out float2 texcoord) {
    texcoord = float2((vertex_id << 1) & 2, vertex_id & 2);
    position = float4(
        texcoord.x * 2.0 - 1.0,
        1.0 - texcoord.y * 2.0,
        0.0,
        1.0);
}

void smaa_edge_vs(
    uint vertex_id : SV_VertexID,
    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float4 offset0 : TEXCOORD1,
    out float4 offset1 : TEXCOORD2,
    out float4 offset2 : TEXCOORD3) {
    fullscreen_vertex(vertex_id, position, texcoord);
    float4 offset[3];
    SMAAEdgeDetectionVS(texcoord, offset);
    offset0 = offset[0];
    offset1 = offset[1];
    offset2 = offset[2];
}

// Keep SV_Position in each pixel entry point for D3D12 pipeline validation.
float4 smaa_edge_ps(
    float4 position : SV_Position,
    float2 texcoord : TEXCOORD0,
    float4 offset0 : TEXCOORD1,
    float4 offset1 : TEXCOORD2,
    float4 offset2 : TEXCOORD3) : SV_Target0 {
    float4 offset[3] = {offset0, offset1, offset2};
    return float4(
        SMAALumaEdgeDetectionPS(texcoord, offset, input0), 0.0, 0.0);
}

void smaa_blend_vs(
    uint vertex_id : SV_VertexID,
    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float2 pixcoord : TEXCOORD1,
    out float4 offset0 : TEXCOORD2,
    out float4 offset1 : TEXCOORD3,
    out float4 offset2 : TEXCOORD4) {
    fullscreen_vertex(vertex_id, position, texcoord);
    float4 offset[3];
    SMAABlendingWeightCalculationVS(
        texcoord, pixcoord, offset);
    offset0 = offset[0];
    offset1 = offset[1];
    offset2 = offset[2];
}

float4 smaa_blend_ps(
    float4 position : SV_Position,
    float2 texcoord : TEXCOORD0,
    float2 pixcoord : TEXCOORD1,
    float4 offset0 : TEXCOORD2,
    float4 offset1 : TEXCOORD3,
    float4 offset2 : TEXCOORD4) : SV_Target0 {
    float4 offset[3] = {offset0, offset1, offset2};
    return SMAABlendingWeightCalculationPS(
        texcoord, pixcoord, offset,
        input0, input1, input2, float4(0.0, 0.0, 0.0, 0.0));
}

void smaa_neighborhood_vs(
    uint vertex_id : SV_VertexID,
    out float4 position : SV_Position,
    out float2 texcoord : TEXCOORD0,
    out float4 offset : TEXCOORD1) {
    fullscreen_vertex(vertex_id, position, texcoord);
    SMAANeighborhoodBlendingVS(texcoord, offset);
}

float4 smaa_neighborhood_ps(
    float4 position : SV_Position,
    float2 texcoord : TEXCOORD0,
    float4 offset : TEXCOORD1) : SV_Target0 {
    return SMAANeighborhoodBlendingPS(
        texcoord, offset, input0, input1);
}
