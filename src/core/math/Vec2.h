#pragma once
#include <cmath>

namespace core {

struct Vec2 {
    float x = 0.f, y = 0.f;

    Vec2() = default;
    Vec2(float x, float y) : x(x), y(y) {}

    Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(float s)       const { return {x * s,   y * s};   }
    Vec2 operator/(float s)       const { return {x / s,   y / s};   }

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s)       { x *= s;   y *= s;   return *this; }

    float dot(const Vec2& o)  const { return x * o.x + y * o.y; }
    float lengthSq()          const { return dot(*this); }
    float length()            const { return std::sqrt(lengthSq()); }
    Vec2  normalized()        const { float l = length(); return l > 1e-6f ? *this / l : Vec2{}; }

    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2& o) const { return !(*this == o); }
};

inline Vec2 operator*(float s, const Vec2& v) { return v * s; }

} // namespace core
