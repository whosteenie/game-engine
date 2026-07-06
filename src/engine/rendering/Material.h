#pragma once

#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/RenderDebug.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class Shader;
class Camera;
class IBL;
class SceneLighting;
class CascadedShadowMap;
class Texture;

class Material
{
public:
    Material(
        const char* vertexShaderPath,
        const char* fragmentShaderPath,
        const glm::vec3& albedo,
        float roughness,
        float metallic);

    ~Material();

    static void ReleaseGlobalGpuResources();
    void InvalidateCachedShader() const;

    using TexturePathResolverFn = std::function<std::string(const std::string& storedPath)>;
    static void SetTexturePathResolver(TexturePathResolverFn resolver);
    static void ClearTexturePathResolver();

    void Apply(
        const Camera& camera,
        const SceneLighting& lighting,
        const IBL& ibl,
        const glm::mat4& model,
        const CascadedShadowMap* shadowMap = nullptr,
        bool receiveShadow = true,
        bool outputLinear = false,
        RenderDebugMode debugMode = RenderDebugMode::None,
        const DirectionalShadowSettings& shadowSettings = DirectionalShadowSettings{},
        const MotionVectorFrameState& motionFrameState = MotionVectorFrameState{},
        const glm::mat4& prevModel = glm::mat4(1.0f)) const;

    const glm::vec3& GetAlbedo() const;
    float GetRoughness() const;
    float GetMetallic() const;

    void SetAlbedo(const glm::vec3& albedo);
    void SetRoughness(float roughness);
    void SetMetallic(float metallic);

    const glm::vec3& GetEmissive() const;
    void SetEmissive(const glm::vec3& emissive);

    // Absolute shader-visible SRV heap index of the albedo texture (for DXR bindless sampling).
    // Ensures the map is loaded; returns UINT32_MAX when there is no albedo texture.
    std::uint32_t GetAlbedoMapSrvIndex() const;

    void SetAlbedoMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetNormalMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetAoMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetRoughnessMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetEmissiveMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetMetallicRoughnessMap(std::shared_ptr<Texture> texture, int texCoordSet = 0, std::string path = {});

    void ClearAlbedoMap();
    void ClearNormalMap();
    void ClearAoMap();
    void ClearRoughnessMap();
    void ClearEmissiveMap();

    const std::string& GetAlbedoMapPath() const;
    const std::string& GetNormalMapPath() const;
    const std::string& GetAoMapPath() const;
    const std::string& GetRoughnessMapPath() const;
    const std::string& GetEmissiveMapPath() const;

    int GetAlbedoTexCoordSet() const;
    int GetNormalTexCoordSet() const;
    int GetAoTexCoordSet() const;
    int GetRoughnessTexCoordSet() const;
    int GetEmissiveTexCoordSet() const;

    void SetAlbedoTexCoordSet(int texCoordSet);
    void SetNormalTexCoordSet(int texCoordSet);
    void SetAoTexCoordSet(int texCoordSet);
    void SetRoughnessTexCoordSet(int texCoordSet);
    void SetEmissiveTexCoordSet(int texCoordSet);

    void SetDoubleSided(bool doubleSided);
    bool IsDoubleSided() const;

    void ApplyMissingTextureMapsFrom(const Material& source);

    bool HasAlbedoMap() const;
    bool HasNormalMap() const;
    bool HasAoMap() const;
    bool HasRoughnessMap() const;
    bool HasEmissiveMap() const;
    bool HasMetallicRoughnessMap() const;

    std::unique_ptr<Material> Clone() const;
    bool ContentEquals(const Material& other) const;

private:
    void BindMaps() const;
    void EnsureShader() const;
    void EnsureMapsLoaded() const;

    std::string m_vertexShaderPath;
    std::string m_fragmentShaderPath;
    mutable std::shared_ptr<Shader> m_shader;
    glm::vec3 m_albedo;
    float m_roughness;
    float m_metallic;
    glm::vec3 m_emissive{0.0f};

    mutable std::shared_ptr<Texture> m_albedoMap;
    mutable std::shared_ptr<Texture> m_normalMap;
    mutable std::shared_ptr<Texture> m_aoMap;
    mutable std::shared_ptr<Texture> m_roughnessMap;
    mutable std::shared_ptr<Texture> m_emissiveMap;

    std::string m_albedoMapPath;
    std::string m_normalMapPath;
    std::string m_aoMapPath;
    std::string m_roughnessMapPath;
    std::string m_emissiveMapPath;

    int m_albedoTexCoordSet = 0;
    int m_normalTexCoordSet = 0;
    int m_aoTexCoordSet = 0;
    int m_roughnessTexCoordSet = 0;
    int m_emissiveTexCoordSet = 0;
    bool m_doubleSided = false;
    bool m_useMetallicRoughnessMap = false;
};
