#include "engine/rendering/Material.h"

#include "engine/assets/TextureCache.h"
#include "engine/components/ComponentSerialization.h"
#include "engine/components/ComponentCompare.h"
#include "engine/camera/Camera.h"
#include "engine/rendering/Constants.h"
#include "engine/lighting/IBL.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/scene/JsonMath.h"
#include "engine/rendering/Shader.h"
#include "engine/rendering/ShaderCache.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/rendering/Texture.h"

#include <array>
#include <filesystem>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace
{
    Material::TexturePathResolverFn g_texturePathResolver;
}

namespace
{
    constexpr unsigned int ShadowMapUnit = 8;
    constexpr unsigned int AlbedoMapUnit = 4;
    constexpr unsigned int NormalMapUnit = 5;
    constexpr unsigned int AoMapUnit = 6;
    constexpr unsigned int RoughnessMapUnit = 7;

    struct DefaultMaterialTextures
    {
        std::shared_ptr<Texture> white;
        std::shared_ptr<Texture> normal;
        std::shared_ptr<Texture> ao;
        std::shared_ptr<Texture> roughness;
    };

    DefaultMaterialTextures& GetDefaultMaterialTextures()
    {
        static DefaultMaterialTextures defaults;
        if (defaults.white != nullptr)
        {
            return defaults;
        }

        {
            DefaultMaterialTextures textures;
            const unsigned char whitePixel[] = {255, 255, 255, 255};
            const unsigned char normalPixel[] = {128, 128, 255, 255};
            const unsigned char aoPixel[] = {255, 255, 255, 255};
            const unsigned char roughnessPixel[] = {128, 128, 128, 255};

            textures.white = Texture::CreateFromPixels(
                whitePixel,
                1,
                1,
                4,
                TextureColorSpace::SRGB);
            textures.normal = Texture::CreateFromPixels(
                normalPixel,
                1,
                1,
                4,
                TextureColorSpace::Linear);
            textures.ao = Texture::CreateFromPixels(
                aoPixel,
                1,
                1,
                4,
                TextureColorSpace::Linear);
            textures.roughness = Texture::CreateFromPixels(
                roughnessPixel,
                1,
                1,
                4,
                TextureColorSpace::Linear);
            defaults = std::move(textures);
        }

        return defaults;
    }
}

void Material::ReleaseGlobalGpuResources()
{
    DefaultMaterialTextures& defaults = GetDefaultMaterialTextures();
    defaults.white.reset();
    defaults.normal.reset();
    defaults.ao.reset();
    defaults.roughness.reset();
}

void Material::SetTexturePathResolver(TexturePathResolverFn resolver)
{
    g_texturePathResolver = std::move(resolver);
}

void Material::ClearTexturePathResolver()
{
    g_texturePathResolver = {};
}

Material::Material(
    const char* vertexShaderPath,
    const char* fragmentShaderPath,
    const glm::vec3& albedo,
    float roughness,
    float metallic)
    : m_vertexShaderPath(vertexShaderPath),
      m_fragmentShaderPath(fragmentShaderPath),
      m_albedo(albedo),
      m_roughness(roughness),
      m_metallic(metallic)
{
}

void Material::EnsureShader() const
{
    if (m_shader != nullptr)
    {
        return;
    }

    m_shader = ShaderCache::Load(m_vertexShaderPath.c_str(), m_fragmentShaderPath.c_str());
}

void Material::EnsureMapsLoaded() const
{
    TextureCache& cache = TextureCache::Get();

    auto resolveLoadPath = [](const std::string& path) -> std::string {
        if (path.empty())
        {
            return path;
        }

        std::error_code error;
        if (fs::exists(fs::path(path), error))
        {
            return path;
        }

        if (g_texturePathResolver)
        {
            const std::string resolvedPath = g_texturePathResolver(path);
            if (!resolvedPath.empty())
            {
                return resolvedPath;
            }
        }

        return path;
    };

    auto loadIfNeeded =
        [&](std::shared_ptr<Texture>& slot, const std::string& path, TextureColorSpace colorSpace) {
            if (path.empty() || (slot != nullptr && slot->IsValid()))
            {
                return;
            }

            try
            {
                const std::string loadPath = resolveLoadPath(path);
                slot = cache.Load(loadPath.c_str(), colorSpace);
            }
            catch (const std::exception&)
            {
                slot.reset();
            }
        };

    loadIfNeeded(m_albedoMap, m_albedoMapPath, TextureColorSpace::SRGB);
    loadIfNeeded(m_normalMap, m_normalMapPath, TextureColorSpace::Linear);
    loadIfNeeded(m_aoMap, m_aoMapPath, TextureColorSpace::Linear);
    loadIfNeeded(m_roughnessMap, m_roughnessMapPath, TextureColorSpace::Linear);
}

Material::~Material() = default;

void Material::Apply(
    const Camera& camera,
    const SceneLighting& lighting,
    const IBL& ibl,
    const glm::mat4& model,
    const CascadedShadowMap* shadowMap,
    const bool receiveShadow,
    const bool outputLinear,
    const RenderDebugMode debugMode,
    const DirectionalShadowSettings& shadowSettings) const
{
    EnsureShader();
    EnsureMapsLoaded();
    m_shader->Use(outputLinear, !outputLinear);
    m_shader->SetMat4("uModel", model);
    m_shader->SetMat4("uView", camera.GetViewMatrix());
    m_shader->SetMat4("uProjection", camera.GetProjectionMatrix());
    m_shader->SetVec3("uViewPos", camera.GetPosition());
    m_shader->SetVec3("uAlbedo", m_albedo);
    m_shader->SetFloat("uRoughness", m_roughness);
    m_shader->SetFloat("uMetallic", m_metallic);
    m_shader->SetInt("uOutputLinear", outputLinear ? 1 : 0);
    m_shader->SetInt("uSplitLightingOutput", outputLinear ? 1 : 0);

    const bool albedoMapReady = m_albedoMap != nullptr && m_albedoMap->IsValid();
    const bool normalMapReady = m_normalMap != nullptr && m_normalMap->IsValid();
    const bool aoMapReady = m_aoMap != nullptr && m_aoMap->IsValid();
    const bool roughnessMapReady = m_roughnessMap != nullptr && m_roughnessMap->IsValid();

    m_shader->SetInt("uUseAlbedoMap", albedoMapReady ? 1 : 0);
    m_shader->SetInt("uUseNormalMap", normalMapReady ? 1 : 0);
    m_shader->SetInt("uUseAoMap", aoMapReady ? 1 : 0);
    m_shader->SetInt("uUseRoughnessMap", roughnessMapReady && !m_useMetallicRoughnessMap ? 1 : 0);
    m_shader->SetInt("uUseMetallicRoughnessMap", m_useMetallicRoughnessMap && roughnessMapReady ? 1 : 0);
    m_shader->SetInt("uAlbedoTexCoordSet", m_albedoTexCoordSet);
    m_shader->SetInt("uNormalTexCoordSet", m_normalTexCoordSet);
    m_shader->SetInt("uAoTexCoordSet", m_aoTexCoordSet);
    m_shader->SetInt("uRoughnessTexCoordSet", m_roughnessTexCoordSet);
    BindMaps();

    if (shadowMap != nullptr)
    {
        const int activeCascadeCount = shadowMap->GetActiveCascadeCount();
        const std::array<glm::mat4, CascadedShadowMap::MaxCascades>& lightSpaceMatrices =
            shadowMap->GetLightSpaceMatrices();
        m_shader->SetMat4("uLightSpaceMatrix", lightSpaceMatrices[0]);
        m_shader->SetMat4Array(
            "uLightSpaceMatrices",
            lightSpaceMatrices.data(),
            CascadedShadowMap::MaxCascades);

        const std::array<float, CascadedShadowMap::MaxCascades>& cascadeEndSplits =
            shadowMap->GetCascadeEndSplits();
        m_shader->SetFloatArray(
            "uCascadeEndSplits",
            cascadeEndSplits.data(),
            CascadedShadowMap::MaxCascades);

        const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& cascadeSetups =
            shadowMap->GetCascadeSetups();
        std::array<float, CascadedShadowMap::MaxCascades> cascadeTexelWorldSizes{};
        std::array<float, CascadedShadowMap::MaxCascades> cascadeClipDepthMin{};
        std::array<float, CascadedShadowMap::MaxCascades> cascadeClipDepthMax{};
        std::array<float, CascadedShadowMap::MaxCascades> cascadeStableOrthoNear{};
        std::array<float, CascadedShadowMap::MaxCascades> cascadeStableOrthoFar{};
        std::array<float, CascadedShadowMap::MaxCascades> cascadeContentOrthoNear{};
        std::array<float, CascadedShadowMap::MaxCascades> cascadeContentOrthoFar{};
        for (int cascadeIndex = 0; cascadeIndex < activeCascadeCount; ++cascadeIndex)
        {
            const ShadowLightSpaceSetup& setup = cascadeSetups[static_cast<std::size_t>(cascadeIndex)];
            cascadeTexelWorldSizes[static_cast<std::size_t>(cascadeIndex)] =
                std::max(setup.texelWorldSizeX, setup.texelWorldSizeY);
            cascadeClipDepthMin[static_cast<std::size_t>(cascadeIndex)] = setup.clipDepthContentMin;
            cascadeClipDepthMax[static_cast<std::size_t>(cascadeIndex)] = setup.clipDepthContentMax;
            cascadeStableOrthoNear[static_cast<std::size_t>(cascadeIndex)] = setup.stableOrthoNear;
            cascadeStableOrthoFar[static_cast<std::size_t>(cascadeIndex)] = setup.stableOrthoFar;
            cascadeContentOrthoNear[static_cast<std::size_t>(cascadeIndex)] = setup.contentOrthoNear;
            cascadeContentOrthoFar[static_cast<std::size_t>(cascadeIndex)] = setup.contentOrthoFar;
        }
        m_shader->SetFloatArray(
            "uCascadeTexelWorldSizes",
            cascadeTexelWorldSizes.data(),
            CascadedShadowMap::MaxCascades);
        m_shader->SetFloatArray(
            "uCascadeClipDepthMin",
            cascadeClipDepthMin.data(),
            CascadedShadowMap::MaxCascades);
        m_shader->SetFloatArray(
            "uCascadeClipDepthMax",
            cascadeClipDepthMax.data(),
            CascadedShadowMap::MaxCascades);
        m_shader->SetFloatArray(
            "uCascadeStableOrthoNear",
            cascadeStableOrthoNear.data(),
            CascadedShadowMap::MaxCascades);
        m_shader->SetFloatArray(
            "uCascadeStableOrthoFar",
            cascadeStableOrthoFar.data(),
            CascadedShadowMap::MaxCascades);
        m_shader->SetFloatArray(
            "uCascadeContentOrthoNear",
            cascadeContentOrthoNear.data(),
            CascadedShadowMap::MaxCascades);
        m_shader->SetFloatArray(
            "uCascadeContentOrthoFar",
            cascadeContentOrthoFar.data(),
            CascadedShadowMap::MaxCascades);

        m_shader->SetFloat("uCascadeBlendRatio", shadowSettings.GetCascadeBlendRatio());
        m_shader->SetInt("uCascadeCount", activeCascadeCount);
        m_shader->SetFloat("uCascadeNearPlane", camera.GetNearPlane());

        shadowMap->BindDepthTexture(ShadowMapUnit);
        m_shader->SetInt("uShadowMap", static_cast<int>(ShadowMapUnit));
        m_shader->SetInt(
            "uReceiveShadow",
            receiveShadow && shadowMap->HasRenderedDepth() ? 1 : 0);
        m_shader->SetInt("uShadowFilterMode", static_cast<int>(shadowSettings.GetFilterMode()));
        m_shader->SetInt("uPcfKernelRadius", shadowSettings.GetPcfKernelRadius());
        m_shader->SetInt("uUsePoissonPcf", shadowSettings.GetUsePoissonPcf() ? 1 : 0);
        m_shader->SetFloat("uMinPenumbraTexels", shadowSettings.GetMinPenumbraTexels());
        m_shader->SetInt("uPcssBlockerRadius", shadowSettings.GetPcssBlockerRadius());
        m_shader->SetFloat("uPcssLightAngularSize", shadowSettings.GetPcssLightAngularSize());
        m_shader->SetFloat("uPcssMinPenumbraTexels", shadowSettings.GetPcssMinPenumbraTexels());
        m_shader->SetFloat("uPcssMaxPenumbraTexels", shadowSettings.GetPcssMaxPenumbraTexels());
        m_shader->SetFloat("uWorldBiasScale", shadowSettings.GetWorldBiasScale());
        m_shader->SetFloat("uDepthBiasScale", shadowSettings.GetDepthBiasScale());
        m_shader->SetFloat(
            "uShadowMapResolution",
            static_cast<float>(shadowMap->GetResolution()));
    }
    else
    {
        m_shader->SetMat4("uLightSpaceMatrix", glm::mat4(1.0f));
        m_shader->SetInt("uReceiveShadow", 0);
    }

    lighting.Apply(*m_shader);
    ibl.BindTextures(*m_shader);

    m_shader->SetInt("uDebugMode", static_cast<int>(debugMode));

    m_shader->FlushUniforms();
}

void Material::BindMaps() const
{
    const DefaultMaterialTextures& defaults = GetDefaultMaterialTextures();

    const Texture& albedoTexture =
        m_albedoMap != nullptr && m_albedoMap->IsValid() ? *m_albedoMap : *defaults.white;
    albedoTexture.Bind(AlbedoMapUnit);

    const Texture& normalTexture =
        m_normalMap != nullptr && m_normalMap->IsValid() ? *m_normalMap : *defaults.normal;
    normalTexture.Bind(NormalMapUnit);

    const Texture& aoTexture = m_aoMap != nullptr && m_aoMap->IsValid() ? *m_aoMap : *defaults.ao;
    aoTexture.Bind(AoMapUnit);

    const Texture& roughnessTexture =
        m_roughnessMap != nullptr && m_roughnessMap->IsValid() ? *m_roughnessMap : *defaults.roughness;
    roughnessTexture.Bind(RoughnessMapUnit);
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
    return !m_albedoMapPath.empty() || (m_albedoMap != nullptr && m_albedoMap->IsValid());
}

bool Material::HasNormalMap() const
{
    return !m_normalMapPath.empty() || (m_normalMap != nullptr && m_normalMap->IsValid());
}

bool Material::HasAoMap() const
{
    return !m_aoMapPath.empty() || (m_aoMap != nullptr && m_aoMap->IsValid());
}

bool Material::HasRoughnessMap() const
{
    return !m_roughnessMapPath.empty() || (m_roughnessMap != nullptr && m_roughnessMap->IsValid());
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

bool Material::ContentEquals(const Material& other) const
{
    using ComponentCompare::FloatsEqual;

    return m_albedo == other.m_albedo
        && FloatsEqual(m_roughness, other.m_roughness)
        && FloatsEqual(m_metallic, other.m_metallic)
        && m_doubleSided == other.m_doubleSided
        && HasAlbedoMap() == other.HasAlbedoMap()
        && HasNormalMap() == other.HasNormalMap()
        && HasAoMap() == other.HasAoMap()
        && HasRoughnessMap() == other.HasRoughnessMap()
        && HasMetallicRoughnessMap() == other.HasMetallicRoughnessMap()
        && m_albedoMapPath == other.m_albedoMapPath
        && m_normalMapPath == other.m_normalMapPath
        && m_aoMapPath == other.m_aoMapPath
        && m_roughnessMapPath == other.m_roughnessMapPath
        && m_albedoTexCoordSet == other.m_albedoTexCoordSet
        && m_normalTexCoordSet == other.m_normalTexCoordSet
        && m_aoTexCoordSet == other.m_aoTexCoordSet
        && m_roughnessTexCoordSet == other.m_roughnessTexCoordSet;
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

    if (!m_albedoMapPath.empty())
    {
        copy->SetAlbedoMap(m_albedoMap, m_albedoMapPath);
    }

    if (!m_normalMapPath.empty())
    {
        copy->SetNormalMap(m_normalMap, m_normalMapPath);
    }

    if (!m_aoMapPath.empty())
    {
        copy->SetAoMap(m_aoMap, m_aoMapPath);
    }

    if (!m_roughnessMapPath.empty())
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

nlohmann::json MaterialToJson(const Material& material, const MaterialStoredPathFn& toStoredPath)
{
    using json = nlohmann::json;

    json maps = json::object();
    if (material.HasAlbedoMap())
    {
        const std::string mapPath = toStoredPath(material.GetAlbedoMapPath());
        if (!mapPath.empty())
        {
            maps["albedo"] = mapPath;
        }
    }

    if (material.HasNormalMap())
    {
        const std::string mapPath = toStoredPath(material.GetNormalMapPath());
        if (!mapPath.empty())
        {
            maps["normal"] = mapPath;
        }
    }

    if (material.HasAoMap())
    {
        const std::string mapPath = toStoredPath(material.GetAoMapPath());
        if (!mapPath.empty())
        {
            maps["ao"] = mapPath;
        }
    }

    if (material.HasRoughnessMap())
    {
        const std::string mapPath = toStoredPath(material.GetRoughnessMapPath());
        if (!mapPath.empty())
        {
            maps["roughness"] = mapPath;
            maps["metallicRoughness"] = material.HasMetallicRoughnessMap();
        }
    }

    return json{
        {"albedo", Vec3ToJson(material.GetAlbedo())},
        {"roughness", material.GetRoughness()},
        {"metallic", material.GetMetallic()},
        {"doubleSided", material.IsDoubleSided()},
        {"maps", maps},
        {"texCoordSets",
         json{
             {"albedo", material.GetAlbedoTexCoordSet()},
             {"normal", material.GetNormalTexCoordSet()},
             {"ao", material.GetAoTexCoordSet()},
             {"roughness", material.GetRoughnessTexCoordSet()},
         }},
    };
}

std::unique_ptr<Material> MaterialFromJson(
    const nlohmann::json& value,
    const MaterialResolvePathFn& resolvePath,
    const MaterialStoredPathFn& toStoredPath)
{
    using json = nlohmann::json;

    const glm::vec3 albedo = Vec3FromJson(value.at("albedo"));
    const float roughness = value.at("roughness").get<float>();
    const float metallic = value.at("metallic").get<float>();

    std::unique_ptr<Material> material = std::make_unique<Material>(
        EngineConstants::LitVertexShader,
        EngineConstants::PbrFragmentShader,
        albedo,
        roughness,
        metallic);

    material->SetDoubleSided(value.value("doubleSided", false));

    if (value.contains("texCoordSets"))
    {
        const json& texCoordSets = value.at("texCoordSets");
        material->SetAlbedoTexCoordSet(texCoordSets.value("albedo", 0));
        material->SetNormalTexCoordSet(texCoordSets.value("normal", 0));
        material->SetAoTexCoordSet(texCoordSets.value("ao", 0));
        material->SetRoughnessTexCoordSet(texCoordSets.value("roughness", 0));
    }

    if (!value.contains("maps"))
    {
        return material;
    }

    const json& maps = value.at("maps");

    auto assignMapPath =
        [&](const char* key, auto setter) {
            if (!maps.contains(key))
            {
                return;
            }

            const std::string storedPath = maps.at(key).get<std::string>();
            if (storedPath.empty())
            {
                return;
            }

            const std::string resolvedPath = resolvePath(storedPath);
            setter(toStoredPath(resolvedPath));
        };

    assignMapPath("albedo", [&](const std::string& path) {
        material->SetAlbedoMap(nullptr, path);
    });
    assignMapPath("normal", [&](const std::string& path) {
        material->SetNormalMap(nullptr, path);
    });
    assignMapPath("ao", [&](const std::string& path) {
        material->SetAoMap(nullptr, path);
    });

    if (maps.contains("roughness"))
    {
        const std::string storedPath = maps.at("roughness").get<std::string>();
        if (!storedPath.empty())
        {
            const std::string resolvedPath = resolvePath(storedPath);
            const std::string relativePath = toStoredPath(resolvedPath);
            const bool metallicRoughness = maps.value("metallicRoughness", false);
            if (metallicRoughness)
            {
                material->SetMetallicRoughnessMap(
                    nullptr,
                    material->GetRoughnessTexCoordSet(),
                    relativePath);
            }
            else
            {
                material->SetRoughnessMap(nullptr, relativePath);
            }
        }
    }

    return material;
}
