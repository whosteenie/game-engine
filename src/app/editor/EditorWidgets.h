#pragma once

#include <glm/glm.hpp>

#include <imgui.h>

#include <string>

namespace EditorWidgets
{
    ImVec4 ErrorTextColor();
    void DrawErrorText(const std::string& message);
    void TextColoredError(const char* fmt, ...) IM_FMTARGS(1);
    // ImGui and printf can display "-0" for negative zero; normalize for UI.
    inline float SanitizeSignedZero(float value)
    {
        return value + 0.0f;
    }

    inline void SanitizeSignedZero(glm::vec3& value)
    {
        value.x = SanitizeSignedZero(value.x);
        value.y = SanitizeSignedZero(value.y);
        value.z = SanitizeSignedZero(value.z);
    }

    // DragFloat with Ctrl+Click / typed entry that blocks non-numeric keystrokes.
    bool DragFloat(
        const char* label,
        float* value,
        float speed = 1.0f,
        float min = 0.0f,
        float max = 0.0f,
        const char* format = "%.3f",
        ImGuiSliderFlags flags = 0);
    bool DragFloat3(
        const char* label,
        float value[3],
        float speed = 1.0f,
        float min = 0.0f,
        float max = 0.0f,
        const char* format = "%.3f",
        ImGuiSliderFlags flags = 0);
    bool DragInt(
        const char* label,
        int* value,
        float speed = 1.0f,
        int min = 0,
        int max = 0,
        const char* format = "%d",
        ImGuiSliderFlags flags = 0);

    bool ColorEditVec3(const char* label, glm::vec3& value);
    bool SliderVec3(const char* label, glm::vec3& value, float min, float max);

    struct TextWrapScope
    {
        TextWrapScope();
        ~TextWrapScope();

        TextWrapScope(const TextWrapScope&) = delete;
        TextWrapScope& operator=(const TextWrapScope&) = delete;

    private:
        bool m_active = false;
    };

    void TextWrappedDisabled(const char* text);
}
