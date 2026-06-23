#include "app/SceneEditor.h"

#include <memory>

#include "app/EditorViewportRect.h"
#include "app/Scene.h"
#include "app/UndoCommand.h"
#include "app/UndoStack.h"
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

    glm::vec2 ViewportPickPixelsToImGui(const glm::vec2& localPixels, const EditorViewportRect& viewport)
    {
        const float scaleX =
            viewport.width > 0 ? viewport.screenWidth / static_cast<float>(viewport.width) : 1.0f;
        const float scaleY =
            viewport.height > 0 ? viewport.screenHeight / static_cast<float>(viewport.height) : 1.0f;
        return glm::vec2(
            viewport.screenX + localPixels.x * scaleX,
            viewport.screenY + localPixels.y * scaleY);
    }

    glm::vec2 GetViewportLocalMouseScreen(const EditorViewportRect& viewport)
    {
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        return glm::vec2(mousePos.x - viewport.screenX, mousePos.y - viewport.screenY);
    }

    glm::vec2 ScreenLocalToPickPixels(const glm::vec2& localScreen, const EditorViewportRect& viewport)
    {
        const float scaleX =
            viewport.screenWidth > 0.0f
                ? static_cast<float>(viewport.width) / viewport.screenWidth
                : 1.0f;
        const float scaleY =
            viewport.screenHeight > 0.0f
                ? static_cast<float>(viewport.height) / viewport.screenHeight
                : 1.0f;
        return glm::vec2(localScreen.x * scaleX, localScreen.y * scaleY);
    }

    bool IsInsideViewportScreen(const glm::vec2& localScreen, const EditorViewportRect& viewport)
    {
        return localScreen.x >= 0.0f
            && localScreen.y >= 0.0f
            && localScreen.x <= viewport.screenWidth
            && localScreen.y <= viewport.screenHeight;
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

    const char* GetGizmoCommandName(TransformTool tool);

    void UpdateTransformGizmo(
        Scene& scene,
        const Camera& camera,
        TransformTool tool,
        TransformSpace space,
        UndoStack* undoStack,
        bool& gizmoWasUsing,
        ObjectTransformMap& gizmoTransformBefore,
        const EditorViewportRect* viewport)
    {
        if (viewport == nullptr || !viewport->valid)
        {
            gizmoWasUsing = false;
            gizmoTransformBefore.clear();
            return;
        }

        if (!scene.HasSelection())
        {
            gizmoWasUsing = false;
            gizmoTransformBefore.clear();
            return;
        }

        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetAlternativeWindow(viewport->imguiWindow);
        ImGuizmo::SetRect(
            viewport->screenX,
            viewport->screenY,
            viewport->screenWidth,
            viewport->screenHeight);

        const bool worldSpace = space == TransformSpace::World;
        glm::mat4 gizmoWorldMatrix = scene.GetSelectionGizmoWorldMatrix(worldSpace);
        const glm::mat4 gizmoWorldMatrixBefore = gizmoWorldMatrix;
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();

        const bool wasUsing = gizmoWasUsing;
        ObjectTransformMap frameStartTransforms;
        if (!wasUsing && undoStack != nullptr)
        {
            frameStartTransforms =
                CaptureLocalTransforms(scene, scene.GetSelection().indices);
        }

        if (ImGuizmo::Manipulate(
                glm::value_ptr(viewMatrix),
                glm::value_ptr(projectionMatrix),
                ToImGuizmoOperation(tool),
                ToImGuizmoMode(space),
                glm::value_ptr(gizmoWorldMatrix)))
        {
            scene.ApplySelectionGizmoWorldMatrix(gizmoWorldMatrixBefore, gizmoWorldMatrix);
        }

        const bool isUsing = ImGuizmo::IsUsing();
        if (isUsing && !wasUsing && undoStack != nullptr)
        {
            gizmoTransformBefore = std::move(frameStartTransforms);
        }

        if (!isUsing && wasUsing && undoStack != nullptr && !gizmoTransformBefore.empty())
        {
            const ObjectTransformMap after =
                CaptureLocalTransforms(scene, scene.GetSelection().indices);
            PushTransformObjects(
                *undoStack,
                std::move(gizmoTransformBefore),
                std::move(after),
                GetGizmoCommandName(tool));
            gizmoTransformBefore.clear();
        }

        gizmoWasUsing = isUsing;
    }

    const char* GetGizmoCommandName(TransformTool tool)
    {
        switch (tool)
        {
        case TransformTool::Translate:
            return "Move";
        case TransformTool::Rotate:
            return "Rotate";
        case TransformTool::Scale:
            return "Scale";
        }

        return "Transform";
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
    bool allowKeyboardInput,
    UndoStack* undoStack,
    const std::string& projectRoot,
    const EditorViewportRect* viewport)
{
    const ImGuiIO& io = ImGui::GetIO();

    const bool editShortcutsAllowed =
        allowKeyboardInput && !io.WantTextInput && !ImGui::IsAnyItemActive();
    const bool ctrlHeld = input.IsKeyDown(GLFW_KEY_LEFT_CONTROL)
        || input.IsKeyDown(GLFW_KEY_RIGHT_CONTROL)
        || io.KeyCtrl;

    if (allowKeyboardInput && input.WasKeyPressed(GLFW_KEY_DELETE) && scene.HasSelection())
    {
        if (undoStack != nullptr)
        {
            PushDeleteSelection(*undoStack, scene, "Delete");
        }
        else
        {
            scene.RemoveSelectedObjects();
        }
    }

    if (editShortcutsAllowed
        && ctrlHeld
        && input.WasKeyPressed(GLFW_KEY_D)
        && scene.HasSelection())
    {
        if (undoStack != nullptr)
        {
            PushInsertSubtree(*undoStack, scene, "Duplicate", [](Scene& target) {
                return target.DuplicateSelectedObjects();
            });
        }
        else
        {
            scene.DuplicateSelectedObjects();
        }
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

    UpdateTransformGizmo(
        scene,
        camera,
        m_tool,
        m_transformSpace,
        undoStack,
        m_gizmoWasUsing,
        m_gizmoTransformBefore,
        viewport);

    const bool gizmoCapturingMouse = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (viewport == nullptr || !viewport->valid)
    {
        if (m_trackingLeftDrag)
        {
            CancelMarqueeDrag();
        }

        return;
    }

    const glm::vec2 localMouseScreen = GetViewportLocalMouseScreen(*viewport);
    const glm::vec2 localMouse = ScreenLocalToPickPixels(localMouseScreen, *viewport);
    const glm::vec2 viewportSize(
        static_cast<float>(viewport->width),
        static_cast<float>(viewport->height));
    const bool insideViewport = IsInsideViewportScreen(localMouseScreen, *viewport);

    if (!m_trackingLeftDrag && (!allowMouseInput || gizmoCapturingMouse || !insideViewport))
    {
        return;
    }

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();

    if (input.WasMouseButtonPressed(GLFW_MOUSE_BUTTON_LEFT) && insideViewport)
    {
        m_trackingLeftDrag = true;
        m_marqueeActive = false;
        m_dragStartFramebuffer = localMouse;
        m_dragCurrentFramebuffer = localMouse;
    }

    if (m_trackingLeftDrag && input.IsMouseButtonDown(GLFW_MOUSE_BUTTON_LEFT))
    {
        m_dragCurrentFramebuffer = localMouse;
        if (!m_marqueeActive
            && glm::length(m_dragCurrentFramebuffer - m_dragStartFramebuffer) >= MarqueeDragThresholdPixels)
        {
            m_marqueeActive = true;
        }

        if (m_marqueeActive)
        {
            DrawMarqueeOverlay(*viewport);
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
        else if (insideViewport)
        {
            const Ray ray = ScreenPointToRay(localMouse, viewportSize, viewMatrix, projectionMatrix);
            const bool repeatClickAtSameSpot = m_hasLastPickScreenPosition
                && glm::length(localMouse - m_lastPickScreenPosition) <= PickRepeatThresholdPixels;
            m_lastPickScreenPosition = localMouse;
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

void SceneEditor::DrawMarqueeOverlay(const EditorViewportRect& viewport) const
{
    if (!m_marqueeActive)
    {
        return;
    }

    const glm::vec2 startImGui = ViewportPickPixelsToImGui(m_dragStartFramebuffer, viewport);
    const glm::vec2 endImGui = ViewportPickPixelsToImGui(m_dragCurrentFramebuffer, viewport);

    const ImVec2 rectMin(
        std::min(startImGui.x, endImGui.x),
        std::min(startImGui.y, endImGui.y));
    const ImVec2 rectMax(
        std::max(startImGui.x, endImGui.x),
        std::max(startImGui.y, endImGui.y));
    const ImVec2 clipMin(viewport.screenX, viewport.screenY);
    const ImVec2 clipMax(
        viewport.screenX + viewport.screenWidth,
        viewport.screenY + viewport.screenHeight);

    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    drawList->PushClipRect(clipMin, clipMax, true);
    drawList->AddRectFilled(
        ImVec2(rectMin.x, rectMin.y),
        ImVec2(rectMax.x, rectMax.y),
        IM_COL32(90, 150, 255, 40));
    drawList->AddRect(
        ImVec2(rectMin.x, rectMin.y),
        ImVec2(rectMax.x, rectMax.y),
        IM_COL32(90, 150, 255, 220),
        0.0f,
        0,
        1.5f);
    drawList->PopClipRect();
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
