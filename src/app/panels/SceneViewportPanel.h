#pragma once

#include "app/editor/EditorViewportRect.h"

#include "engine/rendering/Framebuffer.h"

#include <glm/glm.hpp>
#include <imgui.h>

class Camera;
class Scene;

class SceneViewportPanel
{
public:
    void Draw(Camera& camera, const Scene& scene);

    void CompositeRenderedFrame();

    bool& ShowPanel() { return m_showPanel; }
    const bool& ShowPanel() const { return m_showPanel; }

    bool HasValidRenderTarget() const;
    bool HasGpuFramebuffer() const { return m_framebuffer.IsValid(); }
    bool IsHovered() const { return m_interactionRect.hovered; }
    const EditorViewportRect& GetInteractionRect() const { return m_interactionRect; }

    int GetRenderWidth() const { return m_renderWidth; }
    int GetRenderHeight() const { return m_renderHeight; }

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

    bool m_showPanel = true;
    mutable Framebuffer m_framebuffer;
    mutable int m_renderWidth = 0;
    mutable int m_renderHeight = 0;
    mutable EditorViewportRect m_interactionRect{};
    bool m_wasUsingViewManipulate = false;
    glm::vec3 m_viewManipulateFocus{0.0f};
    float m_viewManipulateDistance = 8.0f;
    ImDrawList* m_compositeDrawList = nullptr;
    ImVec2 m_compositeMin{};
    ImVec2 m_compositeMax{};
    bool m_hasCompositeTarget = false;
};
