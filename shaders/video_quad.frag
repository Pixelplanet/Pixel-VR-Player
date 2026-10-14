#version 450

// Zero-copy dma-buf video, flat screen. The combined image sampler carries a
// VkSamplerYcbcrConversion, so sampling already yields non-linear R'G'B'; we only
// re-linearise to match the sRGB swapchain (same EOTF as the CPU NV12 path).

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;  // xy: crop UV scale
    vec4 range;       // z: sRGB encode flag
    vec4 stereo;      // x: mode (0 mono/1 SBS/2 TB), y: eye, z: swap
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec2 uv = vUV;
    float eye = pc.stereo.y;
    if (pc.stereo.z > 0.5) eye = 1.0 - eye;
    if (pc.stereo.x > 1.5) uv.y = uv.y * 0.5 + eye * 0.5;       // top/bottom
    else if (pc.stereo.x > 0.5) uv.x = uv.x * 0.5 + eye * 0.5;  // side-by-side
    vec3 rgb = texture(video, uv * pc.conversion.xy).rgb;
    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    outColor = vec4(rgb, 1.0);
}
