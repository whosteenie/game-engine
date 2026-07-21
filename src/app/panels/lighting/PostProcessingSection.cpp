#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/RendererSettingUi.h"
#include "app/editor/TuningSectionState.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/scene/document/Scene.h"
#include "app/scene/rendering/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/rendering/post/ScreenSpaceEffects.h"

#include <imgui.h>

void DrawPostProcessingSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const LightingPanelUi::FeatureState features = LightingPanelUi::QueryFeatures(ctx.renderer, screenSpaceEffects);

    if (TuningSectionState::SectionHeader("Post-processing", true))
    {
        bool enabled = screenSpaceEffects.IsEnabled();
        if (ImGui::Checkbox("Enable HDR post-processing", &enabled))
        {
            RendererSettingUi::ApplyChange(
                "post_processing_enabled",
                editContext,
                scene,
                "HDR post-processing",
                [enabled](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetEnabled(enabled);
                    target.MarkDirty();
                });
        }
        RendererSettingUi::MarkRendered("post_processing_enabled");

        if (!enabled)
        {
            LightingPanelUi::DrawWrappedNote(
                "Turn this on for bloom, tonemap, screen-space AO/GI/SSR, and post AA (FXAA/TAA/DLSS). "
                "The deferred lighting pass still runs.");
            return;
        }

        float exposure = screenSpaceEffects.GetExposure();
        UndoableRendererSliderFloat(
            "Exposure (stops)",
            &exposure,
            -2.0f,
            4.0f,
            "%.3f",
            editContext,
            [](Scene& target, float exposureValue) {
                target.GetRenderer().GetScreenSpaceEffects().SetExposure(exposureValue);
                target.MarkDirty();
            });
        RendererSettingUi::MarkRendered("post_exposure");

        int tonemapMode = static_cast<int>(screenSpaceEffects.GetTonemapMode());
        const char* tonemapModes[] = {"Gamma", "Reinhard", "ACES"};
        if (ImGui::Combo("Tonemap", &tonemapMode, tonemapModes, IM_ARRAYSIZE(tonemapModes)))
        {
            RendererSettingUi::ApplyChange(
                "post_tonemap",
                editContext,
                scene,
                "Tonemap",
                [tonemapMode](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetTonemapMode(static_cast<TonemapMode>(tonemapMode));
                    target.MarkDirty();
                });
        }
        RendererSettingUi::MarkRendered("post_tonemap");
        LightingPanelUi::DrawTooltipForLastItem(
            "Maps HDR lighting into the displayable range. ACES is cinematic; Reinhard is softer and simpler.");

        bool bloomEnabled = screenSpaceEffects.IsBloomEnabled();
        if (ImGui::Checkbox("Bloom", &bloomEnabled))
        {
            RendererSettingUi::ApplyChange(
                "post_bloom",
                editContext,
                scene,
                "Bloom",
                [bloomEnabled](Scene& target) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomEnabled(bloomEnabled);
                    target.MarkDirty();
                });
        }
        RendererSettingUi::MarkRendered("post_bloom");

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
                [](Scene& target, float value) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomThreshold(value);
                    target.MarkDirty();
                });
            LightingPanelUi::DrawTooltipForLastItem(
                "Only pixels brighter than this value contribute to bloom.");

            float bloomSoftKnee = screenSpaceEffects.GetBloomSoftKnee();
            UndoableRendererSliderFloat(
                "Bloom soft knee",
                &bloomSoftKnee,
                0.0f,
                1.0f,
                "%.3f",
                editContext,
                [](Scene& target, float value) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomSoftKnee(value);
                    target.MarkDirty();
                });
            LightingPanelUi::DrawTooltipForLastItem(
                "Softens the transition around the threshold to avoid a hard bloom cutoff.");

            float bloomIntensity = screenSpaceEffects.GetBloomIntensity();
            UndoableRendererSliderFloat(
                "Bloom intensity",
                &bloomIntensity,
                0.0f,
                2.0f,
                "%.3f",
                editContext,
                [](Scene& target, float value) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomIntensity(value);
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
                [](Scene& target, float value) {
                    target.GetRenderer().GetScreenSpaceEffects().SetBloomBlurRadius(value);
                    target.MarkDirty();
                });
            LightingPanelUi::DrawTooltipForLastItem(
                "Controls how far bright highlights spread across nearby pixels.");
        }

        if (features.debugViewActive)
        {
            LightingPanelUi::DrawWrappedNote(
                "A debug view is active. Bloom and tonemap still run, but FXAA only affects the final resolved image when debug view is None.");
        }
    }
}
