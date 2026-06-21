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

class SceneEditor
{
public:
    SceneEditor();
    ~SceneEditor();

    TransformTool GetTool() const;
    void SetTool(TransformTool tool);
    bool IsGizmoDragging() const;

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
    SelectionRenderer* m_selectionRenderer = nullptr;
    bool m_gizmoWasUsing = false;
};
