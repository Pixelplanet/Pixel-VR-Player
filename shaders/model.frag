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
    // Preserve the real near-black skin so the printed button labels stay bright
    // and read as highlights on the black plastic.
    vec3 base = tex.rgb * 0.95 + 0.015;
    float diff = max(dot(N, L), 0.0);
    float up = N.y * 0.5 + 0.5;
    vec3 hemi = mix(vec3(0.12, 0.12, 0.16), vec3(0.55, 0.58, 0.66), up);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 40.0) * 0.9;
    float fres = pow(1.0 - max(dot(N, V), 0.0), 3.0) * 0.7;
    vec3 col = base * (hemi + diff * 0.7) + vec3(spec) +
               vec3(0.5, 0.6, 0.8) * fres;
    outColor = vec4(col * pc.color.rgb, tex.a * pc.color.a);
}
