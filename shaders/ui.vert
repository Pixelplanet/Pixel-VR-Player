#version 450

// Procedural vector/SDF quad. The element's centre and half-size (rect) are in
// panel-local metres and `mvp` maps panel space to clip space, so the quad can
// be bent onto a vertical cylinder for the curved main panel. vCoord (-1..1) is
// passed to the fragment shader as before.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 params;       // x: shapeType, y: radius, z: borderWidth, w: bend radius (0 = flat)
    vec4 borderColor;
    vec4 rect;         // centre x, y and half-size w, h (zero size = unit quad)
} pc;

layout(location = 0) out vec2 vCoord;

// Must match kUiBendSegments in vulkan_renderer.cpp (vertex count = 6 * segments).
const int kBendSegments = 24;

void main() {
    vec2 corners[6] = vec2[](
        vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0),
        vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
    float bendR = pc.params.w;
    int segments = (bendR > 0.0) ? kBendSegments : 1;
    int seg = gl_VertexIndex / 6;
    vec2 c = corners[gl_VertexIndex % 6];
    // Split the quad into vertical strips so the curve stays smooth.
    float x0 = -1.0 + 2.0 * float(seg) / float(segments);
    float x1 = -1.0 + 2.0 * float(seg + 1) / float(segments);
    vCoord = vec2(c.x < 0.0 ? x0 : x1, c.y);

    bool unit = (pc.rect.z == 0.0 && pc.rect.w == 0.0);
    vec2 centre = unit ? vec2(0.0) : pc.rect.xy;
    vec2 halfSize = unit ? vec2(1.0) : pc.rect.zw;
    vec2 p = centre + vCoord * halfSize;
    vec3 pos = vec3(p, 0.0);
    if (bendR > 0.0) {
        // Arc length along the panel maps to an angle around a vertical axis
        // on the viewer's side (+z), so the panel wraps toward the viewer.
        float a = p.x / bendR;
        pos = vec3(bendR * sin(a), p.y, bendR * (1.0 - cos(a)));
    }
    gl_Position = pc.mvp * vec4(pos, 1.0);
}
