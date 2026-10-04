#version 450

// Phase 0/1 test: a pose-anchored triangle 2 m in front of the LOCAL origin.
// No vertex buffers; positions are hardcoded and transformed by the eye MVP.

layout(push_constant) uniform Push {
    mat4 mvp;
} pc;

layout(location = 0) out vec3 vColor;

void main() {
    vec3 positions[3] = vec3[](
        vec3( 0.0,  0.5, -2.0),
        vec3(-0.5, -0.5, -2.0),
        vec3( 0.5, -0.5, -2.0));
    vec3 colors[3] = vec3[](
        vec3(1.0, 0.2, 0.2),
        vec3(0.2, 1.0, 0.2),
        vec3(0.2, 0.4, 1.0));

    gl_Position = pc.mvp * vec4(positions[gl_VertexIndex], 1.0);
    vColor = colors[gl_VertexIndex];
}
