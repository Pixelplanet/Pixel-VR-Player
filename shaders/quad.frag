#version 450

layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;
    vec4 range;  // x: luma offset, y: luma scale, z: sRGB encode flag
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    float y = (texture(luma, vUV).r - pc.range.x) * pc.range.y;
    vec2 c = texture(chroma, vUV).rg - vec2(128.0 / 255.0);
    vec3 rgb = clamp(vec3(y + pc.conversion.x * c.y,
                          y + pc.conversion.y * c.x + pc.conversion.z * c.y,
                          y + pc.conversion.w * c.x), 0.0, 1.0);
    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    outColor = vec4(rgb, 1.0);
}
