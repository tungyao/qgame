#pragma once
#include "Vec2.h"
#include <cmath>

namespace core {

// 行主序 3x3 矩阵，用于 2D 仿射变换
struct Mat3 {
    float m[3][3] = {};

    static Mat3 identity() {
        Mat3 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.f;
        return r;
    }

    static Mat3 translation(float tx, float ty) {
        Mat3 r = identity();
        r.m[0][2] = tx;
        r.m[1][2] = ty;
        return r;
    }

    static Mat3 rotation(float radians) {
        float c = std::cos(radians), s = std::sin(radians);
        Mat3 r = identity();
        r.m[0][0] =  c; r.m[0][1] = -s;
        r.m[1][0] =  s; r.m[1][1] =  c;
        return r;
    }

    static Mat3 scale(float sx, float sy) {
        Mat3 r = identity();
        r.m[0][0] = sx;
        r.m[1][1] = sy;
        return r;
    }

    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    r.m[i][j] += m[i][k] * o.m[k][j];
        return r;
    }

    Vec2 transformPoint(Vec2 p) const {
        float x = m[0][0]*p.x + m[0][1]*p.y + m[0][2];
        float y = m[1][0]*p.x + m[1][1]*p.y + m[1][2];
        float w = m[2][0]*p.x + m[2][1]*p.y + m[2][2];
        return {x / w, y / w};
    }

    Vec2 transformVector(Vec2 v) const {
        return {m[0][0]*v.x + m[0][1]*v.y,
                m[1][0]*v.x + m[1][1]*v.y};
    }
};

} // namespace core
