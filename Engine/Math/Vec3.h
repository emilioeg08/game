#pragma once

#include "Engine/Core/Types.h"

#include <cmath>

namespace gx {

// Double-precision 3D vector.
//
// Deterministic code paths must stick to + - * / and sqrt, which IEEE-754 requires to be correctly rounded.
// Transcendental functions (sin, cos, exp, pow, ...) differ between C runtimes and must not feed simulation
// state that has to reproduce across platforms.
struct Vec3d {
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;

    constexpr Vec3d() = default;
    constexpr Vec3d(f64 px, f64 py, f64 pz) : x(px), y(py), z(pz) {}

    constexpr Vec3d operator+(const Vec3d& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3d operator-(const Vec3d& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3d operator-() const { return {-x, -y, -z}; }
    constexpr Vec3d operator*(f64 s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3d operator/(f64 s) const { return {x / s, y / s, z / s}; }

    constexpr Vec3d& operator+=(const Vec3d& o) {
        x += o.x;
        y += o.y;
        z += o.z;
        return *this;
    }
    constexpr Vec3d& operator-=(const Vec3d& o) {
        x -= o.x;
        y -= o.y;
        z -= o.z;
        return *this;
    }
    constexpr Vec3d& operator*=(f64 s) {
        x *= s;
        y *= s;
        z *= s;
        return *this;
    }

    constexpr bool operator==(const Vec3d&) const = default;
};

constexpr Vec3d operator*(f64 s, const Vec3d& v) {
    return v * s;
}

constexpr f64 dot(const Vec3d& a, const Vec3d& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3d cross(const Vec3d& a, const Vec3d& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

constexpr f64 lengthSquared(const Vec3d& v) {
    return dot(v, v);
}

inline f64 length(const Vec3d& v) {
    return std::sqrt(lengthSquared(v));
}

} // namespace gx
