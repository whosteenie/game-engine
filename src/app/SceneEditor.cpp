#include "app/SceneEditor.h"

#include <memory>

#include "app/Scene.h"
#include "engine/Camera.h"
#include "engine/Input.h"
#include "engine/SceneObject.h"
#include "engine/ScenePicker.h"
#include "engine/SceneHierarchy.h"
#include "engine/SelectionRenderer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <vector>

namespace
{
    constexpr float PickRepeatThresholdPixels = 8.0f;

    // Depth cycling always walks the full hit list at the click pixel (see PickSceneObjectCycling).
    // Without Ctrl the cycled object replaces the selection; with Ctrl it is toggled in/out.
    void ApplyViewportPickSelection(Scene& scene, int pickedIndex, bool ctrlHeld)
    {
        if (pickedIndex < 0)
        {
            scene.ClearSelection();
            return;
        }

        if (ctrlHeld)
        {
            scene.ToggleSelected(pickedIndex);
        }
        else
        {
            scene.SelectSingle(pickedIndex);
        }
    }

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

    ImGuizmo::MODE ToImGuizmoMode(TransformSpace space)
    {
        switch (space)
        {
        case TransformSpace::Local:
            return ImGuizmo::LOCAL;
        case TransformSpace::World:
            return ImGuizmo::WORLD;
        }

        return ImGuizmo::LOCAL;
    }

    void UpdateTransformGizmo(
        Scene& scene,
        const Camera& camera,
        TransformTool tool,
        TransformSpace space)
    {
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

        glm::mat4 gizmoWorldMatrix = scene.GetGizmoWorldMatrix(selectedIndex);
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();

        if (ImGuizmo::Manipulate(
                glm::value_ptr(viewMatrix),
                glm::value_ptr(projectionMatrix),
                ToImGuizmoOperation(tool),
                ToImGuizmoMode(space),
                glm::value_ptr(gizmoWorldMatrix)))
        {
            scene.ApplyGizmoWorldMatrix(selectedIndex, gizmoWorldMatrix);
        }
    }
}

SceneEditor::SceneEditor()
    : m_selectionRenderer(std::make_unique<SelectionRenderer>())
{
}

SceneEditor::~SceneEditor() = default;

void SceneEditor::SetTool(TransformTool tool)
{
    m_tool = tool;
}

TransformTool SceneEditor::GetTool() const
{
    return m_tool;
}

TransformSpace SceneEditor::GetTransformSpace() const
{
    return m_transformSpace;
}

void SceneEditor::SetTransformSpace(TransformSpace space)
{
    m_transformSpace = space;
}

void SceneEditor::Update(
    Scene& scene,
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

    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrlHeld = input.IsKeyDown(GLFW_KEY_LEFT_CONTROL)
        || input.IsKeyDown(GLFW_KEY_RIGHT_CONTROL)
        || io.KeyCtrl;
    if (ctrlHeld
        && input.WasKeyPressed(GLFW_KEY_D)
        && scene.HasSelection()
        && !io.WantTextInput
        && !ImGui::IsAnyItemActive())
    {
        scene.DuplicateObject(scene.GetSelectedObjectIndex());
    }

    const bool allowTransformShortcuts = allowKeyboardInput && !input.IsCapturingMouse();
    if (allowTransformShortcuts && input.WasKeyPressed(GLFW_KEY_W))
    {
        SetTool(TransformTool::Translate);
    }
    if (allowTransformShortcuts && input.WasKeyPressed(GLFW_KEY_E))
    {
        SetTool(TransformTool::Rotate);
    }
    if (allowTransformShortcuts && input.WasKeyPressed(GLFW_KEY_R))
    {
        SetTool(TransformTool::Scale);
    }
    if (allowTransformShortcuts && input.WasKeyPressed(GLFW_KEY_L))
    {
        SetTransformSpace(TransformSpace::Local);
    }
    if (allowTransformShortcuts && input.WasKeyPressed(GLFW_KEY_G))
    {
        SetTransformSpace(TransformSpace::World);
    }

    UpdateTransformGizmo(scene, camera, m_tool, m_transformSpace);

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

    const bool repeatClickAtSameSpot = m_hasLastPickScreenPosition &&
        glm::length(mousePosition - m_lastPickScreenPosition) <= PickRepeatThresholdPixels;
    m_lastPickScreenPosition = mousePosition;
    m_hasLastPickScreenPosition = true;

    const int pickedIndex = PickSceneObjectCycling(
        scene.GetObjects(),
        ray,
        scene.GetPrimarySelection(),
        repeatClickAtSameSpot);
    ApplyViewportPickSelection(scene, pickedIndex, ctrlHeld);
}

void SceneEditor::RenderSelectionOverlay(
    const Scene& scene,
    const Camera& camera,
    bool useScreenSpace) const
{
    const int selectedIndex = scene.GetSelectedObjectIndex();
    if (selectedIndex < 0)
    {
        return;
    }

    std::vector<SelectionMeshDraw> meshes;
    CollectRenderableSelectionMeshes(scene.GetObjects(), selectedIndex, meshes);
    if (meshes.empty())
    {
        return;
    }

    m_selectionRenderer->Draw(camera, meshes, useScreenSpace);
}
