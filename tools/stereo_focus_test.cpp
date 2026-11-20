// Headless checks of the 3D Autofocus controller (src/app/stereo_focus.cpp): engage and release
// with hysteresis, the target, the spring, and the timers. Development tool, never released.
//
//   stereo_focus_test        (prints PASS / FAIL lines; exit code 1 on any failure)

#include "app/stereo_focus.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using pixelvr::StereoFocus;
using pixelvr::StereoFocusParams;
namespace stereo = pixelvr::stereo;

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}

constexpr float kDt = 1.0f / 90.0f;  // the headset's frame time
constexpr int kEvery = 3;            // a measurement every 3rd frame, as the renderer does

stereo::DisparityResult at(float metres) {
    stereo::DisparityResult r;
    r.valid = true;
    r.disparityDeg = stereo::metresToDisparity(metres);
    r.score = 0.9f;
    r.variance = 0.01f;
    r.best = 40;
    return r;
}

stereo::DisparityResult invalid() {
    stereo::DisparityResult r;
    r.valid = false;
    return r;
}

// Runs `seconds` of frames with measurement m every kEvery frames (the first frame included).
// Tracks whether the offset only rose / only fell and its extremes.
struct Trace {
    bool rising = true;
    bool falling = true;
    float maxOffset = 0.0f;
    float minOffset = 1e9f;
    bool finite = true;
};

Trace run(StereoFocus& f, float seconds, const stereo::DisparityResult* m, bool gaze = true,
          float dt = kDt) {
    Trace t;
    const int frames = static_cast<int>(std::lround(seconds / dt));
    float prev = f.offsetDeg();
    for (int i = 0; i < frames; ++i) {
        f.update(dt, (i % kEvery == 0) ? m : nullptr, gaze);
        const float o = f.offsetDeg();
        if (!std::isfinite(o)) t.finite = false;
        if (o < prev - 1e-6f) t.rising = false;
        if (o > prev + 1e-6f) t.falling = false;
        t.maxOffset = std::max(t.maxOffset, o);
        t.minOffset = std::min(t.minOffset, o);
        prev = o;
    }
    return t;
}

float expectedTarget(float metres, float strength = 0.7f) {
    return std::min(strength * std::max(0.0f, stereo::metresToDisparity(metres) - 3.6f), 12.0f);
}

bool approx(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// A controller focused on a subject at 0.4 m, settled.
StereoFocus focusedAt04(StereoFocusParams p = {}) {
    StereoFocus f;
    f.setParams(p);
    const auto m = at(0.4f);
    run(f, 5.0f, &m);
    return f;
}

}  // namespace

int main() {
    const float target04 = expectedTarget(0.4f);
    std::printf("target at 0.4 m: %.3f deg (disparity %.3f deg)\n", target04,
                stereo::metresToDisparity(0.4f));

    {  // Sanity of the distance mapping the controller relies on.
        check(approx(stereo::disparityToMetres(3.6f), 1.0f, 0.03f), "3.6 deg is about 1 m");
        check(approx(stereo::disparityToMetres(stereo::metresToDisparity(0.4f)), 0.4f, 1e-3f),
              "metresToDisparity and disparityToMetres are inverse");
    }

    {  // Far subject: nothing happens.
        StereoFocus f;
        const auto m = at(2.0f);
        run(f, 3.0f, &m);
        check(!f.focused() && f.offsetDeg() == 0.0f && f.targetDeg() == 0.0f, "far subject (2 m): no offset");
        check(approx(f.lastDisparityDeg(), stereo::metresToDisparity(2.0f), 1e-5f), "far subject: disparity reported");
    }

    {  // Near subject: engages, rises smoothly, no overshoot, ~95 % after one transition time.
        StereoFocus f;
        const auto m = at(0.4f);
        f.update(kDt, &m, true);  // engages on this frame
        check(f.focused(), "near subject (0.4 m): engages");
        check(approx(f.targetDeg(), target04, 1e-4f), "target = 0.7 * (disparity(0.4 m) - 3.6), capped at 12");
        check(approx(f.focusDistanceM(), 0.4f, 1e-3f), "focus distance reported");
        const Trace t = run(f, 1.0f - kDt, &m);
        const float share = f.offsetDeg() / target04;
        std::printf("      after 1.0 s: %.1f %% of the target\n", share * 100.0f);
        check(t.rising, "offset rises monotonically");
        check(t.maxOffset <= target04 + 1e-5f, "offset never overshoots the target");
        check(share > 0.93f && share < 0.97f, "about 95 % of the target after the transition time");
        const Trace t2 = run(f, 3.0f, &m);
        check(t2.maxOffset <= target04 + 1e-5f && approx(f.offsetDeg(), target04, 1e-3f), "settles on the target");
    }

    {  // The cap.
        StereoFocus f;
        const auto m = at(0.12f);
        run(f, 5.0f, &m);
        check(f.focused() && approx(f.targetDeg(), 12.0f, 1e-5f) && f.offsetDeg() <= 12.0f, "very near (0.12 m): capped at 12 deg");
    }

    {  // Hysteresis: 0.65 m stays focused, 0.72 m releases (closer than 0.6 m, hysteresis 15 %).
        StereoFocus f = focusedAt04();
        const auto m65 = at(0.65f);
        run(f, 1.0f, &m65);
        check(f.focused(), "focused, 0.65 m: stays focused");
        const auto m72 = at(0.72f);
        run(f, 0.5f, &m72);
        check(f.focused(), "focused, 0.72 m for 0.5 s: still holds (farther needs 0.8 s)");
        run(f, 0.7f, &m72);
        check(!f.focused() && f.targetDeg() == 0.0f, "focused, 0.72 m for 1.2 s: releases (target 0)");
        const Trace t = run(f, 3.0f, &m72);
        check(t.falling && t.minOffset >= 0.0f && approx(f.offsetDeg(), 0.0f, 1e-3f), "after release: falls smoothly to 0");

        StereoFocus g;
        run(g, 2.0f, &m65);
        check(!g.focused() && g.offsetDeg() == 0.0f, "not focused, 0.65 m: does not engage");
    }

    {  // Small jitter does not re-target.
        StereoFocus f = focusedAt04();
        const float before = f.targetDeg();
        const float beforeM = f.focusDistanceM();
        bool same = true;
        for (int i = 0; i < 300; ++i) {
            const float d = 0.4f * (1.0f + ((i % 2) ? 0.05f : -0.05f) * ((i % 7) / 6.0f));
            const auto m = at(d);
            f.update(kDt, (i % kEvery == 0) ? &m : nullptr, true);
            if (f.targetDeg() != before || f.focusDistanceM() != beforeM) same = false;
        }
        check(same && f.focused(), "+/-5 % distance jitter: no re-target");
        const auto m30 = at(0.3f);
        run(f, 0.1f, &m30);
        check(approx(f.targetDeg(), expectedTarget(0.3f), 1e-4f) && approx(f.focusDistanceM(), 0.3f, 1e-3f),
              "a real change (0.4 -> 0.3 m): re-targets");
    }

    {  // Invalid measurements: hold for 4 s, then release. Missing ones change nothing.
        StereoFocus f = focusedAt04();
        run(f, 3.0f, nullptr);
        check(f.focused() && approx(f.targetDeg(), target04, 1e-5f), "no measurements for 3 s: holds");
        const auto bad = invalid();
        run(f, 3.9f, &bad);
        check(f.focused() && approx(f.targetDeg(), target04, 1e-5f), "invalid for 3.9 s: holds the target");
        run(f, 0.2f, &bad);
        check(!f.focused() && f.targetDeg() == 0.0f, "invalid for 4.1 s: releases");

        StereoFocus g = focusedAt04();
        const auto good = at(0.4f);
        for (int k = 0; k < 5; ++k) {  // 3 s invalid, then one valid one: the timer starts over
            run(g, 3.0f, &bad);
            run(g, kDt, &good);
        }
        check(g.focused(), "invalid runs broken by valid ones: stays focused");
    }

    {  // Gaze lost or off the picture: holds, and its measurements are not used.
        StereoFocus f = focusedAt04();
        const auto far = at(2.0f);
        run(f, 3.0f, &far, false);
        check(f.focused() && approx(f.offsetDeg(), target04, 1e-3f), "gaze off the picture 3 s: holds");
        run(f, 1.2f, &far, true);
        check(!f.focused() && f.targetDeg() == 0.0f, "gaze back on a far subject: releases (Near Only)");
    }

    {  // Activation "Always": follows every valid reading, no far release (the target is 0 there).
        StereoFocusParams p;
        p.nearOnly = false;
        StereoFocus f;
        f.setParams(p);
        const auto m08 = at(0.8f);
        run(f, 5.0f, &m08);
        check(f.focused() && approx(f.targetDeg(), expectedTarget(0.8f), 1e-4f) && f.targetDeg() > 0.0f,
              "Always, 0.8 m: engages with a small target");
        const auto far = at(2.0f);
        run(f, 5.0f, &far);
        check(f.focused() && f.targetDeg() == 0.0f && approx(f.offsetDeg(), 0.0f, 1e-3f),
              "Always, 2 m: stays engaged, target and offset 0");
        const auto m04 = at(0.4f);
        run(f, 5.0f, &m04);
        check(approx(f.targetDeg(), target04, 1e-4f) && approx(f.offsetDeg(), target04, 1e-3f),
              "Always, back to 0.4 m: re-targets");
        check(f.lastEvent() == StereoFocus::Event::None, "settled: no event");
    }

    {  // Transition 0 snaps.
        StereoFocusParams p;
        p.transitionS = 0.0f;
        StereoFocus f;
        f.setParams(p);
        const auto m = at(0.4f);
        f.update(kDt, &m, true);
        check(f.offsetDeg() == f.targetDeg() && approx(f.offsetDeg(), target04, 1e-4f), "transition 0: snaps to the target");
        const auto far = at(2.0f);
        run(f, 1.2f, &far);
        check(f.offsetDeg() == 0.0f, "transition 0: snaps back on release");
    }

    {  // Strength 0: never any offset.
        StereoFocusParams p;
        p.strength = 0.0f;
        StereoFocus f;
        f.setParams(p);
        const auto m = at(0.3f);
        run(f, 3.0f, &m);
        check(!f.focused() && f.offsetDeg() == 0.0f && f.targetDeg() == 0.0f, "strength 0: no offset");

        StereoFocus g = focusedAt04();
        g.setParams(p);
        check(!g.focused() && g.targetDeg() == 0.0f, "strength set to 0 while focused: releases");
    }

    {  // Strength applies to the target.
        StereoFocusParams p;
        p.strength = 0.4f;
        StereoFocus f = focusedAt04(p);
        check(approx(f.targetDeg(), expectedTarget(0.4f, 0.4f), 1e-4f), "strength 40 %: target scales");
    }

    {  // Reset.
        StereoFocus f = focusedAt04();
        f.reset();
        check(!f.focused() && f.offsetDeg() == 0.0f && f.targetDeg() == 0.0f && f.focusDistanceM() == 0.0f,
              "reset: offset 0 at once");
        run(f, 1.0f, nullptr);
        check(f.offsetDeg() == 0.0f, "reset: stays at 0");
    }

    {  // Large and odd time steps stay stable.
        StereoFocus f;
        const auto m = at(0.4f);
        const Trace t = run(f, 10.0f, &m, true, 1.0f);
        check(t.finite && t.maxOffset <= target04 + 1e-5f && t.rising, "dt 1 s: no overshoot, no NaN");
        run(f, 3.0f, &m);  // settled first: farther readings hold for 0.8 s before it falls
        const auto far = at(2.0f);
        const Trace t2 = run(f, 10.0f, &far, true, 1.0f);
        check(t2.finite && t2.falling && t2.minOffset >= 0.0f, "dt 1 s, release: no undershoot, no NaN");
        f.update(std::nanf(""), &m, true);
        f.update(-1.0f, &m, true);
        f.update(INFINITY, &m, true);
        check(std::isfinite(f.offsetDeg()) && f.offsetDeg() >= 0.0f && f.offsetDeg() <= 12.0f, "NaN / negative / infinite dt: stays sane");
        stereo::DisparityResult nanM = at(0.4f);
        nanM.disparityDeg = std::nanf("");
        f.update(kDt, &nanM, true);
        check(std::isfinite(f.offsetDeg()) && std::isfinite(f.targetDeg()), "NaN disparity: ignored");
    }

    {  // Steadiness (field test 2026-11-19): readings that jump between the subject and the background.
        StereoFocusParams p;
        p.nearOnly = false;
        StereoFocus f;
        f.setParams(p);
        const auto nearM = at(0.4f);
        run(f, 2.0f, &nearM);
        const float settled = f.targetDeg();
        // Every third reading lands on the background (2 m): the median keeps the near subject.
        const auto farM = at(2.0f);
        bool kept = true;
        for (int i = 0; i < 900; ++i) {
            const stereo::DisparityResult* m = nullptr;
            if (i % kEvery == 0) m = ((i / kEvery) % 3 == 2) ? &farM : &nearM;
            f.update(kDt, m, true);
            if (std::fabs(f.targetDeg() - settled) > 1e-4f) kept = false;
        }
        check(kept && f.focused(), "1 reading in 3 on the background: the target holds");
        // A short glance onto the background (0.5 s of far readings) keeps the focus.
        run(f, 0.5f, &farM);
        check(approx(f.targetDeg(), settled, 1e-4f), "0.5 s glance at the background: holds");
        // Looking at the background for longer lets go.
        run(f, 1.0f, &farM);
        check(f.targetDeg() == 0.0f, "1.5 s at the background: lets go (target 0)");
        // Something near again: taken within a few readings.
        run(f, 0.15f, &nearM);
        check(approx(f.targetDeg(), settled, 1e-3f), "near again: taken at once (within 0.15 s)");
        check(approx(f.steadyDisparityDeg(), stereo::metresToDisparity(0.4f), 1e-3f), "steadied disparity reported");
    }

    {  // Parameter clamping.
        StereoFocusParams p;
        p.strength = 5.0f;
        p.closerThanM = 0.0f;
        p.hysteresis = -1.0f;
        p.transitionS = 100.0f;
        p.capDeg = 99.0f;
        StereoFocus f;
        f.setParams(p);
        const StereoFocusParams& q = f.params();
        check(q.strength == 1.0f && q.closerThanM == 0.1f && q.hysteresis == 0.0f && q.transitionS == 10.0f &&
                  q.capDeg == 20.0f,
              "setParams clamps to sane ranges");
        p = StereoFocusParams{};
        p.strength = std::nanf("");
        f.setParams(p);
        check(f.params().strength == StereoFocusParams{}.strength, "setParams: NaN falls back to the default");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
