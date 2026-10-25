#version 450

// Ambient screen glow ("ambilight") for the flat cinema screen (resolved RGBA
// dma-buf path). Drawn on an enlarged copy of the screen mesh *behind* the
// video: each fragment outside the screen rectangle samples a heavily blurred
// (high mip) colour from the nearest screen edge and fades it out radially, so
// the dark void around the screen picks up the colours of the picture.

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;  // xy: crop UV scale
    vec4 range;       // z: sRGB decode flag, w: glow intensity
    vec4 stereo;      // x: mode (0 mono/1 SBS/2 TB), y: eye, z: swap, w: curvature
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Must match kAmbilightScaleX/Y in vulkan_renderer.cpp.
const vec2 kScale = vec2(1.55, 1.90);
const float kAspect = 1.7778;  // screen width / height (glow shape only)

vec3 sampleEye(vec2 uv, float lod) {
    float eye = pc.stereo.y;
    if (pc.stereo.z > 0.5) eye = 1.0 - eye;
    if (pc.stereo.x > 1.5) uv.y = uv.y * 0.5 + eye * 0.5;
    else if (pc.stereo.x > 0.5) uv.x = uv.x * 0.5 + eye * 0.5;
    return textureLod(video, uv * pc.conversion.xy, lod).rgb;
}

void main() {
    vec2 p = (vUV - 0.5) * kScale;          // the screen covers |p| <= 0.5
    vec2 q = abs(p) - 0.5;
    if (q.x < 0.0 && q.y < 0.0) {
        discard;                             // hidden behind the screen anyway
    }
    float d = length(max(q * vec2(kAspect, 1.0), 0.0));   // metres-ish, in screen heights
    float a = 1.0 - smoothstep(0.0, 0.46, d);
    a *= a;
    if (a <= 0.003) {
        discard;
    }

    // Nearest edge point, pulled slightly inwards so we gather the edge band.
    vec2 uvS = clamp(p + 0.5, 0.0, 1.0);
    uvS = 0.5 + (uvS - 0.5) * 0.90;
    float lod = max(float(textureQueryLevels(video)) - 4.0, 0.0);
    vec3 rgb = sampleEye(uvS, lod) * 0.40;
    rgb += sampleEye(clamp(uvS + vec2(0.07, 0.0), 0.0, 1.0), lod) * 0.15;
    rgb += sampleEye(clamp(uvS - vec2(0.07, 0.0), 0.0, 1.0), lod) * 0.15;
    rgb += sampleEye(clamp(uvS + vec2(0.0, 0.07), 0.0, 1.0), lod) * 0.15;
    rgb += sampleEye(clamp(uvS - vec2(0.0, 0.07), 0.0, 1.0), lod) * 0.15;

    if (pc.range.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    // Gentle saturation lift so the glow reads as colour, not grey haze.
    float l = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = max(mix(vec3(l), rgb, 1.25), vec3(0.0));

    outColor = vec4(rgb, a * clamp(pc.range.w, 0.0, 1.0));
}
