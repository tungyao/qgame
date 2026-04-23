#pragma once
#include <cmath>

namespace core {

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;

    Vec3() = default;
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s)       const { return {x * s,   y * s,   z * s};   }
    Vec3 operator/(float s)       const { return {x / s,   y / s,   z / s};   }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }

    float dot(const Vec3& o)   const { return x*o.x + y*o.y + z*o.z; }
    Vec3  cross(const Vec3& o) const {
        return {y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x};
    }
    float lengthSq()  const { return dot(*this); }
    float length()    const { return std::sqrt(lengthSq()); }
    Vec3  normalized() const { float l = length(); return l > 1e-6f ? *this / l : Vec3{}; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

} // namespace core
