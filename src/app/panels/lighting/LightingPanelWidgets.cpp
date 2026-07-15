#include "app/panels/lighting/LightingPanelWidgets.h"

#include "app/panels/lighting/LightingPanelUi.h"
#include "app/scene/SceneRenderer.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rhi/GfxContext.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>

namespace
{
    struct DebugViewCategory
    {
        const char* label;
        const RenderDebugMode* modes;
        int modeCount;
    };

    const RenderDebugMode kFinalImageModes[] = {RenderDebugMode::None};

    const RenderDebugMode kLightingModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::ShadowFactor,
        RenderDebugMode::DirectLighting,
        RenderDebugMode::DirectDiffuseGeom,
        RenderDebugMode::AmbientIbl,
        RenderDebugMode::DiffuseIbl,
        RenderDebugMode::SpecularIbl,
        RenderDebugMode::CompositeOcclusion,
        RenderDebugMode::GeomSunFacing,
    };

    const RenderDebugMode kShadowMapModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::LightSpaceUv,
        RenderDebugMode::LightSpaceDepth,
        RenderDebugMode::CascadeIndex,
        RenderDebugMode::CascadeBlendFactor,
        RenderDebugMode::ViewDepth,
        RenderDebugMode::ShadowFactorUnbiased,
        RenderDebugMode::ShadowMapStoredDepth,
        RenderDebugMode::ShadowDepthSeparation,
        RenderDebugMode::ShadowCompareDepth,
        RenderDebugMode::ShadowBlockedCenter,
    };

    const RenderDebugMode kGBufferModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::GeometricNormal,
        RenderDebugMode::ShadedNormal,
        RenderDebugMode::TangentHandedness,
        RenderDebugMode::GBufferAlbedo,
        RenderDebugMode::GBufferRoughness,
        RenderDebugMode::GBufferMetallic,
        RenderDebugMode::GBufferEmissive,
    };

    const RenderDebugMode kAoModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::Ssao,
        RenderDebugMode::GtaoRaw,
        RenderDebugMode::GtaoFiltered,
    };

    const RenderDebugMode kMotionModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::MotionVectors,
    };

    const RenderDebugMode kRadianceModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::RadianceBuffer,
        RenderDebugMode::RadianceValidity,
        RenderDebugMode::RadianceTemporal,
        RenderDebugMode::GiDisocclusion,
        RenderDebugMode::RadianceTemporalDelta,
    };

    const RenderDebugMode kSsgiModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::SsgiTraceRaw,
        RenderDebugMode::SsgiTraceHitMask,
        RenderDebugMode::SsgiTraceHitDistance,
        RenderDebugMode::SsgiDenoiseSpatial,
        RenderDebugMode::SsgiDenoiseTemporal,
        RenderDebugMode::SsgiDenoiseFinal,
        RenderDebugMode::SsgiInject,
        RenderDebugMode::SsgiFinalContribution,
    };

    const RenderDebugMode kSsrModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::SsrSceneColor,
        RenderDebugMode::SsrSceneValidity,
        RenderDebugMode::SsrTraceRaw,
        RenderDebugMode::SsrTraceConfidence,
        RenderDebugMode::SsrDenoiseSpatial,
        RenderDebugMode::SsrDenoiseTemporal,
        RenderDebugMode::SsrSvgfVariance,
        RenderDebugMode::SsrDenoiseFinal,
        RenderDebugMode::SsrUpscaled,
        RenderDebugMode::SsrSpecReplacement,
    };

    const RenderDebugMode kRayTracingModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::RtDispatchSmoke,
        RenderDebugMode::RtPrimaryHit,
        RenderDebugMode::RtPrimaryDepth,
        RenderDebugMode::RtPrimaryNormal,
        RenderDebugMode::RtReflectionRaw,
        RenderDebugMode::RtReflectionConfidence,
        RenderDebugMode::RtReflectionDenoised,
        RenderDebugMode::RtSpecReplacement,
        RenderDebugMode::RtShadowRaw,
        RenderDebugMode::RtShadowDenoised,
        RenderDebugMode::RtGiRaw,
        RenderDebugMode::RtGiDenoised,
        RenderDebugMode::RtGiInject,
    };

    const RenderDebugMode kRrGuideModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::RrDiffuseAlbedo,
        RenderDebugMode::RrSpecularAlbedo,
        RenderDebugMode::RrNormalRoughness,
    };

    const RenderDebugMode kPtIsolateModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::PtIsolateDirectSun,
        RenderDebugMode::PtIsolateDirectEmissive,
        RenderDebugMode::PtIsolateSurfaceEmissive,
        RenderDebugMode::PtIsolateAmbient,
        RenderDebugMode::PtIsolateAoVisibility,
        RenderDebugMode::PtIsolateSunVisibility,
        RenderDebugMode::PtIsolateIndirect,
        RenderDebugMode::PtIsolatePreClamp,
        RenderDebugMode::PtIsolateSpecHitDist,
        RenderDebugMode::PtRestirLinearDepth,
        RenderDebugMode::PtRestirGeometricNormal,
        RenderDebugMode::PtRestirMaterialId,
        RenderDebugMode::PtRestirLobeClass,
        RenderDebugMode::PtRestirReservoirM,
        RenderDebugMode::PtRestirReservoirAge,
        RenderDebugMode::PtRestirChosenSource,
        RenderDebugMode::PtRestirTemporalRejection,
        RenderDebugMode::PtRestirSpatialSource,
        RenderDebugMode::PtRestirSpatialRejection,
        RenderDebugMode::PtRestirGiReservoirM,
        RenderDebugMode::PtRestirGiReservoirAge,
        RenderDebugMode::PtRestirGiChosenSource,
        RenderDebugMode::PtRestirGiTemporalRejection,
        RenderDebugMode::PtRestirGiUcw,
        RenderDebugMode::PtRestirGiContribution,
        RenderDebugMode::PtRestirGiReuseMinusFresh,
        RenderDebugMode::PtRestirGiReusedRadiance,
        RenderDebugMode::PtEnvDiProbeSampling,
        RenderDebugMode::PtEnvDiProbeBsdfMis,
        RenderDebugMode::PtEnvDiProbeCandidate,
        RenderDebugMode::PtEnvDiProbeRadiance,
        RenderDebugMode::PtEnvDiProbeMetadata,
    };

    const RenderDebugMode kPtDiagnosticModes[] = {
        RenderDebugMode::None,
        RenderDebugMode::PtTemporalRelativeSigma,
        RenderDebugMode::PtTemporalFrameDelta,
        RenderDebugMode::PtMotionReprojectionResidual,
    };

    const DebugViewCategory kDebugViewCategories[] = {
        {"Final image", kFinalImageModes, IM_ARRAYSIZE(kFinalImageModes)},
        {"Lighting & composite", kLightingModes, IM_ARRAYSIZE(kLightingModes)},
        {"Shadow maps", kShadowMapModes, IM_ARRAYSIZE(kShadowMapModes)},
        {"G-buffer", kGBufferModes, IM_ARRAYSIZE(kGBufferModes)},
        {"Ambient occlusion", kAoModes, IM_ARRAYSIZE(kAoModes)},
        {"Motion", kMotionModes, IM_ARRAYSIZE(kMotionModes)},
        {"Radiance assembly", kRadianceModes, IM_ARRAYSIZE(kRadianceModes)},
        {"SSGI", kSsgiModes, IM_ARRAYSIZE(kSsgiModes)},
        {"SSR", kSsrModes, IM_ARRAYSIZE(kSsrModes)},
        {"Ray tracing", kRayTracingModes, IM_ARRAYSIZE(kRayTracingModes)},
        {"DLSS RR guides", kRrGuideModes, IM_ARRAYSIZE(kRrGuideModes)},
        {"Path tracer isolate", kPtIsolateModes, IM_ARRAYSIZE(kPtIsolateModes)},
        {"Path tracer diagnostics", kPtDiagnosticModes, IM_ARRAYSIZE(kPtDiagnosticModes)},
    };

    int FindCategoryIndexForMode(const RenderDebugMode mode)
    {
        // None lives in every category; category membership is only meaningful for real views.
        if (mode == RenderDebugMode::None)
        {
            return -1;
        }

        for (int categoryIndex = 0; categoryIndex < IM_ARRAYSIZE(kDebugViewCategories); ++categoryIndex)
        {
            const DebugViewCategory& category = kDebugViewCategories[categoryIndex];
            for (int modeIndex = 0; modeIndex < category.modeCount; ++modeIndex)
            {
                if (category.modes[modeIndex] == mode)
                {
                    return categoryIndex;
                }
            }
        }
        return 0;
    }

    int FindModeIndexInCategory(const DebugViewCategory& category, const RenderDebugMode mode)
    {
        for (int modeIndex = 0; modeIndex < category.modeCount; ++modeIndex)
        {
            if (category.modes[modeIndex] == mode)
            {
                return modeIndex;
            }
        }
        return 0;
    }

    const char* DebugViewModeComboLabel(const RenderDebugMode mode)
    {
        if (mode == RenderDebugMode::None)
        {
            return "None";
        }

        return RenderDebugModeLabel(mode);
    }

    int FindFirstAvailableModeIndexInCategory(
        const DebugViewCategory& category,
        const SceneRenderer& renderer,
        const ScreenSpaceEffects& screenSpaceEffects)
    {
        for (int modeIndex = 0; modeIndex < category.modeCount; ++modeIndex)
        {
            if (LightingPanelWidgets::IsDebugViewModeAvailable(
                    category.modes[modeIndex], renderer, screenSpaceEffects))
            {
                return modeIndex;
            }
        }
        return 0;
    }

    void ApplyDebugViewSelection(
        const RenderDebugMode selectedMode,
        ScreenSpaceEffects& screenSpaceEffects,
        SceneRenderer& renderer)
    {
        renderer.SetRenderDebugMode(selectedMode);
        if ((IsRtPrimaryDebugMode(selectedMode) || IsRtReflectionDebugMode(selectedMode)
             || IsRtShadowDebugMode(selectedMode) || IsRtGiDebugMode(selectedMode))
            && renderer.GetDxrSettings().IsEnabled()
            && !GfxContext::Get().IsFrameRecording())
        {
            renderer.WarmUpDxrPipelineIfNeeded();
            renderer.GetScreenSpaceEffects().ResetRtPrimaryDebugBlitSettle();
        }
    }

    bool DrawCategoryCombo(int& categoryIndex)
    {
        const DebugViewCategory& category = kDebugViewCategories[categoryIndex];
        bool changed = false;
        if (ImGui::BeginCombo("Category", category.label))
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2(220.0f, 0.0f), ImVec2(FLT_MAX, 320.0f));
            for (int index = 0; index < IM_ARRAYSIZE(kDebugViewCategories); ++index)
            {
                const bool selected = categoryIndex == index;
                if (ImGui::Selectable(kDebugViewCategories[index].label, selected) && !selected)
                {
                    categoryIndex = index;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    bool DrawModeCombo(
        const DebugViewCategory& category,
        int& modeIndexInCategory,
        ScreenSpaceEffects& screenSpaceEffects,
        SceneRenderer& renderer)
    {
        modeIndexInCategory = std::clamp(modeIndexInCategory, 0, category.modeCount - 1);
        const RenderDebugMode currentMode = category.modes[modeIndexInCategory];
        bool changed = false;
        if (ImGui::BeginCombo("View", DebugViewModeComboLabel(currentMode), ImGuiComboFlags_HeightLarge))
        {
            ImGui::SetNextWindowSizeConstraints(ImVec2(260.0f, 0.0f), ImVec2(FLT_MAX, 420.0f));
            for (int index = 0; index < category.modeCount; ++index)
            {
                const RenderDebugMode mode = category.modes[index];
                const bool available = LightingPanelWidgets::IsDebugViewModeAvailable(
                    mode, renderer, screenSpaceEffects);
                if (!available)
                {
                    ImGui::BeginDisabled();
                }

                const bool selected = modeIndexInCategory == index;
                if (ImGui::Selectable(DebugViewModeComboLabel(mode), selected) && !selected && available)
                {
                    modeIndexInCategory = index;
                    ApplyDebugViewSelection(mode, screenSpaceEffects, renderer);
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                if (!available)
                {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        if (const char* reason = LightingPanelWidgets::DebugViewModeUnavailableReason(
                                mode, renderer, screenSpaceEffects))
                        {
                            ImGui::SetTooltip("%s", reason);
                        }
                    }
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
}

namespace LightingPanelWidgets
{
    bool IsDebugViewModeAvailable(
        const RenderDebugMode mode,
        const SceneRenderer& renderer,
        const ScreenSpaceEffects& screenSpaceEffects)
    {
        if (mode == RenderDebugMode::None)
        {
            return true;
        }

        const DxrSettings& dxrSettings = renderer.GetDxrSettings();
        const bool pathTracingActive = dxrSettings.IsPathTracingActive();
        const bool dxrEnabled = dxrSettings.IsEnabled();
        const bool postEnabled = screenSpaceEffects.IsEnabled();

        if (IsSsgiDenoiseDebugMode(mode) || IsRadianceDebugMode(mode) || IsGiTemporalDebugMode(mode))
        {
            return postEnabled && !pathTracingActive;
        }

        if (IsSsrDebugMode(mode))
        {
            return postEnabled && !pathTracingActive;
        }

        if (mode == RenderDebugMode::Ssao || mode == RenderDebugMode::GtaoRaw
            || mode == RenderDebugMode::GtaoFiltered || mode == RenderDebugMode::CompositeOcclusion)
        {
            return postEnabled && !pathTracingActive;
        }

        if (mode == RenderDebugMode::MotionVectors)
        {
            return postEnabled;
        }

        if (IsRrGuideDebugMode(mode))
        {
            return screenSpaceEffects.GetRayReconstruction()
                || (screenSpaceEffects.IsRayReconstructionActive() && dxrEnabled);
        }

        if (IsPtIsolateDebugMode(mode))
        {
            return pathTracingActive && postEnabled;
        }

        if (IsPtTemporalStatsDebugMode(mode) || IsPtMotionReprojectionDebugMode(mode))
        {
            return pathTracingActive && postEnabled;
        }

        if (!IsDxrDebugMode(mode))
        {
            return true;
        }

        if (!dxrEnabled)
        {
            return false;
        }

        if (IsRtReflectionDebugMode(mode) || IsRtShadowDebugMode(mode) || IsRtGiDebugMode(mode)
            || mode == RenderDebugMode::RtDispatchSmoke)
        {
            return !pathTracingActive;
        }

        return true;
    }

    const char* DebugViewModeUnavailableReason(
        const RenderDebugMode mode,
        const SceneRenderer& renderer,
        const ScreenSpaceEffects& screenSpaceEffects)
    {
        if (IsDebugViewModeAvailable(mode, renderer, screenSpaceEffects))
        {
            return nullptr;
        }

        const DxrSettings& dxrSettings = renderer.GetDxrSettings();
        const bool pathTracingActive = dxrSettings.IsPathTracingActive();
        const bool dxrEnabled = dxrSettings.IsEnabled();
        const bool postEnabled = screenSpaceEffects.IsEnabled();

        if (IsDxrDebugMode(mode))
        {
            if (!dxrEnabled)
            {
                return "Enable ray tracing first.";
            }
            if (pathTracingActive
                && (IsRtReflectionDebugMode(mode) || IsRtShadowDebugMode(mode) || IsRtGiDebugMode(mode)
                    || mode == RenderDebugMode::RtDispatchSmoke))
            {
                return "Hybrid RT debug views are not written during path tracing.";
            }
        }

        if (IsRrGuideDebugMode(mode))
        {
            return "Requires DLSS Ray Reconstruction to be enabled.";
        }

        if (IsPtIsolateDebugMode(mode))
        {
            if (!postEnabled)
            {
                return "Enable post-processing first.";
            }
            if (!pathTracingActive)
            {
                return "Enable path tracing first.";
            }
        }

        if (IsPtTemporalStatsDebugMode(mode) || IsPtMotionReprojectionDebugMode(mode))
        {
            if (!postEnabled)
            {
                return "Enable post-processing first.";
            }
            if (!pathTracingActive)
            {
                return "Enable path tracing first.";
            }
        }

        if (!postEnabled)
        {
            return "Enable post-processing first.";
        }

        if (pathTracingActive)
        {
            return "Not produced while path tracing is active.";
        }

        return "Unavailable with current renderer settings.";
    }

    const char* RenderDebugModeHelpText(const RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::LightSpaceUv:
            return "Light-space UV shifts with the flycam: cascades refit to the camera frustum each frame.";
        case RenderDebugMode::CascadeIndex:
            return "Cascade tint is stable per split; brightness encodes view depth within the active cascade.";
        case RenderDebugMode::DirectLighting:
            return "Unshadowed direct (sun diffuse uses geom N·L; spec uses shaded normal + BRDF). "
                   "Should match Direct diffuse geom in the large-scale terminator. "
                   "Final = this × shadow factor + IBL.";
        case RenderDebugMode::DirectDiffuseGeom:
            return "Sun diffuse on geometric normals only. Stable across flycam if this view is flat but Direct lighting is not.";
        case RenderDebugMode::ShadedNormal:
            return "Normal used for lighting after normal-map perturbation. Compare with Geometric normal.";
        case RenderDebugMode::DiffuseIbl:
            return "Indirect diffuse from L2 SH9 irradiance using the shaded normal.";
        case RenderDebugMode::SpecularIbl:
            return "Indirect specular from prefiltered env map. View-dependent by design.";
        case RenderDebugMode::CascadeBlendFactor:
            return "Cascade cross-fade weight. Bright bands on the floor near the camera often indicate a blend seam.";
        case RenderDebugMode::ViewDepth:
            return "Linear view-space Z used for cascade selection. Compare with Cascade index.";
        case RenderDebugMode::ShadowFactorUnbiased:
            return "Shadow map PCF with receiver bias disabled. Use with Shadow depth separation to split map vs bias issues.";
        case RenderDebugMode::ShadowMapStoredDepth:
            return "Raw depth stored in the shadow map at the receiver's light-space UV (center texel, no filtering). Black = near, white = far.";
        case RenderDebugMode::ShadowDepthSeparation:
            return "Receiver clip depth minus stored map depth, scaled by the minimum separation threshold. Mid-gray = match; brighter = behind stored; darker = in front.";
        case RenderDebugMode::GeomSunFacing:
            return "Geometric N·L for the shadow-casting directional light only. White = faces the sun; black = back faces. "
                   "No albedo, no IBL, no composite. Use Direct lighting to preview the lit direct buffer.";
        case RenderDebugMode::ShadowCompareDepth:
            return "Clip depth used in the shadow compare test (unbiased path, tiny depth bias only). Should track stored depth on lit surfaces.";
        case RenderDebugMode::ShadowBlockedCenter:
            return "Raw center-texel compare: receiver clip Z vs stored map depth, no PCF, no receiver bias, no min-separation floor. "
                   "White = lit, black = blocked. Fix this view before tuning bias or blur.";
        case RenderDebugMode::Ssao:
            return "Current AO target for SSAO mode (1 = no occlusion). When AO is off or GTAO is active, this view may show stale data.";
        case RenderDebugMode::GtaoRaw:
            return "Raw GTAO visibility before denoise (1 = no occlusion). Use this to judge whether the horizon pass finds crevices.";
        case RenderDebugMode::GtaoFiltered:
            return "Filtered GTAO visibility after edge-aware denoise. This is the map composited into indirect lighting.";
        case RenderDebugMode::MotionVectors:
            return "Per-pixel screen-space velocity (hue = direction, brightness = magnitude). "
                   "Pan the camera on static geometry for a uniform field. Sky and first frame after load/resize are black.";
        case RenderDebugMode::GBufferAlbedo:
            return "Linear base color (texture × factor), no lighting. Textured objects should match their albedo maps.";
        case RenderDebugMode::GBufferRoughness:
            return "Per-pixel roughness in [0, 1]. White = fully rough; black = mirror-smooth (0).";
        case RenderDebugMode::GBufferMetallic:
            return "Per-pixel metallic in [0, 1]. White = metal; black = dielectric.";
        case RenderDebugMode::GBufferEmissive:
            return "Linear emissive RGB (material emissive property). Black for non-emissive materials until emissive is authored.";
        case RenderDebugMode::RadianceBuffer:
            return "Diffuse-dominant radiance assembled for SSGI trace hits: emissive + stripped indirect IBL + optional fill lights.";
        case RenderDebugMode::RadianceValidity:
            return "Radiance validity mask (A channel). White = traceable geometry; black = sky / background.";
        case RenderDebugMode::RadianceTemporal:
            return "Velocity-reprojected radiance history after temporal accumulation. Color should stabilize after a few frames on a static scene.";
        case RenderDebugMode::GiDisocclusion:
            return "GI reprojection acceptance. Green = history UV in bounds; red = out of bounds or first frame after invalidate.";
        case RenderDebugMode::RadianceTemporalDelta:
            return "Amplified |temporal − raw radiance|. Black = converged. Edge glow during camera motion is OK.";
        case RenderDebugMode::SsgiTraceRaw:
            return "Simulated SSGI trace output (radiance + optional synthetic noise). Should look grainy when noise is on.";
        case RenderDebugMode::SsgiTraceHitMask:
            return "White pixels found an accepted SSGI screen-space hit; black pixels missed or were rejected.";
        case RenderDebugMode::SsgiTraceHitDistance:
            return "SSGI trace confidence after distance, thickness, facing, diffuse, and screen-edge weighting.";
        case RenderDebugMode::SsgiDenoiseSpatial:
            return "After edge-aware spatial blur only. Noise should be reduced vs trace raw; edges should stay sharp.";
        case RenderDebugMode::SsgiDenoiseTemporal:
            return "After spatial + temporal accumulation. Hold still for a smoother result than spatial-only.";
        case RenderDebugMode::SsgiDenoiseFinal:
            return "Full denoise pipeline output (same as temporal stage). Compare against trace raw with noise enabled.";
        case RenderDebugMode::SsgiInject:
            return "Denoised SSGI term injected into composite (strength-scaled in final image). Requires Enable SSGI.";
        case RenderDebugMode::SsgiFinalContribution:
            return "Strength-scaled SSGI term that reaches the composite, isolated from direct/ambient lighting.";
        case RenderDebugMode::SsrSceneColor:
            return "Specular SSR scene-color buffer: fill direct + emissive plus sun direct with shadow. Linear HDR for trace hits.";
        case RenderDebugMode::SsrSceneValidity:
            return "SSR scene-color validity (alpha). White = geometry; black = sky / background.";
        case RenderDebugMode::SsrTraceRaw:
            return "SSR trace radiance (RGB). Stochastic quadratic march. Compare with denoise final after temporal blend ~0.9.";
        case RenderDebugMode::SsrTraceConfidence:
            return "SSR trace confidence (alpha). White = strong hit; black = miss, rough surface, or off-screen exit.";
        case RenderDebugMode::SsrDenoiseSpatial:
            return "SSR after first SVGF à-trous pass (variance-guided). Compare with trace raw; speckle should soften.";
        case RenderDebugMode::SsrDenoiseTemporal:
            return "SSR after SVGF temporal accumulation (color only). Hold camera still ~8 frames to judge stability.";
        case RenderDebugMode::SsrSvgfVariance:
            return "SVGF luminance variance (grayscale). Bright = noisy / still converging; dark = stable.";
        case RenderDebugMode::SsrDenoiseFinal:
            return "Final SVGF output: temporal color + 4-pass variance-guided à-trous (steps 1/2/4/8).";
        case RenderDebugMode::SsrUpscaled:
            return "SSR upscaled to full scene resolution (only when trace quality < full res). At full res, matches denoise final.";
        case RenderDebugMode::SsrSpecReplacement:
            return "SSR specular replacement weight (grayscale). White = full SSR replaces spec IBL; black = spec IBL only.";
        case RenderDebugMode::LightSpaceDepth:
            return "Raw stable clip Z in [0,1] (black=near plane, white=far plane). Magenta = Z out of bounds.";
        case RenderDebugMode::RtDispatchSmoke:
            return "DXR dispatch smoke test output. Confirms the RTPSO and SBT wiring before feature traces.";
        case RenderDebugMode::RtPrimaryHit:
        case RenderDebugMode::RtPrimaryDepth:
        case RenderDebugMode::RtPrimaryNormal:
            return "Path tracer / primary debug hit data at full render resolution.";
        case RenderDebugMode::RtReflectionRaw:
        case RenderDebugMode::RtReflectionConfidence:
        case RenderDebugMode::RtReflectionDenoised:
        case RenderDebugMode::RtSpecReplacement:
            return "RT specular reflection trace stages. Compare raw vs denoised vs composite replacement weight.";
        case RenderDebugMode::RtShadowRaw:
        case RenderDebugMode::RtShadowDenoised:
            return "RT soft sun shadow trace before and after NRD SIGMA denoise (or DLSS-RR when active).";
        case RenderDebugMode::RtGiRaw:
        case RenderDebugMode::RtGiDenoised:
        case RenderDebugMode::RtGiInject:
            return "RT diffuse GI trace, denoise, and inject into the indirect buffer.";
        case RenderDebugMode::RrDiffuseAlbedo:
        case RenderDebugMode::RrSpecularAlbedo:
        case RenderDebugMode::RrNormalRoughness:
            return "DLSS Ray Reconstruction material guide buffers fed to Streamline.";
        case RenderDebugMode::PtIsolateDirectSun:
        case RenderDebugMode::PtIsolateDirectEmissive:
        case RenderDebugMode::PtIsolateSurfaceEmissive:
        case RenderDebugMode::PtIsolateAmbient:
        case RenderDebugMode::PtIsolateAoVisibility:
        case RenderDebugMode::PtIsolateSunVisibility:
        case RenderDebugMode::PtIsolateIndirect:
        case RenderDebugMode::PtIsolatePreClamp:
        case RenderDebugMode::PtIsolateSpecHitDist:
            return "Raw PT term (DLSS/RR/bloom off). Scalar views (AO, sun vis, spec hit dist) are smooth; radiance views are noisy 1 spp. Sky is black except indirect / pre-clamp.";
        case RenderDebugMode::PtRestirReservoirM:
        case RenderDebugMode::PtRestirReservoirAge:
        case RenderDebugMode::PtRestirChosenSource:
        case RenderDebugMode::PtRestirTemporalRejection:
            return "ReSTIR DI P3 temporal diagnostics. Source: green=fresh, blue=history. Rejection: green=accepted, red=no history/match, magenta=ineligible transmission/delta.";
        case RenderDebugMode::PtRestirSpatialSource:
        case RenderDebugMode::PtRestirSpatialRejection:
            return "ReSTIR DI P4 spatial diagnostics. Source: green=center, blue=neighbor. Rejection: green=compatible neighbor accepted, yellow=boiling-filter discard, purple=smooth-metal temporal fallback, red=no compatible candidate, magenta=ineligible surface.";
        case RenderDebugMode::PtRestirGiReservoirM:
        case RenderDebugMode::PtRestirGiReservoirAge:
        case RenderDebugMode::PtRestirGiChosenSource:
        case RenderDebugMode::PtRestirGiTemporalRejection:
            return "ReSTIR GI P6 temporal diagnostics. Source: green=fresh, blue=history. Rejection: green=history accepted, yellow=Jacobian rejection, purple=visibility rejection/fresh fallback, red=no history/surface match, magenta=ineligible or invalid fresh GI sample.";
        case RenderDebugMode::PtRestirGiUcw:
            return "ReSTIR GI reservoir UCW (unbiased contribution weight), Reinhard-normalized grayscale. Spatial discontinuities here (e.g. a hard line at a selection boundary) mean the reused weight differs from the fresh weight = BASIC bias; frame-to-frame flicker means weight variance.";
        case RenderDebugMode::PtRestirGiContribution:
            return "ReSTIR GI final shaded contribution only (base signal excluded), tonemapped grayscale. Isolates exactly what P6 adds: use this to see whether the fly-in grain / curved-surface line lives in the GI term itself.";
        case RenderDebugMode::PtRestirGiReuseMinusFresh:
            return "ReSTIR GI reuse-minus-fresh bias map. 0.5 gray = no bias; brighter = temporal reuse over-brightens vs the fresh P5 estimate; darker = reuse darkens. In reference/path-traced convergence this averages to the exact mean P6 bias, localized per pixel.";
        case RenderDebugMode::PtRestirGiReusedRadiance:
            return "ReSTIR GI reused reservoir's stored secondary radiance (tonemapped). Compare across the line to see whether the held secondary's radiance (vs the weight) carries the bias.";
        case RenderDebugMode::PtTemporalRelativeSigma:
            return "Running luminance sigma / mean for the raw PT output. Hot stable-camera regions identify persistent temporal variance.";
        case RenderDebugMode::PtTemporalFrameDelta:
            return "Amplified per-frame luminance delta for the raw PT output. Use while toggling RR bundle modes or isolate terms.";
        default:
            return nullptr;
        }
    }

    bool DrawDebugViewPicker(ScreenSpaceEffects& screenSpaceEffects, SceneRenderer& renderer)
    {
        static int categoryIndex = 0;
        static int modeIndexInCategory = 0;
        static RenderDebugMode lastSyncedMode = RenderDebugMode::None;

        RenderDebugMode activeMode = renderer.GetRenderDebugMode();
        if (!IsDebugViewModeAvailable(activeMode, renderer, screenSpaceEffects))
        {
            ApplyDebugViewSelection(RenderDebugMode::None, screenSpaceEffects, renderer);
            activeMode = RenderDebugMode::None;
        }

        if (activeMode != lastSyncedMode)
        {
            if (activeMode == RenderDebugMode::None)
            {
                // Stay in the current category; only the view drops back to None.
                modeIndexInCategory = FindModeIndexInCategory(
                    kDebugViewCategories[categoryIndex], RenderDebugMode::None);
            }
            else
            {
                categoryIndex = FindCategoryIndexForMode(activeMode);
                const DebugViewCategory& category = kDebugViewCategories[categoryIndex];
                modeIndexInCategory = FindModeIndexInCategory(category, activeMode);
            }
            lastSyncedMode = activeMode;
        }

        bool changed = false;
        if (DrawCategoryCombo(categoryIndex))
        {
            const DebugViewCategory& selectedCategory = kDebugViewCategories[categoryIndex];
            modeIndexInCategory = FindFirstAvailableModeIndexInCategory(
                selectedCategory, renderer, screenSpaceEffects);
            ApplyDebugViewSelection(
                selectedCategory.modes[modeIndexInCategory], screenSpaceEffects, renderer);
            lastSyncedMode = selectedCategory.modes[modeIndexInCategory];
            changed = true;
        }

        const DebugViewCategory& selectedCategory = kDebugViewCategories[categoryIndex];
        if (DrawModeCombo(selectedCategory, modeIndexInCategory, screenSpaceEffects, renderer))
        {
            lastSyncedMode = renderer.GetRenderDebugMode();
            changed = true;
        }

        if (ImGui::Button("Clear debug view"))
        {
            ApplyDebugViewSelection(RenderDebugMode::None, screenSpaceEffects, renderer);
            modeIndexInCategory = FindModeIndexInCategory(selectedCategory, RenderDebugMode::None);
            lastSyncedMode = RenderDebugMode::None;
            changed = true;
        }

        return changed;
    }
}
