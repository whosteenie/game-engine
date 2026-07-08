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

void DrawSsrSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;

    if (TuningSectionState::SectionHeader("Screen-space reflections (SSR)", true))
    {
        const bool pathTracingScreenSpaceHandled = renderer.GetDxrSettings().IsPathTracingActive();
        if (pathTracingScreenSpaceHandled)
        {
            ImGui::TextDisabled("SSR is handled by the path tracer.");
            ImGui::BeginDisabled();
        }

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

        if (pathTracingScreenSpaceHandled)
        {
            ImGui::EndDisabled();
        }
    }
}
