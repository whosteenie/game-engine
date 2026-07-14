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
#include "app/panels/lighting/LightingPanelShared.h"
#include "app/panels/lighting/LightingPanelUi.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

void DrawDirectionalShadowsSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    const Camera& camera = ctx.camera;
    const LightingPanelUi::FeatureState features =
        LightingPanelUi::QueryFeatures(renderer, ctx.screenSpaceEffects);

    if (TuningSectionState::SectionHeader("Directional Shadows", true))
    {
        if (features.pathTracingActive)
        {
            LightingPanelUi::DrawWrappedNote(
                "Path tracing shades direct sun with RT visibility, not CSM. These settings still affect "
                "raster shadow maps and shadow-map debug views.");
        }

        DirectionalShadowSettings& shadowSettings = renderer.GetDirectionalShadowSettings();
        const CascadedShadowMap& shadowMap = renderer.GetShadowMap();

        if (features.pathTracingActive)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::TreeNodeEx("Quality & filtering", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int shadowFilterMode = static_cast<int>(shadowSettings.GetFilterMode());
            const char* shadowFilterLabels[] = {"PCF (fixed kernel)", "PCSS (soft penumbra)"};
            if (ImGui::Combo(
                    "Shadow filter",
                    &shadowFilterMode,
                    shadowFilterLabels,
                    IM_ARRAYSIZE(shadowFilterLabels)))
            {
                shadowSettings.SetFilterMode(static_cast<DirectionalShadowFilterMode>(shadowFilterMode));
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            RendererSettingUi::MarkRendered("shadow_filter");

            int shadowResolution = shadowSettings.GetShadowMapResolution();
            const char* resolutionLabels[] = {"512", "1024", "2048", "4096", "8192"};
            const int resolutionValues[] = {512, 1024, 2048, 4096, 8192};
            int resolutionIndex = 3;
            for (int index = 0; index < IM_ARRAYSIZE(resolutionValues); ++index)
            {
                if (shadowResolution == resolutionValues[index])
                {
                    resolutionIndex = index;
                    break;
                }
            }
            if (ImGui::Combo("Shadow map resolution", &resolutionIndex, resolutionLabels, IM_ARRAYSIZE(resolutionLabels)))
            {
                shadowSettings.SetShadowMapResolution(resolutionValues[resolutionIndex]);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            RendererSettingUi::MarkRendered("shadow_map_resolution");

            int pcfRadius = shadowSettings.GetPcfKernelRadius();
            if (ImGui::SliderInt("PCF kernel radius", &pcfRadius, 1, 8))
            {
                shadowSettings.SetPcfKernelRadius(pcfRadius);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawWrappedNote("Rotated PCF: 7x7 at radius 3, 9x9 at radius 4.");

            int pcfSampleCount = shadowSettings.GetPcfSampleCount();
            if (ImGui::SliderInt("PCF sample count", &pcfSampleCount, 8, 32))
            {
                shadowSettings.SetPcfSampleCount(pcfSampleCount);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            bool useRotatedPcf = shadowSettings.GetUsePoissonPcf();
            if (ImGui::Checkbox("Rotated PCF", &useRotatedPcf))
            {
                shadowSettings.SetUsePoissonPcf(useRotatedPcf);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawWrappedNote(
                "Per-pixel rotated grid; smoother than axis-aligned, no stochastic grain.");

            float sunAngularDiameter = shadowSettings.GetSunAngularDiameterDegrees();
            if (ImGui::SliderFloat("Sun angular diameter (deg)", &sunAngularDiameter, 0.0f, 5.0f))
            {
                shadowSettings.SetSunAngularDiameterDegrees(sunAngularDiameter);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float minPenumbraTexels = shadowSettings.GetMinPenumbraTexels();
            if (ImGui::SliderFloat("Min penumbra (texels)", &minPenumbraTexels, 0.0f, 16.0f))
            {
                shadowSettings.SetMinPenumbraTexels(minPenumbraTexels);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            bool shadowBlurEnabled = shadowSettings.GetShadowBlurEnabled();
            if (ImGui::Checkbox("Shadow penumbra blur", &shadowBlurEnabled))
            {
                shadowSettings.SetShadowBlurEnabled(shadowBlurEnabled);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawWrappedNote(
                "Separable screen-space blur on shadow visibility (removes PCF grain).");

            if (shadowBlurEnabled)
            {
                float shadowBlurRadius = shadowSettings.GetShadowBlurRadius();
                if (ImGui::SliderFloat("Shadow blur radius (px)", &shadowBlurRadius, 0.0f, 8.0f))
                {
                    shadowSettings.SetShadowBlurRadius(shadowBlurRadius);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);

                float shadowBlurDepthThreshold = shadowSettings.GetShadowBlurDepthThreshold();
                if (ImGui::SliderFloat(
                        "Shadow blur depth threshold",
                        &shadowBlurDepthThreshold,
                        0.01f,
                        1.0f))
                {
                    shadowSettings.SetShadowBlurDepthThreshold(shadowBlurDepthThreshold);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);

                float shadowBlurShadowThreshold = shadowSettings.GetShadowBlurShadowThreshold();
                if (ImGui::SliderFloat(
                        "Shadow blur visibility threshold",
                        &shadowBlurShadowThreshold,
                        0.01f,
                        1.0f))
                {
                    shadowSettings.SetShadowBlurShadowThreshold(shadowBlurShadowThreshold);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);
            }

            if (shadowFilterMode == static_cast<int>(DirectionalShadowFilterMode::PCSS))
            {
                float lightAngularSize = shadowSettings.GetPcssLightAngularSize();
                if (ImGui::SliderFloat("PCSS light size", &lightAngularSize, 0.25f, 24.0f))
                {
                    shadowSettings.SetPcssLightAngularSize(lightAngularSize);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);

                int blockerRadius = shadowSettings.GetPcssBlockerRadius();
                if (ImGui::SliderInt("PCSS blocker radius", &blockerRadius, 1, 6))
                {
                    shadowSettings.SetPcssBlockerRadius(blockerRadius);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);

                float minPenumbra = shadowSettings.GetPcssMinPenumbraTexels();
                if (ImGui::SliderFloat("PCSS min penumbra", &minPenumbra, 0.5f, 16.0f))
                {
                    shadowSettings.SetPcssMinPenumbraTexels(minPenumbra);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);

                float maxPenumbra = shadowSettings.GetPcssMaxPenumbraTexels();
                if (ImGui::SliderFloat("PCSS max penumbra", &maxPenumbra, minPenumbra, 64.0f))
                {
                    shadowSettings.SetPcssMaxPenumbraTexels(maxPenumbra);
                    scene.MarkDirty();
                }
                HandleRendererFieldEditEvents(editContext);
            }

            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Shadow map fit", ImGuiTreeNodeFlags_DefaultOpen))
        {
            LightingPanelUi::DrawWrappedNote(
                "Single shadow map (CSM off). Ortho covers scene casters plus the visible view frustum.");

            bool tightNearPlaneFit = shadowSettings.GetTightNearPlaneXyFit();
            if (ImGui::Checkbox("Tight near-plane XY fit", &tightNearPlaneFit))
            {
                shadowSettings.SetTightNearPlaneXyFit(tightNearPlaneFit);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawWrappedNote(
                "Fit ortho XY to the view frustum; caster bounds still expand Z depth.");

            float xyMargin = shadowSettings.GetXyMarginFraction();
            if (ImGui::SliderFloat("Ortho XY margin", &xyMargin, 0.005f, 0.2f, "%.3f"))
            {
                shadowSettings.SetXyMarginFraction(xyMargin);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float zMargin = shadowSettings.GetZMarginFraction();
            if (ImGui::SliderFloat("Ortho Z margin", &zMargin, 0.02f, 0.5f, "%.3f"))
            {
                shadowSettings.SetZMarginFraction(zMargin);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Bias (acne vs peter-panning)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            float worldBiasScale = shadowSettings.GetWorldBiasScale();
            if (ImGui::SliderFloat("World bias scale", &worldBiasScale, 0.0f, 4.0f))
            {
                shadowSettings.SetWorldBiasScale(worldBiasScale);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float depthBiasScale = shadowSettings.GetDepthBiasScale();
            if (ImGui::SliderFloat("Depth bias scale", &depthBiasScale, 0.0f, 4.0f))
            {
                shadowSettings.SetDepthBiasScale(depthBiasScale);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float casterDepthBiasScale = shadowSettings.GetCasterDepthBiasScale();
            if (ImGui::SliderFloat("Caster depth bias scale", &casterDepthBiasScale, 0.0f, 4.0f))
            {
                shadowSettings.SetCasterDepthBiasScale(casterDepthBiasScale);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            LightingPanelUi::DrawWrappedNote(
                "Receiver scales: raise if acne, lower if shadows detach. "
                "Caster scale adds optional front-face shader bias; 0 relies on slope bias only. "
                "Shadow pass uses front-face culling (back faces) for contact depth.");

            ImGui::TreePop();
        }

        if (features.pathTracingActive)
        {
            ImGui::EndDisabled();
        }

        if (ImGui::TreeNodeEx("Shadow map stats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Resolution: %d x %d", shadowMap.GetResolution(), shadowMap.GetResolution());

            const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& setups =
                shadowMap.GetCascadeSetups();
            const std::array<glm::mat4, CascadedShadowMap::MaxCascades>& lightSpaceMatrices =
                shadowMap.GetLightSpaceMatrices();
            const ShadowLightSpaceSetup& setup = setups[0];
            const float texelSpan = std::max(setup.texelWorldSizeX, setup.texelWorldSizeY);
            ImGui::BulletText(
                "Ortho %.1f x %.1f m | texel %.4f m | content clipZ [%.3f, %.3f]",
                setup.orthoWidth,
                setup.orthoHeight,
                texelSpan,
                setup.clipDepthContentMin,
                setup.clipDepthContentMax);

            if (ImGui::TreeNode("Light-space receiver probe"))
            {
                const auto probeAt = [&](const char* label, const glm::vec3& worldPoint) {
                    const glm::vec4 viewPoint = camera.GetViewMatrix() * glm::vec4(worldPoint, 1.0f);
                    const ShadowReceiverProbeResult probe = EvaluateShadowReceiverProbe(
                        worldPoint,
                        viewPoint.z,
                        lightSpaceMatrices.data(),
                        setups.data(),
                        shadowMap.GetCascadeEndSplits().data(),
                        1);
                    ImGui::Text("%s @ (%.2f, %.2f, %.2f)", label, worldPoint.x, worldPoint.y, worldPoint.z);
                    ImGui::BulletText(
                        "inBounds %s | raw clipZ %.4f | UV (%.3f, %.3f)",
                        probe.inBounds ? "yes" : "no",
                        probe.receiverClipZ,
                        probe.shadowUv.x,
                        probe.shadowUv.y);
                };

                const glm::vec3 focusPoint = camera.GetPosition() + camera.GetFront() * 3.0f;
                probeAt("Focus (3m ahead)", focusPoint);
                probeAt("World origin floor", glm::vec3(0.0f, 0.0f, 0.0f));
                probeAt(
                    "Floor under camera",
                    glm::vec3(camera.GetPosition().x, 0.0f, camera.GetPosition().z));

                LightingPanelUi::DrawWrappedNote(
                    "Magenta in light-space depth debug = clip Z outside [0, 1]. "
                    "UV outside [0, 1] = shadow map coverage.");
                ImGui::TreePop();
            }

            ImGui::Separator();
            LightingPanelUi::DrawWrappedHelp(
                "Large texels or blocky shadows: raise resolution or lower ortho margin. "
                "Debug: Shadow factor (1), Shadow blocked (22).");
            ImGui::TreePop();
        }
    }
}
