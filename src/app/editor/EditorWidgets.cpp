#include "app/editor/EditorWidgets.h"

#include <imgui.h>

#include <cstdarg>

namespace EditorWidgets
{
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
