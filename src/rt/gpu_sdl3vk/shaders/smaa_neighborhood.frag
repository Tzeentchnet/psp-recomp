#version 450
#extension GL_GOOGLE_include_directive : require

layout(location = 0) in vec2 texcoord;
layout(location = 0) out vec4 output_color;
layout(set = 0, binding = 0) uniform sampler2D input0;
layout(set = 0, binding = 1) uniform sampler2D input1;
layout(push_constant) uniform SmaaConstants {
    vec4 metrics;
} smaa;

#define SMAA_RT_METRICS smaa.metrics
#define SMAA_PRESET_HIGH 1
#define SMAA_GLSL_4 1
#include "../../../../third_party/smaa/SMAA.hlsl"

void main() {
    vec4 offset;
    SMAANeighborhoodBlendingVS(texcoord, offset);
    output_color = SMAANeighborhoodBlendingPS(
        texcoord, offset, input0, input1);
}
