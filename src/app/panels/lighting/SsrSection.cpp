#include "app/panels/lighting/LightingPanelSections.h"
#include "app/editor/RendererSettingUi.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/TuningSectionState.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"

#include <imgui.h>


void DrawSsrSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const LightingPanelUi::FeatureState features = LightingPanelUi::QueryFeatures(renderer, screenSpaceEffects);

    if (TuningSectionState::SectionHeader("Screen-space reflections (SSR)", true))
    {
        const bool sectionDisabled =
            !features.postProcessingEnabled || features.pathTracingActive;

        if (!features.postProcessingEnabled)
        {
            LightingPanelUi::DrawWrappedNote("Enable Post-processing to use SSR.");
        }

        if (features.pathTracingActive)
        {
            LightingPanelUi::DrawWrappedNote("Path tracing replaces SSR.");
        }

        LightingPanelUi::DrawWrappedHelp(
            "Specular screen-space trace from the scene-color buffer. Use Diagnostics > SSR views to inspect stages.");

        ImGui::PushID("SSR");
        if (sectionDisabled)
        {
            ImGui::BeginDisabled();
        }

        bool ssrEnabled = screenSpaceEffects.IsSsrEnabled();
        UndoableRendererCheckbox(
            "Enable SSR",
            &ssrEnabled,
            editContext,
            [](Scene& target, bool ssrEnabled) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrEnabled(ssrEnabled);
                target.MarkDirty();
            });
        RendererSettingUi::MarkRendered("ssr_enabled");

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
        RendererSettingUi::MarkRendered("ssr_max_distance");

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
        LightingPanelUi::DrawWrappedNote(
            "Samples = jittered reflection rays averaged per pixel. Costs linearly; reduces speckle without extra blur.");

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
        LightingPanelUi::DrawWrappedNote(
            "Surfaces rougher than cutoff skip trace (IBL fallback in composite). Trace also runs when an SSR debug view is selected.");

        LightingPanelUi::DrawWrappedNote(
            "Multi-sample trace averages hits only (RGB); alpha is mean confidence.");

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

        RendererSettingUi::MarkRendered("ssr_strength");

        bool ssrDenoiseEnabled = screenSpaceEffects.IsSsrDenoiseEnabled();
        UndoableRendererCheckbox(
            "Enable denoise",
            &ssrDenoiseEnabled,
            editContext,
            [](Scene& target, bool ssrDenoiseEnabled) {
                target.GetRenderer().GetScreenSpaceEffects().SetSsrDenoiseEnabled(ssrDenoiseEnabled);
                target.MarkDirty();
            });

        LightingPanelUi::DrawWrappedNote(
            "Quadratic steps + jitter. Spatial filters speckle; temporal uses edge-aware blend (~0.9).");

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
        LightingPanelUi::DrawWrappedNote(
            "SVGF denoise: temporal variance accumulation + 4-pass à-trous. Set debug view to None for composite.");

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

        if (sectionDisabled)
        {
            ImGui::EndDisabled();
        }
        ImGui::PopID();
    }
}
