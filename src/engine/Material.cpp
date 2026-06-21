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
    m_shader->SetVec3("uLightColor", light.GetColor());
    m_shader->SetVec3("uViewPos", camera.GetPosition());
    m_shader->SetVec3("uObjectColor", m_objectColor);
    m_shader->SetFloat("uAmbientStrength", light.GetAmbientStrength());
    m_shader->SetFloat("uDiffuseStrength", light.GetDiffuseStrength());
    m_shader->SetFloat("uSpecularStrength", light.GetSpecularStrength());
    m_shader->SetFloat("uShininess", light.GetShininess());
    m_shader->SetFloat("uAttenConstant", light.GetConstantAttenuation());
    m_shader->SetFloat("uAttenLinear", light.GetLinearAttenuation());
    m_shader->SetFloat("uAttenQuadratic", light.GetQuadraticAttenuation());
    m_shader->SetFloat("uDiffuseWrap", light.GetDiffuseWrap());
    m_shader->SetFloat("uIndirectStrength", light.GetIndirectStrength());
    m_shader->SetVec3("uFillLightDirection", light.GetFillLightDirection());
    m_shader->SetVec3("uFillLightColor", light.GetFillLightColor());
    m_shader->SetFloat("uFillLightStrength", light.GetFillLightStrength());
}
