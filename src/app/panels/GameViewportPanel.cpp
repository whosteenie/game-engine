#include "app/panels/GameViewportPanel.h"

#include "app/editor/EditorPanelConstraints.h"

#include <imgui.h>

bool GameViewportPanel::HasValidRenderTarget() const
{
    return OffscreenViewportPanel::HasValidRenderTarget(m_viewport);
}

std::uintptr_t GameViewportPanel::GetFramebuffer() const
{
    return OffscreenViewportPanel::GetFramebuffer(m_viewport);
}

std::uintptr_t GameViewportPanel::GetColorTexture() const
{
    return OffscreenViewportPanel::GetColorTexture(m_viewport);
}

void GameViewportPanel::EnsureFramebufferSized() const
{
    OffscreenViewportPanel::EnsureFramebufferSized(m_viewport);
}

void GameViewportPanel::ClearRenderTarget() const
{
    OffscreenViewportPanel::ClearRenderTarget(m_viewport);
}

void GameViewportPanel::Draw(const bool hasSceneCamera, const bool willRenderThisFrame)
{
    OffscreenViewportPanel::ResetFrameState(m_viewport);

    EditorPanelConstraints::ApplySceneViewPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Game View", m_viewport.showPanel))
    {
        if (!m_viewport.showPanel)
        {
            OffscreenViewportPanel::OnPanelHidden(m_viewport);
        }
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    OffscreenViewportPanel::UpdateRenderSize(m_viewport, available);

    const bool canCompositeFrame = hasSceneCamera
        && OffscreenViewportPanel::CanCompositeFrame(m_viewport, willRenderThisFrame);
    const OffscreenViewportPanel::ViewportRegion region =
        OffscreenViewportPanel::DrawViewportRegion(m_viewport, available, canCompositeFrame);
    if (!canCompositeFrame)
    {
        const char* primaryLabel = hasSceneCamera ? "Game View" : "No camera in scene";
        const char* secondaryLabel = hasSceneCamera ? nullptr : "Add a Camera object to preview";
        OffscreenViewportPanel::DrawCenteredPlaceholder(
            region.imageMin, available, primaryLabel, secondaryLabel);
    }

    OffscreenViewportPanel::UpdateInteractionRect(
        m_viewport, region.imageMin, region.imageSize, false);

    ImGui::End();
}

void GameViewportPanel::CompositeRenderedFrame()
{
    OffscreenViewportPanel::CompositeRenderedFrame(m_viewport);
}
