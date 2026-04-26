#pragma once
#include <cmath>
#include <cstdint>

namespace engine {

// Phase 5.1: Easing 函数表
enum class Easing : uint8_t {
    Linear,
    QuadIn,    QuadOut,    QuadInOut,
    CubicIn,   CubicOut,   CubicInOut,
    QuartIn,   QuartOut,   QuartInOut,
    SineIn,    SineOut,    SineInOut,
    ExpoIn,    ExpoOut,    ExpoInOut,
    BackIn,    BackOut,    BackInOut,
    ElasticIn, ElasticOut, ElasticInOut,
    BounceIn,  BounceOut,  BounceInOut,
};

namespace easing_detail {
    constexpr float kPi = 3.14159265358979323846f;
    inline float bounceOut(float t) {
        constexpr float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1.f / d1)        return n1 * t * t;
        else if (t < 2.f / d1) { t -= 1.5f / d1;  return n1 * t * t + 0.75f; }
        else if (t < 2.5f / d1){ t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        else                   { t -= 2.625f / d1;return n1 * t * t + 0.984375f; }
    }
}

// 输入 t ∈ [0,1]，返回缓动后的 t (可能略超出 [0,1] — Back/Elastic)
inline float applyEasing(Easing e, float t) {
    using namespace easing_detail;
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;

    switch (e) {
    case Easing::Linear:       return t;

    case Easing::QuadIn:       return t * t;
    case Easing::QuadOut:      return 1.f - (1.f - t) * (1.f - t);
    case Easing::QuadInOut:    return t < 0.5f ? 2.f * t * t : 1.f - 0.5f * (2.f - 2.f * t) * (2.f - 2.f * t);

    case Easing::CubicIn:      return t * t * t;
    case Easing::CubicOut:     { float u = 1.f - t; return 1.f - u * u * u; }
    case Easing::CubicInOut:   return t < 0.5f ? 4.f * t * t * t
                                               : 1.f - std::pow(-2.f * t + 2.f, 3.f) * 0.5f;

    case Easing::QuartIn:      return t * t * t * t;
    case Easing::QuartOut:     { float u = 1.f - t; return 1.f - u * u * u * u; }
    case Easing::QuartInOut:   return t < 0.5f ? 8.f * t * t * t * t
                                               : 1.f - std::pow(-2.f * t + 2.f, 4.f) * 0.5f;

    case Easing::SineIn:       return 1.f - std::cos(t * kPi * 0.5f);
    case Easing::SineOut:      return std::sin(t * kPi * 0.5f);
    case Easing::SineInOut:    return -(std::cos(kPi * t) - 1.f) * 0.5f;

    case Easing::ExpoIn:       return std::pow(2.f, 10.f * t - 10.f);
    case Easing::ExpoOut:      return 1.f - std::pow(2.f, -10.f * t);
    case Easing::ExpoInOut:    return t < 0.5f ? std::pow(2.f, 20.f * t - 10.f) * 0.5f
                                               : (2.f - std::pow(2.f, -20.f * t + 10.f)) * 0.5f;

    case Easing::BackIn:       { constexpr float c1 = 1.70158f, c3 = c1 + 1.f;
                                 return c3 * t * t * t - c1 * t * t; }
    case Easing::BackOut:      { constexpr float c1 = 1.70158f, c3 = c1 + 1.f; float u = t - 1.f;
                                 return 1.f + c3 * u * u * u + c1 * u * u; }
    case Easing::BackInOut:    { constexpr float c1 = 1.70158f, c2 = c1 * 1.525f;
                                 return t < 0.5f
                                    ? (std::pow(2.f * t, 2.f) * ((c2 + 1.f) * 2.f * t - c2)) * 0.5f
                                    : (std::pow(2.f * t - 2.f, 2.f) * ((c2 + 1.f) * (t * 2.f - 2.f) + c2) + 2.f) * 0.5f; }

    case Easing::ElasticIn:    { constexpr float c4 = (2.f * kPi) / 3.f;
                                 return -std::pow(2.f, 10.f * t - 10.f) * std::sin((t * 10.f - 10.75f) * c4); }
    case Easing::ElasticOut:   { constexpr float c4 = (2.f * kPi) / 3.f;
                                 return std::pow(2.f, -10.f * t) * std::sin((t * 10.f - 0.75f) * c4) + 1.f; }
    case Easing::ElasticInOut: { constexpr float c5 = (2.f * kPi) / 4.5f;
                                 return t < 0.5f
                                    ? -(std::pow(2.f, 20.f * t - 10.f) * std::sin((20.f * t - 11.125f) * c5)) * 0.5f
                                    :  (std::pow(2.f, -20.f * t + 10.f) * std::sin((20.f * t - 11.125f) * c5)) * 0.5f + 1.f; }

    case Easing::BounceIn:     return 1.f - bounceOut(1.f - t);
    case Easing::BounceOut:    return bounceOut(t);
    case Easing::BounceInOut:  return t < 0.5f ? (1.f - bounceOut(1.f - 2.f * t)) * 0.5f
                                               : (1.f + bounceOut(2.f * t - 1.f)) * 0.5f;
    }
    return t;
}

} // namespace engine
