#version 450

// Visibility mask (XR_KHR_visibility_mask hidden triangle mesh): the vertices
// lie on the z = -1 plane of the view (x, y = tangents), projected with the
// view's FOV exactly like perspectiveVulkan (Vulkan clip Y points down). The
// viewport is the eye pass's adaptive sub-rect, which maps the whole FOV, so
// the mask lands on the same pixels as the scene. Depth 0 = the near plane:
// later draws with a depth test (LESS) are rejected there.

layout(push_constant) uniform Push {
    vec4 fov;  // tanLeft, tanRight, tanUp, tanDown
} pc;

layout(location = 0) in vec2 inPos;

void main() {
    float x = (2.0 * inPos.x - (pc.fov.y + pc.fov.x)) / (pc.fov.y - pc.fov.x);
    float y = (2.0 * inPos.y - (pc.fov.z + pc.fov.w)) / (pc.fov.w - pc.fov.z);
    gl_Position = vec4(x, y, 0.0, 1.0);
}
