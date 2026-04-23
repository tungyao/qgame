#pragma once
#include <cstdint>

namespace core {

struct Color {
    uint8_t r = 255, g = 255, b = 255, a = 255;

    Color() = default;
    Color(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) : r(r), g(g), b(b), a(a) {}

    static const Color White;
    static const Color Black;
    static const Color Transparent;
    static const Color Red;
    static const Color Green;
    static const Color Blue;

    uint32_t toRGBA() const {
        return (uint32_t(r) << 24) | (uint32_t(g) << 16) | (uint32_t(b) << 8) | a;
    }
};

inline const Color Color::White       {255, 255, 255, 255};
inline const Color Color::Black       {  0,   0,   0, 255};
inline const Color Color::Transparent {  0,   0,   0,   0};
inline const Color Color::Red         {255,   0,   0, 255};
inline const Color Color::Green       {  0, 255,   0, 255};
inline const Color Color::Blue        {  0,   0, 255, 255};

} // namespace core
