#version 450

layout(binding = 0) uniform sampler2D fontAtlas;

// Same block as text.vert. uv is unused by the regular text path (all zero);
// small-text mode (the controller button labels) sets it:
//   uv.x  edge ramp width per sample, screen pixels (> 0 enables the mode)
//   uv.y  coverage contrast exponent (> 1 tightens the edge against the
//         linear-light blend, which otherwise makes light-on-dark text soft)
//   uv.z  stem thickening, screen pixels (keeps thin strokes from fading)
//   uv.w  > 0: four samples spread over the pixel footprint (rotated grid),
//         for an atlas minified several times (no mips) without shimmer
layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 rect;
    vec4 uv;
    vec4 bend;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;  // per-glyph colour from the instance buffer
layout(location = 0) out vec4 outColor;

vec3 toLinear(vec3 srgb) {
    return pow(max(srgb, vec3(0.0)), vec3(2.2));
}

// FreeType SDF: spread = 8 font pixels, contour zero-crossing is 128 / 255.0 = 0.50196078.
// Range [-8, +8] font pixels is mapped across [0, 1].
float fontDist(vec2 uv) {
    return (texture(fontAtlas, uv).r - 0.50196078) * 16.0;
}

void main() {
    // Stable screen-space signed-distance field text rendering:
    // Screen-space derivatives of texture coordinates (vUV) provide a smooth,
    // noise-free measure of font texels per screen pixel across the glyph quad,
    // completely eliminating fragment-derivative jitter during head movement in VR.
    vec2 texSize = vec2(textureSize(fontAtlas, 0));
    vec2 duvx = dFdx(vUV);
    vec2 duvy = dFdy(vUV);
    vec2 ddx = duvx * texSize;
    vec2 ddy = duvy * texSize;
    // RMS font-texel footprint per screen pixel (rotation and perspective invariant)
    float fontPixelsPerScreenPixel = 0.70710678 * sqrt(dot(ddx, ddx) + dot(ddy, ddy));
    float invScale = 1.0 / max(fontPixelsPerScreenPixel, 1e-4);

    float distCentre = fontDist(vUV) * invScale;  // screen pixels
    float a;
    float distHalo;
    if (pc.uv.x > 0.0) {
        // Small text: a narrow linear ramp per sample, optionally averaged over
        // four rotated-grid samples inside the pixel, then a contrast curve.
        float w = pc.uv.x;
        float bias = pc.uv.z;
        if (pc.uv.w > 0.0) {
            const vec2 o0 = vec2(0.125, 0.375);
            const vec2 o1 = vec2(-0.375, 0.125);
            vec2 s0 = duvx * o0.x + duvy * o0.y;
            vec2 s1 = duvx * o1.x + duvy * o1.y;
            float c = clamp((fontDist(vUV + s0) * invScale + bias) / w + 0.5, 0.0, 1.0);
            c += clamp((fontDist(vUV - s0) * invScale + bias) / w + 0.5, 0.0, 1.0);
            c += clamp((fontDist(vUV + s1) * invScale + bias) / w + 0.5, 0.0, 1.0);
            c += clamp((fontDist(vUV - s1) * invScale + bias) / w + 0.5, 0.0, 1.0);
            a = 0.25 * c;
        } else {
            a = clamp((distCentre + bias) / w + 0.5, 0.0, 1.0);
        }
        a = pow(a, max(pc.uv.y, 0.1));
        distHalo = distCentre + bias;
    } else {
        // Convert distance to screen pixels, adding a subtle stem-darkening bias (+0.18 screen px)
        // to preserve thin strokes and legibility against high-contrast dark VR backgrounds.
        float distInScreenPixels = distCentre + 0.18;
        // Crisp Hermite anti-aliasing across 0.9 screen pixels (tighter edge transition for razor-sharp text in VR)
        a = smoothstep(-0.45, 0.45, distInScreenPixels);
        distHalo = distInScreenPixels;
    }
    // Soft dark halo ~1.5 px around each glyph keeps text legible over bright
    // video seen through translucent glass panels (invisible on dark panels).
    float halo = smoothstep(-1.9, -0.3, distHalo) * 0.45;
    float alpha = a + (1.0 - a) * halo;
    if (alpha <= 0.002) {
        discard;
    }

    // Halo is black, so it only contributes coverage; colour comes from the glyph.
    outColor = vec4(toLinear(vColor.rgb) * (a / alpha), vColor.a * alpha);
}
