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
#include "engine/lighting/ShadowMapMath.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/TextureSamplerSettings.h"

#include <imgui.h>

#include <glm/glm.hpp>

#include <functional>

namespace
{
    const char* AntiAliasingModeLabel(AntiAliasingMode mode)
    {
        switch (mode)
        {
        case AntiAliasingMode::FXAA:
            return "FXAA";
        case AntiAliasingMode::TAA:
            return "TAA (coming soon)";
        case AntiAliasingMode::MSAA:
            return "MSAA (coming soon)";
        case AntiAliasingMode::None:
        default:
            return "None";
        }
    }

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
    renderer.PrepareGpuResources();
    if (!renderer.IsGpuResourcesReady() || renderer.HasGpuResourcesInitFailed())
    {
        ImGui::TextUnformatted("Renderer unavailable:");
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("%s", renderer.GetGpuResourcesInitError().c_str());
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }

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

        float ssaoBlurDepthThreshold = screenSpaceEffects.GetSsaoBlurDepthThreshold();
        if (ImGui::SliderFloat("SSAO blur depth threshold", &ssaoBlurDepthThreshold, 0.001f, 0.25f))
        {
            screenSpaceEffects.SetSsaoBlurDepthThreshold(ssaoBlurDepthThreshold);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);
        ImGui::TextDisabled("Edge-aware blur: lower = sharper AO edges across depth discontinuities.");

        int ssaoShaderDebug = screenSpaceEffects.GetSsaoShaderDebugMode();
        if (ImGui::Combo(
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
            "Radius/bias affect the SSAO pass; try Composite occlusion debug view for final factor.");

        float aoStrength = screenSpaceEffects.GetAoStrength();
        if (ImGui::SliderFloat("SSAO blend strength", &aoStrength, 0.0f, 1.0f))
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
                ImGui::TextColored(
                    ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                    "normalSrv is null — geometry-normal G-buffer missing");
            }
            if (ssaoDiag.shadowFactorSrv != 0 && ssaoDiag.normalSrv == ssaoDiag.shadowFactorSrv)
            {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
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
        if (ImGui::BeginCombo("Mode", AntiAliasingModeLabel(currentAaMode)))
        {
            const AntiAliasingMode selectableModes[] = {
                AntiAliasingMode::None,
                AntiAliasingMode::FXAA,
            };
            for (const AntiAliasingMode mode : selectableModes)
            {
                const bool selected = currentAaMode == mode;
                if (ImGui::Selectable(AntiAliasingModeLabel(mode), selected) && !selected)
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
            }

            ImGui::BeginDisabled();
            ImGui::Selectable(AntiAliasingModeLabel(AntiAliasingMode::TAA), false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip("Temporal AA is not implemented yet.");
            }
            ImGui::Selectable(AntiAliasingModeLabel(AntiAliasingMode::MSAA), false);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            {
                ImGui::SetTooltip(
                    "MSAA requires swapchain/geometry sample-count changes and an engine restart.");
            }
            ImGui::EndDisabled();

            ImGui::EndCombo();
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
    }

    if (ImGui::CollapsingHeader("Texture filtering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled("Sampler filter (no mips yet on most textures).");

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
            RenderDebugModeLabel(RenderDebugMode::CompositeOcclusion),
            RenderDebugModeLabel(RenderDebugMode::GeomSunFacing),
            RenderDebugModeLabel(RenderDebugMode::ShadowCompareDepth),
            RenderDebugModeLabel(RenderDebugMode::ShadowBlockedCenter),
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
            ImGui::TextWrapped("Indirect diffuse from L2 SH9 irradiance (geom normal). Should be smooth on spheres.");
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
                "Blurred SSAO factor (1 = no occlusion). Use SSAO shader debug combo for raw/instrumented views. "
                "When SSAO is off the pass is skipped and the buffer is stale.");
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
        ImGui::TextDisabled("HDR+SSAO on by default; enable Bloom in panel for full post stack.");
        ImGui::TextDisabled("Set GAME_ENGINE_RENDER_DEBUG=1 for HDR/SSAO/import stderr logs.");
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
