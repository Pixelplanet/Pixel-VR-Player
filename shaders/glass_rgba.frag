#version 450

// Frosted-glass backdrop (RGBA textures: the dma-buf video resolve, browser
// pages). Keep in sync with glass_nv12.frag.

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    vec4 ndc;   // clip position: vCoord * ndc.xy + ndc.zw (the panel rect, animated)
    vec4 win;   // layer pixel -> panel metres at rest: x0, y0, metres per pixel x, y
    vec4 hu;    // 3x4 rows: panel point (x, y, z, 1) -> the same point in the
    vec4 hv;    //   screen's local frame (metres, y up, +z toward the viewer)
    vec4 hw;
    vec4 misc;  // x: blur lod + 10 * stereo mode, y: alpha, z: corner radius, w: aspect
    vec4 tex;   // xy: crop UV scale (RGBA) or luma offset / scale (NV12), z: sRGB decode, w: panel bend radius
    vec4 scr;   // screen half width (arc), half height, bend radius (0 = flat), edge fade (metres)
    vec4 eye;   // xyz: the head in screen-local metres, w: letterbox share cut at the top and bottom each
} pc;

layout(location = 0) in vec2 vCoord;
layout(location = 0) out vec4 outColor;

vec3 sampleVideo(vec2 uv, float lod) {
    vec3 rgb = textureLod(video, uv * pc.tex.xy, lod).rgb;
    if (pc.tex.z > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    return rgb;
}

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Screen texture coordinates behind this layer pixel; inside = how much of
// the screen shows there. The backdrop runs pc.scr.w metres past the outline
// (the picture's edge repeated) and fades there: it is projected from between
// the eyes, and each eye sees the sharp screen layer's edge that far off. Returns false where the line of
// sight misses the screen surface.
bool screenUv(out vec2 uv, out float inside) {
    // The layer pixel on the panel as SteamVR places it (bent onto the
    // cylinder for a curved panel, whose axis is `R` in front of it).
    vec2 pf = pc.win.xy + gl_FragCoord.xy * pc.win.zw;
    vec3 p = vec3(pf, 0.0);
    float R = pc.tex.w;
    if (R > 0.0) {
        float a = pf.x / R;
        p = vec3(R * sin(a), pf.y, R * (1.0 - cos(a)));
    }
    vec4 p4 = vec4(p, 1.0);
    vec3 s = vec3(dot(pc.hu, p4), dot(pc.hv, p4), dot(pc.hw, p4));
    vec3 e = pc.eye.xyz;
    vec3 d = s - e;
    float Rs = pc.scr.z;
    vec3 q;
    float sx;
    if (Rs > 0.0) {
        // Curved screen: the cylinder x^2 + (z - Rs)^2 = Rs^2 (axis Rs in
        // front of the centre, like quad.vert); the far hit from the viewer.
        float a = d.x * d.x + d.z * d.z;
        float b = e.x * d.x + (e.z - Rs) * d.z;
        float c = e.x * e.x + (e.z - Rs) * (e.z - Rs) - Rs * Rs;
        float disc = b * b - a * c;
        if (a < 1e-8 || disc < 0.0) return false;
        float t = (-b + sqrt(disc)) / a;
        if (t <= 0.0) return false;
        q = e + t * d;
        sx = Rs * atan(q.x, Rs - q.z);
    } else {
        if (abs(d.z) < 1e-6) return false;
        float t = -e.z / d.z;
        if (t <= 0.0) return false;
        q = e + t * d;
        sx = q.x;
    }
    uv = vec2(sx / (2.0 * pc.scr.x) + 0.5, -q.y / (2.0 * pc.scr.y) + 0.5);
    vec2 fade = vec2(max(pc.scr.w, 1e-4)) / (2.0 * pc.scr.xy);
    // Full strength up to `fade` OUTSIDE the outline, then a short fade: the eye that sees the screen's
    // edge further out must not get a strip of the sharp picture beside the blur.
    vec2 o = max(-uv, uv - 1.0);  // how far outside the outline (negative = inside)
    vec2 k = 1.0 - smoothstep(fade, 1.35 * fade, o);
    inside = k.x * k.y;
    return true;
}

void main() {
    // Same rounded-box mask as the glass panel drawn on top (ui.frag shape 25),
    // so the blurred picture never peeks past the panel's corners.
    float aspect = max(pc.misc.w, 1e-4);
    float unitScale = min(aspect, 1.0);
    vec2 pBox = vec2(vCoord.x * aspect, vCoord.y) / unitScale;
    vec2 boxHalf = vec2(aspect, 1.0) / unitScale;
    float d = sdRoundedBox(pBox, boxHalf, clamp(pc.misc.z, 0.0, 1.0));
    float aa = max(length(vec2(dFdx(d), dFdy(d))) * 0.75, 0.0005);
    float mask = 1.0 - smoothstep(-aa, aa, d);
    if (mask <= 0.002) {
        discard;
    }

    vec2 uv;
    float inside;
    if (!screenUv(uv, inside) || inside <= 0.002) {
        discard;
    }

    float lod = mod(pc.misc.x, 10.0);
    int stereo = int(pc.misc.x / 10.0 + 0.5);
    vec2 suv = clamp(uv, 0.0, 1.0);
    suv.y = mix(pc.eye.w, 1.0 - pc.eye.w, suv.y);  // without the black bars (per eye view, so before the halving)
    if (stereo == 2) suv.y *= 0.5;        // top/bottom: left view
    else if (stereo == 1) suv.x *= 0.5;   // side-by-side: left view
    vec3 rgb = sampleVideo(suv, lod);
    // Frosted glass dims and desaturates what shows through.
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(lum), rgb, 0.80) * 0.85;
    // Premultiplied over the cleared layer (see the UI blend state).
    outColor = vec4(rgb, pc.misc.y * mask * inside);
}
