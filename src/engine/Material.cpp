#include "engine/Material.h"

#include "engine/Camera.h"
#include "engine/IBL.h"
#include "engine/SceneLighting.h"
#include "engine/Shader.h"
#include "engine/ShadowMap.h"
#include "engine/Texture.h"

#include <glm/glm.hpp>

namespace
{
    constexpr unsigned int AlbedoMapUnit = 4;
    constexpr unsigned int NormalMapUnit = 5;
    constexpr unsigned int AoMapUnit = 6;
    constexpr unsigned int RoughnessMapUnit = 7;
}

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

    m_shader->SetInt("uUseAlbedoMap", HasAlbedoMap() ? 1 : 0);
    m_shader->SetInt("uUseNormalMap", HasNormalMap() ? 1 : 0);
    m_shader->SetInt("uUseAoMap", HasAoMap() ? 1 : 0);
    m_shader->SetInt("uUseRoughnessMap", HasRoughnessMap() && !m_useMetallicRoughnessMap ? 1 : 0);
    m_shader->SetInt("uUseMetallicRoughnessMap", HasMetallicRoughnessMap() ? 1 : 0);
    m_shader->SetInt("uAlbedoTexCoordSet", m_albedoTexCoordSet);
    m_shader->SetInt("uNormalTexCoordSet", m_normalTexCoordSet);
    m_shader->SetInt("uAoTexCoordSet", m_aoTexCoordSet);
    m_shader->SetInt("uRoughnessTexCoordSet", m_roughnessTexCoordSet);
    BindMaps();

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

void Material::BindMaps() const
{
    if (HasAlbedoMap())
    {
        m_albedoMap->Bind(AlbedoMapUnit);
        m_shader->SetInt("uAlbedoMap", static_cast<int>(AlbedoMapUnit));
    }

    if (HasNormalMap())
    {
        m_normalMap->Bind(NormalMapUnit);
        m_shader->SetInt("uNormalMap", static_cast<int>(NormalMapUnit));
    }

    if (HasAoMap())
    {
        m_aoMap->Bind(AoMapUnit);
        m_shader->SetInt("uAoMap", static_cast<int>(AoMapUnit));
    }

    if (HasRoughnessMap())
    {
        m_roughnessMap->Bind(RoughnessMapUnit);
        m_shader->SetInt("uRoughnessMap", static_cast<int>(RoughnessMapUnit));
    }
}

void Material::SetRoughnessMap(std::shared_ptr<Texture> texture)
{
    m_roughnessMap = std::move(texture);
    m_useMetallicRoughnessMap = false;
}

void Material::SetMetallicRoughnessMap(std::shared_ptr<Texture> texture, int texCoordSet)
{
    m_roughnessMap = std::move(texture);
    m_roughnessTexCoordSet = texCoordSet;
    m_useMetallicRoughnessMap = true;
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

void Material::SetAlbedoMap(std::shared_ptr<Texture> texture)
{
    m_albedoMap = std::move(texture);
}

void Material::SetNormalMap(std::shared_ptr<Texture> texture)
{
    m_normalMap = std::move(texture);
}

void Material::SetAoMap(std::shared_ptr<Texture> texture)
{
    m_aoMap = std::move(texture);
}

void Material::SetAlbedoTexCoordSet(int texCoordSet)
{
    m_albedoTexCoordSet = texCoordSet;
}

void Material::SetNormalTexCoordSet(int texCoordSet)
{
    m_normalTexCoordSet = texCoordSet;
}

void Material::SetAoTexCoordSet(int texCoordSet)
{
    m_aoTexCoordSet = texCoordSet;
}

void Material::SetRoughnessTexCoordSet(int texCoordSet)
{
    m_roughnessTexCoordSet = texCoordSet;
}

void Material::SetDoubleSided(bool doubleSided)
{
    m_doubleSided = doubleSided;
}

bool Material::IsDoubleSided() const
{
    return m_doubleSided;
}

bool Material::HasAlbedoMap() const
{
    return m_albedoMap != nullptr && m_albedoMap->IsValid();
}

bool Material::HasNormalMap() const
{
    return m_normalMap != nullptr && m_normalMap->IsValid();
}

bool Material::HasAoMap() const
{
    return m_aoMap != nullptr && m_aoMap->IsValid();
}

bool Material::HasRoughnessMap() const
{
    return m_roughnessMap != nullptr && m_roughnessMap->IsValid();
}

bool Material::HasMetallicRoughnessMap() const
{
    return m_useMetallicRoughnessMap && HasRoughnessMap();
}
