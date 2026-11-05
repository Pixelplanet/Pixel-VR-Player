#version 450

// Frosted-glass backdrop (zero-copy dma-buf path): samples the mipmapped RGBA
// resolve of the video. Keep in sync with glass_nv12.frag.

layout(binding = 0) uniform sampler2D video;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 h0;    // homography rows: (x, y, 1) in quad coords -> (u*w, v*w, w)
    vec4 h1;    //   h0.w / h1.w: crop UV scale, h2.w: sRGB decode flag
    vec4 h2;
    vec4 misc;  // x: blur lod + 10 * stereo mode, y: alpha, z: corner radius, w: aspect
} pc;

layout(location = 0) in vec2 vCoord;
layout(location = 0) out vec4 outColor;

float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

vec3 sampleVideo(vec2 uv, float lod) {
    vec3 rgb = textureLod(video, uv * vec2(pc.h0.w, pc.h1.w), lod).rgb;
    if (pc.h2.w > 0.5) {
        rgb = mix(rgb / 12.92, pow((rgb + 0.055) / 1.055, vec3(2.4)),
                  step(vec3(0.04045), rgb));
    }
    return rgb;
}

void main() {
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

    vec3 q = vec3(vCoord, 1.0);
    vec3 h = vec3(dot(pc.h0.xyz, q), dot(pc.h1.xyz, q), dot(pc.h2.xyz, q));
    if (h.z <= 1e-5) {
        discard;
    }
    vec2 uv = h.xy / h.z;
    vec2 edge = smoothstep(vec2(0.0), vec2(0.04), uv) * smoothstep(vec2(0.0), vec2(0.04), 1.0 - uv);
    float inside = edge.x * edge.y;
    if (inside <= 0.002) {
        discard;
    }

    float lod = mod(pc.misc.x, 10.0);
    int stereo = int(pc.misc.x / 10.0 + 0.5);
    vec2 suv = clamp(uv, 0.0, 1.0);
    if (stereo == 2) suv.y *= 0.5;
    else if (stereo == 1) suv.x *= 0.5;
    vec3 rgb = sampleVideo(suv, lod);
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = mix(vec3(lum), rgb, 0.80) * 0.85;
    outColor = vec4(rgb, pc.misc.y * mask * inside);
}
