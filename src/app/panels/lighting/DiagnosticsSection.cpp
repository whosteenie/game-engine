#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/TuningSectionState.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelWidgets.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "engine/camera/Camera.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"

#include <imgui.h>

#include <string>

void DrawDiagnosticsSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    SceneRenderer& renderer = ctx.renderer;
    ScreenSpaceEffects& screenSpaceEffects = ctx.screenSpaceEffects;
    const Camera& camera = ctx.camera;
    const int viewportWidth = ctx.viewportWidth;
    const int viewportHeight = ctx.viewportHeight;

    if (TuningSectionState::SectionHeader("Diagnostics", true))
    {
        LightingPanelUi::DrawWrappedHelp(
            "Pick a category, then a view, to isolate a render pass. Write a report if you need to share settings.");

        LightingPanelWidgets::DrawDebugViewPicker(screenSpaceEffects, renderer);

        ImGui::Spacing();
        bool forceDlssReset = screenSpaceEffects.GetForceDlssResetEveryFrame();
        if (ImGui::Checkbox("Force DLSS/RR reset every frame (diagnostic)", &forceDlssReset))
        {
            screenSpaceEffects.SetForceDlssResetEveryFrame(forceDlssReset);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Sets Streamline's actual temporal reset flag on every evaluation. This is session-only "
                "and tests whether an artifact is retained inside DLSS/RR history.");
        }
        if (forceDlssReset)
        {
            LightingPanelUi::DrawWrappedNote(
                "Diagnostic active: Streamline history is reset every frame. Expect substantially more "
                "noise or instability; if the ghost persists, it is already present before DLSS/RR.");
        }

        const RenderDebugMode activeMode = screenSpaceEffects.GetDebugMode();
        if (const char* helpText = LightingPanelWidgets::RenderDebugModeHelpText(activeMode))
        {
            ImGui::Spacing();
            LightingPanelUi::DrawWrappedHelp(helpText);
            if (activeMode == RenderDebugMode::LightSpaceDepth)
            {
                LightingPanelUi::DrawWrappedNote(
                    "If you see hard white/black regions: enable Frustum-only XY fit, move camera to reset stable fit, "
                    "then compare with Shadow map stored depth.");
            }
        }

        ImGui::Spacing();
        LightingPanelUi::DrawWrappedNote("Set GAME_ENGINE_RENDER_DEBUG=1 for HDR/AO/import stderr logs.");

        static std::string diagnosticStatus;
        if (ImGui::Button("Write diagnostics/render_diagnostics.txt"))
        {
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
            LightingPanelUi::DrawWrappedHelp(diagnosticStatus.c_str());
        }
    }
}
