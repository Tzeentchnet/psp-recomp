/* PSP GE vertex stage.
 *
 * ge.c remains authoritative for PSP vertex formats, T&L, clipping, and primitive
 * assembly. The shader raw-loads its canonical 32-byte output stream instead of
 * requiring an input-layout decode and vertex-buffer bind for every frame. */

ByteAddressBuffer ge_vertices : register(t0, space1);

struct VSOutput {
    float4 position              : SV_Position;
    noperspective float2 uv      : TEXCOORD0;
    noperspective float  rw      : TEXCOORD1;
    noperspective float  fog     : TEXCOORD2;
    noperspective float4 color   : COLOR0;
};

VSOutput main(uint vertex_id : SV_VertexID) {
    uint byte_offset = vertex_id << 5;
    uint4 position_bits = ge_vertices.Load4(byte_offset);
    uint4 attributes = ge_vertices.Load4(byte_offset + 16);
    float4 input_position = asfloat(position_bits);
    float2 input_uv = asfloat(attributes.xy);
    float input_fog = asfloat(attributes.z);
    uint packed_color = attributes.w;
    float4 input_color = float4(
        packed_color & 255,
        (packed_color >> 8) & 255,
        (packed_color >> 16) & 255,
        packed_color >> 24) * (1.0 / 255.0);

    VSOutput output;
    /* D3D maps NDC +Y to the top of the viewport, opposite Vulkan's positive-height
     * viewport transform. Both paths therefore land on the same PSP screen pixels. */
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
