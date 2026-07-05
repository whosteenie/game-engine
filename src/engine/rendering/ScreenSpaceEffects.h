#pragma once

#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/SsaoDiagnostics.h"

#include "engine/rhi/d3d12/GpuBuffer.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <memory>
#include <vector>

class Camera;
class EnvironmentMap;
class Framebuffer;
class Shader;

enum class TonemapMode
{
    Gamma = 0,
    Reinhard = 1,
    ACES = 2
};

enum class AntiAliasingMode
{
    None = 0,
    FXAA = 1,
    TAA = 2,
    MSAA = 3,
    SMAA = 4,
    SSAA = 5,
};

enum class AmbientOcclusionMode
{
    Off = 0,
    SSAO = 1,
    GTAO = 2,
};

class ScreenSpaceEffects
{
public:
    static constexpr int KernelSampleCount = 32;

    ScreenSpaceEffects();
    ~ScreenSpaceEffects();

    ScreenSpaceEffects(const ScreenSpaceEffects&) = delete;
    ScreenSpaceEffects& operator=(const ScreenSpaceEffects&) = delete;

    void Resize(int viewportWidth, int viewportHeight);
    void ReloadGeometryMsaaTargets(int viewportWidth, int viewportHeight);

    void PrepareAntiAliasingFrame(Camera& camera, bool freezeJitter = false) const;
    void FinalizeAntiAliasingFrame(const Camera& camera, bool freezeJitter = false) const;
    const MotionVectorFrameState& GetMotionVectorFrameState() const;
    void AdvanceTemporalFrame(const Camera& camera) const;

    void BeginScenePass(const EnvironmentMap& environmentMap) const;
    void EndScenePass() const;
    void BlitRtDispatchSmokeDebug(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight) const;
    void BlitRtPrimaryDebug(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight,
        float maxTraceDistance) const;
    void BlitRtReflectionDebug(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight) const;

    void Apply(
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        const DirectionalShadowSettings& shadowSettings,
        const EnvironmentMap& environmentMap) const;

    bool IsEnabled() const;
    void SetEnabled(bool enabled);

    bool HasSplitLighting() const;

    bool IsSsaoEnabled() const;
    void SetSsaoEnabled(bool enabled);
    AmbientOcclusionMode GetAmbientOcclusionMode() const;
    void SetAmbientOcclusionMode(AmbientOcclusionMode mode);

    float GetSsaoRadius() const;
    void SetSsaoRadius(float radius);

    float GetSsaoBias() const;
    void SetSsaoBias(float bias);

    float GetSsaoPower() const;
    void SetSsaoPower(float power);

    float GetGtaoRadius() const;
    void SetGtaoRadius(float radius);

    float GetGtaoThickness() const;
    void SetGtaoThickness(float thickness);

    float GetGtaoFalloff() const;
    void SetGtaoFalloff(float falloff);

    float GetGtaoPower() const;
    void SetGtaoPower(float power);

    int GetGtaoDirections() const;
    void SetGtaoDirections(int directions);

    int GetGtaoSteps() const;
    void SetGtaoSteps(int steps);

    bool IsGtaoDenoiseEnabled() const;
    void SetGtaoDenoiseEnabled(bool enabled);

    int GetSsaoShaderDebugMode() const;
    void SetSsaoShaderDebugMode(int mode);

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

    bool IsSsrEnabled() const;
    void SetSsrEnabled(bool enabled);
    void InvalidateSsrHistory();

    float GetSsrMaxTraceDistance() const;
    void SetSsrMaxTraceDistance(float distance);

    int GetSsrStepCount() const;
    void SetSsrStepCount(int steps);

    int GetSsrSampleCount() const;
    void SetSsrSampleCount(int samples);

    float GetSsrThickness() const;
    void SetSsrThickness(float thickness);

    float GetSsrRoughnessCutoff() const;
    void SetSsrRoughnessCutoff(float cutoff);

    bool IsSsrDenoiseEnabled() const;
    void SetSsrDenoiseEnabled(bool enabled);

    float GetSsrTemporalBlendFactor() const;
    void SetSsrTemporalBlendFactor(float factor);

    float GetSsrStrength() const;
    void SetSsrStrength(float strength);

    bool GetSsrSceneColorRanLastFrame() const;
    bool GetSsrTraceRanLastFrame() const;
    bool GetSsrDenoiseRanLastFrame() const;
    bool GetSsrTemporalRanLastFrame() const;
    int GetSsrTraceTargetWidth() const;
    int GetSsrTraceTargetHeight() const;

    RenderDebugMode GetDebugMode() const;
    void SetDebugMode(RenderDebugMode mode);
    void ResetRtPrimaryDebugBlitSettle();
    void NotifyRtPrimaryDebugDispatched();
    bool IsRtPrimaryDebugBlitReady() const;
    void SetDxrSmokeDebugSrv(std::uintptr_t srvCpuHandle);
    void SetDxrPrimaryDebugSrvs(
        std::uintptr_t primaryOutputSrvCpuHandle,
        std::uintptr_t primaryMetadataSrvCpuHandle);
    void SetDxrReflectionSrv(
        std::uintptr_t reflectionSrvCpuHandle,
        float uvScaleX = 1.0f,
        float uvScaleY = 1.0f);
    // Scene MRT SRV for external (DXR) consumers; 0 when unavailable.
    std::uintptr_t GetSceneColorSrvCpuHandle(int attachmentIndex) const;
    // Transitions the scene MRTs the reflection trace reads (RT0/1/2/3/5) to a combined
    // pixel + non-pixel shader-read state so DispatchRays can sample them legally.
    void PrepareSceneColorForDxrRead() const;

    AntiAliasingMode GetAntiAliasingMode() const;
    void SetAntiAliasingMode(AntiAliasingMode mode);

    int GetMsaaSampleCount() const;
    void SetMsaaSampleCount(int sampleCount);
    bool IsMsaaPendingReload() const;
    bool IsMsaaEnabled() const { return GetMsaaSampleCount() > 1; }
    void CopySettingsFrom(const ScreenSpaceEffects& source);

    float GetFxaaSubpixQuality() const;
    void SetFxaaSubpixQuality(float quality);

    float GetFxaaEdgeThreshold() const;
    void SetFxaaEdgeThreshold(float threshold);

    float GetRenderScale() const;
    void SetRenderScale(float scale);

    float GetTaaBlendFactor() const;
    void SetTaaBlendFactor(float factor);

    float GetGiTemporalBlendFactor() const;
    void SetGiTemporalBlendFactor(float factor);

    float GetGiDepthThreshold() const;
    void SetGiDepthThreshold(float threshold);

    bool IsSsgiDenoiseEnabled() const;
    void SetSsgiDenoiseEnabled(bool enabled);

    bool IsSsgiNoiseInjectionEnabled() const;
    void SetSsgiNoiseInjectionEnabled(bool enabled);

    float GetSsgiNoiseStrength() const;
    void SetSsgiNoiseStrength(float strength);

    float GetSsgiSpatialDepthThreshold() const;
    void SetSsgiSpatialDepthThreshold(float threshold);

    float GetSsgiSpatialBlurSpread() const;
    void SetSsgiSpatialBlurSpread(float spread);

    float GetSsgiRoughnessSpreadMin() const;
    void SetSsgiRoughnessSpreadMin(float spread);

    float GetSsgiRoughnessSpreadMax() const;
    void SetSsgiRoughnessSpreadMax(float spread);

    bool IsSsgiEnabled() const;
    void SetSsgiEnabled(bool enabled);

    float GetSsgiStrength() const;
    void SetSsgiStrength(float strength);

    float GetSsgiMaxTraceDistance() const;
    void SetSsgiMaxTraceDistance(float distance);

    int GetSsgiStepCount() const;
    void SetSsgiStepCount(int steps);

    float GetSsgiThickness() const;
    void SetSsgiThickness(float thickness);

    float GetSmaaThreshold() const;
    void SetSmaaThreshold(float threshold);

    int GetSmaaSearchSteps() const;
    void SetSmaaSearchSteps(int steps);

    int GetRenderWidth() const;
    int GetRenderHeight() const;

    float GetSsaoBlurDepthThreshold() const;
    void SetSsaoBlurDepthThreshold(float threshold);

    bool BlitDepthToFramebuffer(const Framebuffer* viewportTarget) const;
    std::uintptr_t GetSceneDepthSrvCpuHandle() const;

    void BeginSceneGridPass() const;
    void EndSceneGridPass() const;

    const SsaoDiagnosticsSnapshot& GetSsaoDiagnostics() const;

private:
    struct InternalTarget
    {
        void* resource = nullptr;
        void* allocation = nullptr;
        std::uint32_t srvIndex = UINT32_MAX;
        std::uintptr_t srvCpuHandle = 0;
        std::uint32_t rtvIndex = UINT32_MAX;
        int width = 0;
        int height = 0;
        // Tracked D3D12 resource state; 0 means pixel-shader-resource (set in CreateInternalTarget).
        mutable std::uint32_t resourceState = 0;
    };

    void CreateFullscreenQuad();
    void CreateNoiseTexture();
    void CreateKernel();
    void CreateInternalTarget(InternalTarget& target, int width, int height, int format);
    void DestroyInternalTarget(InternalTarget& target) const;
    void ResizeInternalTarget(InternalTarget& target, int width, int height, int format);
    void ResizeSingleChannelTargets(int width, int height);
    void ResizeHdrColorTarget(int width, int height);
    void ResizeSsrTargets(int width, int height);
    void ResizeBloomTargets(int width, int height);
    void ResizeLdrTonemapTarget(int width, int height);
    void ResizeAntiAliasingTargets(int width, int height);
    float GetActiveRenderScale() const;
    void ResetTaaHistory() const;
    void InvalidateTemporalHistory() const;
    void DrawFullscreenQuad() const;
    void DrawFullscreenPass(Shader& shader, bool viewportLdr) const;
    void DrawFullscreenToTarget(
        Shader& shader,
        InternalTarget& target,
        int width,
        int height,
        const float clearColor[4],
        bool viewportLdr = false) const;
    void BindOutputTarget(const Framebuffer* outputTarget, int viewportWidth, int viewportHeight) const;
    int GetEffectiveGeometryMsaaSampleCount() const;
    void EnsureMsaaDepthResolveShader() const;
    void FinalizePendingSsaoGpuReadback() const;
    void CaptureSsaoDiagnosticsCpu(
        const bool runSsao,
        const bool compositeRan,
        const bool compositeUsesSsao,
        const bool pbrDebugActive,
        const bool useShadowFactorComposite,
        const char* hdrColorSource,
        const char* ssaoDebugViewSource,
        std::uintptr_t hdrColorSrv,
        std::uintptr_t shadowFactorSrv) const;

    GpuBuffer m_quadVb;
    InternalTarget m_noiseTexture;
    InternalTarget m_ssaoTarget;
    InternalTarget m_ssaoBlurTarget;
    InternalTarget m_gtaoRawTarget;
    InternalTarget m_shadowBlurTarget;
    InternalTarget m_shadowBlur2Target;
    InternalTarget m_hdrCompositeTarget;
    InternalTarget m_radianceTarget;
    InternalTarget m_radianceTraceInputTarget;
    InternalTarget m_radianceSpatialBlurTarget;
    InternalTarget m_radianceSpatialTarget;
    InternalTarget m_radianceHistoryTarget;
    InternalTarget m_radianceTemporalTarget;
    InternalTarget m_radianceHistoryDepthTarget;
    InternalTarget m_ssrSceneColorTarget;
    InternalTarget m_ssrTraceTarget;
    InternalTarget m_ssrSpatialBlurTarget;
    InternalTarget m_ssrSpatialTarget;
    InternalTarget m_ssrHistoryTarget;
    InternalTarget m_ssrTemporalTarget;
    InternalTarget m_ssrVarianceHistoryTarget;
    InternalTarget m_ssrVarianceTemporalTarget;
    InternalTarget m_ssrHistoryDepthTarget;
    InternalTarget m_ssrResolvedTarget;
    InternalTarget m_ssrIndirectTarget;
    InternalTarget m_bloomExtractTarget;
    InternalTarget m_bloomBlurTarget;
    InternalTarget m_bloomBlur2Target;
    InternalTarget m_bloomHistoryTarget;
    InternalTarget m_bloomTemporalTarget;
    InternalTarget m_ldrTonemapTarget;
    InternalTarget m_smaaEdgeTarget;
    InternalTarget m_smaaOutputTarget;
    InternalTarget m_taaHistoryTarget;
    InternalTarget m_taaResolveTarget;
    std::unique_ptr<Framebuffer> m_sceneFramebuffer;
    std::unique_ptr<Shader> m_ssaoShader;
    std::unique_ptr<Shader> m_gtaoShader;
    std::unique_ptr<Shader> m_blurShader;
    std::unique_ptr<Shader> m_compositeShader;
    std::unique_ptr<Shader> m_bloomExtractShader;
    std::unique_ptr<Shader> m_bloomBlurShader;
    std::unique_ptr<Shader> m_bloomTemporalShader;
    std::unique_ptr<Shader> m_shadowBlurShader;
    std::unique_ptr<Shader> m_tonemapShader;
    std::unique_ptr<Shader> m_fxaaShader;
    std::unique_ptr<Shader> m_downsampleShader;
    std::unique_ptr<Shader> m_taaShader;
    std::unique_ptr<Shader> m_smaaEdgeShader;
    std::unique_ptr<Shader> m_smaaNeighborShader;
    std::unique_ptr<Shader> m_msaaDepthResolveShader;
    std::unique_ptr<Shader> m_debugChannelShader;
    std::unique_ptr<Shader> m_dxrPrimaryDebugShader;
    std::unique_ptr<Shader> m_velocityDebugShader;
    std::unique_ptr<Shader> m_gbufferDebugShader;
    std::unique_ptr<Shader> m_radianceAssemblyShader;
    std::unique_ptr<Shader> m_radianceDebugShader;
    std::unique_ptr<Shader> m_temporalReprojectShader;
    std::unique_ptr<Shader> m_giDepthHistoryShader;
    std::unique_ptr<Shader> m_giTemporalDebugShader;
    std::unique_ptr<Shader> m_ssgiNoiseInjectShader;
    std::unique_ptr<Shader> m_ssgiDenoiseSpatialShader;
    std::unique_ptr<Shader> m_ssgiDenoiseDebugShader;
    std::unique_ptr<Shader> m_ssgiTraceShader;
    std::unique_ptr<Shader> m_ssrSceneColorShader;
    std::unique_ptr<Shader> m_ssrDebugShader;
    std::unique_ptr<Shader> m_ssrTraceShader;
    std::unique_ptr<Shader> m_ssrTraceDebugShader;
    std::unique_ptr<Shader> m_ssrDenoiseDebugShader;
    std::unique_ptr<Shader> m_ssrSvgfTemporalShader;
    std::unique_ptr<Shader> m_ssrSvgfVarianceTemporalShader;
    std::unique_ptr<Shader> m_ssrSvgfAtrousShader;
    std::unique_ptr<Shader> m_ssrUpscaleShader;
    std::unique_ptr<Shader> m_ssrIndirectShader;

    std::vector<glm::vec3> m_kernelSamples;
    int m_width = 0;
    int m_height = 0;
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;

    bool m_enabled = true;
    mutable bool m_logHdrApplySnapshot = false;
    mutable bool m_logSsaoApplySnapshot = false;
    mutable bool m_pendingSsaoGpuReadback = false;
    mutable std::uint64_t m_ssaoDiagnosticsFrame = 0;
    mutable SsaoDiagnosticsSnapshot m_ssaoDiagnostics{};
    bool m_ssaoEnabled = true;
    AmbientOcclusionMode m_aoMode = AmbientOcclusionMode::SSAO;
    float m_ssaoRadius = 0.6f;
    float m_ssaoBias = 0.012f;
    float m_ssaoPower = 2.2f;
    float m_gtaoRadius = 1.0f;
    float m_gtaoThickness = 0.45f;
    float m_gtaoFalloff = 1.5f;
    float m_gtaoPower = 1.35f;
    int m_gtaoDirections = 4;
    int m_gtaoSteps = 5;
    bool m_gtaoDenoiseEnabled = true;
    int m_ssaoShaderDebugMode = 0;
    float m_aoStrength = 1.0f;
    float m_exposure = 0.0f;
    TonemapMode m_tonemapMode = TonemapMode::Gamma;
    bool m_bloomEnabled = false;
    float m_bloomThreshold = 1.0f;
    float m_bloomSoftKnee = 0.5f;
    float m_bloomIntensity = 0.4f;
    float m_bloomBlurRadius = 1.0f;
    float m_bloomTemporalBlendFactor = 0.88f;
    float m_bloomSameUvBlendFactor = 0.82f;
    float m_bloomDepthThreshold = 0.008f;
    float m_bloomOcclusionDepthThreshold = 0.12f;
    float m_ssrTraceResolutionScale = 1.0f;
    bool m_ssrEnabled = false;
    float m_ssrMaxTraceDistance = 25.0f;
    int m_ssrStepCount = 32;
    int m_ssrSampleCount = 2;
    float m_ssrThickness = 0.5f;
    float m_ssrRoughnessCutoff = 0.6f;
    float m_ssrStepExponent = 2.0f;
    bool m_ssrDenoiseEnabled = true;
    float m_ssrTemporalBlendFactor = 0.90f;
    float m_ssrSameUvBlendFactor = 0.93f;
    float m_ssrStrength = 1.0f;
    float m_ssrSpatialDepthThreshold = 0.010f;
    float m_ssrSpatialBlurSpread = 0.75f;
    float m_ssrRoughnessSpreadMin = 0.35f;
    float m_ssrRoughnessSpreadMax = 1.05f;
    float m_ssrDepthThreshold = 0.003f;
    float m_ssrSvgfPhiEpsilon = 0.002f;
    float m_ssrSvgfFilterStrength = 1.0f;
    mutable bool m_ssrSceneColorRanLastFrame = false;
    mutable bool m_ssrTraceRanLastFrame = false;
    mutable bool m_ssrDenoiseRanLastFrame = false;
    mutable bool m_ssrTemporalRanLastFrame = false;
    mutable bool m_ssrHistoryValid = false;
    mutable int m_ssrFrameIndex = 0;
    mutable std::uintptr_t m_lastSsrSpatialSrv = 0;
    mutable std::uintptr_t m_lastSsrTemporalSrv = 0;
    mutable std::uintptr_t m_lastSsrVarianceSrv = 0;
    mutable std::uintptr_t m_lastSsrDenoiseSrv = 0;
    mutable std::uintptr_t m_lastSsrResolvedSrv = 0;
    RenderDebugMode m_debugMode = RenderDebugMode::None;
    std::uintptr_t m_dxrSmokeDebugSrv = 0;
    std::uintptr_t m_dxrPrimaryOutputSrv = 0;
    std::uintptr_t m_dxrReflectionSrv = 0;
    float m_dxrReflectionUvScaleX = 1.0f;
    float m_dxrReflectionUvScaleY = 1.0f;
    std::uintptr_t m_dxrPrimaryMetadataSrv = 0;
    int m_rtPrimaryDebugSettleFrames = 0;
    AntiAliasingMode m_antiAliasingMode = AntiAliasingMode::None;
    int m_msaaSampleCount = 1;
    float m_fxaaSubpixQuality = 0.75f;
    float m_fxaaEdgeThreshold = 0.03125f;
    float m_renderScale = 1.5f;
    float m_taaBlendFactor = 0.9f;
    float m_giTemporalBlendFactor = 0.9f;
    float m_giDepthThreshold = 0.003f;
    bool m_ssgiDenoiseEnabled = true;
    bool m_ssgiNoiseInjectionEnabled = false;
    float m_ssgiNoiseStrength = 0.12f;
    float m_ssgiSpatialDepthThreshold = 0.02f;
    float m_ssgiSpatialBlurSpread = 1.0f;
    float m_ssgiRoughnessSpreadMin = 0.5f;
    float m_ssgiRoughnessSpreadMax = 1.75f;
    bool m_ssgiEnabled = false;
    float m_ssgiStrength = 0.35f;
    float m_ssgiMaxTraceDistance = 3.0f;
    int m_ssgiStepCount = 12;
    float m_ssgiThickness = 0.5f;
    float m_smaaThreshold = 0.05f;
    int m_smaaSearchSteps = 4;
    float m_ssaoBlurDepthThreshold = 0.02f;

    mutable bool m_taaHistoryValid = false;
    mutable int m_taaFrameIndex = 0;
    mutable bool m_bloomHistoryValid = false;
    mutable int m_bloomTemporalWarmupFrames = 0;
    // Last frame's final bloom SRV, fed into ssr_scene_color so reflections carry bloom
    // halos (bloom runs after SSR each frame). Reset whenever bloom targets are recreated.
    mutable std::uintptr_t m_prevFrameBloomSrv = 0;
    mutable bool m_radianceHistoryValid = false;
    mutable int m_giFrameIndex = 0;
    mutable glm::mat4 m_giPrevViewProjection{1.0f};
    mutable MotionVectorFrameState m_motionVectorFrameState{};
    mutable std::uintptr_t m_lastSsgiInjectSrv = 0;
    AntiAliasingMode m_lastAntiAliasingMode = AntiAliasingMode::None;
};
