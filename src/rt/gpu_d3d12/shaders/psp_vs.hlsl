/* PSP GE vertex stage.
 *
 * Vertices are already transformed by ge.c's CPU T&L. The input layout is the same
 * 32-byte GpuVert used by the Vulkan backend:
 *   POSITION0 float4, TEXCOORD0 float2, TEXCOORD1 float, COLOR0 unorm8x4.
 * All varyings are noperspective because the software rasterizer interpolates u*rw,
 * v*rw, and rw affinely in screen space, then divides in the pixel shader. */

struct VSInput {
    float4 position : POSITION0;
    float2 uv       : TEXCOORD0;
    float  fog      : TEXCOORD1;
    float4 color    : COLOR0;
};

struct VSOutput {
    float4 position              : SV_Position;
    noperspective float2 uv      : TEXCOORD0;
    noperspective float  rw      : TEXCOORD1;
    noperspective float  fog     : TEXCOORD2;
    noperspective float4 color   : COLOR0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    /* D3D maps NDC +Y to the top of the viewport, opposite Vulkan's positive-height
     * viewport transform. Both paths therefore land on the same PSP screen pixels. */
    output.position = float4(
        input.position.x * (1.0 / 256.0) - 1.0,
        1.0 - input.position.y * (1.0 / 136.0),
        input.position.z * (1.0 / 65535.0),
        1.0);
    output.uv = input.uv;
    output.rw = input.position.w;
    output.fog = input.fog;
    output.color = input.color;
    return output;
}
