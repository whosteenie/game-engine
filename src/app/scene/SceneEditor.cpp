#include "app/scene/SceneEditor.h"

#include <memory>

#include "app/editor/EditorViewportRect.h"
#include "app/scene/Scene.h"
#include "app/undo/UndoCommand.h"
#include "app/undo/UndoStack.h"
#include "engine/camera/Camera.h"
#include "engine/platform/Input.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/ScenePicker.h"
#include "engine/scene/SceneHierarchy.h"
#include "engine/gizmos/SelectionRenderer.h"

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <ImGuizmo.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <limits>
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

    bool TryGetSelectionWorldAabb(
        const Scene& scene,
        glm::vec3& boundsMin,
        glm::vec3& boundsMax)
    {
        const std::vector<int>& selectedIndices = scene.GetSelection().indices;
        if (selectedIndices.empty())
        {
            return false;
        }

        boundsMin = glm::vec3(std::numeric_limits<float>::max());
        boundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        for (int objectIndex : selectedIndices)
        {
            glm::vec3 objectMin;
            glm::vec3 objectMax;
            scene.GetWorldBounds(objectIndex, objectMin, objectMax);
            boundsMin = glm::min(boundsMin, objectMin);
            boundsMax = glm::max(boundsMax, objectMax);
        }

        return true;
    }

    glm::vec3 ComputeBoundsSurfaceRestTranslation(
        const glm::vec3& boundsMin,
        const glm::vec3& boundsMax,
        const glm::vec3& hitPoint,
        const glm::vec3& hitNormal)
    {
        const float normalLength = glm::length(hitNormal);
        if (normalLength < 1e-8f)
        {
            return glm::vec3(0.0f);
        }

        const glm::vec3 normal = hitNormal / normalLength;
        const glm::vec3 corners[8] = {
            {boundsMin.x, boundsMin.y, boundsMin.z},
            {boundsMax.x, boundsMin.y, boundsMin.z},
            {boundsMin.x, boundsMax.y, boundsMin.z},
            {boundsMax.x, boundsMax.y, boundsMin.z},
            {boundsMin.x, boundsMin.y, boundsMax.z},
            {boundsMax.x, boundsMin.y, boundsMax.z},
            {boundsMin.x, boundsMax.y, boundsMax.z},
            {boundsMax.x, boundsMax.y, boundsMax.z},
        };

        float minProjection = std::numeric_limits<float>::max();
        for (const glm::vec3& corner : corners)
        {
            minProjection = std::min(minProjection, glm::dot(corner - hitPoint, normal));
        }

        // Push the AABB along the outward normal so its support plane sits on the hit.
        return -minProjection * normal;
    }

    void UpdateTransformGizmo(
        Scene& scene,
        const Camera& camera,
        TransformTool tool,
        TransformSpace space,
        UndoStack* undoStack,
        bool& gizmoWasUsing,
        ObjectTransformMap& gizmoTransformBefore,
        glm::mat4& gizmoUnsnappedMatrix,
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
        const glm::mat4 displayedMatrix = scene.GetSelectionGizmoWorldMatrix(worldSpace);
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 projectionMatrix = camera.GetUnjitteredProjectionMatrix();
        const bool altHeld = ImGui::GetIO().KeyAlt;

        const bool wasUsing = gizmoWasUsing;
        ObjectTransformMap frameStartTransforms;
        if (!wasUsing)
        {
            gizmoUnsnappedMatrix = displayedMatrix;
            if (undoStack != nullptr)
            {
                frameStartTransforms =
                    CaptureLocalTransforms(scene, scene.GetSelection().indices);
            }
        }

        glm::mat4 manipulateMatrix = wasUsing ? gizmoUnsnappedMatrix : displayedMatrix;
        ImGuizmo::Manipulate(
            glm::value_ptr(viewMatrix),
            glm::value_ptr(projectionMatrix),
            ToImGuizmoOperation(tool),
            ToImGuizmoMode(space),
            glm::value_ptr(manipulateMatrix));

        const bool isUsing = ImGuizmo::IsUsing();
        const bool freeMove =
            tool == TransformTool::Translate
            && isUsing
            && ImGuizmo::GetActiveHandleType() == ImGuizmo::MT_MOVE_SCREEN;

        if (isUsing)
        {
            gizmoUnsnappedMatrix = manipulateMatrix;
        }

        glm::mat4 desiredMatrix = manipulateMatrix;
        if (freeMove && altHeld)
        {
            glm::mat4 currentMatrix = scene.GetSelectionGizmoWorldMatrix(worldSpace);
            scene.ApplySelectionGizmoWorldMatrix(currentMatrix, manipulateMatrix);

            const glm::vec2 localMouseScreen = GetViewportLocalMouseScreen(*viewport);
            const glm::vec2 localMouse = ScreenLocalToPickPixels(localMouseScreen, *viewport);
            const glm::vec2 viewportSize(
                static_cast<float>(viewport->width),
                static_cast<float>(viewport->height));
            const Ray ray = ScreenPointToRay(
                localMouse,
                viewportSize,
                viewMatrix,
                projectionMatrix);

            SurfaceHit hit;
            if (RaycastClosestSurface(
                    scene.GetObjects(),
                    ray,
                    scene.GetSelection().indices,
                    hit))
            {
                glm::vec3 boundsMin;
                glm::vec3 boundsMax;
                if (TryGetSelectionWorldAabb(scene, boundsMin, boundsMax))
                {
                    const glm::vec3 translation = ComputeBoundsSurfaceRestTranslation(
                        boundsMin,
                        boundsMax,
                        hit.point,
                        hit.normal);
                    desiredMatrix = manipulateMatrix;
                    desiredMatrix[3] += glm::vec4(translation, 0.0f);
                }
            }
        }

        if (isUsing)
        {
            const glm::mat4 currentMatrix = scene.GetSelectionGizmoWorldMatrix(worldSpace);
            scene.ApplySelectionGizmoWorldMatrix(currentMatrix, desiredMatrix);
        }

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
    const EditorViewportRect* viewport)
{
    const ImGuiIO& io = ImGui::GetIO();

    const bool editShortcutsAllowed =
        allowKeyboardInput && !io.WantTextInput && !ImGui::IsAnyItemActive();
    const bool ctrlHeld = input.IsKeyDown(GLFW_KEY_LEFT_CONTROL)
        || input.IsKeyDown(GLFW_KEY_RIGHT_CONTROL)
        || io.KeyCtrl;
    const bool shiftHeld = input.IsKeyDown(GLFW_KEY_LEFT_SHIFT)
        || input.IsKeyDown(GLFW_KEY_RIGHT_SHIFT)
        || io.KeyShift;

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
    if (allowTransformShortcuts && !ctrlHeld && !shiftHeld && input.WasKeyPressed(GLFW_KEY_F))
    {
        glm::vec3 focus;
        float radius = 0.5f;
        if (scene.TryGetViewFocusPoint(focus, radius))
        {
            camera.FrameTarget(focus, radius);
        }
    }

    UpdateTransformGizmo(
        scene,
        camera,
        m_tool,
        m_transformSpace,
        undoStack,
        m_gizmoWasUsing,
        m_gizmoTransformBefore,
        m_gizmoUnsnappedMatrix,
        viewport);

    const bool gizmoCapturingMouse =
        ImGuizmo::IsOver()
        || ImGuizmo::IsUsing()
        || ImGuizmo::IsViewManipulateHovered()
        || ImGuizmo::IsUsingViewManipulate();
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
    const glm::mat4 projectionMatrix = camera.GetUnjitteredProjectionMatrix();

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

void SceneEditor::ResetInteractionState()
{
    CancelMarqueeDrag();
    m_gizmoWasUsing = false;
    m_gizmoTransformBefore.clear();
    m_gizmoUnsnappedMatrix = glm::mat4(1.0f);
    m_hasLastPickScreenPosition = false;
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
