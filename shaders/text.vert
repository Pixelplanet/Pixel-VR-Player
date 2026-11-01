#version 450

// Batched glyph quads: one instance per glyph (rect, atlas UV rect, colour) read
// from a per-eye instance buffer, so a whole run of text is a single draw. The
// rect is in panel-local metres; `mvp` maps panel space to clip space and the
// optional bend radius wraps the panel onto a vertical cylinder.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;  // unused (per-glyph colour comes from the instance)
    vec4 rect;   // unused
    vec4 uv;     // unused
    vec4 bend;   // x: bend radius (0 = flat)
} pc;

layout(location = 0) in vec4 inRect;   // x, y (bottom-left), w, h
layout(location = 1) in vec4 inUv;     // u0, v0, u1, v1
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vColor;

void main() {
    vec2 corners[6] = vec2[](
        vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
    vec2 c = corners[gl_VertexIndex];
    vec2 p = inRect.xy + c * inRect.zw;
    // v flips because the atlas is top-down while +y is up in panel space.
    vUV = vec2(mix(inUv.x, inUv.z, c.x), mix(inUv.w, inUv.y, c.y));
    vColor = inColor;
    vec3 pos = vec3(p, 0.0);
    float r = pc.bend.x;
    if (r > 0.0) {
        float a = p.x / r;
        pos = vec3(r * sin(a), p.y, r * (1.0 - cos(a)));
    }
    gl_Position = pc.mvp * vec4(pos, 1.0);
}
