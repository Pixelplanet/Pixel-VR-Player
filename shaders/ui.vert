#version 450

// Procedural vector/SDF quad with push constants. A unit quad (-1..1) placed and sized
// by the model matrix; passes normalized 2D coordinate vCoord (-1..1) to fragment shader.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 params;       // x: shapeType, y: radius / paramA, z: borderWidth / paramB, w: extra
    vec4 borderColor;
} pc;

layout(location = 0) out vec2 vCoord;

void main() {
    vec2 positions[6] = vec2[](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    vCoord = positions[gl_VertexIndex];
    gl_Position = pc.mvp * vec4(vCoord, 0.0, 1.0);
}
