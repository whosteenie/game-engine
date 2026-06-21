#pragma once

#include <glm/glm.hpp>

class ShadowMap
{
public:
    static constexpr int DefaultResolution = 2048;

    explicit ShadowMap(int resolution = DefaultResolution);
    ~ShadowMap();

    void BeginPass(const glm::vec3& lightDirectionTowardSource, const glm::vec3& sceneCenter);
    void EndPass();

    const glm::mat4& GetLightSpaceMatrix() const;
    void BindDepthTexture(unsigned int textureUnit) const;

private:
    void CreateResources();

    int m_resolution;
    unsigned int m_fbo = 0;
    unsigned int m_depthTexture = 0;
    glm::mat4 m_lightSpaceMatrix{1.0f};
};
