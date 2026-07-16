#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/RendererSettingUi.h"
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
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

void DrawAmbientOcclusionSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const LightingPanelUi::FeatureState features = LightingPanelUi::QueryFeatures(renderer, screenSpaceEffects);

    if (TuningSectionState::SectionHeader("Ambient occlusion", true))
    {
        const bool sectionDisabled =
            !features.postProcessingEnabled || features.pathTracingActive;

        if (!features.postProcessingEnabled)
        {
            LightingPanelUi::DrawWrappedNote("Enable Post-processing to use screen-space ambient occlusion.");
        }

        if (features.pathTracingActive)
        {
            LightingPanelUi::DrawWrappedNote(
                "Path tracing handles occlusion in the integrator. Hybrid AO is disabled.");
        }

        if (sectionDisabled)
        {
            ImGui::BeginDisabled();
        }

        AmbientOcclusionMode aoMode = screenSpaceEffects.GetAmbientOcclusionMode();
        if (ImGui::BeginCombo("AO mode", AmbientOcclusionModeLabel(aoMode)))
        {
            const AmbientOcclusionMode modes[] = {
                AmbientOcclusionMode::Off,
                AmbientOcclusionMode::SSAO,
                AmbientOcclusionMode::GTAO,
            };
            for (const AmbientOcclusionMode mode : modes)
            {
                const bool selected = aoMode == mode;
                if (ImGui::Selectable(AmbientOcclusionModeLabel(mode), selected) && !selected)
                {
                    RendererSettingUi::ApplyChange(
                        "ao_mode",
                        editContext,
                        scene,
                        "Ambient occlusion mode",
                        [mode](Scene& target) {
                            target.GetRenderer().GetScreenSpaceEffects().SetAmbientOcclusionMode(mode);
                            target.MarkDirty();
                        });
                    ImGui::CloseCurrentPopup();
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        RendererSettingUi::MarkRendered("ao_mode");

        if (aoMode == AmbientOcclusionMode::SSAO)
        {
            float ssaoRadius = screenSpaceEffects.GetSsaoRadius();
            if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.1f, 1.5f))
            {
                screenSpaceEffects.SetSsaoRadius(ssaoRadius);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawWrappedNote("Radius is view-space units (fixed, not scaled by depth).");

            float ssaoBias = screenSpaceEffects.GetSsaoBias();
            if (ImGui::SliderFloat("SSAO bias", &ssaoBias, 0.0f, 0.1f))
            {
                screenSpaceEffects.SetSsaoBias(ssaoBias);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawTooltipForLastItem(
                "Offsets the depth comparison to prevent surfaces from falsely occluding themselves. Too much can remove contact shading.");

            float ssaoPower = screenSpaceEffects.GetSsaoPower();
            if (ImGui::SliderFloat("SSAO intensity", &ssaoPower, 0.5f, 4.0f))
            {
                screenSpaceEffects.SetSsaoPower(ssaoPower);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (aoMode == AmbientOcclusionMode::GTAO)
        {
            float gtaoRadius = screenSpaceEffects.GetGtaoRadius();
            if (ImGui::SliderFloat("GTAO radius", &gtaoRadius, 0.1f, 3.0f))
            {
                screenSpaceEffects.SetGtaoRadius(gtaoRadius);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float gtaoThickness = screenSpaceEffects.GetGtaoThickness();
            if (ImGui::SliderFloat("GTAO thickness", &gtaoThickness, 0.02f, 2.0f))
            {
                screenSpaceEffects.SetGtaoThickness(gtaoThickness);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawTooltipForLastItem(
                "How thick nearby geometry appears to the occlusion test. Higher values can strengthen contact shadows.");

            float gtaoFalloff = screenSpaceEffects.GetGtaoFalloff();
            if (ImGui::SliderFloat("GTAO falloff", &gtaoFalloff, 0.25f, 6.0f))
            {
                screenSpaceEffects.SetGtaoFalloff(gtaoFalloff);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawTooltipForLastItem(
                "How quickly occlusion fades with distance. Higher values keep the effect closer to surfaces.");

            float gtaoPower = screenSpaceEffects.GetGtaoPower();
            if (ImGui::SliderFloat("GTAO intensity", &gtaoPower, 0.25f, 4.0f))
            {
                screenSpaceEffects.SetGtaoPower(gtaoPower);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            int gtaoDirections = screenSpaceEffects.GetGtaoDirections();
            if (ImGui::SliderInt("GTAO directions", &gtaoDirections, 2, 8))
            {
                screenSpaceEffects.SetGtaoDirections(gtaoDirections);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawTooltipForLastItem(
                "Number of angles sampled around each pixel. More directions reduce banding but cost more GPU time.");

            int gtaoSteps = screenSpaceEffects.GetGtaoSteps();
            if (ImGui::SliderInt("GTAO steps", &gtaoSteps, 2, 12))
            {
                screenSpaceEffects.SetGtaoSteps(gtaoSteps);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawTooltipForLastItem(
                "Samples taken along each direction. More steps improve range and stability but cost more GPU time.");

            bool gtaoDenoise = screenSpaceEffects.IsGtaoDenoiseEnabled();
            UndoableRendererCheckbox(
                "GTAO denoise",
                &gtaoDenoise,
                editContext,
                [](Scene& target, bool enabled) {
                    target.GetRenderer().GetScreenSpaceEffects().SetGtaoDenoiseEnabled(enabled);
                    target.MarkDirty();
                });
        }

        float ssaoBlurDepthThreshold = screenSpaceEffects.GetSsaoBlurDepthThreshold();
        if (ImGui::SliderFloat("SSAO blur depth threshold", &ssaoBlurDepthThreshold, 0.001f, 0.25f))
        {
            screenSpaceEffects.SetSsaoBlurDepthThreshold(ssaoBlurDepthThreshold);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);
        LightingPanelUi::DrawWrappedNote(
            "Edge-aware blur: lower = sharper AO edges across depth discontinuities.");

        int ssaoShaderDebug = screenSpaceEffects.GetSsaoShaderDebugMode();
        if (aoMode == AmbientOcclusionMode::SSAO && ImGui::Combo(
                "SSAO shader debug",
                &ssaoShaderDebug,
                "AO output\0"
                "Used samples\0"
                "Occlusion hits\0"
                "Depth delta\0"
                "Force 0.5\0"
                "View depth\0"
                "Proj vs depth\0\0"))
        {
            screenSpaceEffects.SetSsaoShaderDebugMode(ssaoShaderDebug);
        }
        if (ssaoShaderDebug != 0)
        {
            LightingPanelUi::DrawWrappedNote(
                "Shader debug shows raw SSAO target (unblurred). Set Diagnostics > View to SSAO buffer.");
        }
        LightingPanelUi::DrawWrappedNote(
            "Intensity = pow() on AO in composite (indirect only). Blend = how much AO affects indirect.");
        LightingPanelUi::DrawWrappedNote(
            "Use AO debug views for raw or filtered factors; Composite occlusion shows the final factor.");

        float aoStrength = screenSpaceEffects.GetAoStrength();
        if (ImGui::SliderFloat("AO blend strength", &aoStrength, 0.0f, 1.0f))
        {
            screenSpaceEffects.SetAoStrength(aoStrength);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        const SsaoDiagnosticsSnapshot& ssaoDiag = screenSpaceEffects.GetSsaoDiagnostics();
        if (ImGui::TreeNode("SSAO diagnostics (live)"))
        {
            ImGui::Text("Frame %llu", static_cast<unsigned long long>(ssaoDiag.captureFrame));
            ImGui::Text(
                "enabled=%s  pass=%s  composite=%s  compositeSsao=%s  shadowComposite=%s",
                ssaoDiag.enabled ? "yes" : "no",
                ssaoDiag.passExecuted ? "yes" : "no",
                ssaoDiag.compositeRan ? "yes" : "no",
                ssaoDiag.compositeUsesSsao ? "yes" : "no",
                ssaoDiag.shadowComposite ? "yes" : "no");
            ImGui::Text(
                "hdrSrc=%s  debugView=%s  split=%s  geomNormals=%s",
                ssaoDiag.hdrColorSource,
                ssaoDiag.ssaoDebugViewSource,
                ssaoDiag.splitLighting ? "yes" : "no",
                ssaoDiag.geometryNormals ? "yes" : "no");
            ImGui::Text(
                "bindings depth=0x%zx normal=0x%zx noise=0x%zx ssaoBlur=0x%zx shadow=0x%zx",
                ssaoDiag.depthSrv,
                ssaoDiag.normalSrv,
                ssaoDiag.noiseSrv,
                ssaoDiag.ssaoBlurSrv,
                ssaoDiag.shadowFactorSrv);
            ImGui::Text(
                "uniforms uSamples=%s uKernelSize=%s  kernelCount=%d  kernel0=(%.3f, %.3f, %.3f)",
                ssaoDiag.hasUniformSamples ? "ok" : "MISSING",
                ssaoDiag.hasUniformKernelSize ? "ok" : "MISSING",
                ssaoDiag.kernelCount,
                ssaoDiag.kernelSample0X,
                ssaoDiag.kernelSample0Y,
                ssaoDiag.kernelSample0Z);
            if (ssaoDiag.gpuReadbackValid)
            {
                ImGui::Text(
                    "center GPU: hwDepth=%.4f ssaoRaw=%.4f ssaoBlur=%.4f normal=(%.3f, %.3f, %.3f)",
                    ssaoDiag.centerHardwareDepth,
                    ssaoDiag.centerSsaoRaw,
                    ssaoDiag.centerSsaoBlur,
                    ssaoDiag.centerNormalR,
                    ssaoDiag.centerNormalG,
                    ssaoDiag.centerNormalB);
                if (ssaoDiag.centerSsaoBlur >= 0.99f)
                {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                        "ssaoBlur ~1.0 at center => open surface or sky (expected)");
                }
                else if (ssaoDiag.centerSsaoBlur < 0.99f)
                {
                    ImGui::TextColored(
                        ImVec4(0.45f, 1.0f, 0.55f, 1.0f),
                        "ssaoBlur < 1.0 => occlusion detected at center readback");
                }
            }
            else
            {
                ImGui::TextDisabled(
                    "GPU center readback appears after SSAO toggle (one frame delay).");
            }
            if (ssaoDiag.normalSrv == 0)
            {
                EditorWidgets::TextColoredError(
                    "normalSrv is null - geometry-normal G-buffer missing");
            }
            if (ssaoDiag.shadowFactorSrv != 0 && ssaoDiag.normalSrv == ssaoDiag.shadowFactorSrv)
            {
                EditorWidgets::TextColoredError(
                    "normalSrv == shadowFactorSrv - bindings swapped?");
            }
            ImGui::TextDisabled("Toggle SSAO with GAME_ENGINE_RENDER_DEBUG=1 for stderr snapshot.");
            ImGui::TreePop();
        }

        if (sectionDisabled)
        {
            ImGui::EndDisabled();
        }
    }
}
