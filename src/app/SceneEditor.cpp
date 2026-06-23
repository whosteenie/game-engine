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
    constexpr float MarqueeDragThresholdPixels = 5.0f;

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

    void ApplyMarqueeSelection(Scene& scene, const std::vector<int>& pickedIndices, bool ctrlHeld)
    {
        if (pickedIndices.empty())
        {
            if (!ctrlHeld)
            {
                scene.ClearSelection();
            }

            return;
        }

        if (ctrlHeld)
        {
            std::vector<int> mergedSelection = scene.GetSelection().indices;
            for (int objectIndex : pickedIndices)
            {
                if (!scene.IsSelected(objectIndex))
                {
                    mergedSelection.push_back(objectIndex);
                }
            }

            scene.SetSelection(mergedSelection, pickedIndices.back());
            return;
        }

        scene.SetSelection(pickedIndices, pickedIndices.back());
    }

    glm::vec2 FramebufferToImGuiPoint(
        const glm::vec2& framebufferPoint,
        int framebufferWidth,
        int framebufferHeight,
        int windowWidth,
        int windowHeight)
    {
        const float scaleX = framebufferWidth > 0
            ? static_cast<float>(windowWidth) / static_cast<float>(framebufferWidth)
            : 1.0f;
        const float scaleY = framebufferHeight > 0
            ? static_cast<float>(windowHeight) / static_cast<float>(framebufferHeight)
            : 1.0f;
        return glm::vec2(framebufferPoint.x * scaleX, framebufferPoint.y * scaleY);
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
        if (!scene.HasSelection())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

        const bool worldSpace = space == TransformSpace::World;
        glm::mat4 gizmoWorldMatrix = scene.GetSelectionGizmoWorldMatrix(worldSpace);
        const glm::mat4 gizmoWorldMatrixBefore = gizmoWorldMatrix;
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();

        if (ImGuizmo::Manipulate(
                glm::value_ptr(viewMatrix),
                glm::value_ptr(projectionMatrix),
                ToImGuizmoOperation(tool),
                ToImGuizmoMode(space),
                glm::value_ptr(gizmoWorldMatrix)))
        {
            scene.ApplySelectionGizmoWorldMatrix(gizmoWorldMatrixBefore, gizmoWorldMatrix);
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
    const ImGuiIO& io = ImGui::GetIO();

    if (allowKeyboardInput && input.WasKeyPressed(GLFW_KEY_DELETE) && scene.HasSelection())
    {
        scene.RemoveSelectedObjects();
    }

    const bool ctrlHeld = input.IsKeyDown(GLFW_KEY_LEFT_CONTROL)
        || input.IsKeyDown(GLFW_KEY_RIGHT_CONTROL)
        || io.KeyCtrl;
    if (ctrlHeld
        && input.WasKeyPressed(GLFW_KEY_D)
        && scene.HasSelection()
        && !io.WantTextInput
        && !ImGui::IsAnyItemActive())
    {
        scene.DuplicateSelectedObjects();
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
    if (!m_trackingLeftDrag && (!allowMouseInput || gizmoCapturingMouse))
    {
        return;
    }

    const glm::vec2 mousePosition = input.GetCursorPositionFramebufferScaled(
        framebufferWidth,
        framebufferHeight,
        windowWidth,
        windowHeight);
    const glm::vec2 viewportSize(static_cast<float>(framebufferWidth), static_cast<float>(framebufferHeight));
    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();

    if (input.WasMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT))
    {
        m_trackingLeftDrag = true;
        m_marqueeActive = false;
        m_dragStartFramebuffer = mousePosition;
        m_dragCurrentFramebuffer = mousePosition;
    }

    if (m_trackingLeftDrag && input.IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT))
    {
        m_dragCurrentFramebuffer = mousePosition;
        if (!m_marqueeActive
            && glm::length(m_dragCurrentFramebuffer - m_dragStartFramebuffer) >= MarqueeDragThresholdPixels)
        {
            m_marqueeActive = true;
        }

        if (m_marqueeActive)
        {
            DrawMarqueeOverlay(framebufferWidth, framebufferHeight, windowWidth, windowHeight);
        }
    }

    if (input.WasMouseButtonReleased(GLFW_MOUSE_BUTTON_LEFT) && m_trackingLeftDrag)
    {
        if (m_marqueeActive)
        {
            const std::vector<int> pickedIndices = PickObjectsInScreenRect(
                scene.GetObjects(),
                m_dragStartFramebuffer,
                m_dragCurrentFramebuffer,
                viewportSize,
                viewMatrix,
                projectionMatrix);
            ApplyMarqueeSelection(scene, pickedIndices, ctrlHeld);
        }
        else
        {
            const Ray ray = ScreenPointToRay(mousePosition, viewportSize, viewMatrix, projectionMatrix);
            const bool repeatClickAtSameSpot = m_hasLastPickScreenPosition
                && glm::length(mousePosition - m_lastPickScreenPosition) <= PickRepeatThresholdPixels;
            m_lastPickScreenPosition = mousePosition;
            m_hasLastPickScreenPosition = true;

            const int pickedIndex = PickSceneObjectCycling(
                scene.GetObjects(),
                ray,
                scene.GetPrimarySelection(),
                repeatClickAtSameSpot);
            ApplyViewportPickSelection(scene, pickedIndex, ctrlHeld);
        }

        CancelMarqueeDrag();
    }
}

void SceneEditor::HandleEscapeKey(Scene& scene)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput
        || ImGui::IsAnyItemActive()
        || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId))
    {
        return;
    }

    if (m_trackingLeftDrag || m_marqueeActive)
    {
        CancelMarqueeDrag();
    }
    else if (scene.HasSelection())
    {
        scene.ClearSelection();
    }
}

void SceneEditor::CancelMarqueeDrag()
{
    m_trackingLeftDrag = false;
    m_marqueeActive = false;
}

void SceneEditor::DrawMarqueeOverlay(
    int framebufferWidth,
    int framebufferHeight,
    int windowWidth,
    int windowHeight) const
{
    if (!m_marqueeActive)
    {
        return;
    }

    const glm::vec2 startImGui = FramebufferToImGuiPoint(
        m_dragStartFramebuffer,
        framebufferWidth,
        framebufferHeight,
        windowWidth,
        windowHeight);
    const glm::vec2 endImGui = FramebufferToImGuiPoint(
        m_dragCurrentFramebuffer,
        framebufferWidth,
        framebufferHeight,
        windowWidth,
        windowHeight);

    const ImVec2 rectMin(
        std::min(startImGui.x, endImGui.x),
        std::min(startImGui.y, endImGui.y));
    const ImVec2 rectMax(
        std::max(startImGui.x, endImGui.x),
        std::max(startImGui.y, endImGui.y));

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->AddRectFilled(
        rectMin,
        rectMax,
        IM_COL32(90, 150, 255, 40));
    drawList->AddRect(
        rectMin,
        rectMax,
        IM_COL32(90, 150, 255, 220),
        0.0f,
        0,
        1.5f);
}

void SceneEditor::RenderSelectionOverlay(
    const Scene& scene,
    const Camera& camera,
    bool useScreenSpace) const
{
    if (!scene.HasSelection())
    {
        return;
    }

    std::vector<SelectionMeshDraw> meshes;
    CollectSelectionMeshes(scene.GetObjects(), scene.GetSelection().indices, meshes);
    if (meshes.empty())
    {
        return;
    }

    m_selectionRenderer->Draw(camera, meshes, useScreenSpace);
}
