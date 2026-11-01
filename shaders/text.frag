#version 450

layout(binding = 0) uniform sampler2D fontAtlas;

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vColor;  // per-glyph colour from the instance buffer
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

    // Convert distance to screen pixels, adding a subtle stem-darkening bias (+0.18 screen px)
    // to preserve thin strokes and legibility against high-contrast dark VR backgrounds.
    float distInScreenPixels = (distInFontPixels / max(fontPixelsPerScreenPixel, 1e-4)) + 0.18;

    // Crisp Hermite anti-aliasing across 0.9 screen pixels (tighter edge transition for razor-sharp text in VR)
    float a = smoothstep(-0.45, 0.45, distInScreenPixels);
    // Soft dark halo ~1.5 px around each glyph keeps text legible over bright
    // video seen through translucent glass panels (invisible on dark panels).
    float halo = smoothstep(-1.9, -0.3, distInScreenPixels) * 0.45;
    float alpha = a + (1.0 - a) * halo;
    if (alpha <= 0.002) {
        discard;
    }

    // Halo is black, so it only contributes coverage; colour comes from the glyph.
    outColor = vec4(toLinear(vColor.rgb) * (a / alpha), vColor.a * alpha);
}
