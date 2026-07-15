#include "app/panels/LightingPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/EditorSettings.h"
#include "app/panels/lighting/LightingPanelContext.h"
#include "app/panels/lighting/LightingPanelSections.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/ScreenSpaceEffects.h"

#include <imgui.h>

#include <glm/glm.hpp>

void LightingPanel::Draw(
    Scene& scene,
    Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    UndoStack* undoStack,
    EditorSettings* editorSettings) const
{
    UpdateRayTracingDiagnosticCapture(scene, camera, viewportWidth, viewportHeight);
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
        "Camera position: (%.4f, %.4f, %.4f)",
        cameraPosition.x,
        cameraPosition.y,
        cameraPosition.z);
    ImGui::Text(
        "Camera rotation (deg): yaw %.4f, pitch %.4f",
        camera.GetYaw(),
        camera.GetPitch());

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

    const LightingPanelContext panelContext{
        scene,
        camera,
        viewportWidth,
        viewportHeight,
        editContext,
        renderer,
        ibl,
        environmentMap,
        screenSpaceEffects,
        editorSettings,
    };

    DrawSceneSection(panelContext);
    DrawEnvironmentSection(panelContext);
    DrawDirectionalShadowsSection(panelContext);
    DrawPostProcessingSection(panelContext);
    DrawAmbientOcclusionSection(panelContext);
    DrawSsgiSection(panelContext);
    DrawSsrSection(panelContext);
    DrawRayTracingSection(panelContext);
    DrawAntiAliasingSection(panelContext);
    DrawTextureFilteringSection(panelContext);
    DrawDiagnosticsSection(panelContext);

    ImGui::End();
}
