#pragma once

#include <glm/glm.hpp>
#include <memory>

class Camera;
class Input;
class Scene;
class SelectionRenderer;

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
        const Camera& camera,
        Input& input,
        int framebufferWidth,
        int framebufferHeight,
        int windowWidth,
        int windowHeight,
        bool allowMouseInput,
        bool allowKeyboardInput);

    void HandleEscapeKey(Scene& scene);

    void RenderSelectionOverlay(const Scene& scene, const Camera& camera, bool useScreenSpace) const;

private:
    void DrawMarqueeOverlay(
        int framebufferWidth,
        int framebufferHeight,
        int windowWidth,
        int windowHeight) const;
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
};
