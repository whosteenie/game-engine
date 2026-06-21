#include "engine/Material.h"

#include "engine/Camera.h"
#include "engine/Light.h"
#include "engine/Shader.h"

Material::Material(const char* vertexShaderPath, const char* fragmentShaderPath, const glm::vec3& objectColor)
    : m_shader(std::make_unique<Shader>(vertexShaderPath, fragmentShaderPath)),
      m_objectColor(objectColor)
{
}

Material::~Material() = default;

void Material::Apply(const Camera& camera, const Light& light, const glm::mat4& model) const
{
    m_shader->Use();
    m_shader->SetMat4("uModel", model);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uLightPos", light.GetPosition());
    m_shader->SetVec3("uViewPos", camera.GetPosition());
    m_shader->SetVec3("uObjectColor", m_objectColor);
}
