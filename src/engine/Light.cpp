#include "engine/Light.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <cmath>

namespace
{
    glm::vec3 NormalizeOrFallback(const glm::vec3& vector, const glm::vec3& fallback)
    {
        const float length = glm::length(vector);
        if (length < 0.0001f)
        {
            return fallback;
        }

        return vector / length;
    }
}

Light::Light(
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
    float outerCutoffCos)
    : m_type(type),
      m_position(position),
      m_direction(direction),
      m_color(color),
      m_intensity(intensity),
      m_constantAttenuation(constantAttenuation),
      m_linearAttenuation(linearAttenuation),
      m_quadraticAttenuation(quadraticAttenuation),
      m_range(range),
      m_innerCutoffCos(innerCutoffCos),
      m_outerCutoffCos(outerCutoffCos)
{
}

Light Light::MakeDirectional(
    const glm::vec3& directionTowardLight,
    const glm::vec3& color,
    float intensity)
{
    return Light(
        LightType::Directional,
        glm::vec3(0.0f),
        NormalizeOrFallback(directionTowardLight, glm::vec3(0.0f, 1.0f, 0.0f)),
        color,
        intensity,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f);
}

Light Light::MakePoint(
    const glm::vec3& position,
    const glm::vec3& color,
    float intensity,
    float constantAttenuation,
    float linearAttenuation,
    float quadraticAttenuation,
    float range)
{
    return Light(
        LightType::Point,
        position,
        glm::vec3(0.0f, 1.0f, 0.0f),
        color,
        intensity,
        constantAttenuation,
        linearAttenuation,
        quadraticAttenuation,
        range,
        0.0f,
        0.0f);
}

Light Light::MakeSpot(
    const glm::vec3& position,
    const glm::vec3& directionTowardLight,
    const glm::vec3& color,
    float intensity,
    float innerCutoffDegrees,
    float outerCutoffDegrees,
    float constantAttenuation,
    float linearAttenuation,
    float quadraticAttenuation,
    float range)
{
    const float innerCutoffCos = std::cos(glm::radians(innerCutoffDegrees));
    const float outerCutoffCos = std::cos(glm::radians(outerCutoffDegrees));

    return Light(
        LightType::Spot,
        position,
        NormalizeOrFallback(directionTowardLight, glm::vec3(0.0f, -1.0f, 0.0f)),
        color,
        intensity,
        constantAttenuation,
        linearAttenuation,
        quadraticAttenuation,
        range,
        innerCutoffCos,
        outerCutoffCos);
}

LightType Light::GetType() const
{
    return m_type;
}

const glm::vec3& Light::GetPosition() const
{
    return m_position;
}

const glm::vec3& Light::GetDirection() const
{
    return m_direction;
}

const glm::vec3& Light::GetColor() const
{
    return m_color;
}

float Light::GetIntensity() const
{
    return m_intensity;
}

float Light::GetConstantAttenuation() const
{
    return m_constantAttenuation;
}

float Light::GetLinearAttenuation() const
{
    return m_linearAttenuation;
}

float Light::GetQuadraticAttenuation() const
{
    return m_quadraticAttenuation;
}

float Light::GetRange() const
{
    return m_range;
}

float Light::GetInnerCutoffCos() const
{
    return m_innerCutoffCos;
}

float Light::GetOuterCutoffCos() const
{
    return m_outerCutoffCos;
}

void Light::SetDirection(const glm::vec3& directionTowardLight)
{
    const float length = glm::length(directionTowardLight);
    if (length > 0.0001f)
    {
        m_direction = directionTowardLight / length;
    }
}

void Light::SetColor(const glm::vec3& color)
{
    m_color = color;
}

void Light::SetIntensity(float intensity)
{
    m_intensity = intensity;
}
