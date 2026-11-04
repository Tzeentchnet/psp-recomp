#version 450

layout(location = 0) out vec2 texcoord;

void main() {
    texcoord = vec2(
        float((gl_VertexIndex << 1) & 2),
        float(gl_VertexIndex & 2));
    gl_Position = vec4(
        texcoord.x * 2.0 - 1.0,
        texcoord.y * 2.0 - 1.0,
        0.0,
        1.0);
}
