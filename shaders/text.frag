#version 450

layout(binding = 0) uniform sampler2D fontAtlas;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 rect;
    vec4 uv;
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

vec3 toLinear(vec3 srgb) {
    return pow(max(srgb, vec3(0.0)), vec3(2.2));
}

void main() {
    // Stable screen-space signed-distance field text rendering:
    // Screen-space derivatives of texture coordinates (vUV) provide a smooth,
    // noise-free measure of font texels per screen pixel across the glyph quad,
    // completely eliminating fragment-derivative jitter during head movement in VR.
    vec2 texSize = vec2(textureSize(fontAtlas, 0));
    vec2 ddx = dFdx(vUV) * texSize;
    vec2 ddy = dFdy(vUV) * texSize;
    // RMS font-texel footprint per screen pixel (rotation and perspective invariant)
    float fontPixelsPerScreenPixel = 0.70710678 * sqrt(dot(ddx, ddx) + dot(ddy, ddy));

    float d = texture(fontAtlas, vUV).r;
    // FreeType SDF: spread = 8 font pixels, contour zero-crossing is 128 / 255.0 = 0.50196078.
    // Range [-8, +8] font pixels is mapped across [0, 1].
    float distInFontPixels = (d - 0.50196078) * 16.0;

    // Convert distance to screen pixels, adding a subtle stem-darkening bias (+0.14 screen px)
    // to preserve thin strokes and legibility against high-contrast dark VR backgrounds.
    float distInScreenPixels = (distInFontPixels / max(fontPixelsPerScreenPixel, 1e-4)) + 0.14;

    // Smooth Hermite anti-aliasing across 1.4 screen pixels (smooth transition matches VR subpixel PSF)
    float a = smoothstep(-0.70, 0.70, distInScreenPixels);
    if (a <= 0.002) {
        discard;
    }

    outColor = vec4(toLinear(pc.color.rgb), pc.color.a * a);
}
