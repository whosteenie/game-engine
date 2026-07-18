#pragma once

#include "app/scene/editing/ViewportRect.h"
#include "app/editor/OffscreenViewportPanel.h"

#include <imgui.h>

class GameViewportPanel
{
public:
    void Draw(bool hasSceneCamera, bool willRenderThisFrame);

    void CompositeRenderedFrame();
    void InvalidateCompositeFrame()
    {
        OffscreenViewportPanel::InvalidateCompositeFrame(m_viewport);
    }

    bool& ShowPanel() { return m_viewport.showPanel; }
    const bool& ShowPanel() const { return m_viewport.showPanel; }

    bool HasValidRenderTarget() const;
    bool HasGpuFramebuffer() const { return m_viewport.framebuffer.IsValid(); }
    bool IsHovered() const { return m_viewport.interactionRect.hovered; }
    const ViewportRect& GetInteractionRect() const { return m_viewport.interactionRect; }

    int GetRenderWidth() const { return m_viewport.renderWidth; }
    int GetRenderHeight() const { return m_viewport.renderHeight; }
    bool IsLiveResizePending() const
    {
        return OffscreenViewportPanel::IsLiveResizePending(m_viewport);
    }
    bool HasReadyCompositeFrame() const
    {
        return OffscreenViewportPanel::HasReadyCompositeFrame(m_viewport);
    }
    bool HasSceneCamera() const { return m_hasSceneCamera; }

    std::uintptr_t GetFramebuffer() const;
    std::uintptr_t GetColorTexture() const;

    void EnsureFramebufferSized() const;
    void ClearRenderTarget() const;

private:
    OffscreenViewportPanel::State m_viewport{};
    bool m_hasSceneCamera = false;
};
