#pragma once

#include "engine/Light.h"

#include <glm/glm.hpp>
#include <vector>

class Shader;

class SceneLighting
{
public:
    static constexpr int MaxLights = 8;

    void AddLight(const Light& light);
    void ClearLights();

    const std::vector<Light>& GetLights() const;

    float GetAmbientStrength() const;
    void SetAmbientStrength(float ambientStrength);

    float GetIndirectStrength() const;
    void SetIndirectStrength(float indirectStrength);

    const glm::vec3& GetIndirectBounceDirection() const;
    void SetIndirectBounceDirection(const glm::vec3& direction);

    const glm::vec3& GetIndirectBounceColor() const;
    void SetIndirectBounceColor(const glm::vec3& color);

    float GetSpecularStrength() const;
    void SetSpecularStrength(float specularStrength);

    void Apply(Shader& shader) const;

private:
    std::vector<Light> m_lights;
    float m_ambientStrength = 0.05f;
    float m_indirectStrength = 0.12f;
    float m_specularStrength = 0.45f;
    glm::vec3 m_indirectBounceDirection{0.0f, 1.0f, 0.0f};
    glm::vec3 m_indirectBounceColor{1.0f, 0.97f, 0.92f};
};
