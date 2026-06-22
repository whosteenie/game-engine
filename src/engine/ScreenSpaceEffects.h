#pragma once

#include <glm/glm.hpp>
#include <memory>
#include <vector>

class Camera;
class Framebuffer;
class Shader;

enum class TonemapMode
{
    Gamma = 0,
    Reinhard = 1,
    ACES = 2
};

class ScreenSpaceEffects
{
public:
    static constexpr int KernelSampleCount = 32;

    ScreenSpaceEffects();
    ~ScreenSpaceEffects();

    ScreenSpaceEffects(const ScreenSpaceEffects&) = delete;
    ScreenSpaceEffects& operator=(const ScreenSpaceEffects&) = delete;

    void Resize(int width, int height);

    void BeginScenePass() const;
    void EndScenePass() const;

    void Apply(
        const Camera& camera,
        const glm::vec3& lightDirection,
        int viewportWidth,
        int viewportHeight) const;

    bool IsEnabled() const;
    void SetEnabled(bool enabled);

    bool IsSsaoEnabled() const;
    void SetSsaoEnabled(bool enabled);

    bool IsContactShadowsEnabled() const;
    void SetContactShadowsEnabled(bool enabled);

    float GetSsaoRadius() const;
    void SetSsaoRadius(float radius);

    float GetSsaoBias() const;
    void SetSsaoBias(float bias);

    float GetSsaoPower() const;
    void SetSsaoPower(float power);

    float GetContactShadowDistance() const;
    void SetContactShadowDistance(float distance);

    int GetContactShadowSteps() const;
    void SetContactShadowSteps(int steps);

    float GetAoStrength() const;
    void SetAoStrength(float strength);

    float GetContactStrength() const;
    void SetContactStrength(float strength);

    float GetExposure() const;
    void SetExposure(float exposure);

    TonemapMode GetTonemapMode() const;
    void SetTonemapMode(TonemapMode mode);

    void BlitDepthToDefaultFramebuffer(int viewportWidth, int viewportHeight) const;

private:
    void CreateFullscreenQuad();
    void CreateNoiseTexture();
    void CreateKernel();
    void CreateSingleChannelTarget(unsigned int& fbo, unsigned int& texture, int width, int height) const;
    void CreateHdrColorTarget(unsigned int& fbo, unsigned int& texture, int width, int height) const;
    void DestroySingleChannelTarget(unsigned int& fbo, unsigned int& texture) const;
    void DestroyHdrColorTarget(unsigned int& fbo, unsigned int& texture) const;
    void ResizeSingleChannelTargets(int width, int height);
    void ResizeHdrColorTarget(int width, int height);
    void DrawFullscreenQuad() const;

    std::unique_ptr<Framebuffer> m_sceneFramebuffer;
    std::unique_ptr<Shader> m_ssaoShader;
    std::unique_ptr<Shader> m_blurShader;
    std::unique_ptr<Shader> m_contactShadowShader;
    std::unique_ptr<Shader> m_compositeShader;
    std::unique_ptr<Shader> m_tonemapShader;

    unsigned int m_quadVao = 0;
    unsigned int m_quadVbo = 0;
    unsigned int m_noiseTexture = 0;

    unsigned int m_ssaoFbo = 0;
    unsigned int m_ssaoTexture = 0;
    unsigned int m_ssaoBlurFbo = 0;
    unsigned int m_ssaoBlurTexture = 0;
    unsigned int m_contactFbo = 0;
    unsigned int m_contactTexture = 0;
    unsigned int m_contactBlurFbo = 0;
    unsigned int m_contactBlurTexture = 0;
    unsigned int m_hdrCompositeFbo = 0;
    unsigned int m_hdrCompositeTexture = 0;

    std::vector<glm::vec3> m_kernelSamples;
    int m_width = 0;
    int m_height = 0;

    bool m_enabled = true;
    bool m_ssaoEnabled = true;
    bool m_contactShadowsEnabled = true;
    float m_ssaoRadius = 0.35f;
    float m_ssaoBias = 0.015f;
    float m_ssaoPower = 1.6f;
    float m_aoStrength = 0.75f;
    float m_contactStrength = 0.35f;
    float m_contactShadowDistance = 0.08f;
    int m_contactShadowSteps = 6;
    float m_exposure = 0.0f;
    TonemapMode m_tonemapMode = TonemapMode::Gamma;
};
