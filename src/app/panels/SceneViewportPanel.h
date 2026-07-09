#pragma once

#include "app/editor/EditorViewportRect.h"
#include "app/editor/OffscreenViewportPanel.h"

#include <glm/glm.hpp>
#include <imgui.h>

class Camera;
class Scene;

class SceneViewportPanel
{
public:
    void Draw(Camera& camera, const Scene& scene, bool willRenderThisFrame);

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
    void DrawViewGizmo(
        Camera& camera,
        const Scene& scene,
        const ImVec2& imageMin,
        const ImVec2& imageMax);

    OffscreenViewportPanel::State m_viewport{};
    bool m_wasUsingViewManipulate = false;
    glm::vec3 m_viewManipulateFocus{0.0f};
    float m_viewManipulateDistance = 8.0f;
};
