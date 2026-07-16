#pragma once

#include "app/editor/EditorViewportRect.h"
#include "app/editor/OffscreenViewportPanel.h"
#include "engine/scene/SceneObjectId.h"

#include <glm/glm.hpp>
#include <imgui.h>

#include <vector>

class Camera;
class ProjectSession;
class Scene;
class UndoStack;

class SceneViewportPanel
{
public:
    void Draw(
        Camera& camera,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        bool willRenderThisFrame);

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
    const EditorViewportRect& GetInteractionRect() const { return m_viewport.interactionRect; }

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

    std::uintptr_t GetFramebuffer() const;
    std::uintptr_t GetColorTexture() const;

    void EnsureFramebufferSized() const;
    // Project-load benchmarks must not depend on the user's active dock tab. This allocates a
    // stable offscreen target without constructing an ImGui viewport region.
    void EnsureBenchmarkRenderTarget(int width, int height);
    void ClearRenderTarget() const;
    void NotifyFlySpeedChanged(float speed);

private:
    void DrawViewGizmo(
        Camera& camera,
        const Scene& scene,
        const ImVec2& imageMin,
        const ImVec2& imageMax);
    void DrawModelDropTarget(
        Camera& camera,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack);
    void CancelModelDropPreview(Scene& scene);
    void UpdateModelDropPreview(
        Camera& camera,
        Scene& scene,
        const std::vector<int>& previewRoots);
    void DrawFlySpeedOverlay(const ImVec2& imageMin, const ImVec2& imageMax) const;

    OffscreenViewportPanel::State m_viewport{};
    bool m_wasUsingViewManipulate = false;
    glm::vec3 m_viewManipulateFocus{0.0f};
    float m_viewManipulateDistance = 8.0f;
    SceneObjectId m_modelDropPreviewRootId = kInvalidSceneObjectId;
    std::vector<SceneObjectId> m_modelDropPreviewSelectionBeforeIds;
    SceneObjectId m_modelDropPreviewSelectionBeforePrimary = kInvalidSceneObjectId;
    float m_flySpeedOverlayValue = 1.0f;
    double m_flySpeedOverlayStartedAt = -100.0;
    double m_flySpeedOverlayChangedAt = -100.0;
};
