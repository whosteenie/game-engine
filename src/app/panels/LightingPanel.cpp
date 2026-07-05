#include "app/panels/LightingPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/EditorWidgets.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/project/SceneProjectIODetail.h"
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
#include "engine/rhi/GfxContext.h"
#include "engine/assets/FileDialog.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    const char* AntiAliasingModeLabel(AntiAliasingMode mode)
    {
        switch (mode)
        {
        case AntiAliasingMode::FXAA:
            return "FXAA";
        case AntiAliasingMode::TAA:
            return "TAA";
        case AntiAliasingMode::MSAA:
            return "MSAA (not supported)";
        case AntiAliasingMode::SMAA:
            return "SMAA";
        case AntiAliasingMode::SSAA:
            return "SSAA";
        case AntiAliasingMode::None:
        default:
            return "None";
        }
    }

    const char* AmbientOcclusionModeLabel(AmbientOcclusionMode mode)
    {
        switch (mode)
        {
        case AmbientOcclusionMode::SSAO:
            return "SSAO";
        case AmbientOcclusionMode::GTAO:
            return "GTAO";
        case AmbientOcclusionMode::Off:
        default:
            return "Off";
        }
    }

    int IblCubemapResolutionToComboIndex(const EnvironmentIblCubemapResolution resolution)
    {
        switch (resolution)
        {
        case EnvironmentIblCubemapResolution::Size512:
            return 1;
        case EnvironmentIblCubemapResolution::Size1024:
            return 2;
        case EnvironmentIblCubemapResolution::Size2048:
            return 3;
        case EnvironmentIblCubemapResolution::Size4096:
            return 4;
        case EnvironmentIblCubemapResolution::Auto:
        default:
            return 0;
        }
    }

    EnvironmentIblCubemapResolution IblCubemapResolutionFromComboIndex(const int index)
    {
        switch (index)
        {
        case 1:
            return EnvironmentIblCubemapResolution::Size512;
        case 2:
            return EnvironmentIblCubemapResolution::Size1024;
        case 3:
            return EnvironmentIblCubemapResolution::Size2048;
        case 4:
            return EnvironmentIblCubemapResolution::Size4096;
        case 0:
        default:
            return EnvironmentIblCubemapResolution::Auto;
        }
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

    glm::vec3 cameraPosition = camera.GetPosition();
    EditorWidgets::SanitizeSignedZero(cameraPosition);
    ImGui::Text(
        "Camera: (%.1f, %.1f, %.1f)",
        cameraPosition.x,
        cameraPosition.y,
        cameraPosition.z);

    SceneRenderer& renderer = scene.GetRenderer();
    if (renderer.HasPendingRendererSettings())
    {
        ImGui::TextDisabled("Applying project renderer settings...");
        ImGui::End();
        return;
    }

    renderer.PrepareGpuResources();
    if (!renderer.IsGpuResourcesReady())
    {
        ImGui::TextUnformatted("Renderer unavailable:");
        EditorWidgets::DrawErrorText(renderer.GetGpuResourcesInitError());
        ImGui::End();
        return;
    }

    IBL& ibl = renderer.GetIBL();
    EnvironmentMap& environmentMap = renderer.GetEnvironmentMap();
    ScreenSpaceEffects& screenSpaceEffects = renderer.GetScreenSpaceEffects();
    BeginRendererEditFrame(editContext);

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

    if (ImGui::CollapsingHeader("Skybox", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int backgroundMode = static_cast<int>(environmentMap.GetBackgroundMode());
        const char* backgroundModeLabels[] = {"Skybox (HDR)", "Solid color"};
        if (ImGui::Combo("Background", &backgroundMode, backgroundModeLabels, IM_ARRAYSIZE(backgroundModeLabels)))
        {
            environmentMap.SetBackgroundMode(
                static_cast<EnvironmentBackgroundMode>(backgroundMode));
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        const bool skyboxBackground =
            environmentMap.GetBackgroundMode() == EnvironmentBackgroundMode::Skybox;
        if (!skyboxBackground)
        {
            glm::vec3 solidColor = environmentMap.GetSolidBackgroundColorSrgb();
            if (ImGui::ColorEdit3("Background color", glm::value_ptr(solidColor)))
            {
                environmentMap.SetSolidBackgroundColorSrgb(solidColor);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }

        const std::string& skyboxPath = environmentMap.GetHdrPath();
        const std::string activeHdrLabel = skyboxPath.empty()
            ? "none"
            : std::filesystem::path(skyboxPath).filename().string();
        ImGui::Text("Active HDR: %s", activeHdrLabel.c_str());

        int environmentPreset = EnvironmentPresets::FindPresetIndex(skyboxPath);

        std::vector<const char*> presetLabels;
        presetLabels.reserve(EnvironmentPresets::kCount);
        for (std::size_t index = 0; index < EnvironmentPresets::kCount; ++index)
        {
            presetLabels.push_back(EnvironmentPresets::kEntries[index].label);
        }

        if (ImGui::Combo(
                "Environment preset",
                &environmentPreset,
                presetLabels.data(),
                static_cast<int>(presetLabels.size())))
        {
            if (environmentPreset > 0
                && static_cast<std::size_t>(environmentPreset) < EnvironmentPresets::kCount)
            {
                environmentMap.SetHdrPath(EnvironmentPresets::kEntries[environmentPreset].path);
                scene.MarkDirty();
            }
        }
        HandleRendererFieldEditEvents(editContext);

        static char skyboxPathBuffer[512] = {};
        if (skyboxPath.size() < sizeof(skyboxPathBuffer))
        {
            std::strncpy(skyboxPathBuffer, skyboxPath.c_str(), sizeof(skyboxPathBuffer) - 1);
        }

        ImGui::InputText("HDR path", skyboxPathBuffer, sizeof(skyboxPathBuffer));
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            environmentMap.SetHdrPath(skyboxPathBuffer);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        ImGui::SameLine();
        if (ImGui::Button("Browse##SkyboxHdr"))
        {
            std::string selectedPath;
            if (FileDialog::OpenImageFile(selectedPath))
            {
                environmentMap.SetHdrPath(selectedPath);
                scene.MarkDirty();
            }
        }

        float skyboxRotation = environmentMap.GetRotationDegrees();
        if (ImGui::SliderFloat("Rotation Y (deg)", &skyboxRotation, 0.0f, 360.0f))
        {
            environmentMap.SetRotationDegrees(skyboxRotation);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        float skyboxExposure = environmentMap.GetExposure();
        if (ImGui::SliderFloat("Skybox exposure", &skyboxExposure, 0.1f, 4.0f))
        {
            environmentMap.SetExposure(skyboxExposure);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        int iblCubemapResolutionIndex =
            IblCubemapResolutionToComboIndex(environmentMap.GetIblCubemapResolution());
        const char* iblCubemapResolutionLabels[] = {
            "Auto (match HDR)",
            "512",
            "1024",
            "2048",
            "4096",
        };
        if (ImGui::Combo(
                "IBL cubemap resolution",
                &iblCubemapResolutionIndex,
                iblCubemapResolutionLabels,
                IM_ARRAYSIZE(iblCubemapResolutionLabels)))
        {
            environmentMap.SetIblCubemapResolution(
                IblCubemapResolutionFromComboIndex(iblCubemapResolutionIndex));
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);
        ImGui::TextDisabled(
            "Sky background uses the HDR file at full resolution. IBL cubemap resolution "
            "affects reflections only.");

        if (environmentMap.IsLoaded())
        {
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "Status: loaded");
            const IBL& ibl = environmentMap.GetIBL();
            int hdrWidth = 0;
            int hdrHeight = 0;
            ibl.GetHdrDimensions(hdrWidth, hdrHeight);
            if (hdrWidth > 0 && hdrHeight > 0)
            {
                ImGui::Text("HDR resolution: %d x %d", hdrWidth, hdrHeight);
                if (hdrWidth <= 1024 && hdrHeight <= 512)
                {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                        "Low-res 1K asset — re-pick preset or use a 2K/4K HDR for sharper sky.");
                }
            }

            ImGui::Text("IBL cubemap face size: %u px", ibl.GetCubemapFaceResolution());
        }
        else if (!environmentMap.GetLoadError().empty())
        {
            EditorWidgets::TextColoredError("Status: failed");
            ImGui::TextWrapped("%s", environmentMap.GetLoadError().c_str());
        }
        else
        {
            ImGui::TextDisabled("Status: pending");
        }

        ImGui::TextDisabled(
            "For star fields and fine cloud detail, use 2K or 4K HDR files from Poly Haven.");
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
            ImGui::TextDisabled("Per-pixel rotated grid; smoother than axis-aligned, no stochastic grain.");

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
            ImGui::TextDisabled(
                "Single shadow map (CSM off). Ortho covers scene casters plus the visible view frustum.");

            bool tightNearPlaneFit = shadowSettings.GetTightNearPlaneXyFit();
            if (ImGui::Checkbox("Tight near-plane XY fit", &tightNearPlaneFit))
            {
                shadowSettings.SetTightNearPlaneXyFit(tightNearPlaneFit);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled("Fit ortho XY to the view frustum; caster bounds still expand Z depth.");

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
            ImGui::TextDisabled(
                "Receiver scales: raise if acne, lower if shadows detach. "
                "Caster scale adds optional front-face shader bias; 0 relies on slope bias only. "
                "Shadow pass uses front-face culling (back faces) for contact depth.");

            ImGui::TreePop();
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

                ImGui::TextDisabled(
                    "Magenta in light-space depth debug = clip Z outside [0, 1]. "
                    "UV outside [0, 1] = shadow map coverage.");
                ImGui::TreePop();
            }

            ImGui::Separator();
            ImGui::TextWrapped(
                "Large texels or blocky shadows: raise resolution or lower ortho margin. "
                "Debug: Shadow factor (1), Shadow blocked (22).");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("HDR", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float exposure = screenSpaceEffects.GetExposure();
        UndoableRendererSliderFloat(
            "Exposure (stops)",
            &exposure,
            -2.0f,
            4.0f,
            "%.3f",
            editContext,
            [](Scene& target, float exposure) {
                target.GetRenderer().GetScreenSpaceEffects().SetExposure(exposure);
                target.MarkDirty();
            });

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
            UndoableRendererSliderFloat(
                "Bloom threshold",
                &bloomThreshold,
                0.0f,
                3.0f,
                "%.3f",
                editContext,
                [](Scene& target, float bloomThreshold) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomThreshold(bloomThreshold);
                    target.MarkDirty();
                });

            float bloomSoftKnee = screenSpaceEffects.GetBloomSoftKnee();
            UndoableRendererSliderFloat(
                "Bloom soft knee",
                &bloomSoftKnee,
                0.0f,
                1.0f,
                "%.3f",
                editContext,
                [](Scene& target, float bloomSoftKnee) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomSoftKnee(bloomSoftKnee);
                    target.MarkDirty();
                });

            float bloomIntensity = screenSpaceEffects.GetBloomIntensity();
            UndoableRendererSliderFloat(
                "Bloom intensity",
                &bloomIntensity,
                0.0f,
                2.0f,
                "%.3f",
                editContext,
                [](Scene& target, float bloomIntensity) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomIntensity(bloomIntensity);
                    target.MarkDirty();
                });

            float bloomBlurRadius = screenSpaceEffects.GetBloomBlurRadius();
            UndoableRendererSliderFloat(
                "Bloom blur radius",
                &bloomBlurRadius,
                0.25f,
                4.0f,
                "%.3f",
                editContext,
                [](Scene& target, float bloomBlurRadius) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomBlurRadius(bloomBlurRadius);
                    target.MarkDirty();
                });
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
                    ApplyRendererChange(
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

        if (aoMode == AmbientOcclusionMode::SSAO)
        {
            float ssaoRadius = screenSpaceEffects.GetSsaoRadius();
            if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.1f, 1.5f))
            {
                screenSpaceEffects.SetSsaoRadius(ssaoRadius);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled("Radius is view-space units (fixed, not scaled by depth).");

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

            float gtaoFalloff = screenSpaceEffects.GetGtaoFalloff();
            if (ImGui::SliderFloat("GTAO falloff", &gtaoFalloff, 0.25f, 6.0f))
            {
                screenSpaceEffects.SetGtaoFalloff(gtaoFalloff);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

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

            int gtaoSteps = screenSpaceEffects.GetGtaoSteps();
            if (ImGui::SliderInt("GTAO steps", &gtaoSteps, 2, 12))
            {
                screenSpaceEffects.SetGtaoSteps(gtaoSteps);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

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
        ImGui::TextDisabled("Edge-aware blur: lower = sharper AO edges across depth discontinuities.");

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
            ImGui::TextDisabled(
                "Shader debug shows raw SSAO target (unblurred). Set debug view to SSAO buffer.");
        }
        ImGui::TextDisabled(
            "Intensity = pow() on AO in composite (indirect only). Blend = how much AO affects indirect.");
        ImGui::TextDisabled(
            "Use AO buffer/debug views for raw or filtered factors; Composite occlusion shows the final factor.");

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
                    "normalSrv is null — geometry-normal G-buffer missing");
            }
            if (ssaoDiag.shadowFactorSrv != 0 && ssaoDiag.normalSrv == ssaoDiag.shadowFactorSrv)
            {
                EditorWidgets::TextColoredError(
                    "normalSrv == shadowFactorSrv — bindings swapped?");
            }
            ImGui::TextDisabled("Toggle SSAO with GAME_ENGINE_RENDER_DEBUG=1 for stderr snapshot.");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Anti-aliasing", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!screenSpaceEffects.IsEnabled())
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                "Enable HDR post-processing (Screen Space section) for FXAA.");
        }

        const RenderDebugMode debugMode = screenSpaceEffects.GetDebugMode();
        if (debugMode != RenderDebugMode::None)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                "FXAA only affects the final image. Set Diagnostics > Debug view to None.");
            ImGui::TextDisabled("Active debug view: %s", RenderDebugModeLabel(debugMode));
        }

        const AntiAliasingMode currentAaMode = screenSpaceEffects.GetAntiAliasingMode();
        const int msaaSampleCount = screenSpaceEffects.GetMsaaSampleCount();
        const int activeMsaaSampleCount = GfxContext::Get().GetActiveMsaaSampleCount();
        const bool geometryMsaaBlocksTaa =
            msaaSampleCount > 1 || activeMsaaSampleCount > 1;
        const bool taaBlocksGeometryMsaa = currentAaMode == AntiAliasingMode::TAA;

        if (ImGui::BeginCombo("Mode", AntiAliasingModeLabel(currentAaMode)))
        {
            const AntiAliasingMode selectableModes[] = {
                AntiAliasingMode::None,
                AntiAliasingMode::FXAA,
                AntiAliasingMode::SMAA,
                AntiAliasingMode::TAA,
                AntiAliasingMode::SSAA,
            };
            for (const AntiAliasingMode mode : selectableModes)
            {
                const bool disabled =
                    geometryMsaaBlocksTaa && mode == AntiAliasingMode::TAA;
                if (disabled)
                {
                    ImGui::BeginDisabled();
                }

                const bool selected = currentAaMode == mode;
                if (ImGui::Selectable(AntiAliasingModeLabel(mode), selected) && !selected && !disabled)
                {
                    ApplyRendererChange(
                        editContext,
                        scene,
                        "Anti-aliasing",
                        [mode](Scene& target) {
                            target.GetRenderer().GetScreenSpaceEffects().SetAntiAliasingMode(mode);
                            target.MarkDirty();
                        });
                    ImGui::CloseCurrentPopup();
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                if (disabled)
                {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("Unavailable while geometry MSAA is enabled.");
                    }
                }
            }

            ImGui::EndCombo();
        }

        ImGui::Separator();
        if (geometryMsaaBlocksTaa || taaBlocksGeometryMsaa)
        {
            ImGui::TextDisabled(
                "Geometry MSAA above 1× and TAA are mutually exclusive. "
                "Incompatible options are grayed out; choosing MSAA while TAA is active switches post AA to None.");
        }
        else
        {
            ImGui::TextDisabled(
                "Geometry MSAA supersamples the scene pass before post AA (FXAA, TAA, etc.). "
                "Pick 1× for standard single-sample rendering.");
        }

        const char* msaaPreview = "1× (None)";
        if (msaaSampleCount == 2)
        {
            msaaPreview = "2× MSAA";
        }
        else if (msaaSampleCount == 4)
        {
            msaaPreview = "4× MSAA";
        }
        else if (msaaSampleCount == 8)
        {
            msaaPreview = "8× MSAA";
        }

        static constexpr struct MsaaPreset
        {
            int count;
            const char* label;
        } kMsaaPresets[] = {
            {1, "1× (None)"},
            {2, "2× MSAA"},
            {4, "4× MSAA"},
            {8, "8× MSAA"},
        };

        if (ImGui::BeginCombo("Geometry MSAA", msaaPreview))
        {
            for (const MsaaPreset& preset : kMsaaPresets)
            {
                const bool supported = GfxContext::Get().IsMsaaSampleCountSupported(preset.count);
                const bool blockedByTaa = taaBlocksGeometryMsaa && preset.count > 1;
                const bool disabled = !supported || blockedByTaa;
                if (disabled)
                {
                    ImGui::BeginDisabled();
                }

                const bool selected = msaaSampleCount == preset.count;
                if (ImGui::Selectable(preset.label, selected) && !selected && !disabled)
                {
                    ApplyRendererChange(
                        editContext,
                        scene,
                        "Geometry MSAA",
                        [preset](Scene& target) {
                            target.GetRenderer().GetScreenSpaceEffects().SetMsaaSampleCount(preset.count);
                            target.MarkDirty();
                        });
                    ImGui::CloseCurrentPopup();
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }

                if (disabled)
                {
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        if (!supported)
                        {
                            ImGui::SetTooltip("Not supported on this GPU for the scene G-buffer formats.");
                        }
                        else if (blockedByTaa)
                        {
                            ImGui::SetTooltip(
                                "Unavailable while post AA mode is TAA. "
                                "Switch post AA to None or another mode first.");
                        }
                    }
                }
            }
            ImGui::EndCombo();
        }

        static std::string msaaReloadStatus;
        if (screenSpaceEffects.IsMsaaPendingReload())
        {
            if (ImGui::Button("Apply geometry MSAA"))
            {
                msaaReloadStatus.clear();
                if (!renderer.ApplyGeometryMsaaReload(scene, viewportWidth, viewportHeight, &msaaReloadStatus))
                {
                    if (msaaReloadStatus.empty())
                    {
                        msaaReloadStatus = "Failed to reload renderer for geometry MSAA.";
                    }
                }
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Recreates the scene renderer pipeline (framebuffer, shaders, post targets) "
                    "for the selected MSAA sample count.");
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Reload required");
        }
        else if (msaaSampleCount > 1)
        {
            msaaReloadStatus.clear();
            ImGui::TextDisabled("Geometry MSAA active: %d×", msaaSampleCount);
        }

        if (!msaaReloadStatus.empty())
        {
            ImGui::TextWrapped("%s", msaaReloadStatus.c_str());
        }

        if (currentAaMode == AntiAliasingMode::FXAA)
        {
            ImGui::TextDisabled("Tonemap -> FXAA -> viewport");
            float fxaaSubpix = screenSpaceEffects.GetFxaaSubpixQuality();
            if (ImGui::SliderFloat("FXAA subpixel quality", &fxaaSubpix, 0.0f, 1.0f))
            {
                screenSpaceEffects.SetFxaaSubpixQuality(fxaaSubpix);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            float fxaaEdge = screenSpaceEffects.GetFxaaEdgeThreshold();
            if (ImGui::SliderFloat("FXAA edge threshold", &fxaaEdge, 0.03125f, 0.5f))
            {
                screenSpaceEffects.SetFxaaEdgeThreshold(fxaaEdge);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (currentAaMode == AntiAliasingMode::SMAA)
        {
            ImGui::TextDisabled("Tonemap -> SMAA edge -> SMAA blend -> viewport");
            float smaaThreshold = screenSpaceEffects.GetSmaaThreshold();
            if (ImGui::SliderFloat("SMAA edge threshold", &smaaThreshold, 0.01f, 0.25f))
            {
                screenSpaceEffects.SetSmaaThreshold(smaaThreshold);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);

            int smaaSteps = screenSpaceEffects.GetSmaaSearchSteps();
            if (ImGui::SliderInt("SMAA search steps", &smaaSteps, 1, 8))
            {
                screenSpaceEffects.SetSmaaSearchSteps(smaaSteps);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (currentAaMode == AntiAliasingMode::TAA)
        {
            ImGui::TextDisabled(
                "HDR temporal resolve (motion vectors) -> bloom -> tonemap -> viewport");
            float taaBlend = screenSpaceEffects.GetTaaBlendFactor();
            if (ImGui::SliderFloat("TAA history blend", &taaBlend, 0.0f, 0.99f))
            {
                screenSpaceEffects.SetTaaBlendFactor(taaBlend);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }
        else if (currentAaMode == AntiAliasingMode::SSAA)
        {
            ImGui::TextDisabled("Supersampled scene -> tonemap -> downsample -> viewport");
            float renderScale = screenSpaceEffects.GetRenderScale();
            if (ImGui::SliderFloat("Render scale", &renderScale, 1.0f, 2.0f, "%.2fx"))
            {
                screenSpaceEffects.SetRenderScale(renderScale);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
            ImGui::TextDisabled(
                "Internal render: %dx%d",
                std::max(1, static_cast<int>(std::lround(static_cast<float>(viewportWidth) * renderScale))),
                std::max(1, static_cast<int>(std::lround(static_cast<float>(viewportHeight) * renderScale))));
        }
    }

    if (ImGui::CollapsingHeader("Texture filtering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Material textures upload with full mip chains.");

        int textureFilterMode = static_cast<int>(renderer.GetTextureFilterMode());
        const char* filterLabels[] = {"Trilinear", "Bilinear", "Nearest"};
        if (ImGui::Combo("Material sampling", &textureFilterMode, filterLabels, IM_ARRAYSIZE(filterLabels)))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Texture filter",
                [textureFilterMode](Scene& target) {
                    target.GetRenderer().SetTextureFilterMode(
                        static_cast<TextureFilterMode>(textureFilterMode));
                    target.MarkDirty();
                });
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Updates GfxContext for new shaders. Existing PBR shaders keep their baked samplers until restart.");
        }

        int anisotropy = static_cast<int>(renderer.GetTextureAnisotropy());
        if (ImGui::SliderInt("Anisotropic filtering", &anisotropy, 1, 16))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Texture anisotropy",
                [anisotropy](Scene& target) {
                    target.GetRenderer().SetTextureAnisotropy(static_cast<std::uint32_t>(anisotropy));
                    target.MarkDirty();
                });
        }
        HandleRendererFieldEditEvents(editContext);

        float mipBias = renderer.GetTextureMipBias();
        if (ImGui::SliderFloat("Mip bias", &mipBias, -2.0f, 2.0f))
        {
            ApplyRendererChange(
                editContext,
                scene,
                "Texture mip bias",
                [mipBias](Scene& target) {
                    target.GetRenderer().SetTextureMipBias(mipBias);
                    target.MarkDirty();
                });
        }
        HandleRendererFieldEditEvents(editContext);
    }

    if (ImGui::CollapsingHeader("Screen-space GI (SSGI)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "Screen-space indirect from emissive / radiance buffer. "
            "Use Diagnostics debug views for pipeline isolation.");

        if (ImGui::TreeNode("GI temporal"))
        {
            float giBlend = screenSpaceEffects.GetGiTemporalBlendFactor();
            UndoableRendererSliderFloat(
                "GI history blend",
                &giBlend,
                0.0f,
                0.99f,
                "%.3f",
                editContext,
                [](Scene& target, float giBlend) {
                    target.GetRenderer().GetScreenSpaceEffects().SetGiTemporalBlendFactor(giBlend);
                    target.MarkDirty();
                });
            ImGui::TextDisabled(
                "Independent of HDR TAA. Validate with Anti-aliasing = None.");

            float giDepthThreshold = screenSpaceEffects.GetGiDepthThreshold();
            UndoableRendererSliderFloat(
                "GI depth reject threshold",
                &giDepthThreshold,
                0.0005f,
                0.05f,
                "%.4f",
                editContext,
                [](Scene& target, float giDepthThreshold) {
                    target.GetRenderer().GetScreenSpaceEffects().SetGiDepthThreshold(giDepthThreshold);
                    target.MarkDirty();
                });
            ImGui::TextDisabled(
                "Lower rejects more history at geometry changes; higher keeps more accumulation.");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Denoise"))
        {
            bool denoiseEnabled = screenSpaceEffects.IsSsgiDenoiseEnabled();
            UndoableRendererCheckbox(
                "Enable spatial + temporal denoise",
                &denoiseEnabled,
                editContext,
                [](Scene& target, bool denoiseEnabled) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiDenoiseEnabled(denoiseEnabled);
                    target.MarkDirty();
                });
            bool noiseEnabled = screenSpaceEffects.IsSsgiNoiseInjectionEnabled();
            UndoableRendererCheckbox(
                "Synthetic trace noise (test)",
                &noiseEnabled,
                editContext,
                [](Scene& target, bool noiseEnabled) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiNoiseInjectionEnabled(noiseEnabled);
                    target.MarkDirty();
                });
            float noiseStrength = screenSpaceEffects.GetSsgiNoiseStrength();
            UndoableRendererSliderFloat(
                "Noise strength",
                &noiseStrength,
                0.0f,
                0.5f,
                "%.3f",
                editContext,
                [](Scene& target, float noiseStrength) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiNoiseStrength(noiseStrength);
                    target.MarkDirty();
                });
            float blurSpread = screenSpaceEffects.GetSsgiSpatialBlurSpread();
            UndoableRendererSliderFloat(
                "Spatial blur spread",
                &blurSpread,
                0.25f,
                4.0f,
                "%.3f",
                editContext,
                [](Scene& target, float blurSpread) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiSpatialBlurSpread(blurSpread);
                    target.MarkDirty();
                });
            float spatialDepth = screenSpaceEffects.GetSsgiSpatialDepthThreshold();
            UndoableRendererSliderFloat(
                "Spatial depth threshold",
                &spatialDepth,
                0.001f,
                0.1f,
                "%.3f",
                editContext,
                [](Scene& target, float spatialDepth) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiSpatialDepthThreshold(spatialDepth);
                    target.MarkDirty();
                });
            ImGui::TextDisabled(
                "Optional noise → spatial → temporal. Disable synthetic noise for real trace.");
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Trace & inject"))
        {
            bool ssgiEnabled = screenSpaceEffects.IsSsgiEnabled();
            UndoableRendererCheckbox(
                "Enable SSGI",
                &ssgiEnabled,
                editContext,
                [](Scene& target, bool ssgiEnabled) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiEnabled(ssgiEnabled);
                    target.MarkDirty();
                });
            float ssgiStrength = screenSpaceEffects.GetSsgiStrength();
            UndoableRendererSliderFloat(
                "SSGI strength",
                &ssgiStrength,
                0.0f,
                1.5f,
                "%.3f",
                editContext,
                [](Scene& target, float ssgiStrength) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiStrength(ssgiStrength);
                    target.MarkDirty();
                });
            float traceDistance = screenSpaceEffects.GetSsgiMaxTraceDistance();
            UndoableRendererSliderFloat(
                "Max trace distance",
                &traceDistance,
                0.5f,
                10.0f,
                "%.1f m",
                editContext,
                [](Scene& target, float traceDistance) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiMaxTraceDistance(traceDistance);
                    target.MarkDirty();
                });
            int stepCount = screenSpaceEffects.GetSsgiStepCount();
            UndoableRendererSliderInt(
                "Trace steps",
                &stepCount,
                4,
                32,
                editContext,
                [](Scene& target, int stepCount) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiStepCount(stepCount);
                    target.MarkDirty();
                });
            ImGui::TextDisabled(
                "Trace → denoise → inject into indirect before SSAO. AA = None recommended for tuning.");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Screen-space reflections (SSR)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "Specular screen-space trace from Phase S1 scene-color buffer. "
            "Use Diagnostics debug views for pipeline isolation.");

        bool ssrEnabled = screenSpaceEffects.IsSsrEnabled();
        UndoableRendererCheckbox(
            "Enable SSR",
            &ssrEnabled,
            editContext,
            [](Scene& target, bool ssrEnabled) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrEnabled(ssrEnabled);
                target.MarkDirty();
            });

        float ssrMaxDistance = screenSpaceEffects.GetSsrMaxTraceDistance();
        UndoableRendererSliderFloat(
            "Max trace distance",
            &ssrMaxDistance,
            1.0f,
            50.0f,
            "%.1f m",
            editContext,
            [](Scene& target, float ssrMaxDistance) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrMaxTraceDistance(ssrMaxDistance);
                target.MarkDirty();
            });

        int ssrStepCount = screenSpaceEffects.GetSsrStepCount();
        UndoableRendererSliderInt(
            "Trace steps",
            &ssrStepCount,
            4,
            64,
            editContext,
            [](Scene& target, int ssrStepCount) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrStepCount(ssrStepCount);
                target.MarkDirty();
            });

        int ssrSampleCount = screenSpaceEffects.GetSsrSampleCount();
        UndoableRendererSliderInt(
            "Trace samples",
            &ssrSampleCount,
            1,
            8,
            editContext,
            [](Scene& target, int ssrSampleCount) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrSampleCount(ssrSampleCount);
                target.MarkDirty();
            });
        ImGui::TextDisabled(
            "Samples = jittered reflection rays averaged per pixel (2 recommended). "
            "Costs linearly; reduces speckle without extra blur.");

        float ssrThickness = screenSpaceEffects.GetSsrThickness();
        UndoableRendererSliderFloat(
            "Thickness",
            &ssrThickness,
            0.05f,
            2.0f,
            "%.2f",
            editContext,
            [](Scene& target, float ssrThickness) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrThickness(ssrThickness);
                target.MarkDirty();
            });

        float ssrRoughnessCutoff = screenSpaceEffects.GetSsrRoughnessCutoff();
        UndoableRendererSliderFloat(
            "Roughness cutoff",
            &ssrRoughnessCutoff,
            0.1f,
            1.0f,
            "%.2f",
            editContext,
            [](Scene& target, float ssrRoughnessCutoff) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrRoughnessCutoff(ssrRoughnessCutoff);
                target.MarkDirty();
            });
        ImGui::TextDisabled(
            "Surfaces rougher than cutoff skip trace (IBL fallback in composite). "
            "Trace also runs when an SSR debug view is selected.");

        ImGui::TextUnformatted(
            "Multi-sample trace averages hits only (RGB); alpha is mean confidence. "
            "More samples lowers alpha on misses but should not dim denoise RGB.");

        float ssrStrength = screenSpaceEffects.GetSsrStrength();
        UndoableRendererSliderFloat(
            "SSR strength",
            &ssrStrength,
            0.0f,
            2.0f,
            "%.2f",
            editContext,
            [](Scene& target, float ssrStrength) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrStrength(ssrStrength);
                target.MarkDirty();
            });

        bool ssrDenoiseEnabled = screenSpaceEffects.IsSsrDenoiseEnabled();
        UndoableRendererCheckbox(
            "Enable denoise",
            &ssrDenoiseEnabled,
            editContext,
            [](Scene& target, bool ssrDenoiseEnabled) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrDenoiseEnabled(ssrDenoiseEnabled);
                target.MarkDirty();
            });

        ImGui::TextDisabled(
            "Quadratic steps + jitter. Spatial filters speckle only; temporal uses edge-aware blend (~0.9).");

        float ssrTemporalBlend = screenSpaceEffects.GetSsrTemporalBlendFactor();
        UndoableRendererSliderFloat(
            "Temporal blend",
            &ssrTemporalBlend,
            0.0f,
            0.99f,
            "%.2f",
            editContext,
            [](Scene& target, float ssrTemporalBlend) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrTemporalBlendFactor(ssrTemporalBlend);
                target.MarkDirty();
            });
        ImGui::TextDisabled(
            "SVGF denoise: temporal variance accumulation + 4-pass à-trous. Enable SSR and set debug to None for composite.");

        const bool sceneColorRan = screenSpaceEffects.GetSsrSceneColorRanLastFrame();
        const bool traceRan = screenSpaceEffects.GetSsrTraceRanLastFrame();
        const bool denoiseRan = screenSpaceEffects.GetSsrDenoiseRanLastFrame();
        const bool temporalRan = screenSpaceEffects.GetSsrTemporalRanLastFrame();
        ImGui::Text(
            "Status: scene %s | trace %s | denoise %s | temporal %s | %dx%d",
            sceneColorRan ? "ran" : "skip",
            traceRan ? "ran" : "skip",
            denoiseRan ? "ran" : "skip",
            temporalRan ? "ran" : "skip",
            screenSpaceEffects.GetSsrTraceTargetWidth(),
            screenSpaceEffects.GetSsrTraceTargetHeight());
    }

    if (ImGui::CollapsingHeader("Ray tracing"))
    {
        const GfxContext& gfx = GfxContext::Get();
        const bool raytracingSupported = gfx.IsInitialized() && gfx.IsRaytracingSupported();
        const int raytracingTier = gfx.IsInitialized() ? gfx.GetRaytracingTier() : 0;
        const std::string& adapterName =
            gfx.IsInitialized() ? gfx.GetAdapterDescription() : std::string("(GPU not initialized)");

        ImGui::Text("Adapter: %s", adapterName.c_str());
        char tierText[64]{};
        FormatRaytracingTierText(raytracingTier, tierText, sizeof(tierText));
        ImGui::Text("Ray tracing tier: %s", tierText);

        if (!raytracingSupported)
        {
            ImGui::TextColored(
                ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                "Ray tracing requires a DXR Tier 1.0+ GPU and up-to-date driver.");
            if (raytracingTier == 0 && gfx.IsInitialized())
            {
                ImGui::TextDisabled(
                    "Update your graphics driver (NVIDIA 531+ class recommended) if you expect RTX support.");
            }
        }

        DxrSettings& dxrSettings = renderer.GetDxrSettings();

        if (!raytracingSupported)
        {
            ImGui::BeginDisabled();
        }

        ImGui::PushID("RayTracing");
        bool dxrEnabled = dxrSettings.IsEnabled();
        UndoableRendererCheckbox(
            "Enable ray tracing",
            &dxrEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                EngineLog::Breadcrumb(
                    "dxr",
                    std::string("editor: Enable ray tracing -> ") + (enabled ? "on" : "off"));
                if (enabled)
                {
                    ResetDxrBreadcrumbOnceFlags();
                }
                target.GetRenderer().GetDxrSettings().SetEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.GetRenderer().GetScreenSpaceEffects().InvalidateSsrHistory();
                target.MarkDirty();
            });

        bool debugTraceEnabled = dxrSettings.IsDebugTraceEnabled();
        UndoableRendererCheckbox(
            "Enable RT debug trace",
            &debugTraceEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetDebugTraceEnabled(enabled);
                if (enabled && !GfxContext::Get().IsFrameRecording())
                {
                    target.GetRenderer().WarmUpDxrPipelineIfNeeded();
                }
                target.MarkDirty();
            });

        bool reflectionsEnabled = dxrSettings.IsReflectionsEnabled();
        UndoableRendererCheckbox(
            "Enable RT reflections",
            &reflectionsEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetReflectionsEnabled(enabled);
                target.MarkDirty();
            });

        int qualityIndex = static_cast<int>(dxrSettings.GetReflectionsQuality());
        const char* qualityLabels[] = {"Low", "Medium", "High"};
        if (ImGui::Combo("Reflections quality", &qualityIndex, qualityLabels, IM_ARRAYSIZE(qualityLabels)))
        {
            const auto quality = static_cast<DxrReflectionsQuality>(qualityIndex);
            ApplyRendererChange(
                editContext,
                scene,
                "RT reflections quality",
                [quality](Scene& target) {
                    target.GetRenderer().GetDxrSettings().SetReflectionsQuality(quality);
                    target.MarkDirty();
                });
        }

        int samplesPerPixel = dxrSettings.GetReflectionsSamplesPerPixel();
        UndoableRendererSliderInt(
            "Reflection samples / pixel",
            &samplesPerPixel,
            1,
            16,
            editContext,
            [](Scene& target, int samples) {
                target.GetRenderer().GetDxrSettings().SetReflectionsSamplesPerPixel(samples);
                target.MarkDirty();
            });

        float maxTraceDistance = dxrSettings.GetMaxTraceDistance();
        UndoableRendererSliderFloat(
            "Max trace distance",
            &maxTraceDistance,
            1.0f,
            500.0f,
            "%.1f m",
            editContext,
            [](Scene& target, float distance) {
                target.GetRenderer().GetDxrSettings().SetMaxTraceDistance(distance);
                target.MarkDirty();
            });

        bool denoiseEnabled = dxrSettings.IsDenoiseEnabled();
        UndoableRendererCheckbox(
            "Denoise enabled",
            &denoiseEnabled,
            editContext,
            [](Scene& target, bool enabled) {
                target.GetRenderer().GetDxrSettings().SetDenoiseEnabled(enabled);
                target.MarkDirty();
            });

        float temporalBlend = dxrSettings.GetTemporalBlend();
        UndoableRendererSliderFloat(
            "Temporal blend",
            &temporalBlend,
            0.0f,
            0.99f,
            "%.2f",
            editContext,
            [](Scene& target, float blend) {
                target.GetRenderer().GetDxrSettings().SetTemporalBlend(blend);
                target.MarkDirty();
            });

        ImGui::PopID();

        const DxrDiagnostics& dxrDiagnostics = renderer.GetDxrDiagnostics();
        ImGui::Separator();
        ImGui::Text("BLAS count: %u", dxrDiagnostics.blasCount);
        ImGui::Text("TLAS instances: %u", dxrDiagnostics.tlasInstanceCount);
        ImGui::Text("RT triangles (unique): %llu", static_cast<unsigned long long>(dxrDiagnostics.totalRtTriangles));
        const double asMemoryMb =
            static_cast<double>(dxrDiagnostics.asGpuMemoryBytes) / (1024.0 * 1024.0);
        if (asMemoryMb >= 1.0)
        {
            ImGui::Text("AS GPU memory: %.2f MB", asMemoryMb);
        }
        else
        {
            ImGui::Text(
                "AS GPU memory: %.1f KB",
                static_cast<double>(dxrDiagnostics.asGpuMemoryBytes) / 1024.0);
        }
        ImGui::Text("Last build status: %s", dxrDiagnostics.buildStatus.c_str());
        ImGui::Text("Last build time: %.3f ms", dxrDiagnostics.lastBuildTimeMs);

        if (!raytracingSupported)
        {
            ImGui::EndDisabled();
        }

        ImGui::TextDisabled(
            "Acceleration structures build when ray tracing is enabled. DispatchRays arrives in a later DXR phase.");
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
        };

        if (ImGui::Combo(
                "Debug view",
                &debugMode,
                debugModeLabels,
                IM_ARRAYSIZE(debugModeLabels)))
        {
            const auto selectedMode = static_cast<RenderDebugMode>(debugMode);
            screenSpaceEffects.SetDebugMode(selectedMode);
            if ((IsRtPrimaryDebugMode(selectedMode) || IsRtReflectionDebugMode(selectedMode))
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
                "Per-pixel roughness in [0.04, 1]. White = fully rough; black = smoothest allowed value.");
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

    ImGui::End();
}
