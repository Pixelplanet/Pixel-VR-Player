#pragma once

// 3D Autofocus: steadies the raw eye-gaze direction. The eyes never hold perfectly still and
// the tracker adds noise of its own, so the raw direction trembles by a degree or more around
// what you look at. A One Euro filter on the direction: strong smoothing while the gaze rests
// (a fixation), little lag on a saccade (the cutoff rises with the angular speed). A blink or a
// short tracking drop out keeps the last direction for up to kHoldS.
// Also measures, for the info line only, how often the tracker delivers a new sample and how
// much the raw direction trembles. Nothing here is logged or stored.

#include "util/one_euro.hpp"

#include <algorithm>
#include <cmath>

namespace pixelvr {

class GazeFilter {
public:
    static constexpr float kHoldS = 0.3f;

    void reset() {
        valid_ = init_ = false;
        lostS_ = 0.0f;
        speed_ = 0.0f;
    }

    // Once per frame with the raw gaze (unit direction, app space).
    void update(bool rawValid, const float raw[3], float dtS) {
        dtS = std::clamp(std::isfinite(dtS) ? dtS : 0.0f, 0.0f, 0.25f);
        validShare_ += ((rawValid ? 1.0f : 0.0f) - validShare_) * oneEuroAlpha(0.5f, dtS);
        if (!rawValid) {
            lostS_ += dtS;
            if (lostS_ > kHoldS) reset();
            return;
        }
        lostS_ = 0.0f;
        // A new tracker sample (the raw direction changed): its rate, for the info line.
        const bool fresh = !init_ || angleDeg(raw, last_) > 1e-4f;
        sinceSampleS_ += dtS;
        if (fresh && init_) {
            rateHz_ += (1.0f / std::max(sinceSampleS_, 1e-3f) - rateHz_) * oneEuroAlpha(0.5f, sinceSampleS_);
            sinceSampleS_ = 0.0f;
        }
        for (int k = 0; k < 3; ++k) last_[k] = raw[k];
        if (!init_ || dtS <= 0.0f) {
            if (!init_) {
                for (int k = 0; k < 3; ++k) dir_[k] = raw[k];
                init_ = true;
            }
            valid_ = true;
            return;
        }
        const float off = angleDeg(raw, dir_);  // raw vs filtered
        speed_ += (off / dtS - speed_) * oneEuroAlpha(params_.dCutoff, dtS);
        const float a = oneEuroAlpha(params_.minCutoff + params_.beta * speed_, dtS);
        float n = 0.0f;
        for (int k = 0; k < 3; ++k) {
            dir_[k] += (raw[k] - dir_[k]) * a;
            n += dir_[k] * dir_[k];
        }
        n = std::sqrt(std::max(n, 1e-12f));
        for (float& c : dir_) c /= n;
        // Tremble: RMS of raw vs filtered while resting (saccades left out), over about a second.
        if (off < 3.0f) noise2_ += (off * off - noise2_) * oneEuroAlpha(0.3f, dtS);
        valid_ = true;
    }

    bool valid() const { return valid_; }
    const float* dir() const { return dir_; }
    float sampleRateHz() const { return rateHz_; }
    float noiseDeg() const { return std::sqrt(std::max(noise2_, 0.0f)); }
    float validShare() const { return validShare_; }

private:
    static float angleDeg(const float a[3], const float b[3]) {
        const float d = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0f, 1.0f);
        return std::acos(d) * 57.2957795f;
    }

    // Degrees: at rest 0.7 Hz (steady), a saccade at 300 deg/s about 16 Hz (follows at once).
    OneEuroParams params_{0.7f, 0.05f, 2.0f};
    bool init_ = false;
    bool valid_ = false;
    float dir_[3] = {0.0f, 0.0f, -1.0f};
    float last_[3] = {0.0f, 0.0f, -1.0f};
    float speed_ = 0.0f;
    float lostS_ = 0.0f;
    float sinceSampleS_ = 0.0f;
    float rateHz_ = 0.0f;
    float noise2_ = 0.0f;
    float validShare_ = 0.0f;
};

}  // namespace pixelvr
