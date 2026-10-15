#version 450

// Textured controller model: base-color texture (which carries the printed
// button labels and part colors) modulated by simple hemispheric diffuse light.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 quat;
    vec4 color;
} pc;

layout(set = 0, binding = 0) uniform sampler2D baseTex;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(baseTex, vUV);
    vec3 N = normalize(vNormal);
    // Lift the near-black matte skin so the form reads in the headset.
    vec3 base = tex.rgb * 0.75 + 0.22;
    // Hemisphere ambient (brighter from above) reveals the black plastic shape.
    float up = N.y * 0.5 + 0.5;
    vec3 hemi = mix(vec3(0.22, 0.22, 0.27), vec3(0.80, 0.84, 0.92), up);
    float key = max(dot(N, normalize(vec3(0.35, 0.80, 0.45))), 0.0);
    float fill = max(dot(N, normalize(vec3(-0.5, 0.1, -0.6))), 0.0);
    vec3 lit = hemi * 0.55 + key * 0.65 + fill * 0.25;
    outColor = vec4(base * pc.color.rgb * lit, tex.a * pc.color.a);
}
