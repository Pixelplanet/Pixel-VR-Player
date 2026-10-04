#version 450

// Full-screen triangle for 360/180 rendering. Reconstructs a per-pixel world-space
// view ray from the eye FOV tangents and camera orientation; the fragment shader
// maps that ray onto the equirectangular video.

layout(push_constant) uniform Push {
    mat4 viewRotation;  // world-from-view rotation (camera orientation)
    vec4 fov;           // tanLeft, tanRight, tanUp, tanDown
    vec4 mode;          // projection, stereo, eyeIndex, unused
    vec4 conversion;
    vec4 range;
} pc;

layout(location = 0) out vec3 vDir;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 ndc = uv * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);

    // Map NDC to view-space tangents (Vulkan clip Y is down: y=-1 is the top).
    float tx = mix(pc.fov.x, pc.fov.y, ndc.x * 0.5 + 0.5);
    float ty = mix(pc.fov.z, pc.fov.w, ndc.y * 0.5 + 0.5);
    vec3 viewDir = vec3(tx, ty, -1.0);
    vDir = mat3(pc.viewRotation) * viewDir;
}
