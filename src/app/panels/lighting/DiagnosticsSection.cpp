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

        bool useDilatedMotion = screenSpaceEffects.GetUseDilatedDlssMotionVectors();
        if (ImGui::Checkbox("Use dilated DLSS/RR motion vectors (A/B)", &useDilatedMotion))
        {
            screenSpaceEffects.SetUseDilatedDlssMotionVectors(useDilatedMotion);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Supplies a foreground-depth-dilated motion buffer and tells Streamline it is dilated. "
                "This is session-only; compare strafe ghosts with raw vectors.");
        }

        bool reconstructCameraMotion = screenSpaceEffects.GetReconstructDlssCameraMotion();
        if (ImGui::Checkbox("Streamline camera-motion reconstruction (static-scene A/B)", &reconstructCameraMotion))
        {
            screenSpaceEffects.SetReconstructDlssCameraMotion(reconstructCameraMotion);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Supplies zero object motion and has Streamline reconstruct the camera component from its "
                "D24 depth input and clip-to-previous matrix. Test only while scene objects are stationary; "
                "this isolates the engine's camera-motion-vector handoff.");
        }

        bool freezeTemporalJitter = screenSpaceEffects.GetFreezeTemporalJitterDiagnostic();
        if (ImGui::Checkbox("Freeze temporal jitter (DLSS/RR A/B)", &freezeTemporalJitter))
        {
            screenSpaceEffects.SetFreezeTemporalJitterDiagnostic(freezeTemporalJitter);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Removes projection jitter from rendering and sends zero jitter to Streamline. "
                "This is session-only and isolates the DLSS/RR jitter convention from camera motion.");
        }

        bool useSubmissionFrameIndex = screenSpaceEffects.GetUseDlssSubmissionFrameIndexDiagnostic();
        if (ImGui::Checkbox("Use submission-frame Streamline tokens (DLSS/RR A/B)", &useSubmissionFrameIndex))
        {
            screenSpaceEffects.SetUseDlssSubmissionFrameIndexDiagnostic(useSubmissionFrameIndex);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Uses the renderer's monotonic presented-frame identity instead of an internal count of "
                "DLSS evaluations. Resets DLSS/RR history when changed; compare the strafe trail after it settles.");
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
