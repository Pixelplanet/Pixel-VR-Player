#version 450

// Thumbnail quad in panel-local metres (push-constant rect + UV rect). On the
// curved panel it is split into vertical strips (bend.y) so the image follows
// the cylinder instead of cutting a flat chord across it.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 rect;  // x, y (bottom-left), w, h  in panel-local units
    vec4 uv;    // u0, v0, u1, v1
    vec4 bend;  // x: bend radius (0 = flat), y: vertical strips
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    vec2 corners[6] = vec2[](
        vec2(0.0, 0.0), vec2(0.0, 1.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0));
    float r = pc.bend.x;
    int strips = (r > 0.0) ? max(int(pc.bend.y + 0.5), 1) : 1;
    int strip = gl_VertexIndex / 6;
    vec2 c = corners[gl_VertexIndex % 6];
    c.x = (float(strip) + c.x) / float(strips);
    vec2 p = pc.rect.xy + c * pc.rect.zw;
    vUV = vec2(mix(pc.uv.x, pc.uv.z, c.x), mix(pc.uv.w, pc.uv.y, c.y));
    vec3 pos = vec3(p, 0.0);
    if (r > 0.0) {
        float a = p.x / r;
        pos = vec3(r * sin(a), p.y, r * (1.0 - cos(a)));
    }
    gl_Position = pc.mvp * vec4(pos, 1.0);
}
