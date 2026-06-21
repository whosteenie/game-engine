#include "app/SceneEditor.h"

#include "app/DemoScene.h"
#include "engine/Camera.h"
#include "engine/Input.h"
#include "engine/SceneObject.h"
#include "engine/ScenePicker.h"
#include "engine/SelectionRenderer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

namespace
{
    ImGuizmo::OPERATION ToImGuizmoOperation(TransformTool tool)
    {
        switch (tool)
        {
        case TransformTool::Translate:
            return ImGuizmo::TRANSLATE;
        case TransformTool::Rotate:
            return ImGuizmo::ROTATE;
        case TransformTool::Scale:
            return ImGuizmo::SCALE;
        }

        return ImGuizmo::TRANSLATE;
    }

    void UpdateTransformGizmo(DemoScene& scene, const Camera& camera, TransformTool tool, bool& gizmoWasUsing)
    {
        gizmoWasUsing = false;

        const int selectedIndex = scene.GetSelectedObjectIndex();
        if (selectedIndex < 0)
        {
            return;
        }

        SceneObject& selectedObject = scene.GetObject(static_cast<std::size_t>(selectedIndex));
        if (!selectedObject.IsMovable())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

        glm::mat4 modelMatrix = selectedObject.BuildEditMatrix();
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();

        if (ImGuizmo::Manipulate(
                glm::value_ptr(viewMatrix),
                glm::value_ptr(projectionMatrix),
                ToImGuizmoOperation(tool),
                ImGuizmo::LOCAL,
                glm::value_ptr(modelMatrix)))
        {
            selectedObject.ApplyTransformFromMatrix(modelMatrix);
        }

        gizmoWasUsing = ImGuizmo::IsUsing();
    }
}

SceneEditor::SceneEditor()
    : m_selectionRenderer(new SelectionRenderer())
{
}

SceneEditor::~SceneEditor()
{
    delete m_selectionRenderer;
}

void SceneEditor::SetTool(TransformTool tool)
{
    m_tool = tool;
}

TransformTool SceneEditor::GetTool() const
{
    return m_tool;
}

bool SceneEditor::IsGizmoDragging() const
{
    return m_gizmoWasUsing;
}

void SceneEditor::Update(
    DemoScene& scene,
    const Camera& camera,
    Input& input,
    int framebufferWidth,
    int framebufferHeight,
    int windowWidth,
    int windowHeight,
    bool allowMouseInput,
    bool allowKeyboardInput)
{
    if (allowKeyboardInput && input.WasKeyPressed(GLFW_KEY_DELETE) && scene.HasSelection())
    {
        scene.RemoveObject(static_cast<std::size_t>(scene.GetSelectedObjectIndex()));
    }

    if (allowKeyboardInput && input.WasKeyPressed(GLFW_KEY_W))
    {
        SetTool(TransformTool::Translate);
    }
    if (allowKeyboardInput && input.WasKeyPressed(GLFW_KEY_E))
    {
        SetTool(TransformTool::Rotate);
    }
    if (allowKeyboardInput && input.WasKeyPressed(GLFW_KEY_R))
    {
        SetTool(TransformTool::Scale);
    }

    UpdateTransformGizmo(scene, camera, m_tool, m_gizmoWasUsing);

    const bool gizmoCapturingMouse = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (!allowMouseInput || gizmoCapturingMouse)
    {
        return;
    }

    const glm::vec2 mousePosition = input.GetCursorPositionFramebufferScaled(
        framebufferWidth,
        framebufferHeight,
        windowWidth,
        windowHeight);
    const glm::vec2 viewportSize(static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight));
    const Ray ray = ScreenPointToRay(
        mousePosition,
        viewportSize,
        camera.GetViewMatrix(),
        camera.GetProjectionMatrix());

    if (!input.WasMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT))
    {
        return;
    }

    const int pickedIndex = PickSceneObject(scene.GetObjects(), ray, scene.GetAnimationTime());
    if (pickedIndex >= 0)
    {
        scene.SetSelectedObjectIndex(pickedIndex);
    }
    else
    {
        scene.ClearSelection();
    }
}

void SceneEditor::RenderOverlays(
    DemoScene& scene,
    const Camera& camera,
    int /*viewportWidth*/,
    int /*viewportHeight*/)
{
    const int selectedIndex = scene.GetSelectedObjectIndex();
    if (selectedIndex < 0)
    {
        return;
    }

    const double animationTime = scene.GetAnimationTime();
    SceneObject& selectedObject = scene.GetObject(static_cast<std::size_t>(selectedIndex));
    m_selectionRenderer->Draw(camera, selectedObject, animationTime);
}
