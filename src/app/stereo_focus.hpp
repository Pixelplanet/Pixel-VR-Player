#pragma once

// Stereo autofocus, the deciding half (docs/concepts/AUTOFOCUS_PLAN_2026-11-19.md): from the
// measured disparity where the user looks, how far to slide the two eye images against each
// other so a near subject fuses at a comfortable depth, smoothed so it never jumps.
// Pure C++, no rendering or OpenXR; tools/stereo_focus_test.cpp drives it headlessly.

#include "gfx/stereo_disparity.hpp"

#include <array>

namespace pixelvr {

struct StereoFocusParams {
    float strength = 0.7f;      // 0..1: share of the excess disparity taken away
    float closerThanM = 0.6f;   // engage only for a subject nearer than this
    float hysteresis = 0.15f;   // release beyond closerThan * (1 + hysteresis); re-target only on
                                // a distance change larger than hysteresis * current distance
    float transitionS = 1.0f;   // ~95 % of the way to the target after this long; 0 = snap
    float capDeg = 12.0f;       // most offset ever applied (field test: close-ups needed 6.4-8.7 deg)
    float comfortDeg = 3.6f;    // the disparity a focused subject is brought to (about 1 m)
    float baselineM = 0.063f;   // assumed camera baseline
    // true = "Near Only": engage only nearer than closerThanM, release beyond it (+ hysteresis).
    // false = "Always" (the setting's default): follow every valid reading; the target is 0
    // beyond about 1 m anyway (comfortDeg), so far subjects simply get no shift.
    bool nearOnly = true;
};

class StereoFocus {
public:
    void setParams(const StereoFocusParams& p);
    const StereoFocusParams& params() const { return params_; }

    // Once per rendered frame. `measurement` = a new search result this frame, else nullptr.
    // `gazeOnPicture` = the gaze is tracked and lands on the picture (GazeSource::valid).
    // A gaze lost or off the picture holds the focus (its measurements are not used); a run of
    // invalid measurements (low texture, no clear peak: faces, blur) holds the target for 4 s,
    // then releases it.
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
    // What the latest update decided (for the autofocus log).
    enum class Event { None, Engage, Retarget, ReleaseFar, ReleaseInvalid, ReleaseStrength };
    Event lastEvent() const { return event_; }
    float velocityDegS() const { return velocity_; }
    float invalidS() const { return invalidRun_ ? invalidS_ : 0.0f; }
    // The steadied reading the decisions use: the median of the last kMedianOf valid ones.
    float steadyDisparityDeg() const { return steady_; }
    static constexpr int kMedianOf = 5;
    // A change of the focused disparity smaller than this (or than hysteresis x it) is noise.
    static constexpr float kRetargetDeg = 1.5f;
    // Nearer is taken at once; farther (or a release) only after this long of farther readings,
    // so a glance past the subject's edge onto the background does not drop the focus.
    static constexpr float kFartherHoldS = 0.8f;
    float gazeLostS() const { return gazeLostS_; }

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
    float focusDisparity_ = 0.0f;  // the steadied disparity the target was set from
    std::array<float, kMedianOf> recent_{};  // the last valid readings (ring)
    int recentCount_ = 0;
    int recentNext_ = 0;
    float steady_ = 0.0f;
    bool fartherPending_ = false;  // the latest steadied reading is farther than the focus
    float fartherS_ = 0.0f;
    Event event_ = Event::None;
};

}  // namespace pixelvr
