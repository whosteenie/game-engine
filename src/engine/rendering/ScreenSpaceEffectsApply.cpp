#include "engine/rendering/ScreenSpaceEffectsApply.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/platform/RenderPathDiagnostics.h"
#include "engine/platform/FrameDiagnostics.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/post/AmbientOcclusionPass.h"
#include "engine/rendering/post/AntiAliasingPass.h"
#include "engine/rendering/post/BloomTonemapPass.h"
#include "engine/rendering/post/DlssResolvePass.h"
#include "engine/rendering/post/PathTracerDisplayPass.h"
#include "engine/rendering/post/PostProcessDebugPass.h"
#include "engine/rendering/post/ScreenCompositePass.h"
#include "engine/rendering/post/ScreenSpaceGiPass.h"
#include "engine/rendering/post/ScreenSpaceReflectionPass.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"

#include <dxgiformat.h>

namespace
{
    DlssQuality ToDlssQuality(const DlssPreset preset)
    {
        switch (preset)
        {
        case DlssPreset::Quality: return DlssQuality::Quality;
        case DlssPreset::Balanced: return DlssQuality::Balanced;
        case DlssPreset::Performance: return DlssQuality::Performance;
        case DlssPreset::UltraPerformance: return DlssQuality::UltraPerformance;
        default: return DlssQuality::Quality;
        }
    }
}

void ScreenSpaceEffects::InitApplyFrame(
    ApplyFrameState& state,
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    const DirectionalShadowSettings& shadowSettings,
    const EnvironmentMap& environmentMap) const
{
    state.camera = &camera;
    state.shadowSettings = &shadowSettings;
    state.environmentMap = &environmentMap;
    state.outputTarget = GfxContext::Get().GetBoundOutputFramebuffer();
    state.viewportWidth = viewportWidth;
    state.viewportHeight = viewportHeight;

    state.debugMode = m_debugMode;
    state.pbrDebugActive = IsPbrMaterialDebugMode(m_debugMode);
    // PT owns the image — do not schedule hybrid AO/SSR/SSGI/TAA work from leftover UI toggles.
    state.runAo =
        !m_pathTracerActive && m_aoMode != AmbientOcclusionMode::Off && !state.pbrDebugActive;
    state.runSsao = state.runAo && m_aoMode == AmbientOcclusionMode::SSAO;
    state.runGtao = state.runAo && m_aoMode == AmbientOcclusionMode::GTAO;
    state.aoCompositeSrv = m_ssaoTarget.srvCpuHandle;

    state.projectionMatrix = camera.GetProjectionMatrix();
    state.inverseProjectionMatrix = glm::inverse(state.projectionMatrix);
    state.texelSize = glm::vec2(
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height));

    state.useShadowFactorComposite =
        !m_pathTracerActive && m_sceneFramebuffer->HasShadowFactor() && !state.pbrDebugActive;
    state.shadowFactorSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::SunShadowFactor);

    state.rtCompositeWanted = !m_pathTracerActive && m_dxrReflectionCompositeEnabled;
    state.rtHasFreshTrace = m_dxrReflectionSrv != 0;
    state.rtCompositeDebugOnly =
        !m_pathTracerActive && !state.rtCompositeWanted
        && m_debugMode == RenderDebugMode::RtSpecReplacement && m_dxrReflectionSrv != 0;

    state.giInjectSrv = m_dxrGiDenoisedSrv != 0 ? m_dxrGiDenoisedSrv : m_dxrGiRawSrv;
    state.giHasFreshTrace = state.giInjectSrv != 0;
    state.runRtGiInject =
        !m_pathTracerActive &&
        m_dxrGiCompositeEnabled &&
        !state.pbrDebugActive &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_rtGiInjectTarget.resource != nullptr &&
        environmentMap.GetIBL().IsReady();

    state.hdrColorSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting);
    state.hdrColorSource = "scene_direct";
    state.compositeRan = false;
    state.compositeUsesSsao = state.runAo;
    state.ssaoDebugViewSource = "none";

    state.useTaa = !m_pathTracerActive && m_antiAliasingMode == AntiAliasingMode::TAA;
    state.wantDlss = m_antiAliasingMode == AntiAliasingMode::DLAA
        || m_antiAliasingMode == AntiAliasingMode::DLSS;
    state.pathTracerReferenceActive =
        m_pathTracerActive && m_pathTracerConvergenceMode == PtConvergenceMode::Reference;
    state.effectiveWantDlss =
        state.wantDlss && !state.pathTracerReferenceActive && !IsPtIsolateDebugMode(m_debugMode);

    const_cast<ScreenSpaceEffects*>(this)->m_pathTracerDlssResolvedThisFrame = false;
    const_cast<ScreenSpaceEffects*>(this)->m_pathTracerPostIntegrated = false;
    const_cast<ScreenSpaceEffects*>(this)->m_postProcessDebugRenderedThisFrame = false;
}

void ScreenSpaceEffects::FillAmbientOcclusionInputs(ApplyFrameState& state) const
{
    AmbientOcclusionPassInputs& aoInputs = state.aoInputs;
    aoInputs = AmbientOcclusionPassInputs{};
    aoInputs.runSsao = state.runSsao;
    aoInputs.runGtao = state.runGtao;
    aoInputs.projectionMatrix = state.projectionMatrix;
    aoInputs.inverseProjectionMatrix = state.inverseProjectionMatrix;
    aoInputs.viewMatrix = state.camera->GetViewMatrix();
    aoInputs.texelSize = state.texelSize;
    aoInputs.nearPlane = state.camera->GetNearPlane();
    aoInputs.farPlane = state.camera->GetFarPlane();
    aoInputs.hasGeometryNormals = m_sceneFramebuffer->HasGeometryNormals();
    aoInputs.depthSrv = m_sceneFramebuffer->GetDepthSrvCpuHandle();
    aoInputs.normalSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal);
    aoInputs.noiseSrv = m_noiseTexture.srvCpuHandle;
    aoInputs.ssaoRadius = m_ssaoRadius;
    aoInputs.ssaoBias = m_ssaoBias;
    aoInputs.ssaoShaderDebugMode = m_ssaoShaderDebugMode;
    aoInputs.kernelSamples = m_kernelSamples.data();
    aoInputs.kernelSampleCount = KernelSampleCount;
    aoInputs.gtaoRadius = m_gtaoRadius;
    aoInputs.gtaoThickness = m_gtaoThickness;
    aoInputs.gtaoFalloff = m_gtaoFalloff;
    aoInputs.gtaoDirections = m_gtaoDirections;
    aoInputs.gtaoSteps = m_gtaoSteps;
    aoInputs.gtaoDenoiseEnabled = m_gtaoDenoiseEnabled;
    aoInputs.ssaoBlurDepthThreshold = m_ssaoBlurDepthThreshold;
    aoInputs.ssaoShader = m_ssaoShader.get();
    aoInputs.gtaoShader = m_gtaoShader.get();
    aoInputs.blurShader = m_blurShader.get();
    aoInputs.ssaoTarget = const_cast<InternalTarget*>(&m_ssaoTarget);
    aoInputs.ssaoBlurTarget = const_cast<InternalTarget*>(&m_ssaoBlurTarget);
    aoInputs.gtaoRawTarget = const_cast<InternalTarget*>(&m_gtaoRawTarget);
}

void ScreenSpaceEffects::FillScreenSpaceReflectionInputs(ApplyFrameState& state) const
{
    ScreenSpaceReflectionPassInputs& ssrInputs = state.ssrInputs;
    ssrInputs = ScreenSpaceReflectionPassInputs{};
    ssrInputs.pbrDebugActive = state.pbrDebugActive;
    ssrInputs.useShadowFactorComposite = state.useShadowFactorComposite;
    ssrInputs.rtCompositeWanted = state.rtCompositeWanted;
    ssrInputs.ssrEnabled = !m_pathTracerActive && m_ssrEnabled;
    ssrInputs.wantsSsr =
        !m_pathTracerActive && (m_ssrEnabled || IsSsrDebugMode(m_debugMode));
    ssrInputs.isSsrTraceDebug = IsSsrTraceDebugMode(m_debugMode);
    ssrInputs.isSsrDenoiseDebug = IsSsrDenoiseDebugMode(m_debugMode);
    ssrInputs.isSsrCompositeDebug = IsSsrCompositeDebugMode(m_debugMode);
    ssrInputs.ssrSpecReplacementDebug =
        !m_ssrEnabled && m_debugMode == RenderDebugMode::SsrSpecReplacement;
    ssrInputs.sceneHasSplitLighting = m_sceneFramebuffer->HasSplitLighting();
    ssrInputs.sceneHasShadowFactor = m_sceneFramebuffer->HasShadowFactor();
    ssrInputs.sceneHasGeometryNormals = m_sceneFramebuffer->HasGeometryNormals();
    ssrInputs.sceneHasMaterialGbuffer = m_sceneFramebuffer->HasMaterialGbuffer();
    ssrInputs.sceneHasVelocity = m_sceneFramebuffer->HasVelocity();
    ssrInputs.projectionMatrix = state.projectionMatrix;
    ssrInputs.inverseProjectionMatrix = state.inverseProjectionMatrix;
    ssrInputs.unjitteredProjectionMatrix = state.camera->GetUnjitteredProjectionMatrix();
    ssrInputs.viewMatrix = state.camera->GetViewMatrix();
    ssrInputs.texelSize = state.texelSize;
    ssrInputs.motionVectorState = m_motionVectorFrameState;
    ssrInputs.shadowFactorSrv = state.shadowFactorSrv;
    ssrInputs.directLightingSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting);
    ssrInputs.indirectLightingSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::IndirectLighting);
    ssrInputs.depthSrv = m_sceneFramebuffer->GetDepthSrvCpuHandle();
    ssrInputs.normalSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal);
    ssrInputs.material0Srv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
    ssrInputs.material1Srv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic);
    ssrInputs.velocitySrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity);
    ssrInputs.bloomEnabled = m_bloomEnabled;
    ssrInputs.bloomIntensity = m_bloomIntensity;
    ssrInputs.prevFrameBloomSrv = m_prevFrameBloomSrv;
    ssrInputs.ssrMaxTraceDistance = m_ssrMaxTraceDistance;
    ssrInputs.ssrStepCount = m_ssrStepCount;
    ssrInputs.ssrThickness = m_ssrThickness;
    ssrInputs.ssrRoughnessCutoff = m_ssrRoughnessCutoff;
    ssrInputs.ssrStepExponent = m_ssrStepExponent;
    ssrInputs.ssrSampleCount = m_ssrSampleCount;
    ssrInputs.ssrDenoiseEnabled = m_ssrDenoiseEnabled;
    ssrInputs.ssrTraceResolutionScale = m_ssrTraceResolutionScale;
    ssrInputs.ssrTemporalBlendFactor = m_ssrTemporalBlendFactor;
    ssrInputs.ssrSameUvBlendFactor = m_ssrSameUvBlendFactor;
    ssrInputs.ssrDepthThreshold = m_ssrDepthThreshold;
    ssrInputs.ssrSpatialDepthThreshold = m_ssrSpatialDepthThreshold;
    ssrInputs.ssrSpatialBlurSpread = m_ssrSpatialBlurSpread;
    ssrInputs.ssrRoughnessSpreadMin = m_ssrRoughnessSpreadMin;
    ssrInputs.ssrRoughnessSpreadMax = m_ssrRoughnessSpreadMax;
    ssrInputs.ssrSvgfPhiEpsilon = m_ssrSvgfPhiEpsilon;
    ssrInputs.ssrSvgfFilterStrength = m_ssrSvgfFilterStrength;
    ssrInputs.ssrStrength = m_ssrStrength;
    ssrInputs.ssrFrameIndex = m_ssrFrameIndex;
    ssrInputs.ssrHistoryValid = m_ssrHistoryValid;
    if (state.environmentMap->GetIBL().IsReady())
    {
        const IBL& ibl = state.environmentMap->GetIBL();
        ssrInputs.iblReady = true;
        ssrInputs.environmentIntensity = ibl.GetEnvironmentIntensity();
        ssrInputs.maxReflectionLod = ibl.GetMaxReflectionLod();
        ssrInputs.prefilterMapSrv = ibl.GetPrefilterMapSrvCpuHandle();
        ssrInputs.brdfLutSrv = ibl.GetBrdfLutSrvCpuHandle();
    }
    ssrInputs.sceneFramebuffer = m_sceneFramebuffer.get();
    ssrInputs.ssrSceneColorShader = m_ssrSceneColorShader.get();
    ssrInputs.ssrTraceShader = m_ssrTraceShader.get();
    ssrInputs.ssrSvgfTemporalShader = m_ssrSvgfTemporalShader.get();
    ssrInputs.temporalReprojectShader = m_temporalReprojectShader.get();
    ssrInputs.giDepthHistoryShader = m_giDepthHistoryShader.get();
    ssrInputs.ssrSvgfVarianceTemporalShader = m_ssrSvgfVarianceTemporalShader.get();
    ssrInputs.ssrSvgfAtrousShader = m_ssrSvgfAtrousShader.get();
    ssrInputs.ssrUpscaleShader = m_ssrUpscaleShader.get();
    ssrInputs.ssrIndirectShader = m_ssrIndirectShader.get();
    ssrInputs.ssrSceneColorTarget = const_cast<InternalTarget*>(&m_ssrSceneColorTarget);
    ssrInputs.ssrTraceTarget = const_cast<InternalTarget*>(&m_ssrTraceTarget);
    ssrInputs.ssrSpatialTarget = const_cast<InternalTarget*>(&m_ssrSpatialTarget);
    ssrInputs.ssrSpatialBlurTarget = const_cast<InternalTarget*>(&m_ssrSpatialBlurTarget);
    ssrInputs.ssrHistoryTarget = const_cast<InternalTarget*>(&m_ssrHistoryTarget);
    ssrInputs.ssrTemporalTarget = const_cast<InternalTarget*>(&m_ssrTemporalTarget);
    ssrInputs.ssrVarianceHistoryTarget = const_cast<InternalTarget*>(&m_ssrVarianceHistoryTarget);
    ssrInputs.ssrVarianceTemporalTarget = const_cast<InternalTarget*>(&m_ssrVarianceTemporalTarget);
    ssrInputs.ssrHistoryDepthTarget = const_cast<InternalTarget*>(&m_ssrHistoryDepthTarget);
    ssrInputs.ssrResolvedTarget = const_cast<InternalTarget*>(&m_ssrResolvedTarget);
    ssrInputs.ssrIndirectTarget = const_cast<InternalTarget*>(&m_ssrIndirectTarget);
}

void ScreenSpaceEffects::FillScreenSpaceGiInputs(ApplyFrameState& state) const
{
    ScreenSpaceGiPassInputs& ssgiInputs = state.ssgiInputs;
    ssgiInputs = ScreenSpaceGiPassInputs{};
    ssgiInputs.runRadianceAssembly = state.runRadianceAssembly;
    ssgiInputs.ssgiEnabled = !m_pathTracerActive && m_ssgiEnabled;
    ssgiInputs.ssgiDenoiseEnabled = m_ssgiDenoiseEnabled;
    ssgiInputs.ssgiNoiseInjectionEnabled = m_ssgiNoiseInjectionEnabled;
    ssgiInputs.sceneHasGeometryNormals = m_sceneFramebuffer->HasGeometryNormals();
    ssgiInputs.sceneHasMaterialGbuffer = m_sceneFramebuffer->HasMaterialGbuffer();
    ssgiInputs.projectionMatrix = state.projectionMatrix;
    ssgiInputs.inverseProjectionMatrix = state.inverseProjectionMatrix;
    ssgiInputs.viewMatrix = state.camera->GetViewMatrix();
    ssgiInputs.unjitteredProjectionMatrix = state.camera->GetUnjitteredProjectionMatrix();
    ssgiInputs.texelSize = state.texelSize;
    ssgiInputs.motionVectorState = m_motionVectorFrameState;
    ssgiInputs.giFrameIndex = m_giFrameIndex;
    ssgiInputs.radianceHistoryValid = m_radianceHistoryValid;
    ssgiInputs.radianceSrv = m_radianceTarget.srvCpuHandle;
    ssgiInputs.depthSrv = m_sceneFramebuffer->GetDepthSrvCpuHandle();
    ssgiInputs.normalSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal);
    ssgiInputs.material0Srv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
    ssgiInputs.material1Srv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic);
    ssgiInputs.ssgiMaxTraceDistance = m_ssgiMaxTraceDistance;
    ssgiInputs.ssgiStepCount = m_ssgiStepCount;
    ssgiInputs.ssgiThickness = m_ssgiThickness;
    ssgiInputs.ssgiNoiseStrength = m_ssgiNoiseStrength;
    ssgiInputs.ssgiSpatialDepthThreshold = m_ssgiSpatialDepthThreshold;
    ssgiInputs.ssgiSpatialBlurSpread = m_ssgiSpatialBlurSpread;
    ssgiInputs.ssgiRoughnessSpreadMin = m_ssgiRoughnessSpreadMin;
    ssgiInputs.ssgiRoughnessSpreadMax = m_ssgiRoughnessSpreadMax;
    ssgiInputs.giTemporalBlendFactor = m_giTemporalBlendFactor;
    ssgiInputs.giDepthThreshold = m_giDepthThreshold;
    ssgiInputs.sceneFramebuffer = m_sceneFramebuffer.get();
    ssgiInputs.ssgiTraceShader = m_ssgiTraceShader.get();
    ssgiInputs.ssgiNoiseInjectShader = m_ssgiNoiseInjectShader.get();
    ssgiInputs.ssgiDenoiseSpatialShader = m_ssgiDenoiseSpatialShader.get();
    ssgiInputs.temporalReprojectShader = m_temporalReprojectShader.get();
    ssgiInputs.giDepthHistoryShader = m_giDepthHistoryShader.get();
    ssgiInputs.radianceTraceInputTarget = const_cast<InternalTarget*>(&m_radianceTraceInputTarget);
    ssgiInputs.radianceSpatialTarget = const_cast<InternalTarget*>(&m_radianceSpatialTarget);
    ssgiInputs.radianceSpatialBlurTarget = const_cast<InternalTarget*>(&m_radianceSpatialBlurTarget);
    ssgiInputs.radianceHistoryTarget = const_cast<InternalTarget*>(&m_radianceHistoryTarget);
    ssgiInputs.radianceTemporalTarget = const_cast<InternalTarget*>(&m_radianceTemporalTarget);
    ssgiInputs.radianceHistoryDepthTarget = const_cast<InternalTarget*>(&m_radianceHistoryDepthTarget);
}

void ScreenSpaceEffects::FillDlssResolveInputs(ApplyFrameState& state) const
{
    DlssResolvePassInputs& dlssInputs = state.dlssInputs;
    dlssInputs = DlssResolvePassInputs{};
    dlssInputs.camera = state.camera;
    dlssInputs.sceneFramebuffer = m_sceneFramebuffer.get();
    dlssInputs.outputTarget = state.outputTarget;
    dlssInputs.motionVectorState = m_motionVectorFrameState;
    dlssInputs.viewportWidth = state.viewportWidth;
    dlssInputs.viewportHeight = state.viewportHeight;
    dlssInputs.dlssViewportId = m_dlssViewportId;
    dlssInputs.hdrColorSrv = state.hdrColorSrv;
    dlssInputs.hdrCompositeTarget = const_cast<InternalTarget*>(&m_hdrCompositeTarget);
    dlssInputs.pathTracerActive = m_pathTracerActive;
    dlssInputs.pathTracerConvergenceMode = m_pathTracerConvergenceMode;
    dlssInputs.pathTracerOutputResource = m_pathTracerOutputResource;
    dlssInputs.pathTracerOutputResourceState = m_pathTracerOutputResourceState;
    dlssInputs.dxrPathTracerOutputSrv = m_dxrPathTracerOutputSrv;
    dlssInputs.dxrReflectionSrv = m_dxrReflectionSrv;
    dlssInputs.pathTracerGridOverlayEnabled = m_pathTracerGridOverlayDraw != nullptr;
    dlssInputs.quality = m_antiAliasingMode == AntiAliasingMode::DLAA
        ? DlssQuality::DLAA
        : ToDlssQuality(m_dlssPreset);
    dlssInputs.exposure = m_exposure;
    dlssInputs.meanInputLuminance = m_ptMeanLuminance;
    dlssInputs.meanInputLuminanceValid = m_ptBoilMetricValid;
    dlssInputs.dlssSharpness = m_dlssSharpness;
    dlssInputs.rrPreset = m_rrPreset;
    dlssInputs.tonemapMode = static_cast<int>(m_tonemapMode);
    dlssInputs.dlssHistoryValid = m_dlssHistoryValid;
    dlssInputs.forceDlssResetEveryFrame = m_forceDlssResetEveryFrame;
    dlssInputs.useDilatedDlssMotionVectors = m_useDilatedDlssMotionVectors;
    dlssInputs.reconstructDlssCameraMotion = m_reconstructDlssCameraMotion;
    dlssInputs.useSubmissionFrameIndex = m_useDlssSubmissionFrameIndexDiagnostic;
    dlssInputs.bloomEnabled = m_bloomEnabled;
    dlssInputs.bloomThreshold = m_bloomThreshold;
    dlssInputs.bloomSoftKnee = m_bloomSoftKnee;
    dlssInputs.bloomBlurRadius = m_bloomBlurRadius;
    dlssInputs.bloomIntensity = m_bloomIntensity;
    dlssInputs.bloomTemporalBlendFactor = m_bloomTemporalBlendFactor;
    dlssInputs.bloomSameUvBlendFactor = m_bloomSameUvBlendFactor;
    dlssInputs.bloomDepthThreshold = m_bloomDepthThreshold;
    dlssInputs.dlssBloomHistoryValid = m_dlssBloomHistoryValid;
    dlssInputs.dlssBloomTemporalWarmupFrames = m_dlssBloomTemporalWarmupFrames;
    dlssInputs.rayReconstructionActive = IsRayReconstructionActive();
    dlssInputs.dlssOutputTarget = const_cast<InternalTarget*>(&m_dlssOutputTarget);
    dlssInputs.ptDlssMotionTarget = const_cast<InternalTarget*>(&m_ptDlssMotionTarget);
    dlssInputs.dlssDilatedMotionTarget = const_cast<InternalTarget*>(&m_dlssDilatedMotionTarget);
    dlssInputs.rrDiffuseAlbedoTarget = const_cast<InternalTarget*>(&m_rrDiffuseAlbedoTarget);
    dlssInputs.rrSpecularAlbedoTarget = const_cast<InternalTarget*>(&m_rrSpecularAlbedoTarget);
    dlssInputs.rrNormalRoughnessTarget = const_cast<InternalTarget*>(&m_rrNormalRoughnessTarget);
    dlssInputs.rrSpecularHitDistanceTarget =
        const_cast<InternalTarget*>(&m_rrSpecularHitDistanceTarget);
    dlssInputs.dlssBloomExtractTarget = const_cast<InternalTarget*>(&m_dlssBloomExtractTarget);
    dlssInputs.dlssBloomBlurTarget = const_cast<InternalTarget*>(&m_dlssBloomBlurTarget);
    dlssInputs.dlssBloomBlur2Target = const_cast<InternalTarget*>(&m_dlssBloomBlur2Target);
    dlssInputs.dlssBloomHistoryTarget = const_cast<InternalTarget*>(&m_dlssBloomHistoryTarget);
    dlssInputs.dlssBloomTemporalTarget = const_cast<InternalTarget*>(&m_dlssBloomTemporalTarget);
    dlssInputs.bloomExtractShader = m_bloomExtractShader.get();
    dlssInputs.bloomBlurShader = m_bloomBlurShader.get();
    dlssInputs.bloomTemporalShader = m_bloomTemporalShader.get();
    dlssInputs.tonemapShader = m_tonemapShader.get();
    dlssInputs.dlssMotionDilateShader = m_dlssMotionDilateShader.get();
    dlssInputs.fallbackTonemapInputs.hdrColorSrv = state.hdrColorSrv;
    dlssInputs.fallbackTonemapInputs.bloomSrv = state.bloomSrv;
    dlssInputs.fallbackTonemapInputs.texelSize = state.texelSize;
    dlssInputs.fallbackTonemapInputs.exposure = m_exposure;
    dlssInputs.fallbackTonemapInputs.tonemapMode = static_cast<int>(m_tonemapMode);
    dlssInputs.fallbackTonemapInputs.bloomEnabled = m_bloomEnabled;
    dlssInputs.fallbackTonemapInputs.bloomIntensity = m_bloomIntensity;
    dlssInputs.fallbackTonemapInputs.tonemapShader = m_tonemapShader.get();
    dlssInputs.patchPathTracerSkyMotion = [this]() { return PatchPathTracerSkyMotion(); };
    dlssInputs.generateRrGuides = [this]() { GenerateRrGuides(); };
    dlssInputs.generateDilatedDlssMotion =
        [this](const std::uintptr_t depthSrv, const std::uintptr_t motionSrv)
        {
            return GenerateDilatedDlssMotion(depthSrv, motionSrv);
        };
    dlssInputs.generateZeroDlssMotion = [this]() { return GenerateZeroDlssMotion(); };
    // P4b PT RR bundle: prepare callback + the PT depth/motion resources the resolve swaps to
    // per the bundle mode (devdoc/dxr/pt/full-rr-guides.md; gi-shimmer.md switchboard).
    dlssInputs.preparePathTracerRrBundle = [this]() { return PreparePathTracerRrBundle(); };
    dlssInputs.ptRrBundleMode = m_ptRrBundleMode;
    dlssInputs.ptDlssDepthTarget = const_cast<InternalDepthTarget*>(&m_ptDlssDepthTarget);
    dlssInputs.pathTracerMotionResource = m_pathTracerMotionResource;
    dlssInputs.pathTracerMotionResourceState = m_pathTracerMotionResourceState;
    dlssInputs.pathTracerDepthSrv = m_pathTracerDepthSrv;
    dlssInputs.pathTracerMotionSrv = m_pathTracerMotionSrv;
    dlssInputs.drawPathTracerGridOverlay =
        [this, camera = state.camera](PostProcessTarget& target, const int width, const int height)
        {
            DrawPathTracerGridOverlayOntoHdrTarget(
                *camera,
                static_cast<InternalTarget&>(target),
                width,
                height);
        };
}

void ScreenSpaceEffects::RunApplyLightingStage(ApplyFrameState& state) const
{
    const PostProcessContext postContext = BuildPostProcessContext();

    // Path tracing: skip the hybrid lighting chain (AO / shadow blur / SSR / RT composite /
    // SSGI / HDR composite / TAA). PT integrate + bloom below still feed DLSS/tonemap.
    if (!m_pathTracerActive)
    {
        if (state.runSsao || state.runGtao)
        {
            const GfxContext::GpuTimerScope gpuScopeAo("Post-process/AO");
            FillAmbientOcclusionInputs(state);
            AmbientOcclusionPassOutputs aoOutputs{};
            aoOutputs.aoCompositeSrv = m_ssaoTarget.srvCpuHandle;
            AmbientOcclusionPass::Execute(postContext, state.aoInputs, aoOutputs);
            state.aoCompositeSrv = aoOutputs.aoCompositeSrv;
        }

        {
            const GfxContext::GpuTimerScope gpuScopeShadowPrep("Post-process/Shadow prep");
            ScreenCompositePrePassInputs prePassInputs{};
            prePassInputs.pbrDebugActive = state.pbrDebugActive;
            prePassInputs.useShadowFactorComposite = state.useShadowFactorComposite;
            prePassInputs.inverseProjectionMatrix = state.inverseProjectionMatrix;
            prePassInputs.texelSize = state.texelSize;
            prePassInputs.shadowSettings = state.shadowSettings;
            prePassInputs.sceneFramebuffer = m_sceneFramebuffer.get();
            prePassInputs.shadowFactorSrv = state.shadowFactorSrv;
            prePassInputs.shadowBlurShader = m_shadowBlurShader.get();
            prePassInputs.radianceAssemblyShader = m_radianceAssemblyShader.get();
            prePassInputs.shadowBlurTarget = const_cast<InternalTarget*>(&m_shadowBlurTarget);
            prePassInputs.shadowBlur2Target = const_cast<InternalTarget*>(&m_shadowBlur2Target);
            prePassInputs.radianceTarget = const_cast<InternalTarget*>(&m_radianceTarget);

            ScreenCompositePrePassOutputs prePassOutputs{};
            ScreenCompositePass::ExecutePreReflection(postContext, prePassInputs, prePassOutputs);
            state.shadowFactorSrv = prePassOutputs.shadowFactorSrv;
            state.runRadianceAssembly = prePassOutputs.runRadianceAssembly;
        }

        {
            const GfxContext::GpuTimerScope gpuScopeSsr("Post-process/SSR");
            FillScreenSpaceReflectionInputs(state);
            ScreenSpaceReflectionPassOutputs ssrOutputs{};
            ScreenSpaceReflectionPass::Execute(postContext, state.ssrInputs, ssrOutputs);

            m_ssrSceneColorRanLastFrame = ssrOutputs.ssrSceneColorRanLastFrame;
            m_ssrTraceRanLastFrame = ssrOutputs.ssrTraceRanLastFrame;
            m_ssrDenoiseRanLastFrame = ssrOutputs.ssrDenoiseRanLastFrame;
            m_ssrTemporalRanLastFrame = ssrOutputs.ssrTemporalRanLastFrame;
            m_lastSsrSpatialSrv = ssrOutputs.lastSsrSpatialSrv;
            m_lastSsrTemporalSrv = ssrOutputs.lastSsrTemporalSrv;
            m_lastSsrVarianceSrv = ssrOutputs.lastSsrVarianceSrv;
            m_lastSsrDenoiseSrv = ssrOutputs.lastSsrDenoiseSrv;
            m_lastSsrResolvedSrv = ssrOutputs.lastSsrResolvedSrv;
            m_ssrFrameIndex = ssrOutputs.ssrFrameIndex;
            m_ssrHistoryValid = ssrOutputs.ssrHistoryValid;
            state.runSsrIndirect = ssrOutputs.ssrIndirectRan;
            state.indirectCompositeSrv = ssrOutputs.indirectCompositeSrv;
        }

        {
            const GfxContext::GpuTimerScope gpuScopeRtComposite("Post-process/RT composite");
            ScreenCompositeDxrInputs dxrInputs{};
            dxrInputs.pbrDebugActive = state.pbrDebugActive;
            dxrInputs.rtCompositeWanted = state.rtCompositeWanted;
            dxrInputs.rtCompositeDebugOnly = state.rtCompositeDebugOnly;
            dxrInputs.rtHasFreshTrace = state.rtHasFreshTrace;
            dxrInputs.runRtGiInject = state.runRtGiInject;
            dxrInputs.giHasFreshTrace = state.giHasFreshTrace;
            dxrInputs.inverseProjectionMatrix = state.inverseProjectionMatrix;
            dxrInputs.camera = state.camera;
            dxrInputs.environmentMap = state.environmentMap;
            dxrInputs.sceneFramebuffer = m_sceneFramebuffer.get();
            dxrInputs.indirectCompositeSrv = state.indirectCompositeSrv;
            dxrInputs.dxrReflectionSrv = m_dxrReflectionSrv;
            dxrInputs.dxrReflectionDenoisedSrv = m_dxrReflectionDenoisedSrv;
            dxrInputs.dxrReflectionMaxTraceDistance = m_dxrReflectionMaxTraceDistance;
            dxrInputs.dxrReflectionRoughnessCutoff = m_dxrReflectionRoughnessCutoff;
            dxrInputs.dxrReflectionUvScaleX = m_dxrReflectionUvScaleX;
            dxrInputs.dxrReflectionUvScaleY = m_dxrReflectionUvScaleY;
            dxrInputs.giInjectSrv = state.giInjectSrv;
            dxrInputs.dxrGiStrength = m_dxrGiStrength;
            dxrInputs.dxrGiUvScaleX = m_dxrGiUvScaleX;
            dxrInputs.dxrGiUvScaleY = m_dxrGiUvScaleY;
            dxrInputs.dxrIndirectShader = m_dxrIndirectShader.get();
            dxrInputs.dxrGiInjectShader = m_dxrGiInjectShader.get();
            dxrInputs.rtIndirectTarget = const_cast<InternalTarget*>(&m_rtIndirectTarget);
            dxrInputs.rtGiInjectTarget = const_cast<InternalTarget*>(&m_rtGiInjectTarget);

            ScreenCompositeDxrOutputs dxrOutputs{};
            ScreenCompositePass::ExecuteDxrIndirectChain(postContext, dxrInputs, dxrOutputs);
            state.indirectCompositeSrv = dxrOutputs.indirectCompositeSrv;
            state.runRtIndirect = dxrOutputs.runRtIndirect;
        }

        {
            const GfxContext::GpuTimerScope gpuScopeSsgi("Post-process/SSGI");
            FillScreenSpaceGiInputs(state);
            ScreenSpaceGiPassOutputs ssgiOutputs{};
            ScreenSpaceGiPass::Execute(postContext, state.ssgiInputs, ssgiOutputs);

            m_giFrameIndex = ssgiOutputs.giFrameIndex;
            m_radianceHistoryValid = ssgiOutputs.radianceHistoryValid;
            m_lastSsgiInjectSrv = ssgiOutputs.lastSsgiInjectSrv;
            state.runSsgiTrace = ssgiOutputs.runSsgiTrace;
        }

        {
            const GfxContext::GpuTimerScope gpuScopeHdrComposite("Post-process/HDR composite");
            ScreenCompositeHdrInputs hdrInputs{};
            hdrInputs.pbrDebugActive = state.pbrDebugActive;
            hdrInputs.runAo = state.runAo;
            hdrInputs.runGtao = state.runGtao;
            hdrInputs.useShadowFactorComposite = state.useShadowFactorComposite;
            hdrInputs.runRtGiInject = state.runRtGiInject;
            hdrInputs.runSsgiTrace = state.runSsgiTrace;
            hdrInputs.camera = state.camera;
            hdrInputs.environmentMap = state.environmentMap;
            hdrInputs.sceneFramebuffer = m_sceneFramebuffer.get();
            hdrInputs.aoCompositeSrv = state.aoCompositeSrv;
            hdrInputs.shadowFactorSrv = state.shadowFactorSrv;
            hdrInputs.indirectCompositeSrv = state.indirectCompositeSrv;
            hdrInputs.lastSsgiInjectSrv = m_lastSsgiInjectSrv;
            hdrInputs.aoStrength = m_aoStrength;
            hdrInputs.ssaoPower = m_ssaoPower;
            hdrInputs.gtaoPower = m_gtaoPower;
            hdrInputs.ssgiStrength = m_ssgiStrength;
            hdrInputs.dxrShadowDenoisedSrv = m_dxrShadowDenoisedSrv;
            hdrInputs.dxrShadowUvScaleX = m_dxrShadowUvScaleX;
            hdrInputs.dxrShadowUvScaleY = m_dxrShadowUvScaleY;
            hdrInputs.dxrShadowCompositeEnabled = m_dxrShadowCompositeEnabled;
            hdrInputs.debugMode = static_cast<int>(m_debugMode);
            hdrInputs.compositeShader = m_compositeShader.get();
            hdrInputs.radianceTarget = const_cast<InternalTarget*>(&m_radianceTarget);
            hdrInputs.hdrCompositeTarget = const_cast<InternalTarget*>(&m_hdrCompositeTarget);

            ScreenCompositeHdrOutputs hdrOutputs{};
            ScreenCompositePass::ExecuteHdrComposite(postContext, hdrInputs, hdrOutputs);
            state.hdrColorSrv = hdrOutputs.hdrColorSrv;
            state.hdrColorSource = hdrOutputs.hdrColorSource;
            state.compositeRan = hdrOutputs.compositeRan;
        }

        {
            const GfxContext::GpuTimerScope gpuScopeTaa("Post-process/TAA");
            TaaPassInputs taaInputs{};
            taaInputs.useTaa = state.useTaa;
            taaInputs.hdrColorSrv = state.hdrColorSrv;
            taaInputs.texelSize = state.texelSize;
            taaInputs.viewMatrix = state.camera->GetViewMatrix();
            taaInputs.unjitteredProjectionMatrix = state.camera->GetUnjitteredProjectionMatrix();
            taaInputs.motionVectorState = m_motionVectorFrameState;
            taaInputs.taaBlendFactor = m_taaBlendFactor;
            taaInputs.taaHistoryValid = m_taaHistoryValid;
            taaInputs.sceneFramebuffer = m_sceneFramebuffer.get();
            taaInputs.taaShader = m_taaShader.get();
            taaInputs.taaHistoryTarget = const_cast<InternalTarget*>(&m_taaHistoryTarget);
            taaInputs.taaResolveTarget = const_cast<InternalTarget*>(&m_taaResolveTarget);

            TaaPassOutputs taaOutputs{};
            AntiAliasingPass::ExecuteTaa(postContext, taaInputs, taaOutputs);
            if (taaOutputs.ran)
            {
                state.hdrColorSrv = taaOutputs.hdrColorSrv;
                state.hdrColorSource = "hdr_taa";
                const_cast<ScreenSpaceEffects*>(this)->m_taaHistoryValid = taaOutputs.taaHistoryValid;
            }
        }
    }
    // Temporal diagnostic targets exist only for their dedicated debug views. Keeping this
    // behind the view selection avoids full-resolution FP32 history storage during ordinary PT.
    else if (m_dxrPathTracerOutputSrv != 0 && IsPtTemporalStatsDebugMode(m_debugMode))
    {
        const GfxContext::GpuTimerScope gpuScopePtStats("Post-process/PT temporal diagnostics");
        UpdatePathTracerTemporalDiagnostics(*state.camera);
    }
    else if (m_dxrPathTracerOutputSrv != 0
        && (IsPtMotionReprojectionDebugMode(m_debugMode)
            || IsPtDepthReprojectionDebugMode(m_debugMode)
            || IsPtMatrixDepthReprojectionDebugMode(m_debugMode)))
    {
        PreparePathTracerMotionReprojectionAudit();
    }

    if (m_pathTracerActive && m_dxrPathTracerOutputSrv != 0 && !state.effectiveWantDlss
        && !IsPbrMaterialDebugMode(m_debugMode))
    {
        const GfxContext::GpuTimerScope gpuScopePtIntegrate("Post-process/Path tracer integrate");
        const int hdrFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
        const_cast<ScreenSpaceEffects*>(this)->ResizeInternalTarget(
            const_cast<ScreenSpaceEffects*>(this)->m_hdrCompositeTarget,
            m_width,
            m_height,
            hdrFormat);

        PathTracerIntegrateInputs integrateInputs{};
        integrateInputs.pathTracerActive = m_pathTracerActive;
        integrateInputs.pathTracerOutputSrv = m_dxrPathTracerOutputSrv;
        integrateInputs.gridOverlayEnabled = static_cast<bool>(m_pathTracerGridOverlayDraw);
        integrateInputs.hdrCopy = BuildPathTracerHdrCopyInputs();
        integrateInputs.camera = state.camera;
        integrateInputs.drawGridOverlay =
            [this, camera = state.camera](PostProcessTarget& target, const int width, const int height)
            {
                DrawPathTracerGridOverlayOntoHdrTarget(
                    *camera,
                    static_cast<InternalTarget&>(target),
                    width,
                    height);
            };

        PathTracerIntegrateOutputs integrateOutputs{};
        PathTracerDisplayPass::IntegrateIntoHdrChain(
            postContext, integrateInputs, integrateOutputs);
        if (integrateOutputs.integrated)
        {
            state.hdrColorSrv = integrateOutputs.hdrColorSrv;
            state.hdrColorSource = "path_tracer_grid";
            const_cast<ScreenSpaceEffects*>(this)->m_pathTracerPostIntegrated = true;
        }
    }

    state.bloomSrv = 0;
    if (m_bloomEnabled && !IsPbrMaterialDebugMode(m_debugMode) && !state.effectiveWantDlss
        && !IsPtIsolateDebugMode(m_debugMode))
    {
        const GfxContext::GpuTimerScope gpuScopeBloom("Post-process/Bloom");
        FrameDiagnostics::LogHistoryEvent(
            m_dlssViewportId, "render-bloom", m_bloomHistoryValid ? "consume" : "request",
            m_pathTracerActive ? "path-tracer" : "raster",
            m_pathTracerActive ? "pt-guides" : "raster-guides", "none", "existing-quality",
            m_width, m_height, state.viewportWidth, state.viewportHeight,
            false, false, m_bloomHistoryValid ? 0u : 1u);
        RenderResBloomInputs bloomInputs{};
        bloomInputs.hdrColorSrv = state.hdrColorSrv;
        bloomInputs.fullTexelSize = state.texelSize;
        bloomInputs.exposure = m_exposure;
        bloomInputs.bloomThreshold = m_bloomThreshold;
        bloomInputs.bloomSoftKnee = m_bloomSoftKnee;
        bloomInputs.bloomBlurRadius = m_bloomBlurRadius;
        bloomInputs.bloomTemporalBlendFactor = m_bloomTemporalBlendFactor;
        bloomInputs.bloomSameUvBlendFactor = m_bloomSameUvBlendFactor;
        bloomInputs.bloomDepthThreshold = m_bloomDepthThreshold;
        bloomInputs.useMaterialGbuffer = m_sceneFramebuffer->HasMaterialGbuffer();
        if (bloomInputs.useMaterialGbuffer)
        {
            bloomInputs.material0Srv =
                m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
            bloomInputs.material1Srv =
                m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic);
        }
        bloomInputs.hasVelocity = m_sceneFramebuffer->HasVelocity();
        bloomInputs.velocitySrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MotionVelocity);
        bloomInputs.depthSrv = m_sceneFramebuffer->GetDepthSrvCpuHandle();
        bloomInputs.bloomExtractShader = m_bloomExtractShader.get();
        bloomInputs.bloomBlurShader = m_bloomBlurShader.get();
        bloomInputs.bloomTemporalShader = m_bloomTemporalShader.get();
        bloomInputs.bloomExtractTarget = const_cast<InternalTarget*>(&m_bloomExtractTarget);
        bloomInputs.bloomBlurTarget = const_cast<InternalTarget*>(&m_bloomBlurTarget);
        bloomInputs.bloomBlur2Target = const_cast<InternalTarget*>(&m_bloomBlur2Target);
        bloomInputs.bloomTemporalTarget = const_cast<InternalTarget*>(&m_bloomTemporalTarget);
        bloomInputs.bloomHistoryTarget = const_cast<InternalTarget*>(&m_bloomHistoryTarget);
        bloomInputs.bloomHistoryValid = m_bloomHistoryValid;
        bloomInputs.bloomTemporalWarmupFrames = m_bloomTemporalWarmupFrames;

        RenderResBloomOutputs bloomOutputs{};
        if (BloomTonemapPass::ExecuteRenderResBloom(postContext, bloomInputs, bloomOutputs))
        {
            state.bloomSrv = bloomOutputs.bloomSrv;
            m_bloomHistoryValid = bloomOutputs.bloomHistoryValid;
            m_bloomTemporalWarmupFrames = bloomOutputs.bloomTemporalWarmupFrames;
            const_cast<ScreenSpaceEffects*>(this)->m_prevFrameBloomSrv = state.bloomSrv;
        }
    }
    else
    {
        const_cast<ScreenSpaceEffects*>(this)->m_prevFrameBloomSrv = 0;
    }
}

bool ScreenSpaceEffects::RunApplyDebugStage(ApplyFrameState& state) const
{
    const PostProcessContext postContext = BuildPostProcessContext();
    BindOutputTarget(state.outputTarget, state.viewportWidth, state.viewportHeight);

    PostProcessDebugPassInputs debugInputs{};
    debugInputs.debugMode = state.debugMode;
    debugInputs.pbrDebugActive = state.pbrDebugActive;
    debugInputs.runAo = state.runAo;
    debugInputs.runGtao = state.runGtao;
    debugInputs.runRtIndirect = state.runRtIndirect;
    debugInputs.runSsrIndirect = state.runSsrIndirect;
    debugInputs.compositeRan = state.compositeRan;
    debugInputs.compositeUsesSsao = state.compositeUsesSsao;
    debugInputs.useShadowFactorComposite = state.useShadowFactorComposite;
    debugInputs.hdrColorSrv = state.hdrColorSrv;
    debugInputs.aoCompositeSrv = state.aoCompositeSrv;
    debugInputs.shadowFactorSrv = state.shadowFactorSrv;
    debugInputs.hdrColorSource = state.hdrColorSource;
    debugInputs.ssaoDebugViewSource = state.ssaoDebugViewSource;
    debugInputs.ssaoShaderDebugMode = m_ssaoShaderDebugMode;
    debugInputs.ssgiStrength = m_ssgiStrength;
    debugInputs.ssgiEnabled = m_ssgiEnabled;
    debugInputs.ssgiDenoiseEnabled = m_ssgiDenoiseEnabled;
    debugInputs.ssgiNoiseInjectionEnabled = m_ssgiNoiseInjectionEnabled;
    debugInputs.lastSsrSpatialSrv = m_lastSsrSpatialSrv;
    debugInputs.lastSsrTemporalSrv = m_lastSsrTemporalSrv;
    debugInputs.lastSsrVarianceSrv = m_lastSsrVarianceSrv;
    debugInputs.lastSsrDenoiseSrv = m_lastSsrDenoiseSrv;
    debugInputs.lastSsrResolvedSrv = m_lastSsrResolvedSrv;
    debugInputs.lastSsgiInjectSrv = m_lastSsgiInjectSrv;
    debugInputs.sceneFramebuffer = m_sceneFramebuffer.get();
    debugInputs.ssaoTarget = const_cast<InternalTarget*>(&m_ssaoTarget);
    debugInputs.gtaoRawTarget = const_cast<InternalTarget*>(&m_gtaoRawTarget);
    debugInputs.hdrCompositeTarget = const_cast<InternalTarget*>(&m_hdrCompositeTarget);
    debugInputs.radianceTarget = const_cast<InternalTarget*>(&m_radianceTarget);
    debugInputs.radianceTraceInputTarget = const_cast<InternalTarget*>(&m_radianceTraceInputTarget);
    debugInputs.radianceSpatialTarget = const_cast<InternalTarget*>(&m_radianceSpatialTarget);
    debugInputs.radianceHistoryTarget = const_cast<InternalTarget*>(&m_radianceHistoryTarget);
    debugInputs.ssrSceneColorTarget = const_cast<InternalTarget*>(&m_ssrSceneColorTarget);
    debugInputs.ssrTraceTarget = const_cast<InternalTarget*>(&m_ssrTraceTarget);
    debugInputs.ssrIndirectTarget = const_cast<InternalTarget*>(&m_ssrIndirectTarget);
    debugInputs.rtIndirectTarget = const_cast<InternalTarget*>(&m_rtIndirectTarget);
    debugInputs.ptTemporalStatsTarget = const_cast<InternalTarget*>(&m_ptTemporalStatsTarget);
    debugInputs.ptGiDiagnosticRoi = IsPtRestirGiSpatialStatsDebugMode(state.debugMode)
        ? m_ptGiDiagnosticRoi
        : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    const bool ptReprojectionAudit = IsPtMotionReprojectionDebugMode(state.debugMode)
        || IsPtDepthReprojectionDebugMode(state.debugMode)
        || IsPtMatrixDepthReprojectionDebugMode(state.debugMode);
    std::uintptr_t ptAuditDepthSrv = 0;
    if (ptReprojectionAudit)
    {
        // Select exactly the same PT/raster/sky/dilated source the subsequent DLSS evaluation
        // would use. This debug view early-outs before Evaluate, so it is safe to prepare it here.
        m_sceneFramebuffer->EnsureShaderResourceState();
        FillDlssResolveInputs(state);
        const DlssTemporalGuideInputs guides = DlssResolvePass::ResolveTemporalGuideInputs(
            state.dlssInputs);
        debugInputs.ptCurrentRadianceSrv = m_dxrPathTracerOutputSrv;
        debugInputs.ptPreviousRadianceSrv = m_ptTemporalPrevRadianceTarget.srvCpuHandle;
        debugInputs.ptCurrentDepthSrv = guides.depthSrv;
        debugInputs.ptPreviousDepthSrv = m_ptTemporalPrevDepthTarget.srvCpuHandle;
        debugInputs.ptMotionSrv = guides.motionSrv;
        debugInputs.ptPreviousRadianceValid = m_ptTemporalPrevRadianceValid;
        debugInputs.ptClipToPrevClip = DlssResolvePass::BuildClipToPrevClip(state.dlssInputs);
        ptAuditDepthSrv = guides.depthSrv;
        // Bundle preparation / the sky patch are internal fullscreen passes and leave their own
        // target bound. The audit itself must render to the editor viewport, not that scratch
        // target; ordinary PT diagnostics do not have this extra preparation step.
        BindOutputTarget(state.outputTarget, state.viewportWidth, state.viewportHeight);
    }
    debugInputs.debugChannelShader = m_debugChannelShader.get();
    debugInputs.velocityDebugShader = m_velocityDebugShader.get();
    debugInputs.gbufferDebugShader = m_gbufferDebugShader.get();
    debugInputs.radianceDebugShader = m_radianceDebugShader.get();
    debugInputs.ssrDebugShader = m_ssrDebugShader.get();
    debugInputs.ssrTraceDebugShader = m_ssrTraceDebugShader.get();
    debugInputs.ssrDenoiseDebugShader = m_ssrDenoiseDebugShader.get();
    debugInputs.ssgiDenoiseDebugShader = m_ssgiDenoiseDebugShader.get();
    debugInputs.giTemporalDebugShader = m_giTemporalDebugShader.get();
    debugInputs.ptTemporalStatsDebugShader = m_ptTemporalStatsDebugShader.get();
    debugInputs.ptMotionReprojectionDebugShader = m_ptMotionReprojectionDebugShader.get();
    debugInputs.logSsaoApplySnapshot = m_logSsaoApplySnapshot;
    debugInputs.captureSsaoDiagnostics =
        [this](
            const bool runAo,
            const bool compositeRan,
            const bool compositeUsesSsao,
            const bool pbrDebugActive,
            const bool useShadowFactorComposite,
            const char* hdrColorSource,
            const char* ssaoDebugViewSource,
            const std::uintptr_t hdrColorSrv,
            const std::uintptr_t shadowFactorSrv)
        {
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                ssaoDebugViewSource,
                hdrColorSrv,
                shadowFactorSrv);
        };

    PostProcessDebugPassOutputs debugOutputs{};
    const GfxContext::GpuTimerScope gpuScopeDebug("Post-process/Debug view");
    const bool earlyOut = PostProcessDebugPass::TryExecute(
        postContext, debugInputs, debugOutputs);
    const_cast<ScreenSpaceEffects*>(this)->m_postProcessDebugRenderedThisFrame = earlyOut;
    if (debugOutputs.ssaoDebugViewSource != nullptr)
    {
        state.ssaoDebugViewSource = debugOutputs.ssaoDebugViewSource;
    }
    if (debugOutputs.requestGpuReadback)
    {
        const_cast<ScreenSpaceEffects*>(this)->m_pendingSsaoGpuReadback = true;
    }
    if (ptReprojectionAudit && earlyOut)
    {
        // Commit after the viewport draw: this frame remains the "current" comparison input;
        // the next frame samples it as history.
        CommitPathTracerMotionReprojectionAudit(ptAuditDepthSrv);
    }
    return earlyOut;
}

void ScreenSpaceEffects::RunApplyPresentationStage(ApplyFrameState& state) const
{
    const PostProcessContext postContext = BuildPostProcessContext();

    const bool useFxaa = m_antiAliasingMode == AntiAliasingMode::FXAA;
    const bool useSmaa = m_antiAliasingMode == AntiAliasingMode::SMAA;
    const bool useSsaa = m_antiAliasingMode == AntiAliasingMode::SSAA;
    const bool needsLdrIntermediate = AntiAliasingPass::NeedsLdrIntermediate(
        useFxaa,
        useSmaa,
        useSsaa,
        m_width,
        m_height,
        m_viewportWidth,
        m_viewportHeight);

    auto runTonemapPass = [&](const bool drawToLdrTarget) {
        TonemapPassInputs tonemapInputs{};
        tonemapInputs.hdrColorSrv = state.hdrColorSrv;
        tonemapInputs.bloomSrv = state.bloomSrv;
        tonemapInputs.texelSize = state.texelSize;
        tonemapInputs.exposure = m_exposure;
        tonemapInputs.tonemapMode = static_cast<int>(m_tonemapMode);
        tonemapInputs.bloomEnabled = m_bloomEnabled;
        tonemapInputs.bloomIntensity = m_bloomIntensity;
        tonemapInputs.tonemapShader = m_tonemapShader.get();
        tonemapInputs.ldrTonemapTarget = const_cast<InternalTarget*>(&m_ldrTonemapTarget);

        if (drawToLdrTarget)
        {
            BloomTonemapPass::ExecuteTonemapToLdrTarget(postContext, tonemapInputs);
        }
        else
        {
            BloomTonemapPass::ExecuteTonemapToViewport(
                postContext,
                state.outputTarget,
                state.viewportWidth,
                state.viewportHeight,
                tonemapInputs);
        }
    };

    const bool ldrTargetReady =
        m_ldrTonemapTarget.srvCpuHandle != 0 && m_width > 0 && m_height > 0;

    const bool dlssDisplayReady =
        m_dlssOutputTarget.resource != nullptr && m_width > 0 && m_viewportWidth > 0;

    if (state.effectiveWantDlss && dlssDisplayReady)
    {
        FillDlssResolveInputs(state);
        DlssResolvePassOutputs dlssOutputs{};
        DlssResolvePass::Execute(postContext, state.dlssInputs, dlssOutputs);
        if (dlssOutputs.pathTracerDlssResolvedThisFrame)
        {
            const_cast<ScreenSpaceEffects*>(this)->m_pathTracerDlssResolvedThisFrame = true;
        }
        if (dlssOutputs.pathTracerOutputResourceStateValid)
        {
            const_cast<ScreenSpaceEffects*>(this)->m_pathTracerOutputResourceState =
                dlssOutputs.pathTracerOutputResourceState;
        }
        const_cast<ScreenSpaceEffects*>(this)->m_dlssHistoryValid = dlssOutputs.dlssHistoryValid;
        const_cast<ScreenSpaceEffects*>(this)->m_dlssBloomHistoryValid = dlssOutputs.dlssBloomHistoryValid;
        const_cast<ScreenSpaceEffects*>(this)->m_dlssBloomTemporalWarmupFrames =
            dlssOutputs.dlssBloomTemporalWarmupFrames;
        const_cast<ScreenSpaceEffects*>(this)->m_prevFrameBloomSrv = dlssOutputs.prevFrameBloomSrv;
    }
    else if (needsLdrIntermediate && ldrTargetReady)
    {
        SceneRenderTrace::Section tonemapSection("tonemap");
        {
            const GfxContext::GpuTimerScope gpuScopeTonemap("Post-process/Tonemap");
            SceneRenderTrace::Scope tonemapScope("tonemap to ldr");
            runTonemapPass(true);
            tonemapScope.Success();
        }

        {
            const GfxContext::GpuTimerScope gpuScopeLdrAa("Post-process/LDR AA");
            LdrAntiAliasingInputs ldrAaInputs{};
        ldrAaInputs.useFxaa = useFxaa;
        ldrAaInputs.useSmaa = useSmaa;
        ldrAaInputs.useSsaa = useSsaa;
        ldrAaInputs.viewportWidth = state.viewportWidth;
        ldrAaInputs.viewportHeight = state.viewportHeight;
        ldrAaInputs.texelSize = state.texelSize;
        ldrAaInputs.ldrTonemapSrv = m_ldrTonemapTarget.srvCpuHandle;
        ldrAaInputs.fxaaSubpixQuality = m_fxaaSubpixQuality;
        ldrAaInputs.fxaaEdgeThreshold = m_fxaaEdgeThreshold;
        ldrAaInputs.smaaThreshold = m_smaaThreshold;
        ldrAaInputs.smaaSearchSteps = m_smaaSearchSteps;
        ldrAaInputs.fxaaShader = m_fxaaShader.get();
        ldrAaInputs.smaaEdgeShader = m_smaaEdgeShader.get();
        ldrAaInputs.smaaNeighborShader = m_smaaNeighborShader.get();
        ldrAaInputs.downsampleShader = m_downsampleShader.get();
        ldrAaInputs.smaaEdgeTarget = const_cast<InternalTarget*>(&m_smaaEdgeTarget);
        ldrAaInputs.smaaOutputTarget = const_cast<InternalTarget*>(&m_smaaOutputTarget);
        ldrAaInputs.outputTarget = state.outputTarget;
        AntiAliasingPass::ExecuteLdrAntiAliasing(postContext, ldrAaInputs);
        }

        tonemapSection.Success();
    }
    else
    {
        SceneRenderTrace::Section tonemapSection("tonemap");
        {
            const GfxContext::GpuTimerScope gpuScopeTonemap("Post-process/Tonemap");
            SceneRenderTrace::Scope tonemapScope("tonemap direct");
            BindOutputTarget(state.outputTarget, state.viewportWidth, state.viewportHeight);
            runTonemapPass(false);
            tonemapScope.Success();
        }
        tonemapSection.Success();
    }
}

void ScreenSpaceEffects::FinalizeApplyFrame(ApplyFrameState& state) const
{
    if (m_logHdrApplySnapshot)
    {
        m_logHdrApplySnapshot = false;
        std::uint32_t srvUsed = 0;
        std::uint32_t srvCapacity = 0;
        GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
        RenderPathDiagnostics::LogHdrApplySnapshot(
            m_width,
            m_height,
            state.viewportWidth,
            state.viewportHeight,
            m_sceneFramebuffer->IsValid(),
            m_sceneFramebuffer->HasSplitLighting(),
            state.runAo,
            state.useShadowFactorComposite,
            state.outputTarget != nullptr,
            state.hdrColorSrv,
            m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::DirectLighting),
            m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::IndirectLighting),
            m_sceneFramebuffer->GetDepthSrvCpuHandle(),
            srvUsed,
            srvCapacity);
    }

    CaptureSsaoDiagnosticsCpu(
        state.runAo,
        state.compositeRan,
        state.compositeUsesSsao,
        state.pbrDebugActive,
        state.useShadowFactorComposite,
        state.hdrColorSource,
        state.ssaoDebugViewSource,
        state.hdrColorSrv,
        state.shadowFactorSrv);
    if (m_logSsaoApplySnapshot)
    {
        const_cast<ScreenSpaceEffects*>(this)->m_pendingSsaoGpuReadback = true;
    }
}
