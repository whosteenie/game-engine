#include "app/EditorWidgets.h"

#include <imgui.h>

namespace EditorWidgets
{
    bool ColorEditVec3(const char* label, glm::vec3& value)
    {
        return ImGui::ColorEdit3(label, &value.x);
    }

    bool SliderVec3(const char* label, glm::vec3& value, float min, float max)
    {
        return ImGui::SliderFloat3(label, &value.x, min, max);
    }
}
