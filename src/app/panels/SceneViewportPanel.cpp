#include "app/panels/SceneViewportPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/ModelDragDrop.h"
#include "app/project/ProjectSession.h"
#include "app/project/SceneSubtreeArchive.h"
#include "app/scene/document/Scene.h"
#include "app/scene/import/SceneImportService.h"
#include "app/undo/UndoCommand.h"
#include "app/undo/UndoStack.h"
#include "engine/camera/Camera.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/scene/ScenePicker.h"

#include <ImGuizmo.h>
#include <imgui.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace
{
    constexpr float kDropFallbackDistance = 8.0f;
    constexpr double kFlySpeedOverlayVisibleSeconds = 1.8;
    constexpr double kFlySpeedOverlayFadeInSeconds = 0.12;
    constexpr double kFlySpeedOverlayFadeOutSeconds = 0.5;

    glm::vec2 ScreenLocalToPickPixels(const glm::vec2& localScreen, const ViewportRect& viewport)
    {
        const float scaleX = viewport.screenWidth > 0.0f
            ? static_cast<float>(viewport.width) / viewport.screenWidth
            : 1.0f;
        const float scaleY = viewport.screenHeight > 0.0f
            ? static_cast<float>(viewport.height) / viewport.screenHeight
            : 1.0f;
        return glm::vec2(localScreen.x * scaleX, localScreen.y * scaleY);
    }

    void CollectSubtreeIndices(const Scene& scene, int objectIndex, std::vector<int>& outIndices)
    {
        outIndices.push_back(objectIndex);
        for (const int childIndex : scene.GetChildren(objectIndex))
        {
            CollectSubtreeIndices(scene, childIndex, outIndices);
        }
    }

    bool TryGetSubtreeWorldBounds(
        const Scene& scene,
        int rootIndex,
        glm::vec3& outBoundsMin,
        glm::vec3& outBoundsMax)
    {
        std::vector<int> subtreeIndices;
        CollectSubtreeIndices(scene, rootIndex, subtreeIndices);

        bool foundMesh = false;
        outBoundsMin = glm::vec3(std::numeric_limits<float>::max());
        outBoundsMax = glm::vec3(std::numeric_limits<float>::lowest());
        for (const int objectIndex : subtreeIndices)
        {
            if (!scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).HasMesh())
            {
                continue;
            }

            glm::vec3 boundsMin(0.0f);
            glm::vec3 boundsMax(0.0f);
            scene.GetWorldBounds(objectIndex, boundsMin, boundsMax);
            outBoundsMin = glm::min(outBoundsMin, boundsMin);
            outBoundsMax = glm::max(outBoundsMax, boundsMax);
            foundMesh = true;
        }

        return foundMesh;
    }

    glm::vec3 AabbSupportPoint(
        const glm::vec3& boundsMin,
        const glm::vec3& boundsMax,
        const glm::vec3& direction)
    {
        const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
        const glm::vec3 extents = (boundsMax - boundsMin) * 0.5f;
        return center + extents * glm::sign(direction);
    }

    void PlaceImportedModel(
        Scene& scene,
        int rootIndex,
        const Ray& ray,
        const std::vector<int>& importedRoots)
    {
        glm::vec3 boundsMin(0.0f);
        glm::vec3 boundsMax(0.0f);
        const bool hasBounds = TryGetSubtreeWorldBounds(scene, rootIndex, boundsMin, boundsMax);

        glm::vec3 targetPoint = ray.origin + ray.direction * kDropFallbackDistance;
        glm::vec3 translation(0.0f);
        SurfaceHit hit;
        if (RaycastClosestSurface(scene.GetObjects(), ray, importedRoots, hit)
            && glm::dot(hit.normal, -ray.direction) > 0.05f)
        {
            targetPoint = hit.point;
            if (hasBounds)
            {
                const glm::vec3 normal = glm::normalize(hit.normal);
                translation = targetPoint - AabbSupportPoint(boundsMin, boundsMax, -normal);
            }
        }
        else if (hasBounds)
        {
            translation = targetPoint - (boundsMin + boundsMax) * 0.5f;
        }

        glm::mat4 rootWorld = scene.GetWorldMatrix(rootIndex);
        rootWorld[3] += glm::vec4(translation, 0.0f);
        scene.SetObjectWorldMatrix(rootIndex, rootWorld);
    }
}

bool SceneViewportPanel::HasValidRenderTarget() const
{
    return OffscreenViewportPanel::HasValidRenderTarget(m_viewport);
}

std::uintptr_t SceneViewportPanel::GetFramebuffer() const
{
    return OffscreenViewportPanel::GetFramebuffer(m_viewport);
}

std::uintptr_t SceneViewportPanel::GetColorTexture() const
{
    return OffscreenViewportPanel::GetColorTexture(m_viewport);
}

void SceneViewportPanel::EnsureFramebufferSized() const
{
    OffscreenViewportPanel::EnsureFramebufferSized(m_viewport);
}

void SceneViewportPanel::EnsureBenchmarkRenderTarget(const int width, const int height)
{
    m_viewport.showPanel = true;
    m_viewport.resizeStabilizer.Reset();
    m_viewport.renderWidth = std::max(width, 1);
    m_viewport.renderHeight = std::max(height, 1);
    m_viewport.framebuffer.Resize(m_viewport.renderWidth, m_viewport.renderHeight);
}

void SceneViewportPanel::ClearRenderTarget() const
{
    OffscreenViewportPanel::ClearRenderTarget(m_viewport);
}

void SceneViewportPanel::DrawViewGizmo(
    Camera& camera,
    const Scene& scene,
    const ImVec2& imageMin,
    const ImVec2& imageMax)
{
    if (!m_viewport.interactionRect.valid || m_viewport.interactionRect.imguiWindow == nullptr)
    {
        return;
    }

    constexpr float kGizmoSize = 128.0f;
    constexpr float kMargin = 8.0f;
    const ImVec2 gizmoPos(
        imageMax.x - kGizmoSize - kMargin,
        imageMin.y + kMargin);
    const ImVec2 gizmoSize(kGizmoSize, kGizmoSize);

    ImGuizmo::SetAlternativeWindow(m_viewport.interactionRect.imguiWindow);
    ImGuizmo::SetRect(
        m_viewport.interactionRect.screenX,
        m_viewport.interactionRect.screenY,
        m_viewport.interactionRect.screenWidth,
        m_viewport.interactionRect.screenHeight);

    glm::vec3 focus(0.0f);
    float focusRadius = 0.5f;
    if (!scene.TryGetViewFocusPoint(focus, focusRadius))
    {
        focus = glm::vec3(0.0f);
    }

    const bool wasUsingViewManipulate = m_wasUsingViewManipulate;

    glm::mat4 view = camera.GetViewMatrix();
    float orbitLength = std::max(glm::length(camera.GetPosition() - focus), 1.0f);
    if (wasUsingViewManipulate)
    {
        view = camera.BuildViewMatrixLookingAt(m_viewManipulateFocus, m_viewManipulateDistance);
        orbitLength = m_viewManipulateDistance;
    }

    ImGuizmo::ViewManipulate(
        glm::value_ptr(view),
        orbitLength,
        gizmoPos,
        gizmoSize,
        IM_COL32(0, 0, 0, 0));

    const bool usingViewManipulate = ImGuizmo::IsUsingViewManipulate();
    if (usingViewManipulate && !wasUsingViewManipulate)
    {
        m_viewManipulateFocus = focus;
        m_viewManipulateDistance = std::max(glm::length(camera.GetPosition() - focus), 1.0f);
    }

    if (usingViewManipulate)
    {
        camera.ApplyViewManipulateResult(view, m_viewManipulateFocus, m_viewManipulateDistance);
    }

    m_wasUsingViewManipulate = usingViewManipulate;
}

void SceneViewportPanel::DrawModelDropTarget(
    Camera& camera,
    Scene& scene,
    ProjectSession& project,
    UndoStack& undoStack)
{
    const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
    if (m_modelDropPreviewRootId != kInvalidSceneObjectId
        && (activePayload == nullptr
            || !activePayload->IsDataType(ModelDragDrop::kModelFilePayload)))
    {
        CancelModelDropPreview(scene);
    }

    if (!ImGui::BeginDragDropTarget())
    {
        return;
    }

    const ImGuiDragDropFlags acceptFlags =
        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
    if (const ImGuiPayload* payload =
            ImGui::AcceptDragDropPayload(ModelDragDrop::kModelFilePayload, acceptFlags))
    {
        if (payload->Data != nullptr && payload->DataSize > 1)
        {
            const std::string modelPath(static_cast<const char*>(payload->Data));
            std::vector<int> previewRoots;
            if (m_modelDropPreviewRootId == kInvalidSceneObjectId)
            {
                const ArchivedSelectionState selectionBefore = CaptureArchivedSelection(scene);
                m_modelDropPreviewSelectionBeforeIds = selectionBefore.ids;
                m_modelDropPreviewSelectionBeforePrimary = selectionBefore.primary;
                scene.SetDirtyNotificationsSuppressed(true);
                previewRoots = scene.ImportModel(
                    modelPath,
                    -1,
                    project.GetProjectRootDirectory(),
                    true);
                scene.SetDirtyNotificationsSuppressed(false);
                if (!previewRoots.empty())
                {
                    m_modelDropPreviewRootId =
                        scene.GetSceneObject(static_cast<std::size_t>(previewRoots.front())).GetId();
                }
                else if (!scene.GetImportService().GetLastImportError().empty())
                {
                    project.SetStatusMessage(scene.GetImportService().GetLastImportError());
                }
            }
            else
            {
                const int rootIndex = scene.FindObjectIndex(m_modelDropPreviewRootId);
                if (rootIndex >= 0)
                {
                    previewRoots.push_back(rootIndex);
                }
            }

            if (!previewRoots.empty())
            {
                UpdateModelDropPreview(camera, scene, previewRoots);
            }

            if (payload->IsDelivery() && m_modelDropPreviewRootId != kInvalidSceneObjectId)
            {
                const int rootIndex = scene.FindObjectIndex(m_modelDropPreviewRootId);
                SceneSubtreeArchive archive;
                if (rootIndex >= 0 && scene.CreateDeleteArchive({rootIndex}, archive))
                {
                    scene.SelectSingle(rootIndex);
                    archive.selectionBefore = {
                        m_modelDropPreviewSelectionBeforeIds,
                        m_modelDropPreviewSelectionBeforePrimary};
                    archive.selectionAfter = CaptureArchivedSelection(scene);
                    undoStack.Push(std::make_unique<InsertSubtreeCommand>(
                        std::move(archive),
                        "Drop Model"));
                    scene.MarkDirty();
                    project.SetStatusMessage("Model added to scene.");
                }
                else
                {
                    CancelModelDropPreview(scene);
                }

                m_modelDropPreviewRootId = kInvalidSceneObjectId;
                m_modelDropPreviewSelectionBeforeIds.clear();
                m_modelDropPreviewSelectionBeforePrimary = kInvalidSceneObjectId;
            }
        }
    }

    ImGui::EndDragDropTarget();
}

void SceneViewportPanel::NotifyFlySpeedChanged(const float speed)
{
    const double now = ImGui::GetTime();
    const bool overlayAlreadyVisible =
        now - m_flySpeedOverlayChangedAt < kFlySpeedOverlayVisibleSeconds;
    m_flySpeedOverlayValue = speed;
    m_flySpeedOverlayStartedAt = overlayAlreadyVisible
        ? now - kFlySpeedOverlayFadeInSeconds
        : now;
    m_flySpeedOverlayChangedAt = now;
}

void SceneViewportPanel::DrawFlySpeedOverlay(
    const ImVec2& imageMin,
    const ImVec2& imageMax) const
{
    const double now = ImGui::GetTime();
    const float timeSinceChange = static_cast<float>(now - m_flySpeedOverlayChangedAt);
    if (timeSinceChange < 0.0f
        || timeSinceChange >= static_cast<float>(kFlySpeedOverlayVisibleSeconds))
    {
        return;
    }

    const float timeSinceStart = static_cast<float>(now - m_flySpeedOverlayStartedAt);
    const float fadeIn = std::clamp(
        timeSinceStart / static_cast<float>(kFlySpeedOverlayFadeInSeconds),
        0.0f,
        1.0f);
    const float fadeOut = std::clamp(
        (static_cast<float>(kFlySpeedOverlayVisibleSeconds) - timeSinceChange)
            / static_cast<float>(kFlySpeedOverlayFadeOutSeconds),
        0.0f,
        1.0f);
    const float alpha = std::min(fadeIn, fadeOut);

    char label[64]{};
    std::snprintf(label, sizeof(label), "Fly speed  %5.2fx", m_flySpeedOverlayValue);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    constexpr float kOverlayWidth = 156.0f;
    constexpr float kVerticalPadding = 8.0f;
    const ImVec2 boxMin(
        (imageMin.x + imageMax.x - kOverlayWidth) * 0.5f,
        imageMin.y + 18.0f);
    const ImVec2 boxMax(
        boxMin.x + kOverlayWidth,
        boxMin.y + textSize.y + kVerticalPadding * 2.0f);

    ImDrawList* drawList = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
    drawList->AddRectFilled(
        boxMin,
        boxMax,
        IM_COL32(12, 16, 22, static_cast<int>(220.0f * alpha)),
        6.0f);
    drawList->AddRect(
        boxMin,
        boxMax,
        IM_COL32(105, 165, 230, static_cast<int>(210.0f * alpha)),
        6.0f);
    drawList->AddText(
        ImVec2(
            boxMin.x + (kOverlayWidth - textSize.x) * 0.5f,
            boxMin.y + kVerticalPadding),
        IM_COL32(240, 245, 252, static_cast<int>(255.0f * alpha)),
        label);
}

void SceneViewportPanel::UpdateModelDropPreview(
    Camera& camera,
    Scene& scene,
    const std::vector<int>& previewRoots)
{
    const ViewportRect& viewport = m_viewport.interactionRect;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const glm::vec2 localMouse(mouse.x - viewport.screenX, mouse.y - viewport.screenY);
    const Ray dropRay = ScreenPointToRay(
        ScreenLocalToPickPixels(localMouse, viewport),
        glm::vec2(static_cast<float>(viewport.width), static_cast<float>(viewport.height)),
        camera.GetViewMatrix(),
        camera.GetUnjitteredProjectionMatrix());

    scene.SetDirtyNotificationsSuppressed(true);
    PlaceImportedModel(scene, previewRoots.front(), dropRay, previewRoots);
    scene.SetDirtyNotificationsSuppressed(false);
}

void SceneViewportPanel::CancelModelDropPreview(Scene& scene)
{
    const int rootIndex = scene.FindObjectIndex(m_modelDropPreviewRootId);
    if (rootIndex >= 0)
    {
        scene.SetDirtyNotificationsSuppressed(true);
        scene.RemoveObject(static_cast<std::size_t>(rootIndex));
        scene.SetDirtyNotificationsSuppressed(false);
    }

    ApplyArchivedSelection(
        scene,
        {m_modelDropPreviewSelectionBeforeIds, m_modelDropPreviewSelectionBeforePrimary});
    m_modelDropPreviewRootId = kInvalidSceneObjectId;
    m_modelDropPreviewSelectionBeforeIds.clear();
    m_modelDropPreviewSelectionBeforePrimary = kInvalidSceneObjectId;
}

void SceneViewportPanel::Draw(
    Camera& camera,
    Scene& scene,
    ProjectSession& project,
    UndoStack& undoStack,
    const bool willRenderThisFrame)
{
    OffscreenViewportPanel::ResetFrameState(m_viewport);

    EditorPanelConstraints::ApplySceneViewPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Scene View", m_viewport.showPanel))
    {
        if (m_modelDropPreviewRootId != kInvalidSceneObjectId)
        {
            CancelModelDropPreview(scene);
        }
        if (!m_viewport.showPanel)
        {
            OffscreenViewportPanel::OnPanelHidden(m_viewport);
        }
        m_wasUsingViewManipulate = false;
        return;
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    OffscreenViewportPanel::UpdateRenderSize(m_viewport, available);

    const bool canCompositeFrame =
        OffscreenViewportPanel::CanCompositeFrame(m_viewport, willRenderThisFrame);
    const OffscreenViewportPanel::ViewportRegion region =
        OffscreenViewportPanel::DrawViewportRegion(m_viewport, available, canCompositeFrame);
    OffscreenViewportPanel::UpdateInteractionRect(
        m_viewport, region.imageMin, region.imageSize, true);
    DrawModelDropTarget(camera, scene, project, undoStack);
    if (!canCompositeFrame)
    {
        OffscreenViewportPanel::DrawCenteredPlaceholder(region.imageMin, available, "Scene View");
    }

    DrawViewGizmo(camera, scene, region.imageMin, region.imageMax);
    DrawFlySpeedOverlay(region.imageMin, region.imageMax);

    ImGui::End();
}

void SceneViewportPanel::CompositeRenderedFrame()
{
    OffscreenViewportPanel::CompositeRenderedFrame(m_viewport);
}
