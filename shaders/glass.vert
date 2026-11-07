#version 450

// Frosted-glass backdrop quad for the UI layer: the panel rect in clip space.
// The fragment stage finds, per layer pixel, the video screen point behind it.

layout(push_constant) uniform Push {
    vec4 ndc;   // clip position: vCoord * ndc.xy + ndc.zw (the panel rect, animated)
    vec4 win;   // layer pixel -> panel metres at rest: x0, y0, metres per pixel x, y
    vec4 hu;    // 3x4 rows: panel point (x, y, z, 1) -> the same point in the
    vec4 hv;    //   screen's local frame (metres, y up, +z toward the viewer)
    vec4 hw;
    vec4 misc;  // x: blur lod + 10 * stereo mode, y: alpha, z: corner radius, w: aspect
    vec4 tex;   // xy: crop UV scale (RGBA) or luma offset / scale (NV12), z: sRGB decode, w: panel bend radius
    vec4 scr;   // screen half width (arc), half height, bend radius (0 = flat), edge fade (metres)
    vec4 eye;   // xyz: the head in screen-local metres
} pc;

layout(location = 0) out vec2 vCoord;

void main() {
    vec2 corners[6] = vec2[](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    vCoord = corners[gl_VertexIndex % 6];
    gl_Position = vec4(vCoord * pc.ndc.xy + pc.ndc.zw, 0.5, 1.0);
}
