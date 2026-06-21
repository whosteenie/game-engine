#pragma once

#include <glm/glm.hpp>
#include <memory>

class Camera;
class SceneLighting;
class Shader;
class ShadowMap;

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
        const glm::mat4& model,
        const ShadowMap* shadowMap = nullptr,
        bool receiveShadow = false) const;

    const glm::vec3& GetAlbedo() const;
    float GetRoughness() const;
    float GetMetallic() const;

private:
    std::unique_ptr<Shader> m_shader;
    glm::vec3 m_albedo;
    float m_roughness;
    float m_metallic;
};
