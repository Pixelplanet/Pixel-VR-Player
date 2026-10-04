#version 450

// Textured video screen: a 16:9 quad whose distance, yaw and aspect are applied
// by the model matrix on the CPU. Geometry and UVs are hardcoded and indexed by
// gl_VertexIndex (no vertex buffer).

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;
    vec4 range;
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 positions[6] = vec2[](
        vec2(-1.6,  0.9), vec2(-1.6, -0.9), vec2( 1.6, -0.9),
        vec2(-1.6,  0.9), vec2( 1.6, -0.9), vec2( 1.6,  0.9));
    vec2 uvs[6] = vec2[](
        vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0));

    gl_Position = pc.mvp * vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vUV = uvs[gl_VertexIndex];
}
