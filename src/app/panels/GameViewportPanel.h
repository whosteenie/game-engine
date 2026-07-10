#pragma once

#include "app/editor/EditorViewportRect.h"
#include "app/editor/OffscreenViewportPanel.h"

#include <imgui.h>

class GameViewportPanel
{
public:
    void Draw(bool hasSceneCamera, bool willRenderThisFrame);

    void CompositeRenderedFrame();

    bool& ShowPanel() { return m_viewport.showPanel; }
    const bool& ShowPanel() const { return m_viewport.showPanel; }

    bool HasValidRenderTarget() const;
    bool HasGpuFramebuffer() const { return m_viewport.framebuffer.IsValid(); }
    bool IsHovered() const { return m_viewport.interactionRect.hovered; }
    const EditorViewportRect& GetInteractionRect() const { return m_viewport.interactionRect; }

    int GetRenderWidth() const { return m_viewport.renderWidth; }
    int GetRenderHeight() const { return m_viewport.renderHeight; }

    std::uintptr_t GetFramebuffer() const;
    std::uintptr_t GetColorTexture() const;

    void EnsureFramebufferSized() const;
    void ClearRenderTarget() const;

private:
    OffscreenViewportPanel::State m_viewport{};
};
