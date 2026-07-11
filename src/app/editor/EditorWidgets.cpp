#include "app/editor/EditorWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>

namespace EditorWidgets
{
    namespace
    {
        void ApplyNumericTempInputFilter(ImGuiInputTextFlags flags)
        {
            const ImGuiID id = ImGui::GetItemID();
            if (!ImGui::TempInputIsActive(id))
            {
                return;
            }

            if (ImGuiInputTextState* state = ImGui::GetInputTextState(id))
            {
                state->Flags |= flags;
            }
        }
    }

    ImVec4 ErrorTextColor()
    {
        return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
    }

    void DrawErrorText(const std::string& message)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ErrorTextColor());
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }

    void TextColoredError(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        ImGui::TextColoredV(ErrorTextColor(), fmt, args);
        va_end(args);
    }

    bool DragFloat(
        const char* label,
        float* value,
        float speed,
        float min,
        float max,
        const char* format,
        ImGuiSliderFlags flags)
    {
        SanitizeSignedZero(*value);
        const bool changed = ImGui::DragFloat(label, value, speed, min, max, format, flags);
        ApplyNumericTempInputFilter(ImGuiInputTextFlags_CharsScientific);
        if (changed)
        {
            SanitizeSignedZero(*value);
        }
        return changed;
    }

    bool DragFloat3(
        const char* label,
        float value[3],
        float speed,
        float min,
        float max,
        const char* format,
        ImGuiSliderFlags flags)
    {
        SanitizeSignedZero(value[0]);
        SanitizeSignedZero(value[1]);
        SanitizeSignedZero(value[2]);
        const bool changed = ImGui::DragFloat3(label, value, speed, min, max, format, flags);
        if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetActiveID()))
        {
            if (ImGui::TempInputIsActive(state->ID))
            {
                state->Flags |= ImGuiInputTextFlags_CharsScientific;
            }
        }
        if (changed)
        {
            SanitizeSignedZero(value[0]);
            SanitizeSignedZero(value[1]);
            SanitizeSignedZero(value[2]);
        }
        return changed;
    }

    bool DragInt(
        const char* label,
        int* value,
        float speed,
        int min,
        int max,
        const char* format,
        ImGuiSliderFlags flags)
    {
        const bool changed = ImGui::DragInt(label, value, speed, min, max, format, flags);
        ApplyNumericTempInputFilter(ImGuiInputTextFlags_CharsDecimal);
        return changed;
    }

    bool ColorEditVec3(const char* label, glm::vec3& value)
    {
        SanitizeSignedZero(value);
        const bool changed = ImGui::ColorEdit3(label, &value.x);
        if (changed)
        {
            SanitizeSignedZero(value);
        }
        return changed;
    }

    bool SliderVec3(const char* label, glm::vec3& value, float min, float max)
    {
        SanitizeSignedZero(value);
        const bool changed = ImGui::SliderFloat3(label, &value.x, min, max);
        if (changed)
        {
            SanitizeSignedZero(value);
        }
        return changed;
    }

    TextWrapScope::TextWrapScope()
    {
        const float wrapWidth = ImGui::GetCursorPos().x + ImGui::GetContentRegionAvail().x;
        if (wrapWidth > 0.0f)
        {
            ImGui::PushTextWrapPos(wrapWidth);
            m_active = true;
        }
    }

    TextWrapScope::~TextWrapScope()
    {
        if (m_active)
        {
            ImGui::PopTextWrapPos();
        }
    }

    void TextWrappedDisabled(const char* text)
    {
        const TextWrapScope wrap;
        ImGui::TextDisabled("%s", text);
    }
}
