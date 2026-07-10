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
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

void DrawSsgiSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const LightingPanelUi::FeatureState features = LightingPanelUi::QueryFeatures(renderer, screenSpaceEffects);

    if (TuningSectionState::SectionHeader("Screen-space GI (SSGI)", true))
    {
        const bool sectionDisabled =
            !features.postProcessingEnabled || features.pathTracingActive || features.rtGiEnabled;

        if (!features.postProcessingEnabled)
        {
            LightingPanelUi::DrawWrappedNote("Enable Post-processing to use SSGI.");
        }

        if (features.pathTracingActive)
        {
            LightingPanelUi::DrawWrappedNote("Path tracing replaces SSGI.");
        }

        if (features.rtGiEnabled)
        {
            LightingPanelUi::DrawWrappedNote("RT diffuse GI is enabled. Disable RT GI before using SSGI inject.");
        }

        LightingPanelUi::DrawWrappedHelp(
            "Screen-space indirect from the radiance buffer. Use Diagnostics > SSGI views to inspect the pipeline.");

        if (sectionDisabled)
        {
            ImGui::BeginDisabled();
        }

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
                [](Scene& target, bool ssgiEnabledValue) {
                    target.GetRenderer().GetScreenSpaceEffects().SetSsgiEnabled(ssgiEnabledValue);
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

        if (sectionDisabled)
        {
            ImGui::EndDisabled();
        }
    }
}
