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

void DrawHdrSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;

    if (TuningSectionState::SectionHeader("HDR", true))
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
}
