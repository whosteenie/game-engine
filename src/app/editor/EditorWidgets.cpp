#include "app/editor/EditorWidgets.h"

#include <imgui.h>

namespace EditorWidgets
{
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
}
