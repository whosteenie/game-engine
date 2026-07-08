#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentIblSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/EnvironmentPresets.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/platform/EngineLog.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/DxrCapabilities.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/assets/FileDialog.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

void DrawDiagnosticsSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const Camera& camera = ctx.camera;
    const int viewportWidth = ctx.viewportWidth;
    const int viewportHeight = ctx.viewportHeight;

    if (TuningSectionState::SectionHeader("Diagnostics", true))
    {
        ImGui::TextUnformatted(
            "Use debug views to see which render pass causes an artifact. "
            "Write a report and share the txt file for help.");

        int debugMode = static_cast<int>(screenSpaceEffects.GetDebugMode());
        const char* debugModeLabels[] = {
            RenderDebugModeLabel(RenderDebugMode::None),
            RenderDebugModeLabel(RenderDebugMode::ShadowFactor),
            RenderDebugModeLabel(RenderDebugMode::DirectLighting),
            RenderDebugModeLabel(RenderDebugMode::AmbientIbl),
            RenderDebugModeLabel(RenderDebugMode::LightSpaceUv),
            RenderDebugModeLabel(RenderDebugMode::LightSpaceDepth),
            RenderDebugModeLabel(RenderDebugMode::CascadeIndex),
            RenderDebugModeLabel(RenderDebugMode::GeometricNormal),
            RenderDebugModeLabel(RenderDebugMode::TangentHandedness),
            RenderDebugModeLabel(RenderDebugMode::ViewDepth),
            RenderDebugModeLabel(RenderDebugMode::CascadeBlendFactor),
            RenderDebugModeLabel(RenderDebugMode::DiffuseIbl),
            RenderDebugModeLabel(RenderDebugMode::SpecularIbl),
            RenderDebugModeLabel(RenderDebugMode::DirectDiffuseGeom),
            RenderDebugModeLabel(RenderDebugMode::ShadedNormal),
            RenderDebugModeLabel(RenderDebugMode::ShadowFactorUnbiased),
            RenderDebugModeLabel(RenderDebugMode::ShadowMapStoredDepth),
            RenderDebugModeLabel(RenderDebugMode::ShadowDepthSeparation),
            RenderDebugModeLabel(RenderDebugMode::Ssao),
            RenderDebugModeLabel(RenderDebugMode::GtaoRaw),
            RenderDebugModeLabel(RenderDebugMode::GtaoFiltered),
            RenderDebugModeLabel(RenderDebugMode::CompositeOcclusion),
            RenderDebugModeLabel(RenderDebugMode::GeomSunFacing),
            RenderDebugModeLabel(RenderDebugMode::ShadowCompareDepth),
            RenderDebugModeLabel(RenderDebugMode::ShadowBlockedCenter),
            RenderDebugModeLabel(RenderDebugMode::MotionVectors),
            RenderDebugModeLabel(RenderDebugMode::GBufferAlbedo),
            RenderDebugModeLabel(RenderDebugMode::GBufferRoughness),
            RenderDebugModeLabel(RenderDebugMode::GBufferMetallic),
            RenderDebugModeLabel(RenderDebugMode::GBufferEmissive),
            RenderDebugModeLabel(RenderDebugMode::RadianceBuffer),
            RenderDebugModeLabel(RenderDebugMode::RadianceValidity),
            RenderDebugModeLabel(RenderDebugMode::RadianceTemporal),
            RenderDebugModeLabel(RenderDebugMode::GiDisocclusion),
            RenderDebugModeLabel(RenderDebugMode::RadianceTemporalDelta),
            RenderDebugModeLabel(RenderDebugMode::SsgiTraceRaw),
            RenderDebugModeLabel(RenderDebugMode::SsgiDenoiseSpatial),
            RenderDebugModeLabel(RenderDebugMode::SsgiDenoiseTemporal),
            RenderDebugModeLabel(RenderDebugMode::SsgiDenoiseFinal),
            RenderDebugModeLabel(RenderDebugMode::SsgiInject),
            RenderDebugModeLabel(RenderDebugMode::SsgiTraceHitMask),
            RenderDebugModeLabel(RenderDebugMode::SsgiTraceHitDistance),
            RenderDebugModeLabel(RenderDebugMode::SsgiFinalContribution),
            RenderDebugModeLabel(RenderDebugMode::SsrSceneColor),
            RenderDebugModeLabel(RenderDebugMode::SsrSceneValidity),
            RenderDebugModeLabel(RenderDebugMode::SsrTraceRaw),
            RenderDebugModeLabel(RenderDebugMode::SsrTraceConfidence),
            RenderDebugModeLabel(RenderDebugMode::SsrDenoiseSpatial),
            RenderDebugModeLabel(RenderDebugMode::SsrDenoiseTemporal),
            RenderDebugModeLabel(RenderDebugMode::SsrDenoiseFinal),
            RenderDebugModeLabel(RenderDebugMode::SsrSvgfVariance),
            RenderDebugModeLabel(RenderDebugMode::SsrUpscaled),
            RenderDebugModeLabel(RenderDebugMode::SsrSpecReplacement),
            RenderDebugModeLabel(RenderDebugMode::RtDispatchSmoke),
            RenderDebugModeLabel(RenderDebugMode::RtPrimaryHit),
            RenderDebugModeLabel(RenderDebugMode::RtPrimaryDepth),
            RenderDebugModeLabel(RenderDebugMode::RtPrimaryNormal),
            RenderDebugModeLabel(RenderDebugMode::RtReflectionRaw),
            RenderDebugModeLabel(RenderDebugMode::RtReflectionConfidence),
            RenderDebugModeLabel(RenderDebugMode::RtReflectionDenoised),
            RenderDebugModeLabel(RenderDebugMode::RtSpecReplacement),
            RenderDebugModeLabel(RenderDebugMode::RtShadowRaw),
            RenderDebugModeLabel(RenderDebugMode::RtShadowDenoised),
            RenderDebugModeLabel(RenderDebugMode::RtGiRaw),
            RenderDebugModeLabel(RenderDebugMode::RtGiDenoised),
            RenderDebugModeLabel(RenderDebugMode::RtGiInject),
            RenderDebugModeLabel(RenderDebugMode::RrDiffuseAlbedo),
            RenderDebugModeLabel(RenderDebugMode::RrSpecularAlbedo),
            RenderDebugModeLabel(RenderDebugMode::RrNormalRoughness),
        };

        if (ImGui::Combo(
                "Debug view",
                &debugMode,
                debugModeLabels,
                IM_ARRAYSIZE(debugModeLabels)))
        {
            const auto selectedMode = static_cast<RenderDebugMode>(debugMode);
            screenSpaceEffects.SetDebugMode(selectedMode);
            if ((IsRtPrimaryDebugMode(selectedMode) || IsRtReflectionDebugMode(selectedMode)
                 || IsRtShadowDebugMode(selectedMode) || IsRtGiDebugMode(selectedMode))
                && renderer.GetDxrSettings().IsEnabled()
                && !GfxContext::Get().IsFrameRecording())
            {
                renderer.WarmUpDxrPipelineIfNeeded();
                screenSpaceEffects.ResetRtPrimaryDebugBlitSettle();
            }
        }

        if (debugMode == static_cast<int>(RenderDebugMode::LightSpaceUv))
        {
            ImGui::TextWrapped(
                "Light-space UV shifts with the flycam: cascades refit to the camera frustum each frame.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::CascadeIndex))
        {
            ImGui::TextWrapped(
                "Cascade tint is stable per split; brightness encodes view depth within the active cascade.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::DirectLighting))
        {
            ImGui::TextWrapped(
                "Unshadowed direct (sun diffuse uses geom N·L; spec uses shaded normal + BRDF). "
                "Should match Direct diffuse geom (13) in the large-scale terminator. "
                "Final = this × shadow factor + IBL.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::DirectDiffuseGeom))
        {
            ImGui::TextWrapped(
                "Sun diffuse on geometric normals only. Stable across flycam if this view is flat but Direct lighting is not.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ShadedNormal))
        {
            ImGui::TextWrapped("Normal used for lighting after normal-map perturbation. Compare with Geometric normal.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::DiffuseIbl))
        {
            ImGui::TextWrapped("Indirect diffuse from L2 SH9 irradiance using the shaded normal.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SpecularIbl))
        {
            ImGui::TextWrapped("Indirect specular from prefiltered env map. View-dependent by design.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::CascadeBlendFactor))
        {
            ImGui::TextWrapped(
                "Cascade cross-fade weight. Bright bands on the floor near the camera often indicate a blend seam.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ViewDepth))
        {
            ImGui::TextWrapped("Linear view-space Z used for cascade selection. Compare with Cascade index.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ShadowFactorUnbiased))
        {
            ImGui::TextWrapped(
                "Shadow map PCF with receiver bias disabled. Use with Shadow depth separation to split map vs bias issues.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ShadowMapStoredDepth))
        {
            ImGui::TextWrapped(
                "Raw depth stored in the shadow map at the receiver's light-space UV (center texel, no filtering). Black = near, white = far.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ShadowDepthSeparation))
        {
            ImGui::TextWrapped(
                "Receiver clip depth minus stored map depth, scaled by the minimum separation threshold. Mid-gray = match; brighter = behind stored; darker = in front.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GeomSunFacing))
        {
            ImGui::TextWrapped(
                "Geometric N·L for the shadow-casting directional light only. White = faces the sun; black = back faces. "
                "No albedo, no IBL, no composite — not comparable to the final image directly. "
                "Use Direct lighting (2) to preview the lit pass direct buffer.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ShadowCompareDepth))
        {
            ImGui::TextWrapped(
                "Clip depth used in the shadow compare test (unbiased path, tiny depth bias only). Should track stored depth on lit surfaces.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::ShadowBlockedCenter))
        {
            ImGui::TextWrapped(
                "Raw center-texel compare: receiver clip Z vs stored map depth, no PCF, no receiver bias, no min-separation floor. "
                "White = lit, black = blocked. Fix this view before tuning bias or blur.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::Ssao))
        {
            ImGui::TextWrapped(
                "Current AO target for SSAO mode (1 = no occlusion). Use SSAO shader debug combo for raw/instrumented views. "
                "When AO is off or GTAO is active, this view may show the active AO target or stale data.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GtaoRaw))
        {
            ImGui::TextWrapped(
                "Raw GTAO visibility before denoise (1 = no occlusion). Use this to judge whether the horizon pass finds crevices.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GtaoFiltered))
        {
            ImGui::TextWrapped(
                "Filtered GTAO visibility after edge-aware denoise. This is the map composited into indirect lighting.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::MotionVectors))
        {
            ImGui::TextWrapped(
                "Per-pixel screen-space velocity (hue = direction, brightness = magnitude). "
                "Pan the camera on static geometry for a uniform field; move an object for localized color. "
                "Sky and first frame after load/resize are black (zero velocity).");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GBufferAlbedo))
        {
            ImGui::TextWrapped(
                "Linear base color (texture × factor), no lighting. Textured objects should match their albedo maps.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GBufferRoughness))
        {
            ImGui::TextWrapped(
                "Per-pixel roughness in [0, 1]. White = fully rough; black = mirror-smooth (0).");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GBufferMetallic))
        {
            ImGui::TextWrapped(
                "Per-pixel metallic in [0, 1]. White = metal; black = dielectric.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GBufferEmissive))
        {
            ImGui::TextWrapped(
                "Linear emissive RGB (material emissive property). Black for non-emissive materials until emissive is authored.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::RadianceBuffer))
        {
            ImGui::TextWrapped(
                "Diffuse-dominant radiance assembled for SSGI trace hits: emissive + stripped indirect IBL + optional fill lights. "
                "Sky is black; geometry should show soft ambient-tinted color (not final shaded composite).");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::RadianceValidity))
        {
            ImGui::TextWrapped(
                "Radiance validity mask (A channel). White = traceable geometry; black = sky / background.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::RadianceTemporal))
        {
            ImGui::TextWrapped(
                "Velocity-reprojected radiance history after temporal accumulation. "
                "Pan the camera on a static scene — color should stabilize after a few frames.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::GiDisocclusion))
        {
            ImGui::TextWrapped(
                "GI reprojection acceptance (not full depth disocclusion yet). "
                "Green = history UV in bounds; red = out of bounds or first frame after invalidate. "
                "Static scene should be all green. Object-motion rejection is Phase 5.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::RadianceTemporalDelta))
        {
            ImGui::TextWrapped(
                "Amplified |temporal − raw radiance|. Black = converged. Edge glow during camera motion is OK.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiTraceRaw))
        {
            ImGui::TextWrapped(
                "Simulated SSGI trace output (radiance + optional synthetic noise). Should look grainy when noise is on.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiTraceHitMask))
        {
            ImGui::TextWrapped(
                "White pixels found an accepted SSGI screen-space hit; black pixels missed or were rejected.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiTraceHitDistance))
        {
            ImGui::TextWrapped(
                "SSGI trace confidence after distance, thickness, facing, diffuse, and screen-edge weighting. "
                "Black means no useful contribution for reconstruction.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiDenoiseSpatial))
        {
            ImGui::TextWrapped(
                "After edge-aware spatial blur only. Noise should be reduced vs trace raw; edges should stay sharp.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiDenoiseTemporal))
        {
            ImGui::TextWrapped(
                "After spatial + temporal accumulation. Hold still — should be smoother than spatial-only.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiDenoiseFinal))
        {
            ImGui::TextWrapped(
                "Full denoise pipeline output (same as temporal stage). Compare against trace raw with noise enabled.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiInject))
        {
            ImGui::TextWrapped(
                "Denoised SSGI term injected into composite (strength-scaled in final image). "
                "Requires Enable SSGI. Use Ambient / IBL debug to compare indirect before inject.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsgiFinalContribution))
        {
            ImGui::TextWrapped(
                "Strength-scaled SSGI term that reaches the composite, isolated from direct/ambient lighting.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrSceneColor))
        {
            ImGui::TextWrapped(
                "Specular SSR scene-color buffer: fill direct + emissive (RT0) plus sun direct with shadow (RT3). "
                "Linear HDR for trace hits — not IBL. Compare with Radiance buffer to see sun + no IBL strip.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrSceneValidity))
        {
            ImGui::TextWrapped(
                "SSR scene-color validity (alpha). White = geometry; black = sky / background.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrTraceRaw))
        {
            ImGui::TextWrapped(
                "SSR trace radiance (RGB). Stochastic quadratic march — should look noisier than bands after quality pass. "
                "Compare with denoise final after temporal blend ~0.9.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrTraceConfidence))
        {
            ImGui::TextWrapped(
                "SSR trace confidence (alpha). White = strong hit; black = miss, rough surface, or off-screen exit.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrDenoiseSpatial))
        {
            ImGui::TextWrapped(
                "SSR after first SVGF à-trous pass (variance-guided). Compare with trace raw — speckle should soften.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrDenoiseTemporal))
        {
            ImGui::TextWrapped(
                "SSR after SVGF temporal accumulation (color only). Hold camera still ~8 frames to judge stability.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrSvgfVariance))
        {
            ImGui::TextWrapped(
                "SVGF luminance variance (grayscale). Bright = noisy / still converging; dark = stable, minimal filter.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrDenoiseFinal))
        {
            ImGui::TextWrapped(
                "Final SVGF output: temporal color + 4-pass variance-guided à-trous (steps 1/2/4/8).");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrUpscaled))
        {
            ImGui::TextWrapped(
                "SSR upscaled to full scene resolution (only when trace quality < full res). At full res, matches denoise final.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::SsrSpecReplacement))
        {
            ImGui::TextWrapped(
                "SSR specular replacement weight (grayscale): confidence × smoothness² × SSR strength. "
                "White = full SSR replaces spec IBL; black = spec IBL only. Enable SSR and use None for final image.");
        }
        else if (debugMode == static_cast<int>(RenderDebugMode::LightSpaceDepth))
        {
            ImGui::TextWrapped(
                "Raw stable clip Z in [0,1] (black=near plane, white=far plane). "
                "Shows surface detail directly — no normalization. Magenta = Z out of bounds.");
            ImGui::TextDisabled(
                "If you see hard white/black regions: enable Frustum-only XY fit, move camera to reset stable fit, "
                "then compare with Shadow map stored depth.");
        }

        static std::string diagnosticStatus;
        ImGui::TextDisabled("HDR+AO on by default; enable Bloom in panel for full post stack.");
        ImGui::TextDisabled("Set GAME_ENGINE_RENDER_DEBUG=1 for HDR/AO/import stderr logs.");
        if (ImGui::Button("Write diagnostics/render_diagnostics.txt"))
        {
            const ImVec2 viewportSize = ImGui::GetContentRegionAvail();
            RenderDiagnostics::WriteReport(
                scene,
                camera,
                viewportWidth,
                viewportHeight,
                "diagnostics/render_diagnostics.txt",
                diagnosticStatus);
        }

        if (!diagnosticStatus.empty())
        {
            ImGui::TextWrapped("%s", diagnosticStatus.c_str());
        }
    }
}
