#pragma once

#include "app/EditorViewportRect.h"

#include "engine/Framebuffer.h"

#include <glm/glm.hpp>

class Camera;
class Scene;
struct ImVec2;

class SceneViewportPanel
{
public:
    void Draw(Camera& camera, const Scene& scene);

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
    void DrawViewGizmo(
        Camera& camera,
        const Scene& scene,
        const ImVec2& imageMin,
        const ImVec2& imageMax);

    bool m_showPanel = true;
    mutable Framebuffer m_framebuffer;
    mutable int m_renderWidth = 0;
    mutable int m_renderHeight = 0;
    mutable EditorViewportRect m_interactionRect{};
    bool m_wasUsingViewManipulate = false;
    glm::vec3 m_viewManipulateFocus{0.0f};
    float m_viewManipulateDistance = 8.0f;
};
