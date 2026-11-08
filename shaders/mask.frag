#version 450

// The hidden lens area stays black.
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(0.0, 0.0, 0.0, 1.0);
}
