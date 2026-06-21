#include "engine/Material.h"

#include "engine/Camera.h"
#include "engine/IBL.h"
#include "engine/SceneLighting.h"
#include "engine/Shader.h"
#include "engine/ShadowMap.h"

#include <glm/glm.hpp>

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
    const IBL& ibl,
    const glm::mat4& model,
    const ShadowMap* shadowMap,
    bool receiveShadow) const
{
    m_shader->Use();
    m_shader->SetMat4("uModel", model);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uViewPos", camera.GetPosition());
    m_shader->SetVec3("uAlbedo", m_albedo);
    m_shader->SetFloat("uRoughness", m_roughness);
    m_shader->SetFloat("uMetallic", m_metallic);

    if (shadowMap != nullptr)
    {
        m_shader->SetMat4("uLightSpaceMatrix", shadowMap->GetLightSpaceMatrix());
        shadowMap->BindDepthTexture(0);
        m_shader->SetInt("uShadowMap", 0);
        m_shader->SetInt("uReceiveShadow", receiveShadow ? 1 : 0);
    }
    else
    {
        m_shader->SetMat4("uLightSpaceMatrix", glm::mat4(1.0f));
        m_shader->SetInt("uReceiveShadow", 0);
    }

    lighting.Apply(*m_shader);
    ibl.BindTextures(*m_shader);
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

void Material::SetAlbedo(const glm::vec3& albedo)
{
    m_albedo = albedo;
}

void Material::SetRoughness(float roughness)
{
    m_roughness = roughness;
}

void Material::SetMetallic(float metallic)
{
    m_metallic = metallic;
}
