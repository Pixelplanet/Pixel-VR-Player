#pragma once
// Adaptive eye render resolution. The eye swapchains keep their full size; each
// frame the app renders into a sub-rectangle of them (same size for both eyes)
// and submits that rect, so the compositor scales it to the panel. This picks
// that size from the frame timing: down quickly when frames run late, back up
// slowly once there is clear headroom.
//
// Per frame it is fed the app's "work" time: from xrWaitFrame returning to
// just before xrEndFrame, which covers the CPU recording and the waits for
// this frame's GPU work (the renderer waits for every pass before the frame
// ends), i.e. how much of the display period the app's frame needs.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace pixelvr {

class RenderScaleController {
public:
    struct Params {
        float overBudget = 0.90f;    // a frame is "over" past this share of the period
        float downOverShare = 0.25f; // step down when this share of a window is over...
        int downMissed = 2;          // ...or this many frames missed their slot in it
        float downStep = 0.90f;      // linear scale factor per step down (~19% fewer pixels)
        float upStep = 1.05f;        // linear scale factor per step up
        float upPredictMax = 0.80f;  // step up only if p90 work * pixel growth stays under this share
        double window = 0.5;         // seconds per down evaluation; also the minimum time between changes
        double upWindow = 2.0;       // seconds of steady headroom before a step up
        double upAfterDown = 4.0;    // no step up this soon after a step down
    };

    // maxW / maxH: the swapchain size; minW / minH: the floor (clamped to the max).
    void configure(int maxW, int maxH, int minW, int minH, bool fixed) {
        maxW_ = std::max(maxW, 2);
        maxH_ = std::max(maxH, 2);
        minScale_ = std::clamp(std::max(static_cast<float>(minW) / static_cast<float>(maxW_),
                                        static_cast<float>(minH) / static_cast<float>(maxH_)),
                               0.1f, 1.0f);
        fixed_ = fixed;
        scale_ = 1.0f;
        applyScale();
        resetWindows();
        haveChange_ = false;
    }

    // One rendered frame. nowSec: monotonic seconds; workMs: see above;
    // periodMs: the display period (native rate, not a throttled one);
    // missed: display slots this frame skipped (0 when on time). Returns true
    // when the size changed (for the next frame); reason() says why.
    bool update(double nowSec, double workMs, double periodMs, int missed) {
        if (fixed_ || periodMs <= 0.0) return false;
        if (winStart_ < 0.0) winStart_ = nowSec;
        if (upStart_ < 0.0) upStart_ = nowSec;
        ++winFrames_;
        if (workMs > p_.overBudget * periodMs) ++winOver_;
        // Frames that missed their slot (a single hitch counts once, however
        // many slots it skipped).
        winMissed_ += missed > 0 ? 1 : 0;
        upMissed_ += missed > 0 ? 1 : 0;
        if (upWork_.size() < 1024) upWork_.push_back(static_cast<float>(workMs));

        if (nowSec - winStart_ < p_.window) return false;
        const bool canChange = !haveChange_ || nowSec - lastChange_ >= p_.window;
        bool changed = false;
        const bool late = winMissed_ >= p_.downMissed ||
                          static_cast<float>(winOver_) >= p_.downOverShare * static_cast<float>(winFrames_);
        if (late) {
            if (canChange && scale_ > minScale_ + 1e-4f) {
                scale_ = std::max(minScale_, scale_ * p_.downStep);
                std::snprintf(reason_, sizeof(reason_), "late: %d of %d frames over %.1f ms, %d missed",
                              winOver_, winFrames_, p_.overBudget * periodMs, winMissed_);
                lastDown_ = nowSec;
                changed = true;
            }
            // Any late window restarts the wait for steady headroom.
            upWork_.clear();
            upMissed_ = 0;
            upStart_ = nowSec;
        } else if (canChange && scale_ < 1.0f - 1e-4f && nowSec - upStart_ >= p_.upWindow &&
                   (lastDown_ < 0.0 || nowSec - lastDown_ >= p_.upAfterDown) && upMissed_ <= 1 &&
                   !upWork_.empty()) {
            const std::size_t k = (upWork_.size() * 9) / 10;
            std::nth_element(upWork_.begin(), upWork_.begin() + static_cast<std::ptrdiff_t>(k), upWork_.end());
            const double p90 = upWork_[k];
            const float next = std::min(1.0f, scale_ * p_.upStep);
            const double growth = static_cast<double>(next) * next / (static_cast<double>(scale_) * scale_);
            if (p90 * growth < p_.upPredictMax * periodMs) {
                scale_ = next;
                std::snprintf(reason_, sizeof(reason_), "headroom: p90 %.1f ms of %.1f ms", p90, periodMs);
                changed = true;
            }
            upWork_.clear();
            upMissed_ = 0;
            upStart_ = nowSec;
        }
        winFrames_ = winOver_ = winMissed_ = 0;
        winStart_ = nowSec;
        if (changed) {
            applyScale();
            haveChange_ = true;
            lastChange_ = nowSec;
            // The new size's timing starts from scratch.
            upWork_.clear();
            upMissed_ = 0;
            upStart_ = nowSec;
        }
        return changed;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    float scale() const { return scale_; }
    bool fixed() const { return fixed_; }
    const char* reason() const { return reason_; }

private:
    void applyScale() {
        auto dim = [&](int full) {
            if (scale_ >= 1.0f - 1e-4f) return full;
            const int minD = static_cast<int>(std::ceil(static_cast<float>(full) * minScale_));
            int d = static_cast<int>(std::lround(static_cast<float>(full) * scale_ / 8.0f)) * 8;
            return std::clamp(d, std::min(minD, full), full);
        };
        w_ = dim(maxW_);
        h_ = dim(maxH_);
    }
    void resetWindows() {
        winStart_ = upStart_ = lastDown_ = -1.0;
        winFrames_ = winOver_ = winMissed_ = upMissed_ = 0;
        upWork_.clear();
    }

    Params p_;
    int maxW_ = 2, maxH_ = 2, w_ = 2, h_ = 2;
    float minScale_ = 1.0f, scale_ = 1.0f;
    bool fixed_ = false;
    double winStart_ = -1.0, upStart_ = -1.0, lastDown_ = -1.0, lastChange_ = 0.0;
    bool haveChange_ = false;
    int winFrames_ = 0, winOver_ = 0, winMissed_ = 0, upMissed_ = 0;
    std::vector<float> upWork_;
    char reason_[128] = "start";
};

}  // namespace pixelvr
