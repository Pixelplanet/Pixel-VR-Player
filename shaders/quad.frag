#version 450

layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;
    vec4 range;  // x: luma offset, y: luma scale, z: sRGB encode flag
    vec4 stereo; // x: mode (0 mono/1 SBS/2 TB), y: eye, z: swap
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = vUV;
    float eye = pc.stereo.y;
    if (pc.stereo.z > 0.5) eye = 1.0 - eye;
    if (pc.stereo.x > 1.5) uv.y = uv.y * 0.5 + eye * 0.5;       // top/bottom
    else if (pc.stereo.x > 0.5) uv.x = uv.x * 0.5 + eye * 0.5;  // side-by-side
    float y = (texture(luma, uv).r - pc.range.x) * pc.range.y;
    vec2 c = texture(chroma, uv).rg - vec2(128.0 / 255.0);
    vec3 rgb = clamp(vec3(y + pc.conversion.x * c.y,
                          y + pc.conversion.y * c.x + pc.conversion.z * c.y,
                          y + pc.conversion.w * c.x), 0.0, 1.0);
    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    outColor = vec4(rgb, 1.0);
}
