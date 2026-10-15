#version 450

layout(binding = 0) uniform sampler2D fontAtlas;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 rect;
    vec4 uv;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    // Signed-distance text: a crisp ~1px edge in screen space so it stays sharp
    // at any size (a coverage-bitmap fallback also reads acceptably here).
    float d = texture(fontAtlas, vUV).r;
    float a = clamp((d - 0.5) / max(fwidth(d), 0.0001) + 0.5, 0.0, 1.0);
    outColor = vec4(pc.color.rgb, pc.color.a * a);
}
