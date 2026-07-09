#pragma once

#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/MotionVectorFrameState.h"
#include "engine/rendering/SsaoDiagnostics.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/PostProcessDraw.h"
#include "engine/rendering/post/PostProcessTarget.h"
#include "engine/rendering/post/PathTracerDisplayPass.h"

#include "engine/rhi/d3d12/GpuBuffer.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

class Camera;
class EnvironmentMap;
class Shader;

// PathTracerHistoryKey lives in PathTracerDisplayPass.h (HK-C8).

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
    // NVIDIA DLSS via Streamline (devdoc/rendering/dlss-super-resolution.md). Both own the resolve stage like
    // TAA. DLAA = DLSS at native res (pure AA); DLSS = super-resolution (renders below display res).
    DLAA = 6,
    DLSS = 7,
};

// DLSS Super Resolution quality preset. Per-axis internal render scale relative to display res
// (see DlssPresetRenderScale). DLAA (native) is a separate AntiAliasingMode, not a preset.
enum class DlssPreset
{
    Quality = 0,          // ~0.667x per axis
    Balanced = 1,         // ~0.58x
    Performance = 2,      // 0.5x
    UltraPerformance = 3, // ~0.333x
};

// Standard DLSS per-axis render-scale factors. In later phases the exact internal extent comes from
// slDLSSGetOptimalSettings; these fixed ratios drive the internal-res allocation until then.
float DlssPresetRenderScale(DlssPreset preset);

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
    // Phase P0 path tracing (devdoc/dxr/path-tracing.md). When path tracing is the active rendering
    // mode, blit the PT primary-hit output over the final image (reusing the primary-debug shader).
    // Independent of the debug-view combo — driven by SetDxrPathTracerDisplay.
    void SetDxrPathTracerDisplay(
        bool active,
        std::uintptr_t outputSrv,
        std::uintptr_t metadataSrv,
        PtConvergenceMode convergenceMode = PtConvergenceMode::RealTime,
        void* outputResource = nullptr,
        std::uint32_t outputResourceState = 0,
        void* depthResource = nullptr,
        std::uint32_t depthResourceState = 0,
        void* motionResource = nullptr,
        std::uint32_t motionResourceState = 0,
        std::uintptr_t depthSrv = 0,
        std::uintptr_t motionSrv = 0,
        std::uintptr_t diffuseAlbedoSrv = 0,
        std::uintptr_t specularAlbedoSrv = 0,
        std::uintptr_t normalRoughnessSrv = 0);
    bool IsPathTracerDisplayActive() const { return m_pathTracerActive; }
    // Diagnostic RR-input switchboard (DxrSettings::GetPtRrBundleMode; devdoc/dxr/pt/gi-shimmer.md).
    void SetPtRrBundleMode(int mode);
    bool PathTracerResolvedViaDlssThisFrame() const { return m_pathTracerDlssResolvedThisFrame; }
    bool PathTracerPostIntegratedThisFrame() const { return m_pathTracerPostIntegrated; }
    // Editor grid drawn into HDR before bloom when path tracing is active (not into the PT trace).
    using PathTracerGridOverlayFn = std::function<void(const Camera&, bool useDepthTest)>;
    void SetPathTracerGridOverlayCallback(PathTracerGridOverlayFn fn);
    void AccumulatePathTracerReference(
        const PathTracerHistoryKey& historyKey,
        std::uintptr_t currentFrameSrv,
        int width,
        int height);
    std::uint32_t GetPathTracerAccumSampleCount() const { return m_ptAccumSampleCount; }
    void ResetPathTracerAccumulation();
    void InvalidateAllTemporalState() const;
    // Motion/object discontinuity without resetting DLSS/TAA jitter history (play stop).
    void InvalidateMotionHistory() const;
    void BlitPathTracer(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight,
        float maxTraceDistance) const;
    void BlitRtReflectionDebug(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight) const;
    void BlitRtShadowDebug(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight) const;
    void BlitRtGiDebug(
        const Framebuffer* outputTarget,
        int viewportWidth,
        int viewportHeight) const;
    // RR1: visualize a DLSS-RR material guide (diffuse/specular albedo, normal-roughness). Runs the
    // guide pass straight to the output; only active for the RR-guide debug views (no cost otherwise).
    void BlitRrGuideDebug(
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
    bool IsPathTracerBlitReady() const;
    void SetDxrSmokeDebugSrv(std::uintptr_t srvCpuHandle);
    void SetDxrPrimaryDebugSrvs(
        std::uintptr_t primaryOutputSrvCpuHandle,
        std::uintptr_t primaryMetadataSrvCpuHandle);
    void SetDxrReflectionSrv(
        std::uintptr_t reflectionSrvCpuHandle,
        float uvScaleX = 1.0f,
        float uvScaleY = 1.0f,
        std::uintptr_t denoisedSrvCpuHandle = 0,
        float maxTraceDistance = 0.0f);
    // D6: when true (and reflection SRVs are set), Apply() runs the RT specular composite
    // and SKIPS the SSR indirect composite — the two are mutually exclusive per plan.
    void SetDxrReflectionCompositeEnabled(bool enabled) { m_dxrReflectionCompositeEnabled = enabled; }
    // Roughness cutoff for the RT specular composite — receivers rougher than this fade RT->IBL
    // (matches the trace's g_RoughnessCutoff skip so the handoff is seamless).
    void SetDxrReflectionRoughnessCutoff(float cutoff) { m_dxrReflectionRoughnessCutoff = cutoff; }
    // True when a reflection composite (RT reflections or SSR) will run this frame and therefore
    // ADD specular IBL back into the indirect. The PBR raster must OMIT spec IBL from RT1 exactly
    // when this is true (SceneRenderer sets IBL::SetReflectionsReplaceSpecIbl before the scene
    // pass). Single source of truth so the raster omit and the composite run stay in lockstep.
    bool ReflectionCompositeReplacesSpecIbl(
        bool dxrReflectionsEnabled, bool iblReady, RenderDebugMode debugMode) const;
    // Companion for RT diffuse GI: true when the GI inject will run and REPLACE the SH diffuse
    // ambient. The PBR raster must OMIT the SH diffuse ambient exactly when this is true.
    bool GiInjectReplacesDiffuseIbl(bool giActive, bool iblReady, RenderDebugMode debugMode) const;
    // D8: RT sun shadow mask. penumbraSrv drives the raw debug view; denoisedSrv is the
    // SIGMA-denoised mask consumed by the composite. 0 disables.
    void SetDxrShadowSrv(
        std::uintptr_t penumbraSrvCpuHandle,
        std::uintptr_t denoisedSrvCpuHandle = 0,
        float uvScaleX = 1.0f,
        float uvScaleY = 1.0f);
    // When true (and the denoised shadow SRV is set), the composite replaces the CSM sun shadow
    // factor with the RT mask. CSM stays default; RT shadows are a supplemental tier.
    void SetDxrShadowCompositeEnabled(bool enabled) { m_dxrShadowCompositeEnabled = enabled; }
    // D9: RT diffuse GI. rawSrv drives the raw debug view; denoisedSrv is the RELAX_DIFFUSE
    // output consumed by the inject pass (falls back to raw when denoise is off). 0 disables.
    void SetDxrGiSrv(
        std::uintptr_t giRawSrvCpuHandle,
        std::uintptr_t giDenoisedSrvCpuHandle = 0,
        float uvScaleX = 1.0f,
        float uvScaleY = 1.0f);
    // When true (and a GI SRV is set), Apply() runs the GI inject pass (adds diffuse bounce into
    // the indirect chain) and the composite skips the SSGI inject (mutually exclusive).
    void SetDxrGiCompositeEnabled(bool enabled) { m_dxrGiCompositeEnabled = enabled; }
    void SetDxrGiStrength(float strength) { m_dxrGiStrength = strength; }
    // Scene MRT SRV for external (DXR) consumers; 0 when unavailable.
    std::uintptr_t GetSceneColorSrvCpuHandle(GBufferSlot slot) const;
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

    DlssPreset GetDlssPreset() const;
    void SetDlssPreset(DlssPreset preset);

    // DLSS Ray Reconstruction (devdoc/dxr/dlss-rr.md). When active it replaces NRD + the SR model.
    bool GetRayReconstruction() const;
    void SetRayReconstruction(bool enabled);
    // True when RR will own the resolve this frame: a DLSS mode is selected, the RR toggle is on,
    // and RR is supported. (The RT-feature-on precondition is applied by the caller / SceneRenderer.)
    bool IsRayReconstructionActive() const;

    // In-DLSS sharpening (Streamline deprecated; 0 = off). Applied via sl::DLSSOptions::sharpness.
    float GetDlssSharpness() const;
    void SetDlssSharpness(float sharpness);

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

    // log2(render/display) when DLAA/DLSS is active, else 0. Add to the renderer's user mip bias
    // before GfxContext::SetMaterialTextureMipBias (see devdoc/rendering/dlss-super-resolution.md §Mip bias).
    float GetAutoMaterialMipBias() const;

    float GetSsaoBlurDepthThreshold() const;
    void SetSsaoBlurDepthThreshold(float threshold);

    bool BlitDepthToFramebuffer(const Framebuffer* viewportTarget) const;
    std::uintptr_t GetSceneDepthSrvCpuHandle() const;

    void BeginSceneGridPass() const;
    void EndSceneGridPass() const;

    const SsaoDiagnosticsSnapshot& GetSsaoDiagnostics() const;

private:
    using InternalTarget = PostProcessTarget;
    using InternalDepthTarget = PostProcessDepthTarget;

    // RR1: populate the DLSS-RR material guide targets (diffuse/specular albedo, normal-roughness)
    // from the G-buffer at render res. Cheap 3-pass; called only when RR (or an RR-guide debug
    // view) is active, so the RR-off path is unchanged.
    void GenerateRrGuides() const;

    void CreateFullscreenQuad();
    void CreateNoiseTexture();
    void CreateKernel();
    void CreateInternalTarget(InternalTarget& target, int width, int height, int format);
    // Like CreateInternalTarget but also flags the resource for unordered access and creates a UAV
    // (no RTV). Used for the DLSS scaling-output target, which NGX writes via a UAV.
    void CreateUavTarget(InternalTarget& target, int width, int height, int format);
    void DestroyInternalTarget(InternalTarget& target) const;
    void ResizeInternalTarget(InternalTarget& target, int width, int height, int format);
    void ResizeSingleChannelTargets(int width, int height);
    void ResizeHdrColorTarget(int width, int height);
    void ResizeSsrTargets(int width, int height);
    void ResizeBloomTargets(int width, int height);
    void ResizeLdrTonemapTarget(int width, int height);
    void ResizeAntiAliasingTargets(int width, int height);
    void ResizeDlssDisplayTargets(int viewportWidth, int viewportHeight);
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
    void PreparePathTracerDlssHdrInput() const;
    void CopyPathTracerHdrToCompositeTarget(const float clearColor[4]) const;
    void CreateInternalDepthTarget(InternalDepthTarget& target, int width, int height);
    void DestroyInternalDepthTarget(InternalDepthTarget& target) const;
    void ResizeDlssDisplayDepthTarget(int viewportWidth, int viewportHeight);
    void DrawPathTracerGridOverlayOntoHdrTarget(
        const Camera& camera,
        InternalTarget& target,
        int width,
        int height) const;
    void EnsureDepthBlitShader() const;
    void EnsurePtSkyMotionPatchShader() const;
    // P4: resolve the path tracer's R32 primary-depth UAV into m_ptDlssDepthTarget (D24) so DLSS gets
    // a depth buffer in the format Streamline expects. Returns true if the D24 target is ready to feed.
    bool ResolvePathTracerDlssDepth() const;
    // Real-time PT: patch sky pixels in the motion buffer (raster MV + PT sky MV from metadata).
    bool PatchPathTracerSkyMotion() const;
    // P4b: prepare the PT-side RR inputs the current bundle mode asks for (copy bounce-0 material
    // guides into the rr* targets, resolve PT depth to D24). Returns ready bits (1 = guides,
    // 2 = depth); full mode (0) is all-or-nothing. Sets m_ptFullGuidesThisFrame when guides copied
    // so GenerateRrGuides skips raster modes 0-2. See devdoc/dxr/pt/full-rr-guides.md.
    std::uint32_t PreparePathTracerRrBundle() const;
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
    PostProcessContext BuildPostProcessContext() const;
    PathTracerHdrCopyInputs BuildPathTracerHdrCopyInputs() const;

    struct ApplyFrameState;
    void InitApplyFrame(
        ApplyFrameState& state,
        const Camera& camera,
        int viewportWidth,
        int viewportHeight,
        const DirectionalShadowSettings& shadowSettings,
        const EnvironmentMap& environmentMap) const;
    void RunApplyLightingStage(ApplyFrameState& state) const;
    bool RunApplyDebugStage(ApplyFrameState& state) const;
    void RunApplyPresentationStage(ApplyFrameState& state) const;
    void FinalizeApplyFrame(ApplyFrameState& state) const;
    void FillAmbientOcclusionInputs(ApplyFrameState& state) const;
    void FillScreenSpaceReflectionInputs(ApplyFrameState& state) const;
    void FillScreenSpaceGiInputs(ApplyFrameState& state) const;
    void FillDlssResolveInputs(ApplyFrameState& state) const;

    GpuBuffer m_quadVb;
    mutable PostProcessDraw m_draw;
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
    InternalTarget m_rtIndirectTarget; // D6 RT specular composite (mutually exclusive with SSR)
    InternalTarget m_rtGiInjectTarget; // D9 RT diffuse GI inject (additive into the indirect chain)
    // RR1 (dxr-dlss-rr.md): DLSS Ray Reconstruction material guides, render-res, from the G-buffer.
    InternalTarget m_rrDiffuseAlbedoTarget;   // albedo * (1 - metallic)
    InternalTarget m_rrSpecularAlbedoTarget;  // F0 = lerp(0.04, albedo, metallic)
    InternalTarget m_rrNormalRoughnessTarget; // packed: world normal rgb + roughness a
    InternalTarget m_rrSpecularHitDistanceTarget; // R16F reflection ray length (world units), RR4
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
    InternalTarget m_ptAccumSumTarget;
    InternalTarget m_ptAccumScratchTarget;
    // DLSS path (S4): HDR upscale output + display-res bloom chain (post-DLSS tonemap input).
    InternalTarget m_dlssOutputTarget;
    InternalDepthTarget m_dlssDisplayDepthTarget;
    // P4: render-res D24 target holding the path tracer's primary-hit depth for DLSS (resolved from
    // the PT R32 depth UAV). Streamline expects a D24 depth resource; feeding the R32 UAV shimmers.
    InternalDepthTarget m_ptDlssDepthTarget;
    InternalTarget m_ptDlssMotionTarget;
    InternalTarget m_dlssBloomExtractTarget;
    InternalTarget m_dlssBloomBlurTarget;
    InternalTarget m_dlssBloomBlur2Target;
    InternalTarget m_dlssBloomHistoryTarget;
    InternalTarget m_dlssBloomTemporalTarget;
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
    std::unique_ptr<Shader> m_depthBlitShader;
    mutable std::unique_ptr<Shader> m_ptSkyMotionPatchShader;
    std::unique_ptr<Shader> m_debugChannelShader;
    std::unique_ptr<Shader> m_rtReflectionResolveShader;
    std::unique_ptr<Shader> m_dxrPrimaryDebugShader;
    std::unique_ptr<Shader> m_ptAccumulateShader;
    std::unique_ptr<Shader> m_ptMeanShader;
    std::unique_ptr<Shader> m_dxrShadowDebugShader;
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
    std::unique_ptr<Shader> m_dxrIndirectShader;
    std::unique_ptr<Shader> m_dxrGiInjectShader;
    std::unique_ptr<Shader> m_rrGuidesShader;

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
    // Phase P0 path tracing display (reuses the primary-debug blit shader).
    bool m_pathTracerActive = false;
    PtConvergenceMode m_pathTracerConvergenceMode = PtConvergenceMode::RealTime;
    std::uintptr_t m_dxrPathTracerOutputSrv = 0;
    std::uintptr_t m_dxrPathTracerMetadataSrv = 0;
    void* m_pathTracerOutputResource = nullptr;
    std::uint32_t m_pathTracerOutputResourceState = 0;
    void* m_pathTracerDepthResource = nullptr;
    std::uint32_t m_pathTracerDepthResourceState = 0;
    std::uintptr_t m_pathTracerDepthSrv = 0; // R32 primary-depth SRV (P4 resolve source)
    std::uintptr_t m_pathTracerMotionSrv = 0;
    void* m_pathTracerMotionResource = nullptr;
    std::uint32_t m_pathTracerMotionResourceState = 0;
    // P4b bounce-0 RR material guide SRVs (devdoc/dxr/pt/full-rr-guides.md).
    std::uintptr_t m_pathTracerDiffuseAlbedoSrv = 0;
    std::uintptr_t m_pathTracerSpecularAlbedoSrv = 0;
    std::uintptr_t m_pathTracerNormalRoughnessSrv = 0;
    // True after PreparePathTracerRrBundle copied PT guides into the rr* targets this frame:
    // GenerateRrGuides must then skip its raster material modes (0-2) to avoid overwriting them.
    mutable bool m_ptFullGuidesThisFrame = false;
    int m_ptRrBundleMode = 0;
    mutable bool m_pathTracerDlssResolvedThisFrame = false;
    mutable bool m_pathTracerPostIntegrated = false;
    PathTracerGridOverlayFn m_pathTracerGridOverlayDraw;
    mutable std::uint32_t m_ptAccumSampleCount = 0;
    mutable PathTracerHistoryKey m_ptAccumHistoryKey{};
    mutable bool m_ptAccumPingPongReadFromScratch = false;
    mutable std::uintptr_t m_ptAccumSumDisplaySrv = 0;
    std::uintptr_t m_dxrReflectionSrv = 0;
    std::uintptr_t m_dxrReflectionDenoisedSrv = 0;
    float m_dxrReflectionUvScaleX = 1.0f;
    float m_dxrReflectionUvScaleY = 1.0f;
    float m_dxrReflectionMaxTraceDistance = 0.0f;
    bool m_dxrReflectionCompositeEnabled = false;
    float m_dxrReflectionRoughnessCutoff = 0.6f;
    std::uintptr_t m_dxrShadowPenumbraSrv = 0;
    std::uintptr_t m_dxrShadowDenoisedSrv = 0;
    float m_dxrShadowUvScaleX = 1.0f;
    float m_dxrShadowUvScaleY = 1.0f;
    bool m_dxrShadowCompositeEnabled = false;
    std::uintptr_t m_dxrGiRawSrv = 0;
    std::uintptr_t m_dxrGiDenoisedSrv = 0;
    float m_dxrGiUvScaleX = 1.0f;
    float m_dxrGiUvScaleY = 1.0f;
    bool m_dxrGiCompositeEnabled = false;
    float m_dxrGiStrength = 1.0f;
    std::uintptr_t m_dxrPrimaryMetadataSrv = 0;
    int m_rtPrimaryDebugSettleFrames = 0;
    AntiAliasingMode m_antiAliasingMode = AntiAliasingMode::None;
    int m_msaaSampleCount = 1;
    float m_fxaaSubpixQuality = 0.75f;
    float m_fxaaEdgeThreshold = 0.03125f;
    float m_renderScale = 1.5f;
    DlssPreset m_dlssPreset = DlssPreset::Quality;
    bool m_rayReconstruction = false;
    float m_dlssSharpness = 0.0f;
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
    // DLSS temporal history validity (mirrors m_taaHistoryValid but for the DLSS resolve path). When
    // false the next DLSS evaluate sets the SL reset flag. Cleared on mode/preset change, resize,
    // and temporal-history invalidation.
    mutable bool m_dlssHistoryValid = false;
    mutable bool m_dlssBloomHistoryValid = false;
    mutable int m_dlssBloomTemporalWarmupFrames = 0;
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
