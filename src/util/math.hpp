#pragma once

// Minimal, self-contained linear algebra for VR rendering.
// Matrices are 4x4, stored column-major to match Vulkan/GLSL memory layout.

#include <cmath>

namespace pixelvr::math {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

struct Mat4 {
    // Column-major: m[col * 4 + row].
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

inline Mat4 identity() {
    return Mat4{};
}

inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[col * 4 + k];
            }
            r.m[col * 4 + row] = sum;
        }
    }
    return r;
}

inline Mat4 translation(const Vec3& t) {
    Mat4 r;
    r.m[12] = t.x;
    r.m[13] = t.y;
    r.m[14] = t.z;
    return r;
}

inline Mat4 scaling(const Vec3& s) {
    Mat4 r;
    r.m[0] = s.x;
    r.m[5] = s.y;
    r.m[10] = s.z;
    return r;
}

inline Mat4 fromQuat(const Quat& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    Mat4 r;
    r.m[0] = 1.0f - 2.0f * (yy + zz);
    r.m[1] = 2.0f * (xy + wz);
    r.m[2] = 2.0f * (xz - wy);
    r.m[3] = 0.0f;

    r.m[4] = 2.0f * (xy - wz);
    r.m[5] = 1.0f - 2.0f * (xx + zz);
    r.m[6] = 2.0f * (yz + wx);
    r.m[7] = 0.0f;

    r.m[8] = 2.0f * (xz + wy);
    r.m[9] = 2.0f * (yz - wx);
    r.m[10] = 1.0f - 2.0f * (xx + yy);
    r.m[11] = 0.0f;

    r.m[12] = r.m[13] = r.m[14] = 0.0f;
    r.m[15] = 1.0f;
    return r;
}

// Rigid body transform T * R (rotate then translate), as used for a headset pose.
inline Mat4 rigidTransform(const Quat& orientation, const Vec3& position) {
    Mat4 r = fromQuat(orientation);
    r.m[12] = position.x;
    r.m[13] = position.y;
    r.m[14] = position.z;
    return r;
}

// Inverse of a rigid body transform (rotation + translation only). This is the
// view matrix for a given eye pose.
inline Mat4 rigidInverse(const Mat4& t) {
    Mat4 r;
    // Transpose the 3x3 rotation.
    r.m[0] = t.m[0];  r.m[1] = t.m[4];  r.m[2] = t.m[8];   r.m[3] = 0.0f;
    r.m[4] = t.m[1];  r.m[5] = t.m[5];  r.m[6] = t.m[9];   r.m[7] = 0.0f;
    r.m[8] = t.m[2];  r.m[9] = t.m[6];  r.m[10] = t.m[10]; r.m[11] = 0.0f;

    // -R^T * translation.
    const float tx = t.m[12], ty = t.m[13], tz = t.m[14];
    r.m[12] = -(r.m[0] * tx + r.m[4] * ty + r.m[8] * tz);
    r.m[13] = -(r.m[1] * tx + r.m[5] * ty + r.m[9] * tz);
    r.m[14] = -(r.m[2] * tx + r.m[6] * ty + r.m[10] * tz);
    r.m[15] = 1.0f;
    return r;
}

// Asymmetric perspective projection from OpenXR FOV tangents, targeting Vulkan
// clip space (Y down, depth [0, 1]). Pass the tangents of the four half-angles.
// If farZ <= nearZ, an infinite far plane is used.
inline Mat4 perspectiveVulkan(float tanLeft, float tanRight, float tanUp,
                              float tanDown, float nearZ, float farZ) {
    const float width = tanRight - tanLeft;
    // Positive-Y-down clip space (Vulkan) uses (down - up).
    const float height = tanDown - tanUp;

    Mat4 r;
    for (float& v : r.m) {
        v = 0.0f;
    }

    r.m[0] = 2.0f / width;
    r.m[8] = (tanRight + tanLeft) / width;

    r.m[5] = 2.0f / height;
    r.m[9] = (tanUp + tanDown) / height;

    if (farZ <= nearZ) {
        r.m[10] = -1.0f;
        r.m[14] = -nearZ;
    } else {
        r.m[10] = -farZ / (farZ - nearZ);
        r.m[14] = -(farZ * nearZ) / (farZ - nearZ);
    }
    r.m[11] = -1.0f;
    return r;
}

// --- Vector helpers (for controller rays and billboards) -----------------

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 operator*(const Vec3& a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& a) {
    return std::sqrt(dot(a, a));
}
inline Vec3 normalize(const Vec3& a) {
    const float l = length(a);
    return (l > 1e-8f) ? a * (1.0f / l) : a;
}

// Rotates a vector by a quaternion (v' = q * v * q^-1).
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const float s = q.w;
    return u * (2.0f * dot(u, v)) + v * (s * s - dot(u, u)) + cross(u, v) * (2.0f * s);
}

// Transforms a point / direction by a 4x4 matrix (direction ignores translation).
inline Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12],
            m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}
inline Vec3 transformDir(const Mat4& m, const Vec3& d) {
    return {m.m[0] * d.x + m.m[4] * d.y + m.m[8] * d.z,
            m.m[1] * d.x + m.m[5] * d.y + m.m[9] * d.z,
            m.m[2] * d.x + m.m[6] * d.y + m.m[10] * d.z};
}

inline Mat4 rotationX(float angle) {
    Mat4 r;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

} // namespace pixelvr::math
