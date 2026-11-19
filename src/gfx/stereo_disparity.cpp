#include "gfx/stereo_disparity.hpp"

#include "util/math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pixelvr::stereo {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = 180.0f / kPi;
constexpr float kRad = kPi / 180.0f;

// Bilinear read of the luma image at a full-texture UV, clamp-to-edge, pixel centres at
// (x + 0.5) / w as Vulkan samples them. 0..1.
float sampleLuma(const LumaImage& img, float u, float v) {
    const float x = u * static_cast<float>(img.width) - 0.5f;
    const float y = v * static_cast<float>(img.height) - 0.5f;
    const float fx0 = std::floor(x);
    const float fy0 = std::floor(y);
    const float ax = x - fx0;
    const float ay = y - fy0;
    const int x0 = static_cast<int>(fx0);
    const int y0 = static_cast<int>(fy0);
    const int xa = std::clamp(x0, 0, img.width - 1);
    const int xb = std::clamp(x0 + 1, 0, img.width - 1);
    const int ya = std::clamp(y0, 0, img.height - 1);
    const int yb = std::clamp(y0 + 1, 0, img.height - 1);
    const uint8_t* ra = img.data + static_cast<size_t>(ya) * img.stride;
    const uint8_t* rb = img.data + static_cast<size_t>(yb) * img.stride;
    const float top = ra[xa] + (ra[xb] - static_cast<float>(ra[xa])) * ax;
    const float bot = rb[xa] + (rb[xb] - static_cast<float>(rb[xa])) * ax;
    return (top + (bot - top) * ay) * (1.0f / 255.0f);
}

}  // namespace

GazeSource gazeToSource(const SphereView& view, const float gazeDirApp[3]) {
    using namespace math;
    GazeSource out;
    out.projection = view.projection;
    out.stereo = view.stereo;
    out.leftHalf = view.swapEyes ? 1 : 0;
    if (view.stereo != 1 && view.stereo != 2) return out;  // mono: nothing to match
    if (view.projection != 1 && view.projection != 2) return out;

    // The sphere draw's matrices (vulkan_renderer.cpp): rot = adjust * headRot.
    const Mat4 headRot = fromQuat(Quat{view.headRot[0], view.headRot[1], view.headRot[2], view.headRot[3]});
    const Mat4 adjust = multiply(
        fromQuat(Quat{std::sin(0.5f * view.pitchTotal), 0.0f, 0.0f, std::cos(0.5f * view.pitchTotal)}),
        fromQuat(Quat{0.0f, std::sin(0.5f * view.yawTotal), 0.0f, std::cos(0.5f * view.yawTotal)}));
    const Mat4 rot = multiply(adjust, headRot);

    // The gaze in view space, as view tangents; sphere.vert scales them by the zoom.
    const Vec3 g{gazeDirApp[0], gazeDirApp[1], gazeDirApp[2]};
    const Vec3 local = transformDir(rigidInverse(headRot), g);
    if (!(local.z < -1e-4f)) return out;  // behind the view (or NaN)
    const float tx = local.x / -local.z * view.zoom;
    const float ty = local.y / -local.z * view.zoom;

    // sphere.vert: vDir = mat3(viewRotation) * (tx, ty, -1); sphere.frag: lon / lat.
    const Vec3 d = normalize(transformDir(rot, Vec3{tx, ty, -1.0f}));
    const float lon = std::atan2(d.x, -d.z) * kDeg;
    const float lat = std::asin(std::clamp(d.y, -1.0f, 1.0f)) * kDeg;
    if (view.projection == 2) {
        if (d.z > 0.0f) return out;              // behind the 180 picture
        if (std::fabs(lon) > 87.0f) return out;  // within 3 deg of its side edges
    }
    if (std::fabs(lat) > 80.0f) return out;      // too near a pole
    out.valid = true;
    out.lonDeg = lon;
    out.latDeg = lat;
    return out;
}

void sourceUv(const GazeSource& g, float lonDeg, float latDeg, int half, float& u, float& v) {
    const float span = (g.projection == 2) ? 180.0f : 360.0f;
    u = lonDeg / span + 0.5f;
    v = 0.5f - latDeg / 180.0f;
    if (g.projection != 2) u -= std::floor(u);  // 360 wraps within the half (GLSL fract)
    const float h = static_cast<float>(half);
    if (g.stereo == 2) {
        v = v * 0.5f + h * 0.5f;
    } else if (g.stereo == 1) {
        u = u * 0.5f + h * 0.5f;
    }
}

DisparityResult evaluateScores(const float scores[kCandidates], float leftVariance) {
    DisparityResult r;
    r.variance = leftVariance;
    float bestScore = 0.0f;
    for (int c = 0; c < kCandidates; ++c) {
        if (std::isnan(scores[c])) continue;
        if (r.best < 0 || scores[c] > bestScore) {
            r.best = c;
            bestScore = scores[c];
        }
    }
    if (r.best < 0) return r;
    r.score = bestScore;
    float refine = 0.0f;
    // Next to a NaN candidate (a window left the 180 picture) counts as an end of the search too.
    const bool inside = r.best > 0 && r.best < kCandidates - 1 && !std::isnan(scores[r.best - 1]) &&
                        !std::isnan(scores[r.best + 1]);
    if (inside) {
        const float a = scores[r.best - 1];
        const float s = scores[r.best];
        const float c = scores[r.best + 1];
        const float denom = a - 2.0f * s + c;
        if (denom < -1e-9f) refine = std::clamp(0.5f * (a - c) / denom, -0.5f, 0.5f);
    }
    r.disparityDeg = (static_cast<float>(r.best) + refine - static_cast<float>(kZeroCandidate)) * kPitchXDeg;
    r.valid = inside && bestScore >= kMinScore && leftVariance >= kMinVariance;
    return r;
}

void referenceScores(const LumaImage& img, const GazeSource& g, float uvScaleX, float uvScaleY,
                     float scoresOut[kCandidates], float& leftVarianceOut) {
    // The same two loads as the shader's shared memory: the left strip and the right strip, on
    // the half-pitch grid (candidate c, window column i: left 2 i + c, right 2 i - c + 127).
    // kOutside marks a sample past the 180 picture's side edge.
    constexpr float kOutside = -1.0f;
    float left[kWinH][kHalfStripW];
    float right[kWinH][kHalfStripW];
    const int leftHalf = g.leftHalf;
    const int rightHalf = 1 - g.leftHalf;
    const float cy = 0.5f * static_cast<float>(kWinH - 1);
    const float span = (g.projection == 2) ? 180.0f : 360.0f;
    auto luma = [&](float lon, float lat, int half) {
        if (g.projection == 2) {
            const float u0 = lon / span + 0.5f;  // (the shader's test, before the half mapping)
            if (u0 < 0.0f || u0 > 1.0f) return kOutside;
        }
        float u, v;
        sourceUv(g, lon, lat, half, u, v);
        return sampleLuma(img, u * uvScaleX, v * uvScaleY);
    };
    for (int j = 0; j < kWinH; ++j) {
        const float lat = g.latDeg - (static_cast<float>(j) - cy) * kPitchYDeg;
        for (int k = 0; k < kHalfStripW; ++k) {
            left[j][k] = luma(g.lonDeg + static_cast<float>(k - kLeftStripOff) * kHalfPitchXDeg, lat, leftHalf);
            right[j][k] = luma(g.lonDeg + static_cast<float>(k - kRightStripOff) * kHalfPitchXDeg, lat, rightHalf);
        }
    }

    const float n = static_cast<float>(kWinW * kWinH);
    float variances[kCandidates];
    bool ok[kCandidates];
    for (int c = 0; c < kCandidates; ++c) {
        float sl = 0.0f, sr = 0.0f, sll = 0.0f, srr = 0.0f, slr = 0.0f;
        bool inside = true;
        for (int j = 0; j < kWinH; ++j) {
            for (int i = 0; i < kWinW; ++i) {
                const float l = left[j][2 * i + c];
                const float r = right[j][2 * i - c + (kCandidates - 1)];
                if (l < -0.5f || r < -0.5f) inside = false;
                sl += l;
                sr += r;
                sll += l * l;
                srr += r * r;
                slr += l * r;
            }
        }
        const float num = slr - sl * sr / n;
        const float dl = std::max(sll - sl * sl / n, 0.0f);
        const float dr = std::max(srr - sr * sr / n, 0.0f);
        const float den = std::sqrt(dl * dr);
        const float score = (den < 1e-12f) ? 0.0f : num / den;
        scoresOut[c] = inside ? score : std::numeric_limits<float>::quiet_NaN();
        ok[c] = inside;
        const float mean = sl / n;
        variances[c] = std::max(sll / n - mean * mean, 0.0f);  // never below 0 from rounding
    }
    // The variance of the candidate evaluateScores picks (the shader's thread 0 does the same).
    int best = -1;
    for (int c = 0; c < kCandidates; ++c) {
        if (ok[c] && (best < 0 || scoresOut[c] > scoresOut[best])) best = c;
    }
    leftVarianceOut = (best < 0) ? 0.0f : variances[best];
}

float disparityToMetres(float disparityDeg, float baselineM) {
    const float d = std::max(disparityDeg, 0.05f) * kRad;
    return baselineM / (2.0f * std::tan(0.5f * d));
}

float metresToDisparity(float metres, float baselineM) {
    const float m = std::max(metres, 1e-6f);
    return 2.0f * std::atan(baselineM / (2.0f * m)) * kDeg;
}

}  // namespace pixelvr::stereo
