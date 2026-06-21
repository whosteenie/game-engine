#pragma once

#include <glm/glm.hpp>
#include <memory>

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
        bool receiveShadow = false) const;

    const glm::vec3& GetAlbedo() const;
    float GetRoughness() const;
    float GetMetallic() const;

    void SetAlbedo(const glm::vec3& albedo);
    void SetRoughness(float roughness);
    void SetMetallic(float metallic);

    void SetAlbedoMap(std::shared_ptr<Texture> texture);
    void SetNormalMap(std::shared_ptr<Texture> texture);
    void SetAoMap(std::shared_ptr<Texture> texture);
    void SetRoughnessMap(std::shared_ptr<Texture> texture);

    bool HasAlbedoMap() const;
    bool HasNormalMap() const;
    bool HasAoMap() const;
    bool HasRoughnessMap() const;

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
};
