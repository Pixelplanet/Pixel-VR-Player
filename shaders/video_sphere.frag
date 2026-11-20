#version 450

// Zero-copy dma-buf video, equirectangular 360/180. Same ray mapping and stereo
// layout as sphere.frag, but the combined image sampler carries a
// VkSamplerYcbcrConversion so a single texture fetch already returns R'G'B'.
// (The resolved RGBA copy is drawn with this shader too; for 360/180 neither has
// mip levels.)
//
// Footprint supersampling: with no mips, one bilinear tap aliases once a screen
// pixel covers more than about one video texel (zoomed out, 8K at zoom 1, near
// the poles): small bright highlights sparkle. The pixel's footprint in texels
// comes from the screen derivatives of lon / lat; up to 1 texel it stays one tap,
// above that 4 (rotated grid) or 8 taps spread over the footprint parallelogram,
// averaged in the output's linear light. See sphere.frag for the shared rules.

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    mat4 viewRotation;
    vec4 fov;
    vec4 mode;  // x: projection (1=360, 2=180), y: stereo (0/1/2), z: eye
    vec4 conversion;  // xy: crop UV scale (visible / coded); conversion by the sampler
    vec4 range;       // z: sRGB encode flag, w: 3D Autofocus shift
} pc;

layout(location = 0) in vec3 vDir;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;

// Sample positions inside the pixel footprint, in pixel steps: the D3D 4x
// (rotated grid) and 8x MSAA patterns.
const vec2 kGrid4[4] = vec2[](vec2(-0.125, -0.375), vec2(0.375, -0.125),
                              vec2(-0.375, 0.125), vec2(0.125, 0.375));
const vec2 kGrid8[8] = vec2[](vec2(0.0625, -0.1875), vec2(-0.0625, 0.1875),
                              vec2(0.3125, 0.0625), vec2(-0.1875, -0.3125),
                              vec2(-0.3125, 0.3125), vec2(-0.4375, -0.0625),
                              vec2(0.1875, 0.4375), vec2(0.4375, -0.4375));

// How one eye's picture (p in [0,1]^2 = its lon / lat range) sits in the texture.
bool gIs180;
vec2 gLo;      // the eye's half, texture uv before the crop
vec2 gSize;    // its size (0.5 along the stereo split)
vec2 gCrop;    // visible / coded
vec2 gMargin;  // 1 texel: a bilinear tap (and its half-res chroma) stays inside the half

// The 360 wrap / 180 side edge, the poles, the stereo half and the crop. The
// margin keeps every tap off the other eye's half and off the decoder padding.
vec2 toTexture(vec2 p) {
    if (gIs180) {
        p.x = clamp(p.x, 0.0, 1.0);  // a tap past the side edge reads the edge
    } else if (p.x < 0.0 || p.x > 1.0) {
        p.x = fract(p.x);  // shifted past the back: wrap
    }
    p.y = clamp(p.y, 0.0, 1.0);  // a tap past a pole
    vec2 st = (gLo + p * gSize) * gCrop;
    return clamp(st, gLo * gCrop + gMargin, (gLo + gSize) * gCrop - gMargin);
}

// sRGB to linear light (the "encode" flag: the target encodes it again).
vec3 toLinear(vec3 c) {
    return mix(c / 12.92, pow(max((c + 0.055) / 1.055, vec3(0.0)), vec3(2.4)),
               step(vec3(0.04045), c));
}

vec3 sampleRgb(vec2 p) {
    vec3 rgb = textureLod(video, toTexture(p), 0.0).rgb;
    return (pc.range.z > 0.5) ? toLinear(rgb) : rgb;
}

void main() {
    vec3 d = normalize(vDir);
    float projection = pc.mode.x;
    float stereo = pc.mode.y;
    float eye = pc.mode.z;
    if (pc.mode.w > 0.5) { eye = 1.0 - eye; }  // swap left/right eye

    float lon = atan(d.x, -d.z);
    float lat = asin(clamp(d.y, -1.0, 1.0));

    // The pixel's angular footprint, while the whole 2x2 quad is still running
    // (before the 180 early-out). lon jumps by 2 pi behind the viewer: a quad
    // across it takes the short way round.
    vec2 dLon = vec2(dFdx(lon), dFdy(lon));
    dLon -= TWO_PI * round(dLon * (1.0 / TWO_PI));
    vec2 dLat = vec2(dFdx(lat), dFdy(lat));

    // 3D Autofocus: the two eye images slide against each other, each by half
    // of range.w (radians; positive = the picture farther), by the displayed
    // eye (before the swap).
    lon += (pc.mode.z < 0.5 ? 0.5 : -0.5) * pc.range.w;

    gIs180 = projection > 1.5;
    float span = gIs180 ? PI : TWO_PI;
    vec2 p = vec2(lon / span + 0.5, 0.5 - lat / PI);
    // 180, behind the viewer |lon| > pi/2: the range check covers it.
    if (gIs180 && (p.x < 0.0 || p.x > 1.0)) { outColor = vec4(0.0, 0.0, 0.0, 1.0); return; }

    gLo = vec2(0.0);
    gSize = vec2(1.0);
    if (stereo > 1.5) {
        gLo.y = eye * 0.5;  // over-under: left eye = top half
        gSize.y = 0.5;
    } else if (stereo > 0.5) {
        gLo.x = eye * 0.5;  // side-by-side: left eye = left half
        gSize.x = 0.5;
    }
    gCrop = pc.conversion.xy;
    vec2 texSize = max(vec2(textureSize(video, 0)), vec2(64.0));  // coded size (luma plane)
    gMargin = 1.0 / texSize;

    // One screen pixel step right / down, in picture space and in texels.
    vec2 px = vec2(dLon.x / span, -dLat.x / PI);
    vec2 py = vec2(dLon.y / span, -dLat.y / PI);
    vec2 texels = texSize * gCrop * gSize;
    float lx = length(px * texels);
    float ly = length(py * texels);
    float f = max(lx, ly);

    vec3 rgb;
    if (!(f > 1.0)) {  // (also NaN at the exact pole)
        rgb = sampleRgb(p);
    } else {
        // Spread the taps over the part of the footprint the bilinear tap does
        // not already cover (a footprint F wide needs sqrt(F^2 - 1) more), per
        // axis: a footprint long in u only (near a pole) spreads in u only.
        vec2 ax = px * sqrt(1.0 - 1.0 / max(lx * lx, 1.0));
        vec2 ay = py * sqrt(1.0 - 1.0 / max(ly * ly, 1.0));
        rgb = vec3(0.0);
        if (f <= 2.5) {
            for (int i = 0; i < 4; ++i) rgb += sampleRgb(p + ax * kGrid4[i].x + ay * kGrid4[i].y);
            rgb *= 0.25;
        } else {
            for (int i = 0; i < 8; ++i) rgb += sampleRgb(p + ax * kGrid8[i].x + ay * kGrid8[i].y);
            rgb *= 0.125;
        }
    }
    outColor = vec4(rgb, 1.0);
}
