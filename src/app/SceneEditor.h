#pragma once

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

    void RenderSelectionOverlay(const Scene& scene, const Camera& camera) const;

private:
    TransformTool m_tool = TransformTool::Translate;
    TransformSpace m_transformSpace = TransformSpace::Local;
    std::unique_ptr<SelectionRenderer> m_selectionRenderer;
};
