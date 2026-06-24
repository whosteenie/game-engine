#include "app/panels/LightingPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"

#include <imgui.h>

#include <glm/glm.hpp>

#include <functional>

namespace
{
    void ApplyRendererChange(
        RendererEditContext& editContext,
        Scene& scene,
        const char* commandName,
        const std::function<void(Scene&)>& mutate)
    {
        if (editContext.undoStack != nullptr)
        {
            PushRendererMutation(*editContext.undoStack, scene, commandName, mutate);
            return;
        }

        mutate(scene);
    }
}

void LightingPanel::Draw(
    Scene& scene,
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    UndoStack* undoStack) const
{
    EditorPanelConstraints::ApplySideColumnPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Renderer Tuning", m_showPanel))
    {
        return;
    }

    m_rendererEditContext.undoStack = undoStack;
    m_rendererEditContext.scene = &scene;
    RendererEditContext& editContext = m_rendererEditContext;

    const glm::vec3 cameraPosition = camera.GetPosition();
    ImGui::Text("Camera: (%.1f, %.1f, %.1f)", cameraPosition.x, cameraPosition.y, cameraPosition.z);

    SceneRenderer& renderer = scene.GetRenderer();
    IBL& ibl = renderer.GetIBL();
    ScreenSpaceEffects& screenSpaceEffects = renderer.GetScreenSpaceEffects();

    if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool showGizmos = scene.GetShowLightGizmos();
        if (ImGui::Checkbox("Show light gizmos", &showGizmos))
        {
            if (editContext.undoStack != nullptr)
            {
                PushSceneEditorViewMutation(
                    *editContext.undoStack,
                    scene,
                    "Light gizmos",
                    [&](Scene& target) {
                        target.SetShowLightGizmos(showGizmos);
                    });
            }
            else
            {
                scene.SetShowLightGizmos(showGizmos);
            }
        }

        ImGui::TextUnformatted("Create and edit lights from the Hierarchy and Inspector.");
    }

    if (ImGui::CollapsingHeader("Image-Based Lighting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float environmentIntensity = ibl.GetEnvironmentIntensity();
        if (ImGui::SliderFloat("Environment intensity", &environmentIntensity, 0.0f, 2.0f))
        {
            ibl.SetEnvironmentIntensity(environmentIntensity);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);
    }

    if (ImGui::CollapsingHeader("Directional Shadows", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DirectionalShadowSettings& shadowSettings = renderer.GetDirectionalShadowSettings();
        const CascadedShadowMap& shadowMap = renderer.GetShadowMap();

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

            int pcfRadius = shadowSettings.GetPcfKernelRadius();
            if (ImGui::SliderInt("PCF kernel radius", &pcfRadius, 1, 8))
            {
                shadowSettings.SetPcfKernelRadius(pcfRadius);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled("Rotated PCF: 7x7 at radius 3, 9x9 at radius 4.");

            bool useRotatedPcf = shadowSettings.GetUsePoissonPcf();
            if (ImGui::Checkbox("Rotated PCF", &useRotatedPcf))
            {
                shadowSettings.SetUsePoissonPcf(useRotatedPcf);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled("Per-pixel rotated grid; smoother than axis-aligned, no stochastic grain.");

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
            ImGui::TextDisabled("Separable screen-space blur on shadow visibility (removes PCF grain).");

            if (shadowBlurEnabled)
            {
                float shadowBlurRadius = shadowSettings.GetShadowBlurRadius();
                if (ImGui::SliderFloat("Shadow blur radius (px)", &shadowBlurRadius, 0.0f, 8.0f))
                {
                    shadowSettings.SetShadowBlurRadius(shadowBlurRadius);
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

        if (ImGui::TreeNodeEx("Cascade splits (CSM)", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int cascadeCount = shadowSettings.GetCascadeCount();
            if (ImGui::SliderInt("Cascade count", &cascadeCount, 1, DirectionalShadowSettings::MaxCascades))
            {
                shadowSettings.SetCascadeCount(cascadeCount);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float splitLambda = shadowSettings.GetCascadeSplitLambda();
            if (ImGui::SliderFloat("Split lambda", &splitLambda, 0.0f, 1.0f))
            {
                shadowSettings.SetCascadeSplitLambda(splitLambda);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled("0 = uniform splits, 1 = logarithmic (more near detail)");

            float blendRatio = shadowSettings.GetCascadeBlendRatio();
            if (ImGui::SliderFloat("Cascade blend ratio", &blendRatio, 0.0f, 0.5f))
            {
                shadowSettings.SetCascadeBlendRatio(blendRatio);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            bool tightNearPlaneXyFit = shadowSettings.GetTightNearPlaneXyFit();
            if (ImGui::Checkbox("Frustum-only XY fit", &tightNearPlaneXyFit))
            {
                shadowSettings.SetTightNearPlaneXyFit(tightNearPlaneXyFit);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled(
                "Exclude caster bounds from ortho XY (smaller texels). Casters still set Z depth range.");

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
            ImGui::TextDisabled("Raise if you see acne; lower if shadows detach from surfaces.");

            ImGui::TreePop();
        }

        if (ImGui::TreeNodeEx("Live cascade stats", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Text("Active resolution: %d x %d per cascade", shadowMap.GetResolution(), shadowMap.GetResolution());
            ImGui::Text("Active cascades: %d", shadowMap.GetActiveCascadeCount());

            const glm::vec3 focusPoint = camera.GetPosition() + camera.GetFront() * 3.0f;
            const glm::vec4 viewFocus = camera.GetViewMatrix() * glm::vec4(focusPoint, 1.0f);
            const float focusViewDepth = -viewFocus.z;
            int focusCascade = 0;
            const std::array<float, CascadedShadowMap::MaxCascades>& splits = shadowMap.GetCascadeEndSplits();
            for (int cascadeIndex = 0; cascadeIndex < shadowMap.GetActiveCascadeCount() - 1; ++cascadeIndex)
            {
                if (focusViewDepth > splits[static_cast<std::size_t>(cascadeIndex)])
                {
                    focusCascade = cascadeIndex + 1;
                }
            }
            ImGui::Text("Focus view depth (3m ahead): %.2f -> cascade %d", focusViewDepth, focusCascade);

            const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& setups =
                shadowMap.GetCascadeSetups();
            for (int cascadeIndex = 0; cascadeIndex < shadowMap.GetActiveCascadeCount(); ++cascadeIndex)
            {
                const ShadowLightSpaceSetup& setup = setups[static_cast<std::size_t>(cascadeIndex)];
                const float texelSpan =
                    std::max(setup.texelWorldSizeX, setup.texelWorldSizeY);
                const float splitEnd = splits[static_cast<std::size_t>(cascadeIndex)];
                ImGui::BulletText(
                    "C%d: texel %.4f m | split end %.2f | ortho %.1f x %.1f m",
                    cascadeIndex,
                    texelSpan,
                    splitEnd,
                    setup.orthoWidth,
                    setup.orthoHeight);
            }

            ImGui::Separator();
            ImGui::TextWrapped(
                "Blockiness usually means texel size is too large for the view. "
                "Enable tight near-plane XY fit, Poisson PCF, and check focus cascade texel size. "
                "Use debug views: Shadow factor (raw map), Cascade index.");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("HDR", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float exposure = screenSpaceEffects.GetExposure();
        if (ImGui::SliderFloat("Exposure (stops)", &exposure, -2.0f, 4.0f))
        {
            screenSpaceEffects.SetExposure(exposure);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        int tonemapMode = static_cast<int>(screenSpaceEffects.GetTonemapMode());
        const char* tonemapModes[] = {"Gamma", "Reinhard", "ACES"};
        if (ImGui::Combo("Tonemap", &tonemapMode, tonemapModes, IM_ARRAYSIZE(tonemapModes)))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Tonemap",
                [tonemapMode](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetTonemapMode(static_cast<TonemapMode>(tonemapMode));
                    target.MarkDirty();
                });
        }

        bool bloomEnabled = screenSpaceEffects.IsBloomEnabled();
        if (ImGui::Checkbox("Bloom", &bloomEnabled))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Bloom",
                [bloomEnabled](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomEnabled(bloomEnabled);
                    target.MarkDirty();
                });
        }

        if (bloomEnabled)
        {
            float bloomThreshold = screenSpaceEffects.GetBloomThreshold();
            if (ImGui::SliderFloat("Bloom threshold", &bloomThreshold, 0.0f, 3.0f))
            {
                screenSpaceEffects.SetBloomThreshold(bloomThreshold);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float bloomSoftKnee = screenSpaceEffects.GetBloomSoftKnee();
            if (ImGui::SliderFloat("Bloom soft knee", &bloomSoftKnee, 0.0f, 1.0f))
            {
                screenSpaceEffects.SetBloomSoftKnee(bloomSoftKnee);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float bloomIntensity = screenSpaceEffects.GetBloomIntensity();
            if (ImGui::SliderFloat("Bloom intensity", &bloomIntensity, 0.0f, 2.0f))
            {
                screenSpaceEffects.SetBloomIntensity(bloomIntensity);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float bloomBlurRadius = screenSpaceEffects.GetBloomBlurRadius();
            if (ImGui::SliderFloat("Bloom blur radius", &bloomBlurRadius, 0.25f, 4.0f))
            {
                screenSpaceEffects.SetBloomBlurRadius(bloomBlurRadius);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
    }

    if (ImGui::CollapsingHeader("Screen Space", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool enabled = screenSpaceEffects.IsEnabled();
        if (ImGui::Checkbox("Enable HDR post-processing", &enabled))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "HDR post-processing",
                [enabled](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetEnabled(enabled);
                    target.MarkDirty();
                });
        }

        bool ssaoEnabled = screenSpaceEffects.IsSsaoEnabled();
        if (ImGui::Checkbox("SSAO", &ssaoEnabled))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "SSAO",
                [ssaoEnabled](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsaoEnabled(ssaoEnabled);
                    target.MarkDirty();
                });
        }

        float ssaoRadius = screenSpaceEffects.GetSsaoRadius();
        if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.05f, 1.0f))
        {
            screenSpaceEffects.SetSsaoRadius(ssaoRadius);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        float ssaoBias = screenSpaceEffects.GetSsaoBias();
        if (ImGui::SliderFloat("SSAO bias", &ssaoBias, 0.0f, 0.1f))
        {
            screenSpaceEffects.SetSsaoBias(ssaoBias);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        float ssaoPower = screenSpaceEffects.GetSsaoPower();
        if (ImGui::SliderFloat("SSAO intensity", &ssaoPower, 0.5f, 4.0f))
        {
            screenSpaceEffects.SetSsaoPower(ssaoPower);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        float aoStrength = screenSpaceEffects.GetAoStrength();
        if (ImGui::SliderFloat("SSAO blend strength", &aoStrength, 0.0f, 1.0f))
        {
            screenSpaceEffects.SetAoStrength(aoStrength);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);
    }

    if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen))
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
            RenderDebugModeLabel(RenderDebugMode::Ssao),
            RenderDebugModeLabel(RenderDebugMode::CompositeOcclusion),
        };

        if (ImGui::Combo(
                "Debug view",
                &debugMode,
                debugModeLabels,
                IM_ARRAYSIZE(debugModeLabels)))
        {
            screenSpaceEffects.SetDebugMode(static_cast<RenderDebugMode>(debugMode));
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
            ImGui::TextWrapped("Shows unshadowed direct lighting (N·L and BRDF), not the shadow factor pass.");
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
            ImGui::TextWrapped("Indirect diffuse from irradiance cubemap (geom normal). Should be smooth on spheres.");
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

        static std::string diagnosticStatus;
        ImGui::TextDisabled("HDR+SSAO on by default; enable Bloom in panel for full post stack.");
        ImGui::TextDisabled("Set GAME_ENGINE_RENDER_DEBUG=1 for HDR/import console logs.");
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

    ImGui::End();
}
