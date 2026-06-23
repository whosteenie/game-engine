#pragma once

#include "app/editor/EditorViewportRect.h"

#include "engine/rendering/Framebuffer.h"

class GameViewportPanel
{
public:
    void Draw(bool hasSceneCamera, bool hasRenderedFrame);

    bool& ShowPanel() { return m_showPanel; }
    const bool& ShowPanel() const { return m_showPanel; }

    bool HasValidRenderTarget() const;
    bool IsHovered() const { return m_interactionRect.hovered; }
    const EditorViewportRect& GetInteractionRect() const { return m_interactionRect; }

    int GetRenderWidth() const { return m_renderWidth; }
    int GetRenderHeight() const { return m_renderHeight; }

    unsigned int GetFramebuffer() const;
    unsigned int GetColorTexture() const;

    void EnsureFramebufferSized() const;

private:
    bool m_showPanel = true;
    mutable Framebuffer m_framebuffer;
    mutable int m_renderWidth = 0;
    mutable int m_renderHeight = 0;
    mutable EditorViewportRect m_interactionRect{};
};
