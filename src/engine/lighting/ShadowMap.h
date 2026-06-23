#pragma once

#include <glm/glm.hpp>

class ShadowMap
{
public:
    static constexpr int DefaultResolution = 4096;

    explicit ShadowMap(int resolution = DefaultResolution);
    ~ShadowMap();

    ShadowMap(const ShadowMap&) = delete;
    ShadowMap& operator=(const ShadowMap&) = delete;
    ShadowMap(ShadowMap&& other) noexcept;
    ShadowMap& operator=(ShadowMap&& other) noexcept;

    void BeginPass(
        const glm::vec3& lightDirectionTowardSource,
        const glm::vec3& boundsMin,
        const glm::vec3& boundsMax);
    void EndPass();

    const glm::mat4& GetLightSpaceMatrix() const;
    void BindDepthTexture(unsigned int textureUnit) const;

private:
    void CreateResources();
    void DestroyResources();

    int m_resolution;
    unsigned int m_fbo = 0;
    unsigned int m_depthTexture = 0;
    glm::mat4 m_lightSpaceMatrix{1.0f};
};
