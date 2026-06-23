#include "engine/Material.h"

#include "engine/Camera.h"
#include "engine/Constants.h"
#include "engine/IBL.h"
#include "engine/RenderDebug.h"
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
    const bool receiveShadow,
    const bool outputLinear,
    const RenderDebugMode debugMode) const
{
    m_shader->Use();
    m_shader->SetMat4("uModel", model);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uViewPos", camera.GetPosition());
    m_shader->SetVec3("uAlbedo", m_albedo);
    m_shader->SetFloat("uRoughness", m_roughness);
    m_shader->SetFloat("uMetallic", m_metallic);
    m_shader->SetInt("uOutputLinear", outputLinear ? 1 : 0);
    m_shader->SetInt("uDebugMode", static_cast<int>(debugMode));

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

void Material::SetRoughnessMap(std::shared_ptr<Texture> texture, std::string path)
{
    m_roughnessMap = std::move(texture);
    m_roughnessMapPath = std::move(path);
    m_useMetallicRoughnessMap = false;
}

void Material::SetMetallicRoughnessMap(std::shared_ptr<Texture> texture, int texCoordSet, std::string path)
{
    m_roughnessMap = std::move(texture);
    m_roughnessMapPath = std::move(path);
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

void Material::SetAlbedoMap(std::shared_ptr<Texture> texture, std::string path)
{
    m_albedoMap = std::move(texture);
    m_albedoMapPath = std::move(path);
}

void Material::SetNormalMap(std::shared_ptr<Texture> texture, std::string path)
{
    m_normalMap = std::move(texture);
    m_normalMapPath = std::move(path);
}

void Material::SetAoMap(std::shared_ptr<Texture> texture, std::string path)
{
    m_aoMap = std::move(texture);
    m_aoMapPath = std::move(path);
}

void Material::ClearAlbedoMap()
{
    m_albedoMap.reset();
    m_albedoMapPath.clear();
}

void Material::ClearNormalMap()
{
    m_normalMap.reset();
    m_normalMapPath.clear();
}

void Material::ClearAoMap()
{
    m_aoMap.reset();
    m_aoMapPath.clear();
}

void Material::ClearRoughnessMap()
{
    m_roughnessMap.reset();
    m_roughnessMapPath.clear();
    m_useMetallicRoughnessMap = false;
}

const std::string& Material::GetAlbedoMapPath() const
{
    return m_albedoMapPath;
}

const std::string& Material::GetNormalMapPath() const
{
    return m_normalMapPath;
}

const std::string& Material::GetAoMapPath() const
{
    return m_aoMapPath;
}

const std::string& Material::GetRoughnessMapPath() const
{
    return m_roughnessMapPath;
}

int Material::GetAlbedoTexCoordSet() const
{
    return m_albedoTexCoordSet;
}

int Material::GetNormalTexCoordSet() const
{
    return m_normalTexCoordSet;
}

int Material::GetAoTexCoordSet() const
{
    return m_aoTexCoordSet;
}

int Material::GetRoughnessTexCoordSet() const
{
    return m_roughnessTexCoordSet;
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

void Material::ApplyMissingTextureMapsFrom(const Material& source)
{
    if (!HasAlbedoMap() && source.m_albedoMap != nullptr && source.m_albedoMap->IsValid())
    {
        SetAlbedoMap(source.m_albedoMap, source.m_albedoMapPath);
        SetAlbedoTexCoordSet(source.m_albedoTexCoordSet);
    }

    if (!HasNormalMap() && source.m_normalMap != nullptr && source.m_normalMap->IsValid())
    {
        SetNormalMap(source.m_normalMap, source.m_normalMapPath);
        SetNormalTexCoordSet(source.m_normalTexCoordSet);
    }

    if (!HasAoMap() && source.m_aoMap != nullptr && source.m_aoMap->IsValid())
    {
        SetAoMap(source.m_aoMap, source.m_aoMapPath);
        SetAoTexCoordSet(source.m_aoTexCoordSet);
    }

    if (!HasRoughnessMap() && source.m_roughnessMap != nullptr && source.m_roughnessMap->IsValid())
    {
        if (source.m_useMetallicRoughnessMap)
        {
            SetMetallicRoughnessMap(
                source.m_roughnessMap,
                source.m_roughnessTexCoordSet,
                source.m_roughnessMapPath);
        }
        else
        {
            SetRoughnessMap(source.m_roughnessMap, source.m_roughnessMapPath);
            SetRoughnessTexCoordSet(source.m_roughnessTexCoordSet);
        }
    }
}

std::unique_ptr<Material> Material::Clone() const
{
    auto copy = std::make_unique<Material>(
        EngineConstants::LitVertexShader,
        EngineConstants::PbrFragmentShader,
        m_albedo,
        m_roughness,
        m_metallic);

    copy->SetAlbedoTexCoordSet(m_albedoTexCoordSet);
    copy->SetNormalTexCoordSet(m_normalTexCoordSet);
    copy->SetAoTexCoordSet(m_aoTexCoordSet);
    copy->SetRoughnessTexCoordSet(m_roughnessTexCoordSet);
    copy->SetDoubleSided(m_doubleSided);

    if (m_albedoMap != nullptr)
    {
        copy->SetAlbedoMap(m_albedoMap, m_albedoMapPath);
    }

    if (m_normalMap != nullptr)
    {
        copy->SetNormalMap(m_normalMap, m_normalMapPath);
    }

    if (m_aoMap != nullptr)
    {
        copy->SetAoMap(m_aoMap, m_aoMapPath);
    }

    if (m_roughnessMap != nullptr)
    {
        if (m_useMetallicRoughnessMap)
        {
            copy->SetMetallicRoughnessMap(m_roughnessMap, m_roughnessTexCoordSet, m_roughnessMapPath);
        }
        else
        {
            copy->SetRoughnessMap(m_roughnessMap, m_roughnessMapPath);
        }
    }

    return copy;
}
