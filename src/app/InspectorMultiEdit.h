#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

struct MultiBool
{
    bool value = false;
    bool hasMixed = false;

    static MultiBool Collect(const std::vector<bool>& values);
    static MultiBool Collect(const bool* values, std::size_t count);
};

struct MultiVec3
{
    glm::vec3 value = glm::vec3(0.0f);
    bool mixed[3] = {false, false, false};

    static MultiVec3 Collect(const glm::vec3* values, std::size_t count, float epsilon = 1e-4f);
    bool HasAnyMixed() const;
    void ClearMixed();
};

// Multi-edit widgets. Mixed fields show a placeholder until edited; changes apply as absolute
// values to every target in the batch.
bool DrawMultiCheckbox(const char* label, MultiBool& field);
bool DrawMultiVec3Row(
    const char* label,
    MultiVec3& field,
    const glm::vec3& resetValue,
    float dragSpeed,
    const char* format = "%.2f");
