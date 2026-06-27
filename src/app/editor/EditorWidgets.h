#pragma once

#include <glm/glm.hpp>

namespace EditorWidgets
{
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

    bool ColorEditVec3(const char* label, glm::vec3& value);
    bool SliderVec3(const char* label, glm::vec3& value, float min, float max);
}
