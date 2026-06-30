#include "app/panels/SceneViewportPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/scene/Scene.h"
#include "engine/camera/Camera.h"

#include <ImGuizmo.h>
#include <imgui.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

bool SceneViewportPanel::HasValidRenderTarget() const
{
    return OffscreenViewportPanel::HasValidRenderTarget(m_viewport);
}

std::uintptr_t SceneViewportPanel::GetFramebuffer() const
{
    return OffscreenViewportPanel::GetFramebuffer(m_viewport);
}

std::uintptr_t SceneViewportPanel::GetColorTexture() const
{
    return OffscreenViewportPanel::GetColorTexture(m_viewport);
}

void SceneViewportPanel::EnsureFramebufferSized() const
{
    OffscreenViewportPanel::EnsureFramebufferSized(m_viewport);
}

void SceneViewportPanel::ClearRenderTarget() const
{
    OffscreenViewportPanel::ClearRenderTarget(m_viewport);
}

void SceneViewportPanel::DrawViewGizmo(
    Camera& camera,
    const Scene& scene,
    const ImVec2& imageMin,
    const ImVec2& imageMax)
{
    if (!m_viewport.interactionRect.valid || m_viewport.interactionRect.imguiWindow == nullptr)
    {
        return;
    }

    constexpr float kGizmoSize = 128.0f;
    constexpr float kMargin = 8.0f;
    const ImVec2 gizmoPos(
        imageMax.x - kGizmoSize - kMargin,
        imageMin.y + kMargin);
    const ImVec2 gizmoSize(kGizmoSize, kGizmoSize);

    ImGuizmo::SetAlternativeWindow(m_viewport.interactionRect.imguiWindow);
    ImGuizmo::SetRect(
        m_viewport.interactionRect.screenX,
        m_viewport.interactionRect.screenY,
        m_viewport.interactionRect.screenWidth,
        m_viewport.interactionRect.screenHeight);

    glm::vec3 focus(0.0f);
    float focusRadius = 0.5f;
    if (!scene.TryGetViewFocusPoint(focus, focusRadius))
    {
        focus = glm::vec3(0.0f);
    }

    const bool wasUsingViewManipulate = m_wasUsingViewManipulate;

    glm::mat4 view = camera.GetViewMatrix();
    float orbitLength = std::max(glm::length(camera.GetPosition() - focus), 1.0f);
    if (wasUsingViewManipulate)
    {
        view = camera.BuildViewMatrixLookingAt(m_viewManipulateFocus, m_viewManipulateDistance);
        orbitLength = m_viewManipulateDistance;
    }

    ImGuizmo::ViewManipulate(
        glm::value_ptr(view),
        orbitLength,
        gizmoPos,
        gizmoSize,
        IM_COL32(0, 0, 0, 0));

    const bool usingViewManipulate = ImGuizmo::IsUsingViewManipulate();
    if (usingViewManipulate && !wasUsingViewManipulate)
    {
        m_viewManipulateFocus = focus;
        m_viewManipulateDistance = std::max(glm::length(camera.GetPosition() - focus), 1.0f);
    }

    if (usingViewManipulate)
    {
        camera.ApplyViewManipulateResult(view, m_viewManipulateFocus, m_viewManipulateDistance);
    }

    m_wasUsingViewManipulate = usingViewManipulate;
}

void SceneViewportPanel::Draw(Camera& camera, const Scene& scene)
{
    OffscreenViewportPanel::ResetFrameState(m_viewport);

    EditorPanelConstraints::ApplySceneViewPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Scene View", m_viewport.showPanel))
    {
        OffscreenViewportPanel::OnPanelHidden(m_viewport);
        m_wasUsingViewManipulate = false;
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    OffscreenViewportPanel::UpdateRenderSize(m_viewport, available);

    const bool canCompositeFrame =
        m_viewport.framebuffer.IsValid() && m_viewport.framebuffer.GetColorTexture() != 0;
    const OffscreenViewportPanel::ViewportRegion region =
        OffscreenViewportPanel::DrawViewportRegion(m_viewport, available, canCompositeFrame);
    if (!canCompositeFrame)
    {
        OffscreenViewportPanel::DrawCenteredPlaceholder(region.imageMin, available, "Scene View");
    }

    OffscreenViewportPanel::UpdateInteractionRect(
        m_viewport, region.imageMin, region.imageSize, true);
    DrawViewGizmo(camera, scene, region.imageMin, region.imageMax);

    ImGui::End();
}

void SceneViewportPanel::CompositeRenderedFrame()
{
    OffscreenViewportPanel::CompositeRenderedFrame(m_viewport);
}
