#version 450

// Zero-copy dma-buf video, equirectangular 360/180. Same ray mapping and stereo
// layout as sphere.frag, but the combined image sampler carries a
// VkSamplerYcbcrConversion so a single texture fetch already returns R'G'B'.

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    mat4 viewRotation;
    vec4 fov;
    vec4 mode;  // x: projection (1=360, 2=180), y: stereo (0/1/2), z: eye
    vec4 conversion;  // unused (conversion handled by the sampler)
    vec4 range;       // z: sRGB encode flag
} pc;

layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main() {
    vec3 d = normalize(vDir);
    float projection = pc.mode.x;
    float stereo = pc.mode.y;
    float eye = pc.mode.z;
    if (pc.mode.w > 0.5) { eye = 1.0 - eye; }  // swap left/right eye

    float lon = atan(d.x, -d.z);
    float lat = asin(clamp(d.y, -1.0, 1.0));

    float u;
    float v = 0.5 - lat / PI;

    if (projection > 1.5) {
        if (d.z > 0.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        u = lon / PI + 0.5;
        if (u < 0.0 || u > 1.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    } else {
        u = lon / (2.0 * PI) + 0.5;
    }

    if (stereo > 1.5) {
        v = v * 0.5 + eye * 0.5;
    } else if (stereo > 0.5) {
        u = u * 0.5 + eye * 0.5;
    }

    vec3 rgb = texture(video, vec2(u, v) * pc.conversion.xy).rgb;
    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    outColor = vec4(rgb, 1.0);
}
