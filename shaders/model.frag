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
    // The controller skin is near-black matte plastic; lift it so the shape and
    // the printed labels read clearly in the headset instead of a flat black.
    vec3 base = tex.rgb * 0.72 + 0.22;
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.3, 0.85, 0.4));
    float diff = max(dot(N, L), 0.0);
    float ambient = 0.6;
    float shade = ambient + (1.0 - ambient) * diff;
    outColor = vec4(base * pc.color.rgb * shade, tex.a * pc.color.a);
}
