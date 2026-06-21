#pragma once

#include <glm/glm.hpp>
#include <memory>

class Camera;
class Light;
class Shader;

class Material
{
public:
    Material(const char* vertexShaderPath, const char* fragmentShaderPath, const glm::vec3& objectColor);
    ~Material();

    void Apply(const Camera& camera, const Light& light, const glm::mat4& model) const;

private:
    std::unique_ptr<Shader> m_shader;
    glm::vec3 m_objectColor;
};
