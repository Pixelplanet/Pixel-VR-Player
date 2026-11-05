#version 450

// Frosted-glass backdrop quad for the UI layer: a unit quad placed by `mvp`
// (the element rect is baked into it). The fragment stage projects each point
// onto the video screen behind the panel through the homography rows.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 h0;    // homography rows: (x, y, 1) in quad coords -> (u*w, v*w, w)
    vec4 h1;    //   .w of h0/h1/h2: luma offset, luma scale, sRGB decode flag
    vec4 h2;
    vec4 misc;  // x: blur lod + 10 * stereo mode, y: alpha, z: corner radius, w: aspect
} pc;

layout(location = 0) out vec2 vCoord;

void main() {
    vec2 corners[6] = vec2[](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    vCoord = corners[gl_VertexIndex % 6];
    gl_Position = pc.mvp * vec4(vCoord, 0.0, 1.0);
}
