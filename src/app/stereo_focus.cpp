#include "app/stereo_focus.hpp"

#include <algorithm>
#include <cmath>

namespace pixelvr {

namespace {

constexpr float kMaxDtS = 0.1f;           // a longer frame (a hitch) counts as this long
constexpr float kInvalidReleaseS = 4.0f;   // invalid measurements this long release
constexpr float kSpringOmega95 = 4.74f;    // (1 + w t) e^(-w t) = 0.05 at w t = 4.74

float clampOr(float v, float lo, float hi, float fallback) {
    if (!std::isfinite(v)) return fallback;
    return std::clamp(v, lo, hi);
}

}  // namespace

void StereoFocus::setParams(const StereoFocusParams& p) {
    const StereoFocusParams d;
    params_.strength = clampOr(p.strength, 0.0f, 1.0f, d.strength);
    params_.closerThanM = clampOr(p.closerThanM, 0.1f, 3.0f, d.closerThanM);
    params_.hysteresis = clampOr(p.hysteresis, 0.0f, 1.0f, d.hysteresis);
    params_.transitionS = clampOr(p.transitionS, 0.0f, 10.0f, d.transitionS);
    params_.capDeg = clampOr(p.capDeg, 0.0f, 20.0f, d.capDeg);
    params_.comfortDeg = clampOr(p.comfortDeg, 0.0f, 20.0f, d.comfortDeg);
    params_.baselineM = clampOr(p.baselineM, 0.01f, 0.5f, d.baselineM);
    params_.nearOnly = p.nearOnly;

    // A new strength or cap applies to the current focus at once (through the spring).
    if (focused_) {
        if (params_.strength <= 0.0f) {
            release();
        } else if (params_.nearOnly && focusDistanceM_ > params_.closerThanM * (1.0f + params_.hysteresis)) {
            release();  // switched to Near Only with a subject beyond it
        } else {
            target_ = targetFor(focusDistanceM_);
        }
    }
}

void StereoFocus::reset() {
    offset_ = 0.0f;
    velocity_ = 0.0f;
    target_ = 0.0f;
    focused_ = false;
    focusDistanceM_ = 0.0f;
    lastDisparity_ = 0.0f;
    gazeLostS_ = 0.0f;
    invalidS_ = 0.0f;
    invalidRun_ = false;
    focusDisparity_ = 0.0f;
    recent_.fill(0.0f);
    recentCount_ = 0;
    recentNext_ = 0;
    steady_ = 0.0f;
    fartherPending_ = false;
    fartherS_ = 0.0f;
    event_ = Event::None;
}

float StereoFocus::targetFor(float distanceM) const {
    const float disparity = stereo::metresToDisparity(distanceM, params_.baselineM);
    const float t = params_.strength * std::max(0.0f, disparity - params_.comfortDeg);
    return std::min(t, params_.capDeg);
}

void StereoFocus::release() {
    focused_ = false;
    target_ = 0.0f;
    focusDistanceM_ = 0.0f;
    focusDisparity_ = 0.0f;
    invalidS_ = 0.0f;
    invalidRun_ = false;
    fartherPending_ = false;
    fartherS_ = 0.0f;
}

void StereoFocus::update(float dtS, const stereo::DisparityResult* measurement, bool gazeOnPicture) {
    const float dt = std::isfinite(dtS) ? std::clamp(dtS, 0.0f, kMaxDtS) : 0.0f;
    event_ = Event::None;

    // Gaze off the picture (or not tracked, a blink): the focus holds. A measurement that
    // arrives meanwhile was taken for an earlier gaze point and is not used.
    if (gazeOnPicture) {
        gazeLostS_ = 0.0f;
    } else {
        gazeLostS_ += dt;
        measurement = nullptr;
    }

    const bool valid = measurement && measurement->valid && std::isfinite(measurement->disparityDeg);
    // Farther readings in a row: their time counts every frame (results come every few frames),
    // not while the gaze is off the picture.
    if (fartherPending_ && gazeOnPicture) fartherS_ += dt;
    if (valid) {
        invalidRun_ = false;
        invalidS_ = 0.0f;
        lastDisparity_ = measurement->disparityDeg;
        // Single readings jump by several degrees when the window takes in the subject's edge
        // and the background (field test: 30-39 % of them by over 3 deg); the median of the last
        // few does not.
        recent_[static_cast<std::size_t>(recentNext_)] = measurement->disparityDeg;
        recentNext_ = (recentNext_ + 1) % kMedianOf;
        recentCount_ = std::min(recentCount_ + 1, kMedianOf);
        std::array<float, kMedianOf> sorted = recent_;
        std::sort(sorted.begin(), sorted.begin() + recentCount_);
        steady_ = (recentCount_ % 2 == 1)
                      ? sorted[static_cast<std::size_t>(recentCount_ / 2)]
                      : 0.5f * (sorted[static_cast<std::size_t>(recentCount_ / 2 - 1)] +
                                sorted[static_cast<std::size_t>(recentCount_ / 2)]);
        const float disparity = steady_;
        const float distance = stereo::disparityToMetres(disparity, params_.baselineM);
        const float h = params_.hysteresis;
        const float band = std::max(kRetargetDeg, h * std::fabs(focusDisparity_));
        auto focusOn = [&](Event e) {
            focused_ = true;
            focusDisparity_ = disparity;
            focusDistanceM_ = distance;
            target_ = targetFor(distance);
            fartherPending_ = false;
            fartherS_ = 0.0f;
            event_ = e;
        };
        if (params_.strength <= 0.0f) {
            if (focused_) {
                release();
                event_ = Event::ReleaseStrength;
            }
        } else if (!focused_) {
            if (!params_.nearOnly || distance <= params_.closerThanM) focusOn(Event::Engage);
        } else if (disparity > focusDisparity_ + band) {
            focusOn(Event::Retarget);  // nearer: at once
        } else if (disparity < focusDisparity_ - band ||
                   (params_.nearOnly && distance > params_.closerThanM * (1.0f + h))) {
            // Farther (or beyond Near Only's limit): only once it held for kFartherHoldS.
            if (!fartherPending_) {
                fartherPending_ = true;
                fartherS_ = 0.0f;
            }
            if (fartherS_ >= kFartherHoldS) {
                if (params_.nearOnly && distance > params_.closerThanM * (1.0f + h)) {
                    release();
                    event_ = Event::ReleaseFar;
                } else {
                    focusOn(Event::Retarget);
                }
            }
        } else {
            fartherPending_ = false;  // back within the band: the subject is still there
            fartherS_ = 0.0f;
        }
    } else if (measurement) {
        // Invalid: the time counts from the first invalid result of a run, every frame, since
        // results arrive only every few frames.
        if (!invalidRun_) {
            invalidRun_ = true;
            invalidS_ = 0.0f;
        } else {
            invalidS_ += dt;
        }
    } else if (invalidRun_) {
        invalidS_ += dt;
    }
    if (focused_ && invalidRun_ && invalidS_ >= kInvalidReleaseS) {
        release();
        event_ = Event::ReleaseInvalid;
    }

    // Critically damped spring towards the target, exact step (stable for any dt).
    if (params_.transitionS <= 0.0f) {
        offset_ = target_;
        velocity_ = 0.0f;
    } else if (dt > 0.0f) {
        const float w = kSpringOmega95 / params_.transitionS;
        const float x0 = offset_ - target_;
        const float v0 = velocity_;
        const float e = std::exp(-w * dt);
        const float k = v0 + w * x0;
        offset_ = target_ + (x0 + k * dt) * e;
        velocity_ = (v0 - w * k * dt) * e;
        // Settled: land exactly, so the debug line and the push show the target.
        if (std::fabs(offset_ - target_) < 1e-4f && std::fabs(velocity_) < 1e-3f) {
            offset_ = target_;
            velocity_ = 0.0f;
        }
    }
    if (!std::isfinite(offset_) || !std::isfinite(velocity_)) {
        offset_ = target_;
        velocity_ = 0.0f;
    }
    if (offset_ < 0.0f) {
        offset_ = 0.0f;
        velocity_ = std::max(0.0f, velocity_);
    } else if (offset_ > params_.capDeg) {
        offset_ = params_.capDeg;
        velocity_ = std::min(0.0f, velocity_);
    }
}

}  // namespace pixelvr
