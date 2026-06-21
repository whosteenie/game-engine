#include "engine/Material.h"

#include "engine/Camera.h"
#include "engine/SceneLighting.h"
#include "engine/Shader.h"

Material::Material(
    const char* vertexShaderPath,
    const char* fragmentShaderPath,
    const glm::vec3& albedo,
    float roughness,
    float metallic)
    : m_shader(std::make_unique<Shader>(vertexShaderPath, fragmentShaderPath)),
      m_albedo(albedo),
      m_roughness(roughness),
      m_metallic(metallic)
{
}

Material::~Material() = default;

void Material::Apply(
    const Camera& camera,
    const SceneLighting& lighting,
    const glm::mat4& model) const
{
    m_shader->Use();
    m_shader->SetMat4("uModel", model);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uViewPos", camera.GetPosition());
    m_shader->SetVec3("uAlbedo", m_albedo);
    m_shader->SetFloat("uRoughness", m_roughness);
    m_shader->SetFloat("uMetallic", m_metallic);

    lighting.Apply(*m_shader);
}

const glm::vec3& Material::GetAlbedo() const
{
    return m_albedo;
}

float Material::GetRoughness() const
{
    return m_roughness;
}

float Material::GetMetallic() const
{
    return m_metallic;
}
