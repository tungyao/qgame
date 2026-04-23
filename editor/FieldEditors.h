#pragma once
#include <imgui.h>
#include <core/math/Vec2.h>
#include <core/math/Color.h>
#include <core/math/Rect.h>

namespace editor {

inline void editFloat(float* ptr, const char* label, float step = 1.0f, float min = 0.0f, float max = 0.0f) {
    if (max > min) {
        ImGui::DragFloat(label, ptr, step, min, max);
    } else {
        ImGui::DragFloat(label, ptr, step);
    }
}

inline void editInt(int* ptr, const char* label, int step = 1, int min = 0, int max = 0) {
    if (max > min) {
        ImGui::DragInt(label, ptr, static_cast<float>(step), min, max);
    } else {
        ImGui::DragInt(label, ptr, static_cast<float>(step));
    }
}

inline void editBool(bool* ptr, const char* label) {
    ImGui::Checkbox(label, ptr);
}

inline void editVec2(core::Vec2* ptr, const char* label) {
    ImGui::DragFloat2(label, &ptr->x, 1.0f);
}

inline void editVec2(float* ptr, const char* label) {
    ImGui::DragFloat2(label, ptr, 1.0f);
}

inline void editColor(core::Color* ptr, const char* label) {
    float c[4] = {
        ptr->r / 255.0f,
        ptr->g / 255.0f,
        ptr->b / 255.0f,
        ptr->a / 255.0f
    };
    if (ImGui::ColorEdit4(label, c)) {
        ptr->r = static_cast<uint8_t>(c[0] * 255.0f);
        ptr->g = static_cast<uint8_t>(c[1] * 255.0f);
        ptr->b = static_cast<uint8_t>(c[2] * 255.0f);
        ptr->a = static_cast<uint8_t>(c[3] * 255.0f);
    }
}

inline void editColor(uint32_t* ptr, const char* label) {
    uint8_t r = (*ptr >> 24) & 0xFF;
    uint8_t g = (*ptr >> 16) & 0xFF;
    uint8_t b = (*ptr >> 8) & 0xFF;
    uint8_t a = *ptr & 0xFF;
    float c[4] = { r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f };
    if (ImGui::ColorEdit4(label, c)) {
        *ptr = (static_cast<uint32_t>(c[0] * 255.0f) << 24) |
               (static_cast<uint32_t>(c[1] * 255.0f) << 16) |
               (static_cast<uint32_t>(c[2] * 255.0f) << 8) |
               static_cast<uint32_t>(c[3] * 255.0f);
    }
}

inline void editRect(core::Rect* ptr, const char* label) {
    ImGui::DragFloat4(label, &ptr->x, 1.0f);
}

} // namespace editor