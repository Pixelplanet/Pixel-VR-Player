#version 450

// Zero-copy dma-buf video, flat screen. The combined image sampler carries a
// VkSamplerYcbcrConversion, so sampling already yields non-linear R'G'B'; we only
// re-linearise to match the sRGB swapchain (same EOTF as the CPU NV12 path).

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;  // unused (conversion handled by the sampler)
    vec4 range;       // z: sRGB encode flag
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 rgb = texture(video, vUV * pc.conversion.xy).rgb;
    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    outColor = vec4(rgb, 1.0);
}
