#version 450

// Lit controller model: position + normal vertex input, drawn at the controller
// pose. Normals are rotated to world space by the controller orientation quat so
// a single fixed light direction shades the convex body (back faces are culled,
// so no depth buffer is needed).

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 quat;   // controller orientation (x,y,z,w)
    vec4 color;
} pc;

layout(location = 0) out vec3 vNormal;

vec3 rotate_quat(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    vNormal = rotate_quat(pc.quat, inNormal);
}
