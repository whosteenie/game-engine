#pragma once

#include <glm/glm.hpp>

class Light
{
public:
    explicit Light(const glm::vec3& position);

    const glm::vec3& GetPosition() const;
    const glm::vec3& GetColor() const;
    float GetAmbientStrength() const;
    float GetDiffuseStrength() const;
    float GetSpecularStrength() const;
    float GetShininess() const;
    float GetConstantAttenuation() const;
    float GetLinearAttenuation() const;
    float GetQuadraticAttenuation() const;
    float GetDiffuseWrap() const;
    float GetIndirectStrength() const;
    const glm::vec3& GetFillLightDirection() const;
    const glm::vec3& GetFillLightColor() const;
    float GetFillLightStrength() const;

    void SetPosition(const glm::vec3& position);

private:
    glm::vec3 m_position;
    glm::vec3 m_color{1.0f, 0.97f, 0.92f};
    float m_ambientStrength = 0.05f;
    float m_diffuseStrength = 1.0f;
    float m_specularStrength = 0.45f;
    float m_shininess = 64.0f;
    float m_constantAttenuation = 1.0f;
    float m_linearAttenuation = 0.07f;
    float m_quadraticAttenuation = 0.017f;
    float m_diffuseWrap = 0.35f;
    float m_indirectStrength = 0.12f;

    // Direction toward the fill light source (at infinity).
    glm::vec3 m_fillLightDirection{glm::normalize(glm::vec3(-0.4f, 0.25f, -0.55f))};
    glm::vec3 m_fillLightColor{0.65f, 0.72f, 0.9f};
    float m_fillLightStrength = 0.3f;
};
