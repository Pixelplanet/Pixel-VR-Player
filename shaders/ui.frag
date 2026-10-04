#version 450

// Procedural vector/SDF UI fragment shader.
// Supports rounded boxes with antialiased borders, and crisp vector icons.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 params;       // x: shapeType, y: radius, z: borderWidth, w: extra
    vec4 borderColor;
} pc;

layout(location = 0) in vec2 vCoord;
layout(location = 0) out vec4 outColor;

// SDF for rounded box
float sdRoundedBox(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Inigo Quilez 2D triangle SDF
float sdTriangle(vec2 p, vec2 p0, vec2 p1, vec2 p2) {
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 pq0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 pq1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 pq2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// SDF line segment
float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

void main() {
    int shape = int(pc.params.x + 0.5);
    vec2 p = vCoord;
    float d = 0.0;
    bool hasBorder = false;

    if (shape == 0) {
        // Shape 0: Rounded rectangle (e.g. button body, panel, progress bar)
        float r = clamp(pc.params.y, 0.0, 1.0);
        d = sdRoundedBox(p, vec2(1.0), r);
        hasBorder = (pc.params.z > 0.0);
    } else if (shape == 1) {
        // Shape 1: Play icon (triangle pointing right)
        vec2 pPlay = p - vec2(0.04, 0.0);
        d = sdTriangle(pPlay, vec2(-0.45, 0.55), vec2(0.55, 0.0), vec2(-0.45, -0.55)) - 0.06;
    } else if (shape == 2) {
        // Shape 2: Pause icon (two vertical bars)
        vec2 pBar = vec2(abs(p.x) - 0.30, p.y);
        d = sdRoundedBox(pBar, vec2(0.12, 0.52), 0.04);
    } else if (shape == 3) {
        // Shape 3: Rewind (<<) (two left-pointing triangles)
        vec2 tri0 = vec2(0.10, 0.45);
        vec2 tri1 = vec2(-0.38, 0.0);
        vec2 tri2 = vec2(0.10, -0.45);
        float d1 = sdTriangle(p - vec2(-0.06, 0.0), tri0, tri1, tri2) - 0.04;
        float d2 = sdTriangle(p - vec2(0.42, 0.0), tri0, tri1, tri2) - 0.04;
        d = min(d1, d2);
    } else if (shape == 4) {
        // Shape 4: Fast Forward (>>) (two right-pointing triangles)
        vec2 tri0 = vec2(-0.10, 0.45);
        vec2 tri1 = vec2(0.38, 0.0);
        vec2 tri2 = vec2(-0.10, -0.45);
        float d1 = sdTriangle(p - vec2(0.06, 0.0), tri0, tri1, tri2) - 0.04;
        float d2 = sdTriangle(p - vec2(-0.42, 0.0), tri0, tri1, tri2) - 0.04;
        d = min(d1, d2);
    } else if (shape == 5) {
        // Shape 5: Files / Folder icon
        float body = sdRoundedBox(p - vec2(0.0, -0.10), vec2(0.65, 0.45), 0.08);
        float tab = sdRoundedBox(p - vec2(-0.30, 0.40), vec2(0.32, 0.16), 0.06);
        d = min(body, tab);
    } else if (shape == 6) {
        // Shape 6: Settings Gear icon
        float r = length(p);
        float theta = atan(p.y, p.x);
        float teeth = sin(6.0 * theta);
        float gearOuter = r - (0.48 + 0.14 * smoothstep(-0.4, 0.4, teeth));
        float gearHole = 0.20 - r;
        d = max(gearOuter, gearHole);
    } else if (shape == 7) {
        // Shape 7: Recenter / Target icon
        float r = length(p);
        float ring = abs(r - 0.52) - 0.06;
        float dotCenter = r - 0.16;
        float tickH = sdRoundedBox(vec2(abs(p.x) - 0.62, p.y), vec2(0.10, 0.03), 0.01);
        float tickV = sdRoundedBox(vec2(p.x, abs(p.y) - 0.62), vec2(0.03, 0.10), 0.01);
        d = min(min(ring, dotCenter), min(tickH, tickV));
    } else if (shape == 8) {
        // Shape 8: Reticle Cursor (outer ring + center dot)
        float r = length(p);
        float outerRing = abs(r - 0.70) - 0.08;
        float centerDot = r - 0.22;
        d = min(outerRing, centerDot);
    } else if (shape == 9) {
        // Shape 9: Close / X icon
        float s1 = sdSegment(p, vec2(-0.45, -0.45), vec2(0.45, 0.45)) - 0.10;
        float s2 = sdSegment(p, vec2(-0.45, 0.45), vec2(0.45, -0.45)) - 0.10;
        d = min(s1, s2);
    } else if (shape == 10) {
        // Shape 10: Checkmark (✓)
        float s1 = sdSegment(p, vec2(-0.45, -0.05), vec2(-0.12, -0.45)) - 0.09;
        float s2 = sdSegment(p, vec2(-0.12, -0.45), vec2(0.48, 0.45)) - 0.09;
        d = min(s1, s2);
    } else if (shape == 11) {
        // Shape 11: Stop icon (filled rounded square)
        d = sdRoundedBox(p, vec2(0.46), 0.10);
    } else {
        d = sdRoundedBox(p, vec2(1.0), 0.0);
    }

    float aa = fwidth(d);
    if (aa < 1e-4) aa = 0.015;
    float fillAlpha = 1.0 - smoothstep(-aa * 0.5, aa * 0.5, d);

    if (fillAlpha <= 0.0) {
        discard;
    }

    vec4 finalColor = pc.color;
    if (hasBorder) {
        float borderWidth = pc.params.z;
        float innerD = d + borderWidth;
        float borderAlpha = 1.0 - smoothstep(-aa * 0.5, aa * 0.5, innerD);
        finalColor = mix(pc.borderColor, pc.color, borderAlpha);
    }

    finalColor.a *= fillAlpha;
    outColor = finalColor;
}
