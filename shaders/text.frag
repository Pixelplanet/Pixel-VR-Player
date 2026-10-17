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
    // Signed-distance text: use Euclidean screen-space gradient with a 0.9px
    // half-width (~1.8px total ramp) and a slight bold bias (0.465) so thin
    // stems never shimmer or drop out across head micro-movements in VR.
    float d = texture(fontAtlas, vUV).r;
    float w = max(length(vec2(dFdx(d), dFdy(d))) * 0.92, 0.025);
    float a = smoothstep(0.465 - w, 0.465 + w, d);
    if (a <= 0.002) {
        discard;
    }
    outColor = vec4(toLinear(pc.color.rgb), pc.color.a * a);
}
