#include "engine/Light.h"

Light::Light(const glm::vec3& position)
    : m_position(position)
{
}

const glm::vec3& Light::GetPosition() const
{
    return m_position;
}

void Light::SetPosition(const glm::vec3& position)
{
    m_position = position;
}
