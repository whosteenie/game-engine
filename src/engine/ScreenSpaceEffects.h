#pragma once

#include "engine/DirectionalShadowSettings.h"
#include "engine/RenderDebug.h"
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
        int viewportWidth,
        int viewportHeight,
        const DirectionalShadowSettings& shadowSettings) const;

    bool IsEnabled() const;
    void SetEnabled(bool enabled);

    bool IsSsaoEnabled() const;
    void SetSsaoEnabled(bool enabled);

    float GetSsaoRadius() const;
    void SetSsaoRadius(float radius);

    float GetSsaoBias() const;
    void SetSsaoBias(float bias);

    float GetSsaoPower() const;
    void SetSsaoPower(float power);

    float GetAoStrength() const;
    void SetAoStrength(float strength);

    float GetExposure() const;
    void SetExposure(float exposure);

    TonemapMode GetTonemapMode() const;
    void SetTonemapMode(TonemapMode mode);

    bool IsBloomEnabled() const;
    void SetBloomEnabled(bool enabled);

    float GetBloomThreshold() const;
    void SetBloomThreshold(float threshold);

    float GetBloomSoftKnee() const;
    void SetBloomSoftKnee(float softKnee);

    float GetBloomIntensity() const;
    void SetBloomIntensity(float intensity);

    float GetBloomBlurRadius() const;
    void SetBloomBlurRadius(float blurRadius);

    RenderDebugMode GetDebugMode() const;
    void SetDebugMode(RenderDebugMode mode);

    void BlitDepthToFramebuffer(unsigned int drawFramebuffer, int viewportWidth, int viewportHeight) const;

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
    void ResizeBloomTargets(int width, int height);
    void DrawFullscreenQuad() const;

    std::unique_ptr<Framebuffer> m_sceneFramebuffer;
    std::unique_ptr<Shader> m_ssaoShader;
    std::unique_ptr<Shader> m_blurShader;
    std::unique_ptr<Shader> m_compositeShader;
    std::unique_ptr<Shader> m_bloomExtractShader;
    std::unique_ptr<Shader> m_bloomBlurShader;
    std::unique_ptr<Shader> m_shadowBlurShader;
    std::unique_ptr<Shader> m_tonemapShader;
    std::unique_ptr<Shader> m_debugChannelShader;

    unsigned int m_quadVao = 0;
    unsigned int m_quadVbo = 0;
    unsigned int m_noiseTexture = 0;

    unsigned int m_ssaoFbo = 0;
    unsigned int m_ssaoTexture = 0;
    unsigned int m_ssaoBlurFbo = 0;
    unsigned int m_ssaoBlurTexture = 0;
    unsigned int m_shadowBlurFbo = 0;
    unsigned int m_shadowBlurTexture = 0;
    unsigned int m_shadowBlur2Fbo = 0;
    unsigned int m_shadowBlur2Texture = 0;
    unsigned int m_hdrCompositeFbo = 0;
    unsigned int m_hdrCompositeTexture = 0;
    unsigned int m_bloomExtractFbo = 0;
    unsigned int m_bloomExtractTexture = 0;
    unsigned int m_bloomBlurFbo = 0;
    unsigned int m_bloomBlurTexture = 0;
    unsigned int m_bloomBlur2Fbo = 0;
    unsigned int m_bloomBlur2Texture = 0;

    std::vector<glm::vec3> m_kernelSamples;
    int m_width = 0;
    int m_height = 0;

    bool m_enabled = true;
    bool m_ssaoEnabled = true;
    float m_ssaoRadius = 0.35f;
    float m_ssaoBias = 0.015f;
    float m_ssaoPower = 1.6f;
    float m_aoStrength = 0.75f;
    float m_exposure = 0.0f;
    TonemapMode m_tonemapMode = TonemapMode::Gamma;
    bool m_bloomEnabled = false;
    float m_bloomThreshold = 1.0f;
    float m_bloomSoftKnee = 0.5f;
    float m_bloomIntensity = 0.4f;
    float m_bloomBlurRadius = 1.0f;
    RenderDebugMode m_debugMode = RenderDebugMode::None;
};
