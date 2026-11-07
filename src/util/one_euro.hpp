#pragma once
// One Euro filter (Casiez et al. 2012) for the laser's aim ray: strong smoothing
// while the hand is nearly still (kills tremor), little lag on fast moves.
// The cutoff rises with the filtered speed: cutoff = minCutoff + beta * |speed|.

#include <algorithm>
#include <cmath>

namespace pixelvr {

struct OneEuroParams {
    float minCutoff = 1.0f;  // Hz, at rest
    float beta = 0.0f;       // Hz per unit/s of speed
    float dCutoff = 2.5f;    // Hz, for the speed estimate
};

inline float oneEuroAlpha(float cutoffHz, float dt) {
    const float tau = 1.0f / (6.2831853f * std::max(cutoffHz, 1e-3f));
    return 1.0f / (1.0f + tau / std::max(dt, 1e-5f));
}

// Filters a position (3 floats) and an orientation (unit quaternion x, y, z, w).
class OneEuroPose {
public:
    void reset() { init_ = false; }

    void filter(float pos[3], float quat[4], float dt, const OneEuroParams& p, const OneEuroParams& r) {
        if (!init_ || dt <= 0.0f || dt > 0.25f) {
            for (int k = 0; k < 3; ++k) pos_[k] = pos[k];
            for (int k = 0; k < 4; ++k) quat_[k] = quat[k];
            linSpeed_ = angSpeed_ = 0.0f;
            init_ = true;
            return;
        }
        // Position: speed in m/s.
        {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) d2 += (pos[k] - pos_[k]) * (pos[k] - pos_[k]);
            const float speed = std::sqrt(d2) / dt;
            linSpeed_ += (speed - linSpeed_) * oneEuroAlpha(p.dCutoff, dt);
            const float a = oneEuroAlpha(p.minCutoff + p.beta * linSpeed_, dt);
            for (int k = 0; k < 3; ++k) pos_[k] += (pos[k] - pos_[k]) * a;
        }
        // Orientation: angular speed in rad/s, nlerp toward the new pose.
        {
            float q[4] = {quat[0], quat[1], quat[2], quat[3]};
            float dot = q[0] * quat_[0] + q[1] * quat_[1] + q[2] * quat_[2] + q[3] * quat_[3];
            if (dot < 0.0f) {
                for (float& c : q) c = -c;
                dot = -dot;
            }
            const float ang = 2.0f * std::acos(std::min(dot, 1.0f));
            angSpeed_ += (ang / dt - angSpeed_) * oneEuroAlpha(r.dCutoff, dt);
            const float a = oneEuroAlpha(r.minCutoff + r.beta * angSpeed_, dt);
            float n = 0.0f;
            for (int k = 0; k < 4; ++k) {
                quat_[k] += (q[k] - quat_[k]) * a;
                n += quat_[k] * quat_[k];
            }
            n = std::sqrt(std::max(n, 1e-12f));
            for (float& c : quat_) c /= n;
        }
        for (int k = 0; k < 3; ++k) pos[k] = pos_[k];
        for (int k = 0; k < 4; ++k) quat[k] = quat_[k];
    }

private:
    bool init_ = false;
    float pos_[3] = {0.0f, 0.0f, 0.0f};
    float quat_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float linSpeed_ = 0.0f;
    float angSpeed_ = 0.0f;
};

}  // namespace pixelvr
