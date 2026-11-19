#pragma once

// Stereo autofocus, the deciding half (docs/concepts/AUTOFOCUS_PLAN_2026-11-19.md): from the
// measured disparity where the user looks, how far to slide the two eye images against each
// other so a near subject fuses at a comfortable depth, smoothed so it never jumps.
// Pure C++, no rendering or OpenXR; tools/stereo_focus_test.cpp drives it headlessly.

#include "gfx/stereo_disparity.hpp"

namespace pixelvr {

struct StereoFocusParams {
    float strength = 0.7f;      // 0..1: share of the excess disparity taken away
    float closerThanM = 0.6f;   // engage only for a subject nearer than this
    float hysteresis = 0.15f;   // release beyond closerThan * (1 + hysteresis); re-target only on
                                // a distance change larger than hysteresis * current distance
    float transitionS = 1.0f;   // ~95 % of the way to the target after this long; 0 = snap
    float capDeg = 6.0f;        // most offset ever applied
    float comfortDeg = 3.6f;    // the disparity a focused subject is brought to (about 1 m)
    float baselineM = 0.063f;   // assumed camera baseline
};

class StereoFocus {
public:
    void setParams(const StereoFocusParams& p);
    const StereoFocusParams& params() const { return params_; }

    // Once per rendered frame. `measurement` = a new search result this frame, else nullptr.
    // `gazeOnPicture` = the gaze is tracked and lands on the picture (GazeSource::valid).
    // A gaze lost or off the picture for longer than 0.5 s releases the focus (target 0); a
    // run of invalid measurements (low texture, no clear peak) holds the target for 1 s, then
    // releases it.
    void update(float dtS, const stereo::DisparityResult* measurement, bool gazeOnPicture);

    // A new clip, a seek, a format change or the feature turned off: start from 0 at once.
    void reset();
    // Paused: keep the current offset, no updates needed (the caller just stops calling update).

    // The offset to apply now, degrees (>= 0: pushes the picture farther), and the debug state.
    float offsetDeg() const { return offset_; }
    float targetDeg() const { return target_; }
    bool focused() const { return focused_; }
    float focusDistanceM() const { return focusDistanceM_; }  // last accepted distance, 0 = none
    float lastDisparityDeg() const { return lastDisparity_; }

private:
    float targetFor(float distanceM) const;  // the offset for a subject at distanceM
    void release();                           // target 0, not focused

    StereoFocusParams params_;
    float offset_ = 0.0f;          // spring position (deg)
    float velocity_ = 0.0f;        // spring velocity (deg/s)
    float target_ = 0.0f;
    bool focused_ = false;
    float focusDistanceM_ = 0.0f;
    float lastDisparity_ = 0.0f;
    float gazeLostS_ = 0.0f;
    float invalidS_ = 0.0f;
    bool invalidRun_ = false;      // the latest measurement was invalid (invalidS_ is running)
};

}  // namespace pixelvr
