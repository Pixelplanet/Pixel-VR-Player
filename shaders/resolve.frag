#version 450

// Samples the imported YCbCr frame (the sampler carries a VkSamplerYcbcrConversion,
// so a single fetch yields non-linear R'G'B') and writes it verbatim into the RGBA
// resolve target. The sRGB EOTF stays in the video shaders that read this target.

layout(binding = 0) uniform sampler2D ycbcr;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(ycbcr, vUV).rgb, 1.0);
}
