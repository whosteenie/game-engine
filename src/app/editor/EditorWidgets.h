#pragma once

#include <glm/glm.hpp>

namespace EditorWidgets
{
    bool ColorEditVec3(const char* label, glm::vec3& value);
    bool SliderVec3(const char* label, glm::vec3& value, float min, float max);
}
