#pragma once
#include "Vec2.h"

namespace core {

struct Rect {
    float x = 0.f, y = 0.f;
    float w = 0.f, h = 0.f;

    Rect() = default;
    Rect(float x, float y, float w, float h) : x(x), y(y), w(w), h(h) {}

    float left()   const { return x; }
    float right()  const { return x + w; }
    float top()    const { return y; }
    float bottom() const { return y + h; }
    Vec2  center() const { return {x + w * 0.5f, y + h * 0.5f}; }

    bool contains(Vec2 p)      const { return p.x >= x && p.x < right() && p.y >= y && p.y < bottom(); }
    bool overlaps(const Rect& o) const {
        return right() > o.x && x < o.right() && bottom() > o.y && y < o.bottom();
    }

    Rect intersect(const Rect& o) const {
        float l = x   > o.x   ? x   : o.x;
        float t = y   > o.y   ? y   : o.y;
        float r = right()  < o.right()  ? right()  : o.right();
        float b = bottom() < o.bottom() ? bottom() : o.bottom();
        if (r <= l || b <= t) return {};
        return {l, t, r - l, b - t};
    }
};

} // namespace core
