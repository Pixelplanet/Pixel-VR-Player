// 3D Autofocus, the measuring half (src/gfx/stereo_disparity.hpp): the gaze-to-video mapping,
// the CPU reference of the block-matching shader on synthetic stereo frames with known
// disparities, the gates, and the distance conversion. Development tool, never released.
//
//   stereo_disparity_test

#include "gfx/stereo_disparity.hpp"
#include "util/math.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace pixelvr;
using namespace pixelvr::stereo;

namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

constexpr float kPi = 3.14159265358979f;
constexpr float kRad = kPi / 180.0f;
constexpr float kDeg = 180.0f / kPi;

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), f, a, b, c);
    return buf;
}

// --- A smooth, deterministic texture over the sphere (value noise, periodic in longitude) ---

struct Lattice {
    float cellDeg = 1.5f;
    int nx = 0, ny = 0;
    std::vector<float> v;
    Lattice(float cell, uint32_t seed) : cellDeg(cell) {
        nx = static_cast<int>(std::lround(360.0f / cell));
        ny = static_cast<int>(std::lround(180.0f / cell)) + 2;
        v.resize(static_cast<size_t>(nx) * ny);
        uint32_t s = seed;
        for (float& x : v) {
            s = s * 1664525u + 1013904223u;  // LCG
            x = static_cast<float>(s >> 8) / 16777216.0f;
        }
    }
    float at(int x, int y) const {
        x %= nx;
        if (x < 0) x += nx;
        y = y < 0 ? 0 : (y >= ny ? ny - 1 : y);
        return v[static_cast<size_t>(y) * nx + x];
    }
    float sample(float lonDeg, float latDeg) const {
        const float fx = (lonDeg + 180.0f) / cellDeg;
        const float fy = (latDeg + 90.0f) / cellDeg;
        const float x0 = std::floor(fx), y0 = std::floor(fy);
        float ax = fx - x0, ay = fy - y0;
        ax = ax * ax * (3.0f - 2.0f * ax);
        ay = ay * ay * (3.0f - 2.0f * ay);
        const int ix = static_cast<int>(x0), iy = static_cast<int>(y0);
        const float a = at(ix, iy) + (at(ix + 1, iy) - at(ix, iy)) * ax;
        const float b = at(ix, iy + 1) + (at(ix + 1, iy + 1) - at(ix, iy + 1)) * ax;
        return a + (b - a) * ay;
    }
};

const Lattice& coarse() {
    static const Lattice l(3.0f, 12345u);
    return l;
}
const Lattice& fine() {
    static const Lattice l(1.5f, 987654321u);
    return l;
}
float texture(float lonDeg, float latDeg) {
    return 0.15f + 0.35f * coarse().sample(lonDeg, latDeg) + 0.45f * fine().sample(lonDeg, latDeg);
}

float wrapDeg(float d) {
    return std::remainder(d, 360.0f);
}

// A stereo equirect frame: the left-eye half shows texture(); the right-eye half shows the same,
// but within `radiusDeg` of (lon0, lat0) shifted so the patch sits `dispDeg` further left
// (lonR = lonL - disp). `leftHalf` = the half holding the left eye. `padRows` = extra rows of
// junk below the picture (the decoder padding the uvScale crop removes).
struct Frame {
    int width = 0, height = 0;  // picture size (without padding)
    int padRows = 0;
    std::vector<uint8_t> px;
    LumaImage image() const {
        LumaImage img;
        img.width = width;
        img.height = height + padRows;
        img.stride = width;
        img.data = px.data();
        return img;
    }
};

Frame makeFrame(int projection, int stereo, int width, int height, int leftHalf, float lon0, float lat0,
                float dispDeg, float radiusDeg, bool flat = false, int padRows = 0) {
    Frame f;
    f.width = width;
    f.height = height;
    f.padRows = padRows;
    f.px.assign(static_cast<size_t>(width) * (height + padRows), 0);
    const int hw = (stereo == 1) ? width / 2 : width;
    const int hh = (stereo == 2) ? height / 2 : height;
    const float span = (projection == 2) ? 180.0f : 360.0f;
    for (int half = 0; half < 2; ++half) {
        const bool rightEye = (half != leftHalf);
        for (int y = 0; y < hh; ++y) {
            const float lat = (0.5f - (y + 0.5f) / hh) * 180.0f;
            for (int x = 0; x < hw; ++x) {
                const float lon = ((x + 0.5f) / hw - 0.5f) * span;
                float val = texture(lon, lat);
                const bool inPatch = std::hypot(wrapDeg(lon - lon0), lat - lat0) < radiusDeg;
                if (rightEye && inPatch) val = texture(lon + dispDeg, lat);
                if (flat && inPatch) val = 0.5f;
                const int px = (stereo == 1) ? x + half * hw : x;
                const int py = (stereo == 2) ? y + half * hh : y;
                const int q = static_cast<int>(std::lround(val * 255.0f));
                f.px[static_cast<size_t>(py) * width + px] = static_cast<uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
            }
        }
    }
    for (int y = height; y < height + padRows; ++y) {
        for (int x = 0; x < width; ++x) f.px[static_cast<size_t>(y) * width + x] = static_cast<uint8_t>((x * 37 + y * 11) & 255);
    }
    return f;
}

// A stereo equirect frame from a function: luma(half, lon, lat) for each half.
template <typename Fn>
Frame makeFrameFrom(int projection, int stereo, int width, int height, Fn luma) {
    Frame f;
    f.width = width;
    f.height = height;
    f.px.assign(static_cast<size_t>(width) * height, 0);
    const int hw = (stereo == 1) ? width / 2 : width;
    const int hh = (stereo == 2) ? height / 2 : height;
    const float span = (projection == 2) ? 180.0f : 360.0f;
    for (int half = 0; half < 2; ++half) {
        for (int y = 0; y < hh; ++y) {
            const float lat = (0.5f - (y + 0.5f) / hh) * 180.0f;
            for (int x = 0; x < hw; ++x) {
                const float lon = ((x + 0.5f) / hw - 0.5f) * span;
                const int px = (stereo == 1) ? x + half * hw : x;
                const int py = (stereo == 2) ? y + half * hh : y;
                const int q = static_cast<int>(std::lround(luma(half, lon, lat) * 255.0f));
                f.px[static_cast<size_t>(py) * width + px] = static_cast<uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
            }
        }
    }
    return f;
}

DisparityResult measure(const Frame& f, const GazeSource& g, float uvScaleX = 1.0f, float uvScaleY = 1.0f) {
    float scores[kCandidates];
    float variance = 0.0f;
    referenceScores(f.image(), g, uvScaleX, uvScaleY, scores, variance);
    return evaluateScores(scores, variance);
}

GazeSource source(int projection, int stereo, int leftHalf, float lon, float lat) {
    GazeSource g;
    g.valid = true;
    g.projection = projection;
    g.stereo = stereo;
    g.leftHalf = leftHalf;
    g.lonDeg = lon;
    g.latDeg = lat;
    return g;
}

void dirFromLonLat(float lonDeg, float latDeg, float out[3]) {
    const float lo = lonDeg * kRad, la = latDeg * kRad;
    out[0] = std::sin(lo) * std::cos(la);
    out[1] = std::sin(la);
    out[2] = -std::cos(lo) * std::cos(la);
}

void caseDisparity(const char* label, int projection, int stereo, int w, int h, float lon0, float lat0, float disp,
                   int leftHalf = 0) {
    const Frame f = makeFrame(projection, stereo, w, h, leftHalf, lon0, lat0, disp, 25.0f);
    const DisparityResult r = measure(f, source(projection, stereo, leftHalf, lon0, lat0));
    check(r.valid && std::fabs(r.disparityDeg - disp) < 0.1f,
          std::string(label) + fmt(": disparity %.2f deg measured %.3f (score %.3f)", disp, r.disparityDeg, r.score) +
              (r.valid ? "" : " INVALID"));
}

// --- gazeToSource against a separate computation with quaternion rotations ---

math::Quat axisAngle(float ax, float ay, float az, float angle) {
    const float l = std::sqrt(ax * ax + ay * ay + az * az);
    const float s = std::sin(0.5f * angle) / l;
    return math::Quat{ax * s, ay * s, az * s, std::cos(0.5f * angle)};
}

// What the sphere shaders show along the gaze: the gaze in head space (conjugate rotation),
// tangents times the zoom, then pitch(yaw(head(t))) as rot = adjust * headRot does.
bool expectedLonLat(const SphereView& v, const float gz[3], float& lon, float& lat) {
    using namespace math;
    const Quat h{v.headRot[0], v.headRot[1], v.headRot[2], v.headRot[3]};
    const Quat hc{-h.x, -h.y, -h.z, h.w};
    const Vec3 local = rotate(hc, Vec3{gz[0], gz[1], gz[2]});
    if (local.z >= -1e-4f) return false;
    const Vec3 t{local.x / -local.z * v.zoom, local.y / -local.z * v.zoom, -1.0f};
    const Vec3 d = normalize(rotate(axisAngle(1, 0, 0, v.pitchTotal),
                                    rotate(axisAngle(0, 1, 0, v.yawTotal), rotate(h, t))));
    lon = std::atan2(d.x, -d.z) * kDeg;
    lat = std::asin(d.y) * kDeg;
    return true;
}

void testGazeToSource() {
    // Straight ahead.
    {
        SphereView v;
        const float g[3] = {0.0f, 0.0f, -1.0f};
        const GazeSource s = gazeToSource(v, g);
        check(s.valid && std::fabs(s.lonDeg) < 1e-4f && std::fabs(s.latDeg) < 1e-4f && s.leftHalf == 0,
              "gaze: straight ahead, identity head -> lon 0 lat 0, left half 0");
    }
    // Yaw / pitch of the sphere, by hand.
    {
        SphereView v;
        v.yawTotal = 30.0f * kRad;
        const float g[3] = {0.0f, 0.0f, -1.0f};
        const GazeSource s = gazeToSource(v, g);
        check(s.valid && std::fabs(s.lonDeg + 30.0f) < 1e-3f && std::fabs(s.latDeg) < 1e-3f,
              fmt("gaze: sphere yaw +30 deg -> lon -30 (got %.3f)", s.lonDeg));
        v.yawTotal = 0.0f;
        v.pitchTotal = 20.0f * kRad;
        const GazeSource p = gazeToSource(v, g);
        check(p.valid && std::fabs(p.lonDeg) < 1e-3f && std::fabs(p.latDeg - 20.0f) < 1e-3f,
              fmt("gaze: sphere pitch +20 deg -> lat +20 (got %.3f)", p.latDeg));
    }
    // Zoom scales the view tangents.
    {
        SphereView v;
        v.zoom = 2.0f;
        float g[3];
        dirFromLonLat(10.0f, 0.0f, g);
        const GazeSource s = gazeToSource(v, g);
        const float want = std::atan(2.0f * std::tan(10.0f * kRad)) * kDeg;
        check(s.valid && std::fabs(s.lonDeg - want) < 1e-3f, fmt("gaze: zoom 2, 10 deg right -> lon %.3f (got %.3f)", want, s.lonDeg));
    }
    // Head turned left 40 deg, looking where the head points.
    {
        SphereView v;
        const math::Quat q = axisAngle(0, 1, 0, 40.0f * kRad);
        v.headRot[0] = q.x; v.headRot[1] = q.y; v.headRot[2] = q.z; v.headRot[3] = q.w;
        const math::Vec3 fwd = math::rotate(q, math::Vec3{0.0f, 0.0f, -1.0f});
        const float g[3] = {fwd.x, fwd.y, fwd.z};
        const GazeSource s = gazeToSource(v, g);
        check(s.valid && std::fabs(s.lonDeg + 40.0f) < 1e-3f, fmt("gaze: head turned left 40 deg -> lon -40 (got %.3f)", s.lonDeg));
    }
    // Random heads, yaw, pitch, zoom and gazes against the quaternion computation.
    {
        uint32_t seed = 4242u;
        auto rnd = [&seed]() {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float>(seed >> 8) / 16777216.0f;
        };
        int compared = 0, bad = 0;
        float worst = 0.0f;
        for (int n = 0; n < 2000; ++n) {
            SphereView v;
            const math::Quat q = axisAngle(rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f, (rnd() - 0.5f) * 2.0f * kPi);
            v.headRot[0] = q.x; v.headRot[1] = q.y; v.headRot[2] = q.z; v.headRot[3] = q.w;
            v.yawTotal = (rnd() - 0.5f) * 2.0f * kPi;
            v.pitchTotal = (rnd() - 0.5f) * 1.2f;
            v.zoom = 0.6f + rnd() * 1.6f;
            v.projection = 1;
            v.stereo = (n & 1) ? 2 : 1;
            // A gaze within 40 deg of where the head points.
            const math::Vec3 off = math::normalize(math::Vec3{(rnd() - 0.5f) * 1.4f, (rnd() - 0.5f) * 1.4f, -1.0f});
            const math::Vec3 gw = math::rotate(q, off);
            const float g[3] = {gw.x, gw.y, gw.z};
            float lon = 0.0f, lat = 0.0f;
            if (!expectedLonLat(v, g, lon, lat)) continue;
            const GazeSource s = gazeToSource(v, g);
            if (std::fabs(lat) > 79.0f) continue;  // the pole gate
            ++compared;
            const float e = std::fmax(std::fabs(wrapDeg(s.lonDeg - lon)) * std::cos(lat * kRad), std::fabs(s.latDeg - lat));
            if (!s.valid || e > 0.01f) ++bad;
            worst = std::fmax(worst, e);
        }
        check(compared > 1500 && bad == 0,
              fmt("gaze: %.0f random views match the quaternion computation (worst %.5f deg, %.0f off)", compared, worst, bad));
    }
    // Rejections.
    {
        SphereView v;
        const float behind[3] = {0.0f, 0.0f, 1.0f};
        check(!gazeToSource(v, behind).valid, "gaze: behind the view -> invalid");
        v.stereo = 0;
        const float ahead[3] = {0.0f, 0.0f, -1.0f};
        check(!gazeToSource(v, ahead).valid, "gaze: mono -> invalid");
        v.stereo = 1;
        v.projection = 2;
        float g[3];
        dirFromLonLat(86.0f, 0.0f, g);
        check(gazeToSource(v, g).valid, "gaze: 180, lon 86 -> valid");
        dirFromLonLat(88.0f, 0.0f, g);
        check(!gazeToSource(v, g).valid, "gaze: 180, lon 88 (within 3 deg of the edge) -> invalid");
        dirFromLonLat(-88.0f, 0.0f, g);
        check(!gazeToSource(v, g).valid, "gaze: 180, lon -88 -> invalid");
        // Head turned round: in front of the view, behind the 180 picture.
        const math::Quat q = axisAngle(0, 1, 0, kPi);
        v.headRot[0] = q.x; v.headRot[1] = q.y; v.headRot[2] = q.z; v.headRot[3] = q.w;
        check(!gazeToSource(v, behind).valid, "gaze: 180, behind the picture -> invalid");
        v.projection = 1;
        check(gazeToSource(v, behind).valid, "gaze: 360, the same gaze -> valid");
        SphereView p;
        dirFromLonLat(0.0f, 79.0f, g);
        check(gazeToSource(p, g).valid, "gaze: lat 79 -> valid");
        dirFromLonLat(0.0f, 81.0f, g);
        check(!gazeToSource(p, g).valid, "gaze: lat 81 (near the pole) -> invalid");
        p.swapEyes = true;
        dirFromLonLat(5.0f, 5.0f, g);
        const GazeSource s = gazeToSource(p, g);
        check(s.valid && s.leftHalf == 1, "gaze: swap eyes -> left eye reads half 1");
    }
}

void testSourceUv() {
    float u = 0.0f, v = 0.0f;
    GazeSource g = source(1, 1, 0, 0.0f, 0.0f);
    sourceUv(g, 0.0f, 0.0f, 0, u, v);
    check(std::fabs(u - 0.25f) < 1e-6f && std::fabs(v - 0.5f) < 1e-6f, "uv: 360 SBS centre, half 0 -> (0.25, 0.5)");
    sourceUv(g, 190.0f, 45.0f, 1, u, v);
    check(std::fabs(u - (0.5f + 0.5f * (10.0f / 360.0f))) < 1e-5f && std::fabs(v - 0.25f) < 1e-6f,
          "uv: 360 SBS lon 190 wraps to -170 within half 1");
    g = source(2, 2, 0, 0.0f, 0.0f);
    sourceUv(g, 45.0f, 0.0f, 1, u, v);
    check(std::fabs(u - 0.75f) < 1e-6f && std::fabs(v - 0.75f) < 1e-6f, "uv: 180 TB lon 45, half 1 -> (0.75, 0.75)");
    sourceUv(g, 95.0f, 0.0f, 0, u, v);
    check(u > 1.0f, "uv: 180 does not wrap past the edge");
}

void testEvaluate() {
    float s[kCandidates];
    auto parabola = [&s](float peak, float top) {
        for (int c = 0; c < kCandidates; ++c) s[c] = top - 0.01f * (c - peak) * (c - peak);
    };
    parabola(40.3f, 0.95f);
    DisparityResult r = evaluateScores(s, 0.01f);
    check(r.valid && r.best == 40 && std::fabs(r.disparityDeg - (40.3f - kZeroCandidate) * kPitchXDeg) < 1e-3f,
          fmt("evaluate: parabola peak at 40.3 -> %.4f deg", r.disparityDeg));
    r = evaluateScores(s, kMinVariance * 0.5f);
    check(!r.valid, "evaluate: too little texture -> invalid");
    parabola(40.0f, 0.5f);
    check(!evaluateScores(s, 0.01f).valid, "evaluate: best score 0.5 -> invalid");
    parabola(0.0f, 0.95f);
    check(!evaluateScores(s, 0.01f).valid, "evaluate: peak at the first candidate -> invalid");
    parabola(127.0f, 0.95f);
    check(!evaluateScores(s, 0.01f).valid, "evaluate: peak at the last candidate -> invalid");
    parabola(40.3f, 0.95f);
    for (int c = 0; c < 40; ++c) s[c] = std::nanf("");  // (windows that left the 180 picture)
    r = evaluateScores(s, 0.01f);
    check(r.best == 40 && !r.valid, "evaluate: peak next to a NaN candidate -> invalid (an end of the search)");
    parabola(40.3f, 0.95f);
    for (int c = 0; c < 30; ++c) s[c] = std::nanf("");
    r = evaluateScores(s, 0.01f);
    check(r.valid && r.best == 40, "evaluate: NaN candidates away from the peak are skipped");
    for (float& x : s) x = std::nanf("");
    check(!evaluateScores(s, 0.01f).valid, "evaluate: no numbers -> invalid");
}

void testDistance() {
    const float m = disparityToMetres(3.6f);
    check(std::fabs(m - 1.0f) < 0.02f, fmt("distance: 3.6 deg -> %.4f m (about 1 m)", m));
    bool ok = true;
    for (float d : {0.3f, 1.0f, 2.5f, 3.6f, 7.0f, 15.0f}) {
        if (std::fabs(metresToDisparity(disparityToMetres(d)) - d) > 1e-3f) ok = false;
    }
    check(ok, "distance: metresToDisparity inverts disparityToMetres");
    check(std::isfinite(disparityToMetres(0.0f)) && std::isfinite(disparityToMetres(-2.0f)),
          "distance: zero / negative disparity stays finite (clamped)");
    check(std::fabs(metresToDisparity(1.0f) - 3.608f) < 0.01f, fmt("distance: 1 m -> %.3f deg", metresToDisparity(1.0f)));
}

void testMatching() {
    // Side by side 360, 2048 x 1024.
    for (float d : {0.0f, 2.0f, 3.3f, 5.5f, 9.0f, -1.5f})
        caseDisparity("360 SBS lon 30 lat 10", 1, 1, 2048, 1024, 30.0f, 10.0f, d);
    // Across the 360 seam: the right window wraps from -180 to +180.
    caseDisparity("360 SBS across the seam", 1, 1, 2048, 1024, -175.0f, 0.0f, 5.5f);
    // Top / bottom 180, 2048 x 2048.
    for (float d : {0.0f, 2.0f, 5.5f, 9.0f, 14.0f})
        caseDisparity("180 TB lon -20 lat -15", 2, 2, 2048, 2048, -20.0f, -15.0f, d);
    caseDisparity("180 SBS lon 40 lat 30", 2, 1, 4096, 2048, 40.0f, 30.0f, 4.2f);

    // Through gazeToSource with an identity head.
    {
        const Frame f = makeFrame(1, 1, 2048, 1024, 0, -60.0f, 5.0f, 5.5f, 25.0f);
        SphereView v;
        float g[3];
        dirFromLonLat(-60.0f, 5.0f, g);
        const GazeSource s = gazeToSource(v, g);
        const DisparityResult r = measure(f, s);
        check(s.valid && r.valid && std::fabs(r.disparityDeg - 5.5f) < 0.1f,
              fmt("matching via gazeToSource: 5.5 deg measured %.3f", r.disparityDeg));
    }

    // Decoder padding cropped by the uv scale.
    {
        const Frame f = makeFrame(2, 2, 2048, 2048, 0, 10.0f, 0.0f, 3.0f, 25.0f, false, 64);
        const float sy = 2048.0f / (2048.0f + 64.0f);
        const DisparityResult r = measure(f, source(2, 2, 0, 10.0f, 0.0f), 1.0f, sy);
        check(r.valid && std::fabs(r.disparityDeg - 3.0f) < 0.1f,
              fmt("matching with a padding crop (uvScaleY %.4f): 3.0 deg measured %.3f", sy, r.disparityDeg));
    }

    // A flat patch (wider than every candidate's windows reach: s / 2 + 2 deg): the variance gate.
    {
        const Frame f = makeFrame(1, 1, 2048, 1024, 0, 30.0f, 10.0f, 5.5f, 14.0f, true);
        float scores[kCandidates];
        float variance = 0.0f;
        referenceScores(f.image(), source(1, 1, 0, 30.0f, 10.0f), 1.0f, 1.0f, scores, variance);
        const DisparityResult r = evaluateScores(scores, variance);
        check(!r.valid && variance < kMinVariance, fmt("flat patch -> invalid (variance %.6f)", variance));
    }

    // Swapped eyes: the left eye is in the right half.
    {
        const Frame f = makeFrame(1, 1, 2048, 1024, 1, 30.0f, 10.0f, 5.5f, 25.0f);
        SphereView v;
        v.swapEyes = true;
        float g[3];
        dirFromLonLat(30.0f, 10.0f, g);
        const GazeSource s = gazeToSource(v, g);
        const DisparityResult r = measure(f, s);
        check(s.leftHalf == 1 && r.valid && std::fabs(r.disparityDeg - 5.5f) < 0.1f,
              fmt("swap eyes: left half 1, near patch still +5.5 deg (measured %.3f)", r.disparityDeg));
        GazeSource wrong = s;
        wrong.leftHalf = 0;
        const DisparityResult w = measure(f, wrong);
        check(!(w.valid && w.disparityDeg > 0.0f), "swap eyes: the wrong eye order does not read as near");
    }

    // A small near subject (radius 4 deg, 7.2 deg = 0.5 m) on a far background (0 deg), gazed at
    // with the combined gaze: midway between its two images, (lonL + lonR) / 2. Neither image of
    // the subject is at the gaze point itself.
    {
        const float lonG = 20.0f, lat0 = 5.0f, disp = 7.2f, radius = 4.0f;
        const float lonL = lonG + 0.5f * disp, lonR = lonG - 0.5f * disp;
        const Frame f = makeFrameFrom(1, 1, 2048, 1024, [&](int half, float lon, float lat) {
            const float at = (half == 0) ? lonL : lonR;
            if (std::hypot(wrapDeg(lon - at), lat - lat0) < radius) return texture(lon - at + 140.0f, lat);
            return texture(lon, lat);
        });
        const DisparityResult r = measure(f, source(1, 1, 0, lonG, lat0));
        check(r.valid && std::fabs(r.disparityDeg - disp) < 0.15f,
              fmt("small near subject, gaze midway between its images: %.1f deg measured %.3f (score %.3f)", disp,
                  r.disparityDeg, r.score) + (r.valid ? "" : " INVALID"));
    }

    // 180 side by side, gaze near the left edge (lon -86): the right windows of the larger
    // disparities reach past -90 deg, where u < 0 would read the left eye's half near +90. That
    // half holds there exactly what the left window sees at 16 deg, the right eye something else:
    // the candidates past the edge are NaN, so no false "near" match.
    {
        const float lon0 = -86.0f;
        const Frame f = makeFrameFrom(2, 1, 4096, 2048, [&](int half, float lon, float lat) {
            if (half == 1) return texture(lon + 200.0f, lat);
            if (lon > 80.0f) return texture(lon - 164.0f, lat);  // (-96 .. -92 read as +84 .. +88)
            return texture(lon, lat);
        });
        float scores[kCandidates];
        float variance = 0.0f;
        const GazeSource g = source(2, 1, 0, lon0, 0.0f);
        referenceScores(f.image(), g, 1.0f, 1.0f, scores, variance);
        const DisparityResult r = evaluateScores(scores, variance);
        const int c16 = kZeroCandidate + static_cast<int>(std::lround(16.0f / kPitchXDeg));
        check(std::isnan(scores[c16]) && !std::isnan(scores[kZeroCandidate]),
              "180 edge: a candidate whose right window leaves the picture is NaN, no shift is not");
        check(!(r.valid && r.disparityDeg > 4.0f),
              fmt("180 edge: the other eye's half past the edge is not matched (%.3f deg)", r.disparityDeg) +
                  (r.valid ? "" : " invalid"));
    }
    // ... and a real match near that edge still works.
    caseDisparity("180 SBS near the left edge", 2, 1, 4096, 2048, -80.0f, 0.0f, 2.0f);
    caseDisparity("180 SBS near the right edge", 2, 1, 4096, 2048, 80.0f, 0.0f, 2.0f);
}

}  // namespace

int main() {
    testSourceUv();
    testGazeToSource();
    testEvaluate();
    testDistance();
    testMatching();
    std::printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
