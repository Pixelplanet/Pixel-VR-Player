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
    vec4 extra;  // x: how far the glow reaches, in screen heights (0 = the default 0.46)
                 // y: share of the frame left out at the top and at the bottom (black bars)
                 // z: 1 = this draw is the copy of the picture on the screen's own surface
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Must match kAmbilightScaleX/Y in vulkan_renderer.cpp.
const vec2 kScale = vec2(1.55, 1.90);
const float kAspect = 1.7778;  // screen width / height (glow shape only)

vec3 sampleEye(vec2 uv, float lod) {
    float eye = pc.stereo.y;
    if (pc.stereo.z > 0.5) eye = 1.0 - eye;
    uv.y = pc.extra.y + uv.y * (1.0 - 2.0 * pc.extra.y);  // without the black bars
    if (pc.stereo.x > 1.5) uv.y = uv.y * 0.5 + eye * 0.5;
    else if (pc.stereo.x > 0.5) uv.x = uv.x * 0.5 + eye * 0.5;
    return textureLod(video, uv * pc.conversion.xy, lod).rgb;
}

void main() {
    vec2 p = (vUV - 0.5) * kScale;          // the screen covers |p| <= 0.5
    vec2 q = abs(p) - 0.5;
    // No hole behind the screen: the screen covers the glow anyway, and the glow is not on the
    // screen's own surface (a curved screen comes forward at its sides, the glow does not), so the
    // edge of a hole showed as a black sliver beside the screen's edge when seen at an angle.
    if (pc.extra.z > 0.5) {
        // Drawn a second time on the screen's own surface (extra.z = 1, the whole quad is the picture):
        // Behind the screen itself: a soft, opaque copy of the picture instead of glow. Where two
        // screens overlap, the front screen's soft edge lets the eye picture show through for a
        // texel; with another screen's glow there it looked like a gap between the screens.
        vec3 own = sampleEye(vUV, min(2.0, float(textureQueryLevels(video)) - 1.0));
        if (pc.range.z > 0.5) {
            own = mix(own / 12.92, pow((own + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), own));
        }
        outColor = vec4(own, 1.0);
        return;
    }
    if (pc.range.w <= 0.0) discard;  // no glow for this screen
    float d = length(max(q * vec2(kAspect, 1.0), 0.0));   // metres-ish, in screen heights
    float reach = pc.extra.x > 0.0 ? pc.extra.x : 0.46;
    float a = 1.0 - smoothstep(0.0, reach, d);
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

    // A dark edge gives no glow at all (it would only paint black over a neighbour's glow).
    float lit = smoothstep(0.0, 0.05, max(rgb.r, max(rgb.g, rgb.b)));
    outColor = vec4(rgb, a * lit * clamp(pc.range.w, 0.0, 1.0));
}
