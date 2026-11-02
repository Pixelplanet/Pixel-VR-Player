#version 450

// Procedural vector/SDF UI fragment shader.
// Supports rounded boxes with antialiased borders, and crisp vector icons.

layout(push_constant) uniform Push {
    mat4 mvp;
    vec4 color;
    vec4 params;       // x: shapeType, y: radius, z: borderWidth, w: bend radius (vertex stage)
    vec4 borderColor;
    vec4 rect;         // centre x, y and half-size w, h (zero size = unit quad)
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

    // Boxes are evaluated in an aspect-correct space measured in units of the
    // shorter half-extent, so corner radii and border widths stay uniform on
    // wide pills and tall bars alike (radius 1.0 = full capsule / circle).
    float aspect = (pc.rect.w != 0.0) ? max(abs(pc.rect.z / pc.rect.w), 1e-4) : 1.0;
    float unitScale = min(aspect, 1.0);
    vec2 pBox = vec2(p.x * aspect, p.y) / unitScale;
    vec2 boxHalf = vec2(aspect, 1.0) / unitScale;

    if (shape == 20) {
        // Shape 20: Soft glow / shadow halo. The quad is the element grown by
        // `spread` (params.z, in short-half-extent units); alpha falls off
        // smoothly from the element edge to the quad edge.
        // A negative radius makes the halo hollow (outside the element only), so
        // drop shadows don't darken what shows through translucent panels.
        bool hollow = pc.params.y < 0.0;
        float spread = max(pc.params.z, 1e-3);
        vec2 inner = max(boxHalf - vec2(spread), vec2(1e-3));
        float r = clamp(abs(pc.params.y), 0.0, 1.0) * min(inner.x, inner.y);
        float dg = sdRoundedBox(pBox, inner, r);
        if (hollow && dg <= 0.0) {
            discard;
        }
        float t = clamp(dg / spread, 0.0, 1.0);
        float a = (1.0 - t) * (1.0 - t) * (1.0 - t);  // cubic: tight halo, fast fade
        if (a <= 0.002) {
            discard;
        }
        outColor = vec4(pow(max(pc.color.rgb, vec3(0.0)), vec3(2.2)), pc.color.a * a);
        return;
    }

    // Shape 25 (glass panel) is a rounded box plus bevel + reflection sheen.
    bool glass = (shape == 25);
    if (shape == 0 || glass) {
        // Shape 0: Rounded rectangle (e.g. button body, panel, progress bar)
        float r = clamp(pc.params.y, 0.0, 1.0);
        d = sdRoundedBox(pBox, boxHalf, r);
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
    } else if (shape == 12) {
        // Shape 12: Download (down arrow onto a base line)
        float shaft = sdRoundedBox(p - vec2(0.0, 0.18), vec2(0.09, 0.42), 0.04);
        float head = sdTriangle(p - vec2(0.0, -0.20), vec2(-0.36, 0.30),
                                vec2(0.36, 0.30), vec2(0.0, -0.28)) - 0.03;
        float base = sdRoundedBox(p - vec2(0.0, -0.62), vec2(0.52, 0.08), 0.03);
        d = min(min(shaft, head), base);
    } else if (shape == 13) {
        // Shape 13: List / queue (three bars with leading dots) for the
        // downloads window, so it is distinct from the plain download arrow.
        float b1 = sdRoundedBox(p - vec2(0.10, 0.46), vec2(0.40, 0.07), 0.05);
        float b2 = sdRoundedBox(p - vec2(0.10, 0.00), vec2(0.40, 0.07), 0.05);
        float b3 = sdRoundedBox(p - vec2(0.10, -0.46), vec2(0.40, 0.07), 0.05);
        float p1 = sdRoundedBox(p - vec2(-0.50, 0.46), vec2(0.10, 0.10), 0.05);
        float p2 = sdRoundedBox(p - vec2(-0.50, 0.00), vec2(0.10, 0.10), 0.05);
        float p3 = sdRoundedBox(p - vec2(-0.50, -0.46), vec2(0.10, 0.10), 0.05);
        d = min(min(min(b1, b2), min(b3, p1)), min(p2, p3));
    } else if (shape == 14) {
        // Shape 14: Hero Glow Ring (circular outline for primary buttons)
        float r = length(p);
        d = abs(r - 0.72) - 0.08;
    } else if (shape == 15) {
        // Shape 15: Audio Speaker icon (cone + sound wave)
        float box = sdRoundedBox(p - vec2(-0.32, 0.0), vec2(0.12, 0.18), 0.03);
        float cone = sdTriangle(p, vec2(-0.20, 0.18), vec2(0.16, 0.45), vec2(-0.20, -0.18)) - 0.02;
        float cone2 = sdTriangle(p, vec2(-0.20, -0.18), vec2(0.16, -0.45), vec2(0.16, 0.45)) - 0.02;
        float rArc = length(p - vec2(-0.05, 0.0));
        float arc1 = max(abs(rArc - 0.46) - 0.06, -p.x + 0.10);
        float arc2 = max(abs(rArc - 0.68) - 0.06, -p.x + 0.22);
        d = min(min(box, min(cone, cone2)), min(arc1, arc2));
    } else if (shape == 16) {
        // Shape 16: VR Headset / Display goggles
        float gBody = sdRoundedBox(p, vec2(0.55, 0.30), 0.14);
        float nose = sdTriangle(p - vec2(0.0, -0.30), vec2(-0.16, -0.08), vec2(0.16, -0.08), vec2(0.0, 0.12)) - 0.04;
        float strapL = sdRoundedBox(p - vec2(-0.62, 0.0), vec2(0.08, 0.10), 0.03);
        float strapR = sdRoundedBox(p - vec2(0.62, 0.0), vec2(0.08, 0.10), 0.03);
        d = max(min(gBody, min(strapL, strapR)), -nose);
    } else if (shape == 17) {
        // Shape 17: Curved Cinema Display icon
        float rScreen = length(p - vec2(0.0, 1.3));
        float arcDisp = abs(rScreen - 1.62) - 0.10;
        float cutX = abs(p.x) - 0.58;
        float cutY = abs(p.y) - 0.32;
        d = max(arcDisp, max(cutX, cutY));
    } else if (shape == 18) {
        // Shape 18: Chevron (>) for breadcrumbs & pagination
        float c1 = sdSegment(p, vec2(-0.22, 0.40), vec2(0.24, 0.0)) - 0.08;
        float c2 = sdSegment(p, vec2(0.24, 0.0), vec2(-0.22, -0.40)) - 0.08;
        d = min(c1, c2);
    } else if (shape == 19) {
        // Shape 19: Home / Library icon
        float roof = sdTriangle(p - vec2(0.0, 0.12), vec2(-0.52, 0.0), vec2(0.52, 0.0), vec2(0.0, 0.42)) - 0.04;
        float baseH = sdRoundedBox(p - vec2(0.0, -0.20), vec2(0.35, 0.26), 0.04);
        d = min(roof, baseH);
    } else if (shape == 21) {
        // Shape 21: Five-point star (bookmarks), Inigo Quilez sdStar5
        const vec2 k1 = vec2(0.809016994375, -0.587785252292);
        const vec2 k2 = vec2(-k1.x, k1.y);
        vec2 ps = vec2(abs(p.x), p.y + 0.07);
        float rf = 0.42;
        float r0 = 0.72;
        ps -= 2.0 * max(dot(k1, ps), 0.0) * k1;
        ps -= 2.0 * max(dot(k2, ps), 0.0) * k2;
        ps.x = abs(ps.x);
        ps.y -= r0;
        vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
        float h = clamp(dot(ps, ba) / dot(ba, ba), 0.0, r0);
        d = length(ps - ba * h) * sign(ps.y * ba.x - ps.x * ba.y) - 0.04;
    } else if (shape == 22) {
        // Shape 22: Padlock (vault)
        float body = sdRoundedBox(p - vec2(0.0, -0.22), vec2(0.50, 0.38), 0.10);
        vec2 pq = p - vec2(0.0, 0.16);
        float shackle = abs(length(pq) - 0.30) - 0.08;
        shackle = max(shackle, -pq.y);
        float legs = sdRoundedBox(vec2(abs(p.x) - 0.30, p.y - 0.10), vec2(0.08, 0.10), 0.02);
        float keyhole = length(p - vec2(0.0, -0.18)) - 0.09;
        d = max(min(body, min(shackle, legs)), -keyhole);
    } else if (shape == 23) {
        // Shape 23: Sort (three left-aligned bars of decreasing length)
        float b1 = sdRoundedBox(p - vec2(0.00, 0.46), vec2(0.62, 0.09), 0.06);
        float b2 = sdRoundedBox(p - vec2(-0.16, 0.00), vec2(0.46, 0.09), 0.06);
        float b3 = sdRoundedBox(p - vec2(-0.32, -0.46), vec2(0.30, 0.09), 0.06);
        d = min(b1, min(b2, b3));
    } else if (shape == 26) {
        // Shape 26: Speaker muted (speaker body + cone + small cross)
        float box = sdRoundedBox(p - vec2(-0.46, 0.0), vec2(0.12, 0.18), 0.03);
        float cone = sdTriangle(p - vec2(-0.14, 0.0), vec2(-0.20, 0.18), vec2(0.16, 0.45), vec2(-0.20, -0.18)) - 0.02;
        float cone2 = sdTriangle(p - vec2(-0.14, 0.0), vec2(-0.20, -0.18), vec2(0.16, -0.45), vec2(0.16, 0.45)) - 0.02;
        vec2 q = p - vec2(0.42, 0.0);
        float x1 = sdSegment(q, vec2(-0.18, -0.18), vec2(0.18, 0.18)) - 0.07;
        float x2 = sdSegment(q, vec2(-0.18, 0.18), vec2(0.18, -0.18)) - 0.07;
        d = min(min(box, min(cone, cone2)), min(x1, x2));
    } else if (shape == 27) {
        // Shape 27: Move (cross with four arrow heads)
        float bar1 = sdRoundedBox(p, vec2(0.50, 0.06), 0.03);
        float bar2 = sdRoundedBox(p, vec2(0.06, 0.50), 0.03);
        float up = sdTriangle(p, vec2(-0.20, 0.48), vec2(0.20, 0.48), vec2(0.0, 0.78)) - 0.02;
        float dn = sdTriangle(p, vec2(-0.20, -0.48), vec2(0.0, -0.78), vec2(0.20, -0.48)) - 0.02;
        float lf = sdTriangle(p, vec2(-0.48, -0.20), vec2(-0.48, 0.20), vec2(-0.78, 0.0)) - 0.02;
        float rt = sdTriangle(p, vec2(0.48, -0.20), vec2(0.78, 0.0), vec2(0.48, 0.20)) - 0.02;
        d = min(min(bar1, bar2), min(min(up, dn), min(lf, rt)));
    } else if (shape == 28) {
        // Shape 28: Open padlock (shackle lifted and swung to the left)
        float body = sdRoundedBox(p - vec2(0.0, -0.22), vec2(0.50, 0.38), 0.10);
        vec2 pq = p - vec2(-0.30, 0.34);
        float shackle = abs(length(pq) - 0.30) - 0.08;
        shackle = max(shackle, -pq.y);
        float leg = sdRoundedBox(vec2(p.x + 0.60, p.y - 0.26), vec2(0.08, 0.10), 0.02);
        float keyhole = length(p - vec2(0.0, -0.18)) - 0.09;
        d = max(min(body, min(shackle, leg)), -keyhole);
    } else if (shape == 29) {
        // Shape 29: Layout (three screens: two below, one centred above)
        float top = sdRoundedBox(p - vec2(0.0, 0.25), vec2(0.30, 0.19), 0.05);
        vec2 q = vec2(abs(p.x) - 0.35, p.y + 0.25);
        float bottom = sdRoundedBox(q, vec2(0.30, 0.19), 0.05);
        d = min(top, bottom);
    } else if (shape == 30) {
        // Shape 30: Frame (screen outline with a small stand)
        float outline = abs(sdRoundedBox(p - vec2(0.0, 0.08), vec2(0.66, 0.44), 0.08)) - 0.07;
        float stand = sdRoundedBox(p - vec2(0.0, -0.58), vec2(0.26, 0.05), 0.03);
        d = min(outline, stand);
    } else if (shape == 31 || shape == 32) {
        // Shapes 31 / 32: double-headed swap arrow (horizontal / vertical)
        vec2 q = (shape == 31) ? p : p.yx;
        float bar = sdRoundedBox(q, vec2(0.46, 0.08), 0.04);
        float lt = sdTriangle(q, vec2(-0.82, 0.0), vec2(-0.40, -0.34), vec2(-0.40, 0.34)) - 0.03;
        float rt = sdTriangle(q, vec2(0.82, 0.0), vec2(0.40, 0.34), vec2(0.40, -0.34)) - 0.03;
        d = min(bar, min(lt, rt));
    } else if (shape == 33) {
        // Shape 33: Globe (outline, a meridian ellipse, equator and two parallels)
        float rim = abs(length(p) - 0.62) - 0.07;
        float meridian = abs(length(p / vec2(0.42, 1.0)) - 0.62) * 0.42 - 0.05;
        float equator = max(abs(p.y) - 0.05, length(p) - 0.62);
        float parallels = max(abs(abs(p.y) - 0.31) - 0.045, length(p) - 0.62);
        d = min(min(rim, meridian), min(equator, parallels));
    } else {
        d = sdRoundedBox(p, vec2(1.0), 0.0);
    }

    // Exact screen-space gradient magnitude of the distance field along the contour normal.
    // Produces a crisp, razor-sharp 1.5 screen-pixel anti-aliased outline without crawling.
    float gradLen = length(vec2(dFdx(d), dFdy(d)));
    float aa = max(gradLen * 0.75, 0.0005);
    float fillAlpha = 1.0 - smoothstep(-aa, aa, d);

    if (fillAlpha <= 0.002) {
        discard;
    }

    vec4 fillLin = vec4(pow(max(pc.color.rgb, vec3(0.0)), vec3(2.2)), pc.color.a);
    if (glass) {
        // Glass slab: distance inside the edge in metres drives a thin bevel
        // (lit top edge, slightly shadowed bottom edge) so the panel reads as
        // having thickness, plus a soft diagonal reflection sheen.
        vec2 pp = pc.rect.xy + vCoord * pc.rect.zw;  // panel-space metres
        float halfMin = min(abs(pc.rect.z), abs(pc.rect.w));
        float inside = max(-d * halfMin, 0.0);
        float bevel = exp(-inside / 0.009);
        float topness = smoothstep(-0.9, 0.9, vCoord.y);
        fillLin.rgb += vec3(0.80, 0.88, 1.00) * bevel * 0.050 * topness;
        fillLin.rgb *= 1.0 - bevel * 0.30 * (1.0 - topness);
        // The sheen's horizontal position follows the eye (borderColor.a encodes
        // it), giving the parallax of a reflection of a distant light.
        float bandX = (pc.borderColor.a - 0.5) * 4.0;
        float sAxis = (pp.x - bandX) * 0.80 + pp.y * 0.60;
        float broad = exp(-(sAxis * sAxis) / (2.0 * 0.11 * 0.11));
        float fine = exp(-((sAxis - 0.20) * (sAxis - 0.20)) / (2.0 * 0.018 * 0.018));
        fillLin.rgb += vec3(0.85, 0.92, 1.00) * (broad * 0.012 + fine * 0.010);
    }
    if (shape == 0 || glass) {
        // Frosted glass: a gentle top-lit luminance gradient
        float topLight = mix(0.92, 1.10, clamp(0.5 * (vCoord.y + 1.0), 0.0, 1.0));
        fillLin.rgb *= topLight;
    }
    vec4 finalColor = fillLin;
    if (hasBorder) {
        // Hairline floor of ~1.25 screen pixels keeps thin rims visible without
        // turning every element into a heavy outline
        float minBorder = gradLen * 1.25;
        float borderWidth = max(pc.params.z, minBorder);
        float innerD = d + borderWidth;
        float borderAlpha = 1.0 - smoothstep(-aa, aa, innerD);
        // (The glass panel uses borderColor.a for its sheen, so its rim alpha is fixed.)
        vec4 bordLin = vec4(pow(max(pc.borderColor.rgb, vec3(0.0)), vec3(2.2)),
                            glass ? 0.42 : pc.borderColor.a);
        if (shape == 0 || glass) {
            // Soft specular catchlight along the upper rim, fading toward the bottom
            float rimLight = mix(0.55, 1.25, smoothstep(-0.6, 0.95, vCoord.y));
            bordLin.rgb *= rimLight;
            bordLin.a *= mix(0.65, 1.0, smoothstep(-0.6, 0.95, vCoord.y));
        }
        finalColor = mix(bordLin, fillLin, borderAlpha);
    }

    finalColor.a *= fillAlpha;
    outColor = finalColor;
}
