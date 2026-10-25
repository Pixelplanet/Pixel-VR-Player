#version 450

// Ambient screen glow for the CPU NV12 upload path. Same geometry and falloff
// as ambilight.frag, but converts YUV -> RGB like quad.frag.

layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;
    vec4 range;  // x: luma offset, y: luma scale, z: sRGB decode flag, w: glow intensity
    vec4 stereo; // x: mode (0 mono/1 SBS/2 TB), y: eye, z: swap, w: curvature
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

const vec2 kScale = vec2(1.55, 1.90);
const float kAspect = 1.7778;

vec3 sampleEye(vec2 uv) {
    float eye = pc.stereo.y;
    if (pc.stereo.z > 0.5) eye = 1.0 - eye;
    if (pc.stereo.x > 1.5) uv.y = uv.y * 0.5 + eye * 0.5;
    else if (pc.stereo.x > 0.5) uv.x = uv.x * 0.5 + eye * 0.5;
    float lodY = max(float(textureQueryLevels(luma)) - 4.0, 0.0);
    float lodC = max(float(textureQueryLevels(chroma)) - 3.0, 0.0);
    float y = (textureLod(luma, uv, lodY).r - pc.range.x) * pc.range.y;
    vec2 c = textureLod(chroma, uv, lodC).rg - vec2(128.0 / 255.0);
    return clamp(vec3(y + pc.conversion.x * c.y,
                      y + pc.conversion.y * c.x + pc.conversion.z * c.y,
                      y + pc.conversion.w * c.x), 0.0, 1.0);
}

void main() {
    vec2 p = (vUV - 0.5) * kScale;
    vec2 q = abs(p) - 0.5;
    if (q.x < 0.0 && q.y < 0.0) {
        discard;
    }
    float d = length(max(q * vec2(kAspect, 1.0), 0.0));
    float a = 1.0 - smoothstep(0.0, 0.46, d);
    a *= a;
    if (a <= 0.003) {
        discard;
    }

    vec2 uvS = clamp(p + 0.5, 0.0, 1.0);
    uvS = 0.5 + (uvS - 0.5) * 0.90;
    vec3 rgb = sampleEye(uvS) * 0.40;
    rgb += sampleEye(clamp(uvS + vec2(0.07, 0.0), 0.0, 1.0)) * 0.15;
    rgb += sampleEye(clamp(uvS - vec2(0.07, 0.0), 0.0, 1.0)) * 0.15;
    rgb += sampleEye(clamp(uvS + vec2(0.0, 0.07), 0.0, 1.0)) * 0.15;
    rgb += sampleEye(clamp(uvS - vec2(0.0, 0.07), 0.0, 1.0)) * 0.15;

    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    float l = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = max(mix(vec3(l), rgb, 1.25), vec3(0.0));

    outColor = vec4(rgb, a * clamp(pc.range.w, 0.0, 1.0));
}
