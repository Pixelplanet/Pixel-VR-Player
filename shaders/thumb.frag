#version 450

layout(binding = 0) uniform sampler2D thumbTex;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 rect;  // x, y (bottom-left), w, h in panel-local units
    vec4 uv;    // u0, v0, u1, v1
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 col = texture(thumbTex, vUV);

    // Exact metric rounded rectangle SDF matching panel styling with screen-space AA
    vec2 p = abs(vUV - 0.5) * pc.rect.zw;
    vec2 b = 0.5 * pc.rect.zw - vec2(0.018);
    vec2 q = max(p - b, 0.0);
    float dist = length(q) - 0.018;
    float aa = max(length(vec2(dFdx(dist), dFdy(dist))) * 1.15, 0.0025);
    float alpha = 1.0 - smoothstep(-aa, aa, dist);
    if (alpha <= 0.002) {
        discard;
    }

    // Thumbnail pixels are sRGB; convert to linear for the VK_FORMAT_*_SRGB framebuffer
    vec3 linRgb = pow(max(col.rgb, vec3(0.0)), vec3(2.2));
    outColor = vec4(linRgb * pc.color.rgb, col.a * pc.color.a * alpha);
}
