#version 450

// Textured controller model with glossy plastic shading: the base-color texture
// (part colors + printed labels) lit by hemisphere ambient + a key light, with
// Blinn-Phong specular and a fresnel rim so the matte-black shell reads as 3D.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 quat;
    vec4 color;
    vec4 eye;
    vec4 origin;
} pc;

layout(set = 0, binding = 0) uniform sampler2D baseTex;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vView;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(baseTex, vUV);
    vec3 N = normalize(vNormal);
    vec3 V = normalize(vView);
    vec3 L = normalize(vec3(0.3, 0.9, 0.45));
    // The controller is matte black plastic: keep the real skin colour and shade
    // with plain diffuse only (no specular/rim) so there is no shimmer, while the
    // printed button labels still read.
    float diff = max(dot(N, L), 0.0);
    float amb = 0.03 + 0.03 * (N.y * 0.5 + 0.5);
    outColor = vec4(tex.rgb * (amb + 0.30 * diff) * pc.color.rgb,
                    tex.a * pc.color.a);
}
