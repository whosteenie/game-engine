#include "app/panels/SceneViewportPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/scene/Scene.h"
#include "engine/camera/Camera.h"

#include <ImGuizmo.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>

namespace
{
    void UpdateInteractionRect(
        EditorViewportRect& interactionRect,
        const ImVec2& imageMin,
        const ImVec2& imageSize,
        int renderWidth,
        int renderHeight)
    {
        const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        interactionRect.valid = renderWidth > 0 && renderHeight > 0;
        interactionRect.screenX = imageMin.x;
        interactionRect.screenY = imageMin.y;
        interactionRect.screenWidth = imageSize.x;
        interactionRect.screenHeight = imageSize.y;
        interactionRect.framebufferX = static_cast<int>(imageMin.x * framebufferScale.x);
        interactionRect.framebufferY = static_cast<int>(imageMin.y * framebufferScale.y);
        interactionRect.width = renderWidth;
        interactionRect.height = renderHeight;
    }
}

bool SceneViewportPanel::HasValidRenderTarget() const
{
    return m_showPanel && m_renderWidth > 0 && m_renderHeight > 0;
}

std::uintptr_t SceneViewportPanel::GetFramebuffer() const
{
    return m_framebuffer.GetFramebuffer();
}

std::uintptr_t SceneViewportPanel::GetColorTexture() const
{
    return m_framebuffer.GetColorTexture();
}

void SceneViewportPanel::EnsureFramebufferSized() const
{
    if (!HasValidRenderTarget())
    {
        return;
    }

    (void)m_framebuffer.Resize(m_renderWidth, m_renderHeight);
}

void SceneViewportPanel::ClearRenderTarget() const
{
    if (m_framebuffer.IsValid())
    {
        m_framebuffer.ClearRenderTarget();
    }
}

void SceneViewportPanel::DrawViewGizmo(
    Camera& camera,
    const Scene& scene,
    const ImVec2& imageMin,
    const ImVec2& imageMax)
{
    if (!m_interactionRect.valid || m_interactionRect.imguiWindow == nullptr)
    {
        return;
    }

    constexpr float kGizmoSize = 128.0f;
    constexpr float kMargin = 8.0f;
    const ImVec2 gizmoPos(
        imageMax.x - kGizmoSize - kMargin,
        imageMin.y + kMargin);
    const ImVec2 gizmoSize(kGizmoSize, kGizmoSize);

    ImGuizmo::SetAlternativeWindow(m_interactionRect.imguiWindow);
    ImGuizmo::SetRect(
        m_interactionRect.screenX,
        m_interactionRect.screenY,
        m_interactionRect.screenWidth,
        m_interactionRect.screenHeight);

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
    m_interactionRect = {};

    EditorPanelConstraints::ApplySceneViewPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Scene View", m_showPanel))
    {
        m_renderWidth = 0;
        m_renderHeight = 0;
        m_wasUsingViewManipulate = false;
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
    const int requestedWidth = std::max(1, static_cast<int>(available.x * framebufferScale.x + 0.5f));
    const int requestedHeight = std::max(1, static_cast<int>(available.y * framebufferScale.y + 0.5f));

    // Avoid recreating GPU resources when ImGui layout jitters by a pixel.
    if (m_renderWidth <= 0
        || m_renderHeight <= 0
        || std::abs(requestedWidth - m_renderWidth) > 1
        || std::abs(requestedHeight - m_renderHeight) > 1)
    {
        m_renderWidth = requestedWidth;
        m_renderHeight = requestedHeight;
    }

    if (m_framebuffer.IsValid() && m_framebuffer.GetColorTexture() != 0)
    {
        const ImTextureID textureId = static_cast<ImTextureID>(m_framebuffer.GetColorTexture());
#if defined(GAME_ENGINE_D3D12)
        ImGui::Image(textureId, available);
#else
        ImGui::Image(textureId, available, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
#endif
    }
    else
    {
        ImGui::Dummy(available);
        const ImVec2 cursor = ImGui::GetItemRectMin();
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 regionMax(cursor.x + available.x, cursor.y + available.y);
        drawList->AddRectFilled(cursor, regionMax, IM_COL32(34, 34, 38, 255));

        constexpr const char* kPlaceholderLabel = "Scene View";
        const ImVec2 textSize = ImGui::CalcTextSize(kPlaceholderLabel);
        drawList->AddText(
            ImVec2(cursor.x + (available.x - textSize.x) * 0.5f, cursor.y + (available.y - textSize.y) * 0.5f),
            IM_COL32(130, 130, 140, 255),
            kPlaceholderLabel);
    }

    const ImVec2 imageMin = ImGui::GetItemRectMin();
    const ImVec2 imageMax = ImGui::GetItemRectMax();
    const ImVec2 imageSize(imageMax.x - imageMin.x, imageMax.y - imageMin.y);
    m_interactionRect.hovered =
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
        && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_RootWindow);
    m_interactionRect.imguiWindow = ImGui::GetCurrentWindow();
    UpdateInteractionRect(m_interactionRect, imageMin, imageSize, m_renderWidth, m_renderHeight);
    DrawViewGizmo(camera, scene, imageMin, imageMax);

    ImGui::End();
}
