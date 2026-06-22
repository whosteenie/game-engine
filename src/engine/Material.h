#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <string>

class Camera;
class IBL;
class SceneLighting;
class Shader;
class ShadowMap;
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

    void Apply(
        const Camera& camera,
        const SceneLighting& lighting,
        const IBL& ibl,
        const glm::mat4& model,
        const ShadowMap* shadowMap = nullptr,
        bool receiveShadow = true,
        bool outputLinear = false) const;

    const glm::vec3& GetAlbedo() const;
    float GetRoughness() const;
    float GetMetallic() const;

    void SetAlbedo(const glm::vec3& albedo);
    void SetRoughness(float roughness);
    void SetMetallic(float metallic);

    void SetAlbedoMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetNormalMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetAoMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetRoughnessMap(std::shared_ptr<Texture> texture, std::string path = {});
    void SetMetallicRoughnessMap(std::shared_ptr<Texture> texture, int texCoordSet = 0, std::string path = {});

    void ClearAlbedoMap();
    void ClearNormalMap();
    void ClearAoMap();
    void ClearRoughnessMap();

    const std::string& GetAlbedoMapPath() const;
    const std::string& GetNormalMapPath() const;
    const std::string& GetAoMapPath() const;
    const std::string& GetRoughnessMapPath() const;

    int GetAlbedoTexCoordSet() const;
    int GetNormalTexCoordSet() const;
    int GetAoTexCoordSet() const;
    int GetRoughnessTexCoordSet() const;

    void SetAlbedoTexCoordSet(int texCoordSet);
    void SetNormalTexCoordSet(int texCoordSet);
    void SetAoTexCoordSet(int texCoordSet);
    void SetRoughnessTexCoordSet(int texCoordSet);

    void SetDoubleSided(bool doubleSided);
    bool IsDoubleSided() const;

    bool HasAlbedoMap() const;
    bool HasNormalMap() const;
    bool HasAoMap() const;
    bool HasRoughnessMap() const;
    bool HasMetallicRoughnessMap() const;

    std::unique_ptr<Material> Clone() const;

private:
    void BindMaps() const;

    std::unique_ptr<Shader> m_shader;
    glm::vec3 m_albedo;
    float m_roughness;
    float m_metallic;

    std::shared_ptr<Texture> m_albedoMap;
    std::shared_ptr<Texture> m_normalMap;
    std::shared_ptr<Texture> m_aoMap;
    std::shared_ptr<Texture> m_roughnessMap;

    std::string m_albedoMapPath;
    std::string m_normalMapPath;
    std::string m_aoMapPath;
    std::string m_roughnessMapPath;

    int m_albedoTexCoordSet = 0;
    int m_normalTexCoordSet = 0;
    int m_aoTexCoordSet = 0;
    int m_roughnessTexCoordSet = 0;
    bool m_doubleSided = false;
    bool m_useMetallicRoughnessMap = false;
};
