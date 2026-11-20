#version 450

// Samples an equirectangular video from a per-pixel view ray. Supports full 360
// and front-hemisphere 180, with mono / side-by-side / over-under stereo layouts
// selected per eye.
//
// Minification (a screen pixel covering more than one video texel: zoomed out,
// 8K at zoom 1, near the poles) is filtered over the pixel's footprint, which
// comes from the screen derivatives of lon / lat (wrap-free, unlike those of the
// wrapped u, and taken before the 180 early-out):
// - with a mip chain (the CPU upload builds one): one textureGrad tap with those
//   derivatives, so the hardware's trilinear / anisotropic filter does it;
// - without mips (as video_sphere.frag): one bilinear tap up to 1 texel, above
//   that 4 (rotated grid) or 8 taps spread over the footprint parallelogram,
//   averaged in the output's linear light. Every tap stays inside this eye's
//   half (and the 180 picture), a texel off its edge.

layout(binding = 0) uniform sampler2D luma;
layout(binding = 1) uniform sampler2D chroma;

layout(push_constant) uniform Push {
    mat4 viewRotation;
    vec4 fov;
    vec4 mode;  // x: projection (1=360, 2=180), y: stereo (0=mono,1=SBS,2=TB), z: eye
    vec4 conversion;
    vec4 range;  // x: luma offset, y: luma scale, z: sRGB encode flag, w: Focus shift (grip + stick)
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
vec2 gLo;      // the eye's half
vec2 gSize;    // its size (0.5 along the stereo split)
vec2 gMargin;  // 1 luma texel = half a chroma texel: a tap stays inside the half

// The 360 wrap / 180 side edge, the poles and the stereo half. The margin keeps
// every tap off the other eye's half.
vec2 toTexture(vec2 p) {
    if (gIs180) {
        p.x = clamp(p.x, 0.0, 1.0);  // a tap past the side edge reads the edge
    } else if (p.x < 0.0 || p.x > 1.0) {
        p.x = fract(p.x);  // shifted past the back: wrap
    }
    p.y = clamp(p.y, 0.0, 1.0);  // a tap past a pole
    vec2 st = gLo + p * gSize;
    return clamp(st, gLo + gMargin, gLo + gSize - gMargin);
}

vec3 toRgb(float y, vec2 c) {
    float yv = (y - pc.range.x) * pc.range.y;
    vec2 cc = c - vec2(128.0 / 255.0);
    return clamp(vec3(yv + pc.conversion.x * cc.y,
                      yv + pc.conversion.y * cc.x + pc.conversion.z * cc.y,
                      yv + pc.conversion.w * cc.x), 0.0, 1.0);
}

// sRGB to linear light (the "encode" flag: the target encodes it again).
vec3 toLinear(vec3 c) {
    return mix(c / 12.92, pow(max((c + 0.055) / 1.055, vec3(0.0)), vec3(2.4)),
               step(vec3(0.04045), c));
}

vec3 outSpace(vec3 rgb) {
    return (pc.range.z > 0.5) ? toLinear(rgb) : rgb;
}

vec3 sampleRgb(vec2 p) {
    vec2 st = toTexture(p);
    return outSpace(toRgb(textureLod(luma, st, 0.0).r, textureLod(chroma, st, 0.0).rg));
}

void main() {
    vec3 d = normalize(vDir);
    float projection = pc.mode.x;
    float stereo = pc.mode.y;
    float eye = pc.mode.z;
    if (pc.mode.w > 0.5) { eye = 1.0 - eye; }  // swap left/right eye

    float lon = atan(d.x, -d.z);              // 0 = forward (-z)
    float lat = asin(clamp(d.y, -1.0, 1.0));

    // The pixel's angular footprint, while the whole 2x2 quad is still running
    // (before the 180 early-out). lon jumps by 2 pi behind the viewer: a quad
    // across it takes the short way round.
    vec2 dLon = vec2(dFdx(lon), dFdy(lon));
    dLon -= TWO_PI * round(dLon * (1.0 / TWO_PI));
    vec2 dLat = vec2(dFdx(lat), dFdy(lat));

    // Focus (grip + stick): the two eye images slide against each other, each by half
    // of range.w (radians; positive = the picture farther), by the displayed
    // eye (before the swap).
    lon += (pc.mode.z < 0.5 ? 0.5 : -0.5) * pc.range.w;

    gIs180 = projection > 1.5;
    float span = gIs180 ? PI : TWO_PI;
    vec2 p = vec2(lon / span + 0.5, 0.5 - lat / PI);
    // 180: front hemisphere only (behind the viewer |lon| > pi/2, so the
    // range check covers it, and the shifted edge too).
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
    vec2 texSize = max(vec2(textureSize(luma, 0)), vec2(64.0));
    gMargin = 1.0 / texSize;

    // One screen pixel step right / down, in picture space.
    vec2 px = vec2(dLon.x / span, -dLat.x / PI);
    vec2 py = vec2(dLon.y / span, -dLat.y / PI);

    vec3 rgb;
    if (textureQueryLevels(luma) > 1) {
        // Mipmapped: the hardware filter, fed the wrap-free derivatives.
        vec2 st = toTexture(p);
        vec2 gx = px * gSize;
        vec2 gy = py * gSize;
        rgb = outSpace(toRgb(textureGrad(luma, st, gx, gy).r, textureGrad(chroma, st, gx, gy).rg));
    } else {
        vec2 texels = texSize * gSize;
        float lx = length(px * texels);
        float ly = length(py * texels);
        float f = max(lx, ly);
        if (!(f > 1.0)) {  // (also NaN at the exact pole)
            rgb = sampleRgb(p);
        } else {
            // Spread the taps over the part of the footprint the bilinear tap
            // does not already cover (a footprint F wide needs sqrt(F^2 - 1)
            // more), per axis: a footprint long in u only (near a pole) spreads
            // in u only.
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
    }
    outColor = vec4(rgb, 1.0);
}
