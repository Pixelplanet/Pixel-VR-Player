#version 450

// Simple hemispheric diffuse shading for the controller body.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 quat;
    vec4 color;
} pc;

layout(location = 0) in vec3 vNormal;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(vec3(0.3, 0.85, 0.4));
    float diff = max(dot(N, L), 0.0);
    float ambient = 0.35;
    float shade = ambient + (1.0 - ambient) * diff;
    outColor = vec4(pc.color.rgb * shade, pc.color.a);
}
