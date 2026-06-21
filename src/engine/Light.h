#pragma once

#include <glm/glm.hpp>

enum class LightType
{
    Directional = 0,
    Point = 1,
    Spot = 2
};

class Light
{
public:
    static Light MakeDirectional(
        const glm::vec3& directionTowardLight,
        const glm::vec3& color,
        float intensity);

    static Light MakePoint(
        const glm::vec3& position,
        const glm::vec3& color,
        float intensity,
        float constantAttenuation = 1.0f,
        float linearAttenuation = 0.07f,
        float quadraticAttenuation = 0.017f,
        float range = 0.0f);

    static Light MakeSpot(
        const glm::vec3& position,
        const glm::vec3& directionTowardLight,
        const glm::vec3& color,
        float intensity,
        float innerCutoffDegrees,
        float outerCutoffDegrees,
        float constantAttenuation = 1.0f,
        float linearAttenuation = 0.07f,
        float quadraticAttenuation = 0.017f,
        float range = 0.0f);

    LightType GetType() const;
    const glm::vec3& GetPosition() const;
    const glm::vec3& GetDirection() const;
    const glm::vec3& GetColor() const;
    float GetIntensity() const;
    float GetConstantAttenuation() const;
    float GetLinearAttenuation() const;
    float GetQuadraticAttenuation() const;
    float GetRange() const;
    float GetInnerCutoffCos() const;
    float GetOuterCutoffCos() const;

    void SetDirection(const glm::vec3& directionTowardLight);
    void SetColor(const glm::vec3& color);
    void SetIntensity(float intensity);

private:
    Light(
        LightType type,
        const glm::vec3& position,
        const glm::vec3& direction,
        const glm::vec3& color,
        float intensity,
        float constantAttenuation,
        float linearAttenuation,
        float quadraticAttenuation,
        float range,
        float innerCutoffCos,
        float outerCutoffCos);

    LightType m_type;
    glm::vec3 m_position;
    glm::vec3 m_direction;
    glm::vec3 m_color;
    float m_intensity;
    float m_constantAttenuation;
    float m_linearAttenuation;
    float m_quadraticAttenuation;
    float m_range;
    float m_innerCutoffCos;
    float m_outerCutoffCos;
};
