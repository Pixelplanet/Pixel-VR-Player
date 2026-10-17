#version 450

// Textured video screen: a 16:9 screen with parametric curvature (Flat -> Curved).
// 64 horizontal segments (384 vertices) are procedurally evaluated from gl_VertexIndex.
// pc.stereo.w carries the curvature factor in [0.0, 1.0] (0 = flat, >0 = curved arc).

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 conversion;
    vec4 range;
    vec4 stereo; // x: stereoMode, y: viewIndex, z: swapEyes, w: curvature
} pc;

layout(location = 0) out vec2 vUV;

void main() {
    const int kSegments = 64;
    int seg = gl_VertexIndex / 6;
    int vid = gl_VertexIndex % 6;

    float u0 = float(seg) / float(kSegments);
    float u1 = float(seg + 1) / float(kSegments);

    // Quad vertex coordinates for this segment
    float u = u0;
    float y = 0.9;
    if (vid == 1) { u = u0; y = -0.9; }
    else if (vid == 2) { u = u1; y = -0.9; }
    else if (vid == 3) { u = u0; y =  0.9; }
    else if (vid == 4) { u = u1; y = -0.9; }
    else if (vid == 5) { u = u1; y =  0.9; }

    vUV = vec2(u, (0.9 - y) / 1.8);

    // Nominal screen width = 3.2 (-1.6 to +1.6)
    const float kWidth = 3.2;
    float curv = clamp(pc.stereo.w, 0.0, 1.0);

    float x = (u - 0.5) * kWidth;
    float z = 0.0;
    if (curv > 0.005) {
        // Pure cylindrical arc of constant arc length kWidth.
        // At curv=1.0, total wrap angle is ~100 degrees (1.75 rad) around the viewer.
        float totalAngle = curv * 1.75;
        float radius = kWidth / totalAngle;
        float theta = (u - 0.5) * totalAngle;
        x = radius * sin(theta);
        z = radius * (1.0 - cos(theta));
    }

    gl_Position = pc.mvp * vec4(x, y, z, 1.0);
}

