#version 450

// Glyph quad in panel-local metres. The push constant carries the glyph's
// position rect and atlas UV rect; geometry is derived from gl_VertexIndex.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 rect;  // x, y (bottom-left), w, h  in panel-local units
    vec4 uv;    // u0, v0, u1, v1
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 corners[6] = vec2[](
        vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
    vec2 c = corners[gl_VertexIndex];
    vec2 pos = pc.rect.xy + c * pc.rect.zw;
    // v flips because the atlas is top-down while +y is up in panel space.
    vUV = vec2(mix(pc.uv.x, pc.uv.z, c.x), mix(pc.uv.w, pc.uv.y, c.y));
    gl_Position = pc.mvp * vec4(pos, 0.0, 1.0);
}
