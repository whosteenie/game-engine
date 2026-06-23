#pragma once

#include "app/undo/UndoCommand.h"
#include "app/editor/EditorViewportRect.h"

#include <glm/glm.hpp>
#include <memory>
#include <string>

class Camera;
class Input;
class Scene;
class SelectionRenderer;
class UndoStack;

enum class TransformTool
{
    Translate = 0,
    Rotate = 1,
    Scale = 2
};

enum class TransformSpace
{
    Local = 0,
    World = 1
};

class SceneEditor
{
public:
    SceneEditor();
    ~SceneEditor();

    TransformTool GetTool() const;
    void SetTool(TransformTool tool);
    TransformSpace GetTransformSpace() const;
    void SetTransformSpace(TransformSpace space);

    void Update(
        Scene& scene,
        Camera& camera,
        Input& input,
        int framebufferWidth,
        int framebufferHeight,
        int windowWidth,
        int windowHeight,
        bool allowMouseInput,
        bool allowKeyboardInput,
        UndoStack* undoStack,
        const std::string& projectRoot,
        const EditorViewportRect* viewport);

    void HandleEscapeKey(Scene& scene);
    void ResetInteractionState();

    void RenderSelectionOverlay(const Scene& scene, const Camera& camera, bool useScreenSpace) const;

private:
    void DrawMarqueeOverlay(const EditorViewportRect& viewport) const;
    void CancelMarqueeDrag();

    TransformTool m_tool = TransformTool::Translate;
    TransformSpace m_transformSpace = TransformSpace::Local;
    std::unique_ptr<SelectionRenderer> m_selectionRenderer;
    glm::vec2 m_lastPickScreenPosition = glm::vec2(0.0f);
    bool m_hasLastPickScreenPosition = false;
    bool m_trackingLeftDrag = false;
    bool m_marqueeActive = false;
    glm::vec2 m_dragStartFramebuffer = glm::vec2(0.0f);
    glm::vec2 m_dragCurrentFramebuffer = glm::vec2(0.0f);
    bool m_gizmoWasUsing = false;
    ObjectTransformMap m_gizmoTransformBefore;
};
