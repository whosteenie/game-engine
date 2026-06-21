#pragma once

#include <glm/glm.hpp>
#include <memory>

class Camera;
class SceneObject;
class Shader;

class SelectionRenderer
{
public:
    SelectionRenderer();
    ~SelectionRenderer();

    void Draw(const Camera& camera, const SceneObject& object, const glm::mat4& worldMatrix) const;

private:
    std::unique_ptr<Shader> m_shader;
    mutable unsigned int m_vao = 0;
    mutable unsigned int m_vbo = 0;
};
