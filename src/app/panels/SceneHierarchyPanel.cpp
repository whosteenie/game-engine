#include "app/editor/EditorReorderDragDrop.h"
#include "app/panels/SceneHierarchyPanel.h"

#include "app/editor/EditorClipboard.h"
#include "app/editor/EditorPanelConstraints.h"
#include "app/project/ProjectSession.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneImportService.h"
#include "app/undo/UndoCommand.h"
#include "engine/assets/FileDialog.h"
#include "engine/lighting/Light.h"
#include "engine/scene/SceneObject.h"
#include "engine/scene/SceneHierarchy.h"
#include "engine/scene/SceneObjectId.h"
#include "engine/scene/ScenePrimitive.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    SceneObjectId GetObjectId(const Scene& scene, int objectIndex)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= scene.GetObjects().size())
        {
            return kInvalidSceneObjectId;
        }

        return scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetId();
    }

    bool IsNodeExpanded(
        const Scene& scene,
        int objectIndex,
        const std::unordered_map<SceneObjectId, bool>& openStates)
    {
        const auto iterator = openStates.find(GetObjectId(scene, objectIndex));
        return iterator != openStates.end() && iterator->second;
    }

    void BuildVisibleObjectOrder(
        const Scene& scene,
        int objectIndex,
        const std::unordered_map<SceneObjectId, bool>& openStates,
        std::vector<int>& outVisibleIndices)
    {
        outVisibleIndices.push_back(objectIndex);

        if (!IsNodeExpanded(scene, objectIndex, openStates))
        {
            return;
        }

        for (int childIndex : scene.GetChildren(objectIndex))
        {
            BuildVisibleObjectOrder(scene, childIndex, openStates, outVisibleIndices);
        }
    }

    void BuildVisibleObjectOrder(
        const Scene& scene,
        const std::unordered_map<SceneObjectId, bool>& openStates,
        std::vector<int>& outVisibleIndices)
    {
        outVisibleIndices.clear();
        for (int rootIndex : scene.GetRootObjectIndices())
        {
            BuildVisibleObjectOrder(scene, rootIndex, openStates, outVisibleIndices);
        }
    }

    void SelectVisibleRange(
        Scene& scene,
        int anchorIndex,
        int clickedIndex,
        const std::vector<int>& visibleIndices)
    {
        const auto anchorIterator =
            std::find(visibleIndices.begin(), visibleIndices.end(), anchorIndex);
        const auto clickedIterator =
            std::find(visibleIndices.begin(), visibleIndices.end(), clickedIndex);
        if (anchorIterator == visibleIndices.end() || clickedIterator == visibleIndices.end())
        {
            scene.SelectSingle(clickedIndex);
            return;
        }

        const auto rangeBegin = std::min(anchorIterator, clickedIterator);
        const auto rangeEnd = std::max(anchorIterator, clickedIterator);
        std::vector<int> rangeIndices(rangeBegin, rangeEnd + 1);
        scene.SetSelection(rangeIndices, clickedIndex);
    }

    void HandleHierarchyRowSelection(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        int objectIndex,
        const std::unordered_map<SceneObjectId, bool>& openStates)
    {
        std::vector<int> visibleIndices;
        BuildVisibleObjectOrder(scene, openStates, visibleIndices);

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl)
        {
            panel.ClearPendingSelectionCollapse();
            scene.ToggleSelected(objectIndex);
        }
        else if (io.KeyShift)
        {
            panel.ClearPendingSelectionCollapse();
            const int anchorIndex = scene.GetPrimarySelection();
            if (anchorIndex >= 0)
            {
                SelectVisibleRange(scene, anchorIndex, objectIndex, visibleIndices);
            }
            else
            {
                scene.SelectSingle(objectIndex);
            }
        }
        else if (
            scene.IsSelected(objectIndex)
            && scene.GetSelection().indices.size() > 1)
        {
            // Keep the multi-select so a subsequent drag can reparent the package.
            // Collapse to this row on mouse-up if no drag started.
            panel.BeginPendingSelectionCollapse(objectIndex);
        }
        else
        {
            panel.ClearPendingSelectionCollapse();
            scene.SelectSingle(objectIndex);
        }
    }

    void HandleHierarchyKeyboardNavigation(
        Scene& scene,
        int& primaryIndex,
        std::unordered_map<SceneObjectId, bool>& openStates,
        bool& scrollSelectionIntoView)
    {
        if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows))
        {
            return;
        }

        if (ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive())
        {
            return;
        }

        std::vector<int> visibleIndices;
        BuildVisibleObjectOrder(scene, openStates, visibleIndices);
        if (visibleIndices.empty())
        {
            return;
        }

        const ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            int nextIndex = primaryIndex;
            if (primaryIndex < 0)
            {
                nextIndex = visibleIndices.front();
            }
            else
            {
                const auto currentIterator =
                    std::find(visibleIndices.begin(), visibleIndices.end(), primaryIndex);
                if (currentIterator != visibleIndices.end() && currentIterator + 1 != visibleIndices.end())
                {
                    nextIndex = *(currentIterator + 1);
                }
            }

            if (io.KeyShift)
            {
                if (primaryIndex < 0)
                {
                    scene.SelectSingle(nextIndex);
                }
                else
                {
                    std::vector<int> indices = scene.GetSelection().indices;
                    if (!scene.IsSelected(nextIndex))
                    {
                        indices.push_back(nextIndex);
                    }

                    scene.SetSelection(indices, nextIndex);
                }
            }
            else
            {
                scene.SelectSingle(nextIndex);
            }

            primaryIndex = nextIndex;
            scrollSelectionIntoView = true;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            int nextIndex = primaryIndex;
            if (primaryIndex < 0)
            {
                nextIndex = visibleIndices.back();
            }
            else
            {
                const auto currentIterator =
                    std::find(visibleIndices.begin(), visibleIndices.end(), primaryIndex);
                if (currentIterator != visibleIndices.end() && currentIterator != visibleIndices.begin())
                {
                    nextIndex = *(currentIterator - 1);
                }
            }

            if (io.KeyShift)
            {
                if (primaryIndex < 0)
                {
                    scene.SelectSingle(nextIndex);
                }
                else
                {
                    std::vector<int> indices = scene.GetSelection().indices;
                    if (!scene.IsSelected(nextIndex))
                    {
                        indices.push_back(nextIndex);
                    }

                    scene.SetSelection(indices, nextIndex);
                }
            }
            else
            {
                scene.SelectSingle(nextIndex);
            }

            primaryIndex = nextIndex;
            scrollSelectionIntoView = true;
        }

        if (!ImGui::IsKeyPressed(ImGuiKey_DownArrow) && !ImGui::IsKeyPressed(ImGuiKey_UpArrow) && primaryIndex >= 0)
        {
            const std::vector<int> children = scene.GetChildren(primaryIndex);
            if (children.empty())
            {
                return;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)
                && !IsNodeExpanded(scene, primaryIndex, openStates))
            {
                openStates[GetObjectId(scene, primaryIndex)] = true;
                scrollSelectionIntoView = true;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)
                     && IsNodeExpanded(scene, primaryIndex, openStates))
            {
                openStates[GetObjectId(scene, primaryIndex)] = false;
                scrollSelectionIntoView = true;
            }
        }
    }

    bool AddPrimitiveFromMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ScenePrimitive primitive,
        int parentIndex)
    {
        if (ImGui::MenuItem(GetScenePrimitiveDisplayName(primitive)))
        {
            const std::string commandName =
                std::string("Create ") + GetScenePrimitiveDisplayName(primitive);
            panel.PushInsertMutation(scene, commandName, [&](Scene& target) {
                const int newIndex = target.AddObject(primitive, parentIndex);
                target.SelectSingle(newIndex);
                return std::vector<int>{newIndex};
            });
            return true;
        }

        return false;
    }

    bool AddEmptyFromMenu(const SceneHierarchyPanel& panel, Scene& scene, int parentIndex)
    {
        if (ImGui::MenuItem("Empty"))
        {
            panel.PushInsertMutation(scene, "Create Empty", [&](Scene& target) {
                const int newIndex = target.AddEmptyObject(parentIndex);
                target.SelectSingle(newIndex);
                return std::vector<int>{newIndex};
            });
            return true;
        }

        return false;
    }

    bool AddLightFromMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        LightType type,
        int parentIndex)
    {
        const char* label = "Light";
        const char* commandName = "Create Light";
        switch (type)
        {
        case LightType::Directional:
            label = "Directional Light";
            commandName = "Create Directional Light";
            break;
        case LightType::Point:
            label = "Point Light";
            commandName = "Create Point Light";
            break;
        case LightType::Spot:
            label = "Spot Light";
            commandName = "Create Spot Light";
            break;
        }

        if (ImGui::MenuItem(label))
        {
            panel.PushInsertMutation(scene, commandName, [&](Scene& target) {
                const int newIndex = target.AddLightObject(type, parentIndex);
                target.SelectSingle(newIndex);
                return std::vector<int>{newIndex};
            });
            return true;
        }

        return false;
    }

    bool AddCameraFromMenu(const SceneHierarchyPanel& panel, Scene& scene, int parentIndex)
    {
        if (ImGui::MenuItem("Camera"))
        {
            panel.PushInsertMutation(scene, "Create Camera", [&](Scene& target) {
                const int newIndex = target.AddCameraObject(parentIndex);
                target.SelectSingle(newIndex);
                return std::vector<int>{newIndex};
            });
            return true;
        }

        return false;
    }

    void DrawLightMenu(const SceneHierarchyPanel& panel, Scene& scene, int parentIndex)
    {
        if (ImGui::BeginMenu("Light"))
        {
            AddLightFromMenu(panel, scene, LightType::Directional, parentIndex);
            AddLightFromMenu(panel, scene, LightType::Point, parentIndex);
            AddLightFromMenu(panel, scene, LightType::Spot, parentIndex);
            ImGui::EndMenu();
        }
    }

    std::string BuildHierarchyLabel(const SceneObject& object)
    {
        if (object.HasCamera())
        {
            return "[Camera] " + object.GetName();
        }

        if (object.HasLight())
        {
            return "[Light] " + object.GetName();
        }

        if (object.IsRenderable())
        {
            return object.GetName();
        }

        return "[Empty] " + object.GetName();
    }

    void Draw3DObjectMenu(const SceneHierarchyPanel& panel, Scene& scene, int parentIndex)
    {
        if (ImGui::BeginMenu("3D Object"))
        {
            AddPrimitiveFromMenu(panel, scene, ScenePrimitive::Cube, parentIndex);
            AddPrimitiveFromMenu(panel, scene, ScenePrimitive::Sphere, parentIndex);
            AddPrimitiveFromMenu(panel, scene, ScenePrimitive::Cylinder, parentIndex);
            AddPrimitiveFromMenu(panel, scene, ScenePrimitive::Capsule, parentIndex);
            AddPrimitiveFromMenu(panel, scene, ScenePrimitive::Plane, parentIndex);
            ImGui::EndMenu();
        }
    }

    void ImportModelFromDialog(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        int parentIndex)
    {
        std::string modelPath;
        if (!FileDialog::OpenModelFile(modelPath))
        {
            return;
        }

        const std::string& projectRoot = project.GetProjectRootDirectory();
        panel.PushInsertMutation(scene, "Import Model", [&](Scene& target) {
            const std::vector<int> importedIndices =
                target.ImportModel(modelPath, parentIndex, projectRoot);
            if (!importedIndices.empty())
            {
                target.SelectSingle(importedIndices.front());
            }

            return importedIndices;
        });

        if (!scene.GetImportService().GetLastImportError().empty())
        {
            project.SetStatusMessage(scene.GetImportService().GetLastImportError());
        }
        else if (!scene.GetImportService().GetLastImportWarning().empty())
        {
            project.SetStatusMessage(scene.GetImportService().GetLastImportWarning());
        }
    }

    void DrawCreateObjectMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        int parentIndex)
    {
        AddEmptyFromMenu(panel, scene, parentIndex);
        AddCameraFromMenu(panel, scene, parentIndex);
        DrawLightMenu(panel, scene, parentIndex);
        Draw3DObjectMenu(panel, scene, parentIndex);
        if (ImGui::MenuItem("Import Model..."))
        {
            ImportModelFromDialog(panel, scene, project, parentIndex);
        }
    }

    void DrawObjectContextMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        int objectIndex,
        std::unordered_map<SceneObjectId, bool>& openStates,
        int& pendingRenameIndex,
        int& renameTargetIndex,
        bool& beginRenameNextFrame,
        char* renameBuffer,
        std::size_t renameBufferSize,
        bool& focusRenameInput)
    {
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }

        scene.SelectSingle(objectIndex);
        DrawCreateObjectMenu(panel, scene, project, objectIndex);

        ImGui::Separator();
        if (ImGui::MenuItem("Rename", "F2"))
        {
            renameTargetIndex = objectIndex;
            beginRenameNextFrame = true;
            pendingRenameIndex = -1;
            std::snprintf(
                renameBuffer,
                renameBufferSize,
                "%s",
                scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetName().c_str());
            focusRenameInput = true;
        }

        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        {
            panel.PushInsertMutation(scene, "Duplicate", [objectIndex](Scene& target) {
                const int duplicatedIndex = target.DuplicateObject(objectIndex);
                if (duplicatedIndex >= 0)
                {
                    target.SelectSingle(duplicatedIndex);
                    return std::vector<int>{duplicatedIndex};
                }

                return std::vector<int>{};
            });
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Cut", "Ctrl+X"))
        {
            CutSelection(undoStack, clipboard, scene);
        }

        if (ImGui::MenuItem("Copy", "Ctrl+C"))
        {
            CopySelection(clipboard, scene);
        }

        if (ImGui::MenuItem("Paste", "Ctrl+V", false, clipboard.HasContent()))
        {
            PushPasteFromClipboard(
                undoStack,
                scene,
                clipboard,
                objectIndex,
                HierarchyInsertMode::AsChild);
        }

            if (ImGui::MenuItem("Delete"))
        {
            const std::string objectName =
                scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetName();
            PushDeleteObjects(
                undoStack,
                scene,
                "Delete \"" + objectName + "\"",
                {objectIndex});
        }

        ImGui::EndPopup();
    }

    bool DrawInlineRenameField(
        char* renameBuffer,
        std::size_t renameBufferSize,
        bool& focusRenameInput,
        bool& renameInputEngaged,
        bool& cancelRename)
    {
        if (focusRenameInput)
        {
            ImGui::SetKeyboardFocusHere();
        }

        const float textLineHeight = ImGui::GetTextLineHeight();
        const float rawFramePaddingY = (textLineHeight - ImGui::GetFontSize()) * 0.5f;
        const float framePaddingY = rawFramePaddingY > 0.0f ? rawFramePaddingY : 0.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, framePaddingY));

        const float inputWidth = ImGui::GetContentRegionAvail().x;
        ImGui::SetNextItemWidth(inputWidth > 0.0f ? inputWidth : -FLT_MIN);
        const ImGuiInputTextFlags inputFlags =
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll;

        const bool confirmed = ImGui::InputText("##RenameInput", renameBuffer, renameBufferSize, inputFlags);

        ImGui::PopStyleVar();

        if (ImGui::IsItemActive() || ImGui::IsItemFocused())
        {
            renameInputEngaged = true;
            focusRenameInput = false;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
        {
            cancelRename = true;
        }
        else if (renameInputEngaged && ImGui::IsItemDeactivated() && !confirmed)
        {
            cancelRename = true;
        }

        return confirmed;
    }

    std::vector<SceneObjectId> CollectHierarchyDragObjectIds(const Scene& scene, int draggedIndex)
    {
        std::vector<SceneObjectId> objectIds;
        if (draggedIndex < 0 || static_cast<std::size_t>(draggedIndex) >= scene.GetObjects().size())
        {
            return objectIds;
        }

        if (scene.IsSelected(draggedIndex) && scene.GetSelection().indices.size() > 1)
        {
            const std::vector<int> roots =
                FilterToTopmostSelectedIndices(scene.GetObjects(), scene.GetSelection().indices);
            objectIds.reserve(roots.size());
            for (int rootIndex : roots)
            {
                objectIds.push_back(GetObjectId(scene, rootIndex));
            }
            return objectIds;
        }

        objectIds.push_back(GetObjectId(scene, draggedIndex));
        return objectIds;
    }

    bool CanAcceptHierarchyPackageDrop(
        const Scene& scene,
        const std::vector<SceneObjectId>& objectIds,
        int referenceIndex,
        HierarchyInsertMode mode)
    {
        for (SceneObjectId objectId : objectIds)
        {
            const int objectIndex = scene.FindObjectIndex(objectId);
            if (objectIndex < 0 || objectIndex == referenceIndex)
            {
                continue;
            }

            if (scene.CanPlaceObjectInHierarchy(objectIndex, referenceIndex, mode)
                && scene.WouldPlaceObjectInHierarchyChange(objectIndex, referenceIndex, mode))
            {
                return true;
            }
        }

        return false;
    }

    void DrawHierarchyDragDropSource(const Scene& scene, int objectIndex, const std::string& label, bool enabled)
    {
        if (!enabled)
        {
            return;
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
        {
            ImGui::SetDragDropPayload(
                EditorReorderDragDrop::kHierarchyDragDropPayload,
                &objectIndex,
                sizeof(int));

            const std::vector<SceneObjectId> packageIds = CollectHierarchyDragObjectIds(scene, objectIndex);
            if (packageIds.size() > 1)
            {
                ImGui::Text("%d objects", static_cast<int>(packageIds.size()));
            }
            else
            {
                ImGui::TextUnformatted(label.c_str());
            }
            ImGui::EndDragDropSource();
        }
    }

    bool IsHierarchyDragActive()
    {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        return payload != nullptr
            && payload->IsDataType(EditorReorderDragDrop::kHierarchyDragDropPayload);
    }

    void DrawHierarchyInsertGapLine(float lineMinX = -1.0f)
    {
        const ImVec2 gapMin = ImGui::GetItemRectMin();
        const ImVec2 gapMax = ImGui::GetItemRectMax();
        const float lineY = (gapMin.y + gapMax.y) * 0.5f;
        const float x0 = lineMinX >= 0.0f ? lineMinX : gapMin.x;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(x0, lineY),
            ImVec2(gapMax.x, lineY),
            IM_COL32(90, 150, 255, 255),
            2.0f);
    }

    bool IsHierarchyInsertGapValid(
        const Scene& scene,
        int draggedIndex,
        int referenceIndex,
        HierarchyInsertMode mode)
    {
        return scene.CanPlaceObjectInHierarchy(draggedIndex, referenceIndex, mode)
            && scene.WouldPlaceObjectInHierarchyChange(draggedIndex, referenceIndex, mode);
    }

    bool TryAcceptHierarchyInsertGap(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        int referenceIndex,
        HierarchyInsertMode mode,
        const ImGuiPayload* activePayload,
        bool drawLine,
        bool bottomStickyEligible,
        bool useBottomInsertLineY = false,
        float lineMinX = -1.0f)
    {
        if (activePayload == nullptr
            || !activePayload->IsDataType(EditorReorderDragDrop::kHierarchyDragDropPayload))
        {
            return false;
        }

        const int draggedIndex = *static_cast<const int*>(activePayload->Data);
        if (!IsHierarchyInsertGapValid(scene, draggedIndex, referenceIndex, mode))
        {
            return false;
        }

        if (drawLine)
        {
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float resolvedLineMinX = lineMinX >= 0.0f ? lineMinX : itemMin.x;
            if (bottomStickyEligible)
            {
                panel.UpdateDragInsertLatch(
                    referenceIndex,
                    resolvedLineMinX,
                    itemMin.y,
                    itemMax.x,
                    itemMax.y,
                    useBottomInsertLineY);
                panel.DrawDragInsertLatchLine();
            }
            else
            {
                panel.ClearDragInsertLatch();
                DrawHierarchyInsertGapLine(resolvedLineMinX);
            }
        }

        const ImGuiDragDropFlags acceptFlags =
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    EditorReorderDragDrop::kHierarchyDragDropPayload,
                    acceptFlags))
        {
            if (payload->IsDelivery())
            {
                const SceneObjectId draggedId = GetObjectId(scene, draggedIndex);
                const SceneObjectId referenceId = GetObjectId(scene, referenceIndex);
                panel.PushReparentMutation(
                    scene,
                    "Reparent",
                    std::vector<SceneObjectId>{draggedId},
                    referenceId,
                    mode);
            }

            return true;
        }

        return false;
    }

    void DrawHierarchyBackgroundContextMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard);

    void DrawHierarchyInsertGap(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        int referenceIndex,
        HierarchyInsertMode mode,
        bool bottomStickyEligible = false,
        float lineMinX = -1.0f)
    {
        ImGui::PushID(referenceIndex);
        ImGui::PushID(static_cast<int>(mode));
        ImGui::InvisibleButton(
            "##HierarchyInsertGap",
            ImVec2(-FLT_MIN, EditorReorderDragDrop::kInsertGapHitHeight));

        if (!IsHierarchyDragActive())
        {
            DrawHierarchyBackgroundContextMenu(panel, scene, project, undoStack, clipboard);
        }

        if (ImGui::BeginDragDropTarget())
        {
            TryAcceptHierarchyInsertGap(
                panel,
                scene,
                referenceIndex,
                mode,
                ImGui::GetDragDropPayload(),
                true,
                bottomStickyEligible,
                false,
                lineMinX);
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
        ImGui::PopID();
    }

    // Single 14px gap that can resolve to After(previous) at the previous row indent
    // or Before(current) at the current indent — used when leaving a subtree so
    // "below last child" reparent is available without doubling spacing.
    void DrawHierarchyTransitionInsertGap(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        int previousIndex,
        float previousRowMinX,
        int currentIndex)
    {
        ImGui::PushID(previousIndex);
        ImGui::PushID(currentIndex);
        ImGui::PushID("HierarchyTransitionGap");
        ImGui::InvisibleButton(
            "##HierarchyInsertGap",
            ImVec2(-FLT_MIN, EditorReorderDragDrop::kInsertGapHitHeight));

        if (!IsHierarchyDragActive())
        {
            DrawHierarchyBackgroundContextMenu(panel, scene, project, undoStack, clipboard);
        }

        if (ImGui::BeginDragDropTarget())
        {
            const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
            if (activePayload != nullptr
                && activePayload->IsDataType(EditorReorderDragDrop::kHierarchyDragDropPayload))
            {
                const int draggedIndex = *static_cast<const int*>(activePayload->Data);
                const bool afterValid = IsHierarchyInsertGapValid(
                    scene,
                    draggedIndex,
                    previousIndex,
                    HierarchyInsertMode::After);
                const bool beforeValid = IsHierarchyInsertGapValid(
                    scene,
                    draggedIndex,
                    currentIndex,
                    HierarchyInsertMode::Before);

                const ImVec2 gapMin = ImGui::GetItemRectMin();
                const bool preferAfter =
                    afterValid
                    && (!beforeValid || ImGui::GetIO().MousePos.x >= previousRowMinX);

                if (preferAfter)
                {
                    TryAcceptHierarchyInsertGap(
                        panel,
                        scene,
                        previousIndex,
                        HierarchyInsertMode::After,
                        activePayload,
                        true,
                        false,
                        false,
                        previousRowMinX);
                }
                else if (beforeValid)
                {
                    TryAcceptHierarchyInsertGap(
                        panel,
                        scene,
                        currentIndex,
                        HierarchyInsertMode::Before,
                        activePayload,
                        true,
                        false,
                        false,
                        gapMin.x);
                }
            }
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }

    struct HierarchyRowHit
    {
        int objectIndex = -1;
        ImRect displayRect{};
    };

    struct HierarchyDrawState
    {
        int previousVisibleIndex = -1;
        float previousRowMinX = 0.0f;
        std::vector<HierarchyRowHit> rowHits;
    };

    void RecordHierarchyRowHit(HierarchyDrawState& drawState, int objectIndex)
    {
        ImGuiContext& g = *GImGui;
        ImRect rect = g.LastItemData.Rect;
        if ((g.LastItemData.StatusFlags & ImGuiItemStatusFlags_HasDisplayRect) != 0)
        {
            rect = g.LastItemData.DisplayRect;
        }

        drawState.rowHits.push_back(HierarchyRowHit{objectIndex, rect});
    }

    // Use the painted TreeNode display rect (selection highlight), not the larger interaction
    // Rect that can spill into reorder gaps. Hit → select; miss → clear empty space.
    bool IsHierarchyRowHit(const ImRect& displayRect, const ImVec2& mouse)
    {
        return displayRect.Contains(mouse);
    }

    int FindHierarchyRowHitIndex(
        const std::vector<HierarchyRowHit>& rowHits,
        const ImVec2& mouse)
    {
        for (const HierarchyRowHit& hit : rowHits)
        {
            if (IsHierarchyRowHit(hit.displayRect, mouse))
            {
                return hit.objectIndex;
            }
        }

        return -1;
    }

    int GetObjectParentIndex(const Scene& scene, int objectIndex)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= scene.GetObjects().size())
        {
            return -1;
        }

        return scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetParentIndex();
    }

    bool ShouldOfferAfterPreviousInLeadingGap(
        const Scene& scene,
        int previousIndex,
        int currentIndex)
    {
        if (previousIndex < 0)
        {
            return false;
        }

        const int currentParent = GetObjectParentIndex(scene, currentIndex);
        const int previousParent = GetObjectParentIndex(scene, previousIndex);
        // Next row is the first child under previous → only Before(current).
        // Next row is a sibling → Before(current) already equals After(previous).
        // Otherwise we left a subtree and After(previous) is the "below last child" slot.
        return currentParent != previousIndex && currentParent != previousParent;
    }

    void DrawHierarchyLeadingInsertGap(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        int objectIndex,
        const HierarchyDrawState& drawState)
    {
        if (ShouldOfferAfterPreviousInLeadingGap(
                scene,
                drawState.previousVisibleIndex,
                objectIndex))
        {
            DrawHierarchyTransitionInsertGap(
                panel,
                scene,
                project,
                undoStack,
                clipboard,
                drawState.previousVisibleIndex,
                drawState.previousRowMinX,
                objectIndex);
            return;
        }

        DrawHierarchyInsertGap(
            panel,
            scene,
            project,
            undoStack,
            clipboard,
            objectIndex,
            HierarchyInsertMode::Before);
    }

    void DrawHierarchyBottomDropZone(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        int referenceIndex,
        float height,
        float lineMinX = -1.0f)
    {
        ImGui::InvisibleButton("##HierarchyBottomDropZone", ImVec2(-FLT_MIN, height));

        if (!ImGui::BeginDragDropTarget())
        {
            return;
        }

        const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
        const bool hasActiveLatch = panel.HasDragInsertLatchFor(referenceIndex);
        const ImVec2 zoneMin = ImGui::GetItemRectMin();
        const ImVec2 zoneMax = ImGui::GetItemRectMax();
        const float resolvedLineMinX = lineMinX >= 0.0f ? lineMinX : zoneMin.x;

        if (hasActiveLatch)
        {
            panel.UpdateDragInsertLatch(
                referenceIndex,
                resolvedLineMinX,
                zoneMin.y,
                zoneMax.x,
                zoneMax.y);
            panel.DrawDragInsertLatchLine();
        }

        TryAcceptHierarchyInsertGap(
            panel,
            scene,
            referenceIndex,
            HierarchyInsertMode::After,
            activePayload,
            !hasActiveLatch,
            true,
            true,
            resolvedLineMinX);

        ImGui::EndDragDropTarget();
    }

    void DrawHierarchyReparentIndicator()
    {
        const ImVec2 rowMin = ImGui::GetItemRectMin();
        const ImVec2 rowMax = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRect(rowMin, rowMax, IM_COL32(90, 150, 255, 255), 0.0f, 0, 2.0f);
    }

    void DrawHierarchyRowDropTarget(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        int referenceIndex,
        std::unordered_map<SceneObjectId, bool>& openStates)
    {
        if (!ImGui::BeginDragDropTarget())
        {
            return;
        }

        const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
        if (activePayload != nullptr
            && activePayload->IsDataType(EditorReorderDragDrop::kHierarchyDragDropPayload))
        {
            const int draggedIndex = *static_cast<const int*>(activePayload->Data);
            const HierarchyInsertMode mode = HierarchyInsertMode::AsChild;
            const std::vector<SceneObjectId> packageIds =
                CollectHierarchyDragObjectIds(scene, draggedIndex);
            if (CanAcceptHierarchyPackageDrop(scene, packageIds, referenceIndex, mode))
            {
                panel.ClearDragInsertLatch();
                DrawHierarchyReparentIndicator();
                const SceneObjectId referenceId = GetObjectId(scene, referenceIndex);
                const bool hasChildren = !scene.GetChildren(referenceIndex).empty();
                panel.TryExpandNodeOnDragHover(
                    referenceId,
                    hasChildren,
                    IsNodeExpanded(scene, referenceIndex, openStates),
                    openStates);

                const ImGuiDragDropFlags acceptFlags =
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(
                            EditorReorderDragDrop::kHierarchyDragDropPayload,
                            acceptFlags))
                {
                    if (payload->IsDelivery())
                    {
                        panel.PushReparentMutation(
                            scene,
                            "Reparent",
                            packageIds,
                            referenceId,
                            mode);
                    }
                }
            }
        }

        ImGui::EndDragDropTarget();
    }

    void DrawHierarchyBackgroundContextMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard)
    {
        if (ImGui::BeginPopupContextItem())
        {
            DrawCreateObjectMenu(panel, scene, project, -1);
            if (clipboard.HasContent())
            {
                ImGui::Separator();
                if (ImGui::MenuItem("Paste", "Ctrl+V"))
                {
                    PushPasteFromClipboard(
                        undoStack,
                        scene,
                        clipboard,
                        -1,
                        HierarchyInsertMode::After);
                }
            }

            ImGui::EndPopup();
        }
    }

    void DrawHierarchyNode(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        UndoStack& undoStack,
        EditorClipboard& clipboard,
        int objectIndex,
        int primaryIndex,
        std::unordered_map<SceneObjectId, bool>& openStates,
        bool& scrollSelectionIntoView,
        int& pendingRenameIndex,
        int& renameTargetIndex,
        bool& beginRenameNextFrame,
        char* renameBuffer,
        std::size_t renameBufferSize,
        bool& focusRenameInput,
        bool& renameInputEngaged,
        HierarchyDrawState& drawState)
    {
        DrawHierarchyLeadingInsertGap(
            panel,
            scene,
            project,
            undoStack,
            clipboard,
            objectIndex,
            drawState);

        const SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        const std::vector<int> children = scene.GetChildren(objectIndex);
        const bool isRenaming = objectIndex == pendingRenameIndex;

        ImGui::PushID(objectIndex);

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow
            | ImGuiTreeNodeFlags_SpanAvailWidth
            | ImGuiTreeNodeFlags_FramePadding;
        if (scene.IsSelected(objectIndex) && !isRenaming)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        if (children.empty())
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        else
        {
            ImGui::SetNextItemOpen(IsNodeExpanded(scene, objectIndex, openStates), ImGuiCond_Always);
        }

        const std::string label = BuildHierarchyLabel(object);
        bool opened = false;
        bool cancelRename = false;

        if (isRenaming)
        {
            flags &= ~ImGuiTreeNodeFlags_SpanAvailWidth;

            const float rowY = ImGui::GetCursorPosY();
            const float rowStartX = ImGui::GetCursorPosX();
            const float labelX = rowStartX + ImGui::GetTreeNodeToLabelSpacing();

            if (!children.empty())
            {
                ImGui::SetNextItemOpen(true, ImGuiCond_Always);
                const ImGuiTreeNodeFlags arrowFlags =
                    flags | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanLabelWidth;
                opened = ImGui::TreeNodeEx("##RenameAnchor", arrowFlags);
            }

            ImGui::SetCursorPos(ImVec2(labelX, rowY));

            if (DrawInlineRenameField(
                    renameBuffer,
                    renameBufferSize,
                    focusRenameInput,
                    renameInputEngaged,
                    cancelRename))
            {
                if (renameBuffer[0] != '\0')
                {
                    const SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
                    PushSetObjectName(
                        undoStack,
                        scene,
                        object.GetId(),
                        object.GetName(),
                        renameBuffer);
                }

                pendingRenameIndex = -1;
                focusRenameInput = false;
                renameInputEngaged = false;
            }
            else if (cancelRename)
            {
                pendingRenameIndex = -1;
                focusRenameInput = false;
                renameInputEngaged = false;
            }

            drawState.previousVisibleIndex = objectIndex;
            drawState.previousRowMinX = ImGui::GetWindowPos().x + rowStartX - ImGui::GetScrollX();
        }
        else
        {
            opened = ImGui::TreeNodeEx(label.c_str(), flags);
            if (!children.empty())
            {
                openStates[object.GetId()] = opened;
            }

            if (objectIndex == primaryIndex && scrollSelectionIntoView)
            {
                ImGui::SetScrollHereY(0.5f);
            }

            RecordHierarchyRowHit(drawState, objectIndex);

            const bool allowDragDrop = true;
            DrawHierarchyDragDropSource(scene, objectIndex, label, allowDragDrop);
            DrawHierarchyRowDropTarget(panel, scene, objectIndex, openStates);

            drawState.previousVisibleIndex = objectIndex;
            drawState.previousRowMinX = ImGui::GetItemRectMin().x;
        }

        if (!isRenaming)
        {
            DrawObjectContextMenu(
                panel,
                scene,
                project,
                undoStack,
                clipboard,
                objectIndex,
                openStates,
                pendingRenameIndex,
                renameTargetIndex,
                beginRenameNextFrame,
                renameBuffer,
                renameBufferSize,
                focusRenameInput);
        }

        if (opened && !children.empty())
        {
            for (int childIndex : children)
            {
                DrawHierarchyNode(
                    panel,
                    scene,
                    project,
                    undoStack,
                    clipboard,
                    childIndex,
                    primaryIndex,
                    openStates,
                    scrollSelectionIntoView,
                    pendingRenameIndex,
                    renameTargetIndex,
                    beginRenameNextFrame,
                    renameBuffer,
                    renameBufferSize,
                    focusRenameInput,
                    renameInputEngaged,
                    drawState);
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    void DrawAddObjectPopup(const SceneHierarchyPanel& panel, Scene& scene, ProjectSession& project)
    {
        if (ImGui::BeginPopup("AddObjectPopup"))
        {
            DrawCreateObjectMenu(panel, scene, project, -1);
            ImGui::EndPopup();
        }
    }
}

void SceneHierarchyPanel::PushInsertMutation(
    Scene& scene,
    const std::string& commandName,
    const std::function<std::vector<int>(Scene&)>& mutate) const
{
    if (m_drawUndoStack != nullptr)
    {
        PushInsertSubtree(*m_drawUndoStack, scene, commandName, mutate);
        return;
    }

    if (mutate)
    {
        mutate(scene);
    }
}

void SceneHierarchyPanel::PushReparentMutation(
    Scene& scene,
    const std::string& commandName,
    const std::vector<SceneObjectId>& objectIds,
    SceneObjectId referenceId,
    HierarchyInsertMode mode) const
{
    if (m_drawUndoStack != nullptr)
    {
        PushReparentObjects(*m_drawUndoStack, scene, commandName, objectIds, referenceId, mode);
        return;
    }

    const int referenceIndex = scene.FindObjectIndex(referenceId);
    if (referenceIndex < 0)
    {
        return;
    }

    for (SceneObjectId objectId : objectIds)
    {
        const int objectIndex = scene.FindObjectIndex(objectId);
        const int currentReferenceIndex = scene.FindObjectIndex(referenceId);
        if (objectIndex >= 0 && currentReferenceIndex >= 0)
        {
            scene.PlaceObjectInHierarchy(objectIndex, currentReferenceIndex, mode);
        }
    }

    if (objectIds.size() == 1)
    {
        const int selectedIndex = scene.FindObjectIndex(objectIds.front());
        if (selectedIndex >= 0)
        {
            scene.SelectSingle(selectedIndex);
        }
    }
}

void SceneHierarchyPanel::ClearDragInsertLatch() const
{
    m_dragInsertLatchReferenceIndex = -1;
}

void SceneHierarchyPanel::UpdateDragInsertLatch(
    int referenceIndex,
    float itemMinX,
    float itemMinY,
    float itemMaxX,
    float itemMaxY,
    bool useBottomInsertLineY) const
{
    const bool isNewLatch = m_dragInsertLatchReferenceIndex != referenceIndex;
    m_dragInsertLatchReferenceIndex = referenceIndex;
    m_dragInsertLatchLineMinX = itemMinX;
    m_dragInsertLatchLineMaxX = itemMaxX;
    if (isNewLatch)
    {
        m_dragInsertLatchLineY = useBottomInsertLineY
            ? itemMinY - EditorReorderDragDrop::kInsertGapHitHeight * 0.5f
            : (itemMinY + itemMaxY) * 0.5f;
    }
}

bool SceneHierarchyPanel::HasDragInsertLatchFor(int referenceIndex) const
{
    return m_dragInsertLatchReferenceIndex == referenceIndex;
}

void SceneHierarchyPanel::DrawDragInsertLatchLine() const
{
    if (m_dragInsertLatchReferenceIndex < 0)
    {
        return;
    }

    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(m_dragInsertLatchLineMinX, m_dragInsertLatchLineY),
        ImVec2(m_dragInsertLatchLineMaxX, m_dragInsertLatchLineY),
        IM_COL32(90, 150, 255, 255),
        2.0f);
}

void SceneHierarchyPanel::TryExpandNodeOnDragHover(
    SceneObjectId referenceId,
    bool hasChildren,
    bool isAlreadyExpanded,
    std::unordered_map<SceneObjectId, bool>& openStates) const
{
    m_dragExpandHoverSeenThisFrame = true;

    if (!hasChildren || isAlreadyExpanded)
    {
        return;
    }

    const double now = ImGui::GetTime();
    if (m_dragExpandHoverNodeId != referenceId)
    {
        m_dragExpandHoverNodeId = referenceId;
        m_dragExpandHoverStartTime = now;
        return;
    }

    if (now - m_dragExpandHoverStartTime
        >= static_cast<double>(EditorReorderDragDrop::kDragExpandDelaySeconds))
    {
        openStates[referenceId] = true;
    }
}

void SceneHierarchyPanel::BeginPendingSelectionCollapse(int objectIndex) const
{
    m_pendingSelectionCollapseIndex = objectIndex;
    m_pendingSelectionCollapseDragged = false;
}

void SceneHierarchyPanel::ClearPendingSelectionCollapse() const
{
    m_pendingSelectionCollapseIndex = -1;
    m_pendingSelectionCollapseDragged = false;
}

void SceneHierarchyPanel::UpdatePendingSelectionCollapse(Scene& scene) const
{
    if (m_pendingSelectionCollapseIndex < 0)
    {
        return;
    }

    if (IsHierarchyDragActive())
    {
        m_pendingSelectionCollapseDragged = true;
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        return;
    }

    if (!m_pendingSelectionCollapseDragged
        && m_pendingSelectionCollapseIndex >= 0
        && static_cast<std::size_t>(m_pendingSelectionCollapseIndex) < scene.GetObjects().size())
    {
        scene.SelectSingle(m_pendingSelectionCollapseIndex);
    }

    ClearPendingSelectionCollapse();
}

void SceneHierarchyPanel::Draw(
    Scene& scene,
    ProjectSession& project,
    UndoStack& undoStack,
    EditorClipboard& clipboard) const
{
    m_drawUndoStack = &undoStack;
    m_drawClipboard = &clipboard;

    EditorPanelConstraints::ApplySideColumnPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Hierarchy", m_showPanel))
    {
        return;
    }

    const std::vector<SceneObject>& objects = scene.GetObjects();
    int primaryIndex = scene.GetPrimarySelection();

    if (objects.empty())
    {
        ImGui::TextUnformatted("No scene objects.");
        if (ImGui::SmallButton("+ Create"))
        {
            ImGui::OpenPopup("AddObjectPopup");
        }
        DrawAddObjectPopup(*this, scene, project);

        if (clipboard.HasContent() && ImGui::BeginPopupContextWindow())
        {
            if (ImGui::MenuItem("Paste", "Ctrl+V"))
            {
                PushPasteFromClipboard(
                    undoStack,
                    scene,
                    clipboard,
                    -1,
                    HierarchyInsertMode::After);
            }
            ImGui::EndPopup();
        }

        ImGui::End();
        return;
    }

    if (primaryIndex >= static_cast<int>(objects.size()))
    {
        primaryIndex = static_cast<int>(objects.size()) - 1;
        scene.SelectSingle(primaryIndex);
    }

    if (ImGui::SmallButton("+ Create"))
    {
        ImGui::OpenPopup("AddObjectPopup");
    }
    DrawAddObjectPopup(*this, scene, project);
    primaryIndex = scene.GetPrimarySelection();

    ImGui::BeginChild("HierarchyList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);

    // Slightly taller row highlights / hit targets. TreeNodes only honor FramePadding.y
    // when ImGuiTreeNodeFlags_FramePadding is set (see DrawHierarchyNode). Extra pad is
    // offset by a matching ItemSpacing shrink so reorder gaps don't grow.
    constexpr float kHierarchyRowExtraPadY = 2.0f;
    const ImGuiStyle& hierarchyStyle = ImGui::GetStyle();
    ImGui::PushStyleVar(
        ImGuiStyleVar_FramePadding,
        ImVec2(hierarchyStyle.FramePadding.x, hierarchyStyle.FramePadding.y + kHierarchyRowExtraPadY));
    ImGui::PushStyleVar(
        ImGuiStyleVar_ItemSpacing,
        ImVec2(
            hierarchyStyle.ItemSpacing.x,
            hierarchyStyle.ItemSpacing.y > kHierarchyRowExtraPadY
                ? hierarchyStyle.ItemSpacing.y - kHierarchyRowExtraPadY
                : 0.0f));

    if (m_beginRenameNextFrame)
    {
        m_pendingRenameIndex = m_renameTargetIndex;
        m_beginRenameNextFrame = false;
        m_renameInputEngaged = false;
    }

    if (m_pendingRenameIndex < 0)
    {
        HandleHierarchyKeyboardNavigation(
            scene,
            primaryIndex,
            m_nodeOpenStates,
            m_scrollSelectionIntoView);

        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows)
            && !ImGui::GetIO().WantTextInput
            && !ImGui::IsAnyItemActive()
            && ImGui::IsKeyPressed(ImGuiKey_F2)
            && primaryIndex >= 0
            && primaryIndex < static_cast<int>(objects.size()))
        {
            m_renameTargetIndex = primaryIndex;
            m_beginRenameNextFrame = true;
            m_pendingRenameIndex = -1;
            std::snprintf(
                m_renameBuffer,
                sizeof(m_renameBuffer),
                "%s",
                objects[static_cast<std::size_t>(primaryIndex)].GetName().c_str());
            m_focusRenameInput = true;
        }
    }

    if (!IsHierarchyDragActive())
    {
        m_dragExpandHoverNodeId = kInvalidSceneObjectId;
        ClearDragInsertLatch();
    }
    else
    {
        m_dragExpandHoverSeenThisFrame = false;
    }

    HierarchyDrawState hierarchyDrawState;
    for (int objectIndex : scene.GetRootObjectIndices())
    {
        DrawHierarchyNode(
            *this,
            scene,
            project,
            undoStack,
            clipboard,
            objectIndex,
            primaryIndex,
            m_nodeOpenStates,
            m_scrollSelectionIntoView,
            m_pendingRenameIndex,
            m_renameTargetIndex,
            m_beginRenameNextFrame,
            m_renameBuffer,
            sizeof(m_renameBuffer),
            m_focusRenameInput,
            m_renameInputEngaged,
            hierarchyDrawState);
    }

    if (IsHierarchyDragActive() && !m_dragExpandHoverSeenThisFrame)
    {
        m_dragExpandHoverNodeId = kInvalidSceneObjectId;
    }

    std::vector<int> visibleObjectIndices;
    BuildVisibleObjectOrder(scene, m_nodeOpenStates, visibleObjectIndices);
    if (!visibleObjectIndices.empty())
    {
        DrawHierarchyInsertGap(
            *this,
            scene,
            project,
            undoStack,
            clipboard,
            visibleObjectIndices.back(),
            HierarchyInsertMode::After,
            true,
            hierarchyDrawState.previousRowMinX);
    }

    const ImVec2 backgroundSpace = ImGui::GetContentRegionAvail();
    if (backgroundSpace.y > 0.0f)
    {
        if (IsHierarchyDragActive() && !visibleObjectIndices.empty())
        {
            DrawHierarchyBottomDropZone(
                *this,
                scene,
                visibleObjectIndices.back(),
                backgroundSpace.y,
                hierarchyDrawState.previousRowMinX);
        }
        else
        {
            ImGui::InvisibleButton("##HierarchyBackground", backgroundSpace);
            DrawHierarchyBackgroundContextMenu(*this, scene, project, undoStack, clipboard);
        }
    }

    // Left-click in the list: hit a painted row highlight → select; otherwise clear.
    // Scroll wheel does not clear (not a left click). Scrollbar / panel-resize clicks
    // land outside InnerRect, so they also leave selection alone.
    if (!IsHierarchyDragActive()
        && m_pendingRenameIndex < 0
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup))
    {
        const ImGuiWindow* listWindow = ImGui::GetCurrentWindow();
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (listWindow->InnerRect.Contains(mouse))
        {
            const int hitIndex =
                FindHierarchyRowHitIndex(hierarchyDrawState.rowHits, mouse);
            if (hitIndex >= 0)
            {
                HandleHierarchyRowSelection(*this, scene, hitIndex, m_nodeOpenStates);
            }
            else
            {
                scene.ClearSelection();
                ClearPendingSelectionCollapse();
            }
        }
    }

    if (IsHierarchyDragActive())
    {
        const ImGuiWindow* listWindow = ImGui::GetCurrentWindow();
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        const bool mouseInsideList =
            mousePos.x >= listWindow->InnerRect.Min.x && mousePos.x <= listWindow->InnerRect.Max.x
            && mousePos.y >= listWindow->InnerRect.Min.y && mousePos.y <= listWindow->InnerRect.Max.y;
        if (!mouseInsideList)
        {
            ClearDragInsertLatch();
        }
    }

    m_scrollSelectionIntoView = false;

    UpdatePendingSelectionCollapse(scene);

    ImGui::PopStyleVar(2);
    ImGui::EndChild();

    const std::string& importError = scene.GetImportService().GetLastImportError();
    if (!importError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Import failed: %s", importError.c_str());
    }

    const std::string& importWarning = scene.GetImportService().GetLastImportWarning();
    if (!importWarning.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "Import warning: %s", importWarning.c_str());
    }

    m_drawUndoStack = nullptr;
    m_drawClipboard = nullptr;

    ImGui::End();
}
