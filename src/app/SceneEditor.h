#pragma once

class Camera;
class DemoScene;
class Input;
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
        DemoScene& scene,
        const Camera& camera,
        Input& input,
        int framebufferWidth,
        int framebufferHeight,
        int windowWidth,
        int windowHeight,
        bool allowMouseInput,
        bool allowKeyboardInput);

    void RenderOverlays(DemoScene& scene, const Camera& camera, int viewportWidth, int viewportHeight);

private:
    TransformTool m_tool = TransformTool::Translate;
    TransformSpace m_transformSpace = TransformSpace::Local;
    SelectionRenderer* m_selectionRenderer = nullptr;
};
