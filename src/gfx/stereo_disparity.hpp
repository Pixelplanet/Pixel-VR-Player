#pragma once

// Stereo autofocus, the measuring half (docs/concepts/AUTOFOCUS_PLAN_2026-11-19.md): where the
// eye gaze lands in a stereo 180 / 360 equirect video, and how far apart the gazed-at patch is
// in the two eye images (the disparity), from the scores of a block-matching search.
//
// The GPU does the matching (shaders/stereo_disparity.comp); referenceScores() is the same
// computation on the CPU for the tests. Both use the constants and the sampling rule below.
//
// Disparity convention: lonL - lonR in degrees of longitude, measured on the SOURCE video (the
// applied focus shift does not change it). Positive = the patch sits more to the right in the
// image shown to the left eye = nearer than infinity (about 3.6 deg = 1 m with a 63 mm rig).

#include <cstdint>

namespace pixelvr::stereo {

// The matching windows: kWinW x kWinH samples over kWinWDeg x kWinHDeg (longitude x latitude),
// one in each eye image. The search tries kCandidates disparities, one sample pitch apart;
// candidate kZeroCandidate is none. The search is symmetric about the gaze point: the eyes
// fixate a subject's two images, and the one combined gaze ray points about midway between them
// (the applied shift cancels in it), so for disparity s the left window sits s / 2 to the right
// of the gaze point and the right window s / 2 to the left.
constexpr int kWinW = 24;
constexpr int kWinH = 16;
constexpr float kWinWDeg = 4.0f;
constexpr float kWinHDeg = 3.0f;
constexpr float kPitchXDeg = kWinWDeg / kWinW;  // 0.1667 deg: also the candidate step
constexpr float kPitchYDeg = kWinHDeg / kWinH;  // 0.1875 deg
constexpr float kHalfPitchXDeg = 0.5f * kPitchXDeg;  // each window moves this far per candidate
constexpr int kCandidates = 128;
constexpr int kZeroCandidate = 16;              // covers -2.7 .. +18.5 deg
// Each eye's samples per row, on a half-pitch grid (strip column k):
constexpr int kHalfStripW = 2 * (kWinW - 1) + kCandidates;                       // 174
constexpr int kLeftStripOff = (kWinW - 1) + kZeroCandidate;                      // 39
constexpr int kRightStripOff = (kCandidates - 1) + (kWinW - 1) - kZeroCandidate;  // 134
// The shader's shared memory (both strips, then per candidate a score, a variance, a flag);
// the renderer leaves the measurement off on a device that offers less.
constexpr uint32_t kSharedBytes = 4u * (2u * kWinH * kHalfStripW + 3u * kCandidates);

// Sample positions (both the CPU reference and the compute shader use exactly these):
//   left  strip column k, row j: lon = lon0 + (k - kLeftStripOff) * kHalfPitchXDeg
//   right strip column k, row j: lon = lon0 + (k - kRightStripOff) * kHalfPitchXDeg
//                                lat = lat0 - (j - (kWinH - 1) / 2) * kPitchYDeg (both)
//   candidate c, window column i: left column 2 i + c, right column 2 i - c + (kCandidates - 1)
// That is left lon = lon0 + s / 2 + (i - (kWinW - 1) / 2) * kPitchXDeg and right lon = that - s,
// s = (c - kZeroCandidate) * kPitchXDeg. i in [0, kWinW), j in [0, kWinH) (row 0 = top). A sample
// is read bilinearly, as luma. On 180 a sample past the picture's side edge (u outside [0, 1]
// before the half mapping; the sphere draws black there) is outside: a candidate with one in
// either window scores NaN.

// How the sphere is shown this frame (the renderer's sphere push, see the sphere draw in
// vulkan_renderer.cpp): what gazeToSource needs to repeat the shaders' ray mapping.
struct SphereView {
    float headRot[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // left eye view orientation (x, y, z, w), app space
    float zoom = 1.0f;        // sphereZoom_ (scales the view tangents)
    float pitchTotal = 0.0f;  // spherePitch_ - recenterPitch_ (radians)
    float yawTotal = 0.0f;    // sphereYaw_ + sphereYawOffset_ (radians)
    int projection = 1;       // 1 = 360, 2 = 180 (push.mode[0])
    int stereo = 1;           // 1 = side by side, 2 = top / bottom (push.mode[1]); 0 = mono
    bool swapEyes = false;    // push.mode[3]
};

// The gaze point on the source video.
struct GazeSource {
    bool valid = false;
    float lonDeg = 0.0f;  // where the (combined) gaze ray samples the unshifted sphere mapping:
                          // about midway between the fixated subject's two images
    float latDeg = 0.0f;
    int projection = 1;   // 1 = 360, 2 = 180
    int stereo = 1;       // 1 = side by side, 2 = top / bottom
    int leftHalf = 0;     // the half (0 = left / top, 1 = right / bottom) shown to the left eye
};

// Maps an eye-gaze direction (unit vector, app reference space) through the same rotation, zoom
// and lon / lat mapping as sphere.vert / sphere.frag. Not valid for mono, for a gaze behind the
// view, on 180 within 3 deg of the picture's side edges or behind it, or nearer than 10 deg to
// a pole.
GazeSource gazeToSource(const SphereView& view, const float gazeDirApp[3]);

// Full-texture UV (before the decoder-padding crop) of a source point in one half: the shaders'
// mapping, u = lon / span + 0.5 (span 180 or 360 deg; 360 wraps), v = 0.5 - lat / 180, then
// into the half (side by side: u * 0.5 + half * 0.5; top / bottom: v * 0.5 + half * 0.5).
void sourceUv(const GazeSource& g, float lonDeg, float latDeg, int half, float& u, float& v);

// The outcome of one search.
struct DisparityResult {
    bool valid = false;        // passed the gates below
    float disparityDeg = 0.0f; // refined, lonL - lonR (see the convention above)
    float score = 0.0f;        // best zero-mean normalised cross-correlation (-1 .. 1)
    float variance = 0.0f;     // of the best candidate's left window luma (0..1 units): texture
    int best = -1;             // best candidate index
};

// Gates: best score >= kMinScore, left variance >= kMinVariance (enough texture), best not at
// either end of the search (the first / last candidate, or next to a NaN one). Then a parabola
// through the best and its neighbours refines the peak: refine = 0.5 (a - c) / (a - 2 s + c),
// disparity = (best + refine - kZeroCandidate) * pitch. NaN scores are skipped.
constexpr float kMinScore = 0.6f;
constexpr float kMinVariance = 2e-4f;
DisparityResult evaluateScores(const float scores[kCandidates], float leftVariance);

// An 8-bit luma image (the full texture: both halves), read bilinearly with clamp-to-edge;
// with uvScale = the crop the shaders apply on the direct path (1 = none).
struct LumaImage {
    int width = 0;
    int height = 0;
    int stride = 0;  // bytes per row
    const uint8_t* data = nullptr;
};

// CPU reference of the compute shader: the kCandidates scores (NaN = a window leaves the 180
// picture) and the variance of the best candidate's left window (best as evaluateScores picks
// it; 0 when none) for the gaze point g (both halves of img). Same sampling rule, same NCC.
void referenceScores(const LumaImage& img, const GazeSource& g, float uvScaleX, float uvScaleY,
                     float scoresOut[kCandidates], float& leftVarianceOut);

// Distance in metres for a disparity, with a camera baseline (metres): d = b / (2 tan(D / 2)),
// D clamped to at least 0.05 deg.
float disparityToMetres(float disparityDeg, float baselineM = 0.063f);
// The inverse: the disparity (degrees) of a point at `metres`.
float metresToDisparity(float metres, float baselineM = 0.063f);

// The push constants of shaders/stereo_disparity.comp (36 bytes, this order). One workgroup per
// dispatch; set 0 binding 0 = the sphere's video sampler, set 1 binding 0 = the FocusOut buffer
// (float scores[kCandidates]; float variance; uint serial).
struct DisparityPush {
    float lon0Deg;     // the gaze point (GazeSource)
    float lat0Deg;
    float uvScaleX;    // the decoder-padding crop (1 = none)
    float uvScaleY;
    int32_t projection;  // 1 = 360, 2 = 180
    int32_t stereo;      // 1 = side by side, 2 = top / bottom
    int32_t leftHalf;    // the half shown to the left eye (0 / 1)
    int32_t lumaOnly;    // 1 = the texture is luma (.r), 0 = RGB
    uint32_t serial;     // copied to FocusOut.serial
};
static_assert(sizeof(DisparityPush) == 36, "matches the shader's push block");

}  // namespace pixelvr::stereo
