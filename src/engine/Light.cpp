#include "engine/Light.h"

Light::Light(const glm::vec3& position)
    : m_position(position)
{
}

const glm::vec3& Light::GetPosition() const
{
    return m_position;
}

const glm::vec3& Light::GetColor() const
{
    return m_color;
}

float Light::GetAmbientStrength() const
{
    return m_ambientStrength;
}

float Light::GetDiffuseStrength() const
{
    return m_diffuseStrength;
}

float Light::GetSpecularStrength() const
{
    return m_specularStrength;
}

float Light::GetShininess() const
{
    return m_shininess;
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

float Light::GetDiffuseWrap() const
{
    return m_diffuseWrap;
}

float Light::GetIndirectStrength() const
{
    return m_indirectStrength;
}

const glm::vec3& Light::GetFillLightDirection() const
{
    return m_fillLightDirection;
}

const glm::vec3& Light::GetFillLightColor() const
{
    return m_fillLightColor;
}

float Light::GetFillLightStrength() const
{
    return m_fillLightStrength;
}

void Light::SetPosition(const glm::vec3& position)
{
    m_position = position;
}
