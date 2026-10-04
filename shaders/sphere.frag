#version 450

// Samples an equirectangular video from a per-pixel view ray. Supports full 360
// and front-hemisphere 180, with mono / side-by-side / over-under stereo layouts
// selected per eye.

layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;

layout(push_constant) uniform Push {
    mat4 viewRotation;
    vec4 fov;
    vec4 mode;  // x: projection (1=360, 2=180), y: stereo (0=mono,1=SBS,2=TB), z: eye
    vec4 conversion;
    vec4 range;  // x: luma offset, y: luma scale, z: sRGB encode flag
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

    float lon = atan(d.x, -d.z);              // 0 = forward (-z)
    float lat = asin(clamp(d.y, -1.0, 1.0));

    float u;
    float v = 0.5 - lat / PI;

    if (projection > 1.5) {
        // 180: front hemisphere only.
        if (d.z > 0.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
        u = lon / PI + 0.5;
        if (u < 0.0 || u > 1.0) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    } else {
        u = lon / (2.0 * PI) + 0.5;
    }

    if (stereo > 1.5) {
        v = v * 0.5 + eye * 0.5;   // over-under: left eye = top half
    } else if (stereo > 0.5) {
        u = u * 0.5 + eye * 0.5;   // side-by-side: left eye = left half
    }

    vec2 st = vec2(u, v);
    float yv = (texture(luma, st).r - pc.range.x) * pc.range.y;
    vec2 cc = texture(chroma, st).rg - vec2(128.0 / 255.0);
    vec3 rgb = clamp(vec3(yv + pc.conversion.x * cc.y,
                          yv + pc.conversion.y * cc.x + pc.conversion.z * cc.y,
                          yv + pc.conversion.w * cc.x), 0.0, 1.0);
    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    outColor = vec4(rgb, 1.0);
}
