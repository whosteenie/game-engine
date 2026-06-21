#pragma once

#include <glm/glm.hpp>

class Light
{
public:
    explicit Light(const glm::vec3& position);

    const glm::vec3& GetPosition() const;
    void SetPosition(const glm::vec3& position);

private:
    glm::vec3 m_position;
};
