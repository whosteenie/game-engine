#include "app/SceneHierarchyPanel.h"

#include "app/EditorClipboard.h"
#include "app/EditorPanelLayout.h"
#include "app/ProjectSession.h"
#include "app/Scene.h"
#include "app/UndoCommand.h"
#include "engine/FileDialog.h"
#include "engine/Light.h"
#include "engine/SceneObject.h"
#include "engine/SceneHierarchy.h"
#include "engine/SceneObjectId.h"
#include "engine/ScenePrimitive.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    constexpr const char* kHierarchyDragDropPayload = "SCENE_HIERARCHY_ITEM";
    constexpr float kHierarchyInsertGapHeight = 4.0f;

    SceneObjectId GetObjectId(const Scene& scene, int objectIndex)
    {
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= scene.GetObjects().size())
        {
            return kInvalidSceneObjectId;
        }

        return scene.GetObject(static_cast<std::size_t>(objectIndex)).GetId();
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
        Scene& scene,
        int objectIndex,
        const std::unordered_map<SceneObjectId, bool>& openStates)
    {
        std::vector<int> visibleIndices;
        BuildVisibleObjectOrder(scene, openStates, visibleIndices);

        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl)
        {
            scene.ToggleSelected(objectIndex);
        }
        else if (io.KeyShift)
        {
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
        else
        {
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
                target.SetSelectedObjectIndex(newIndex);
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
                target.SetSelectedObjectIndex(newIndex);
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
                target.SetSelectedObjectIndex(newIndex);
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
                target.SetSelectedObjectIndex(importedIndices.front());
            }

            return importedIndices;
        });

        if (!scene.GetLastImportError().empty())
        {
            project.SetStatusMessage(scene.GetLastImportError());
        }
        else if (!scene.GetLastImportWarning().empty())
        {
            project.SetStatusMessage(scene.GetLastImportWarning());
        }
    }

    void DrawCreateObjectMenu(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        ProjectSession& project,
        int parentIndex)
    {
        AddEmptyFromMenu(panel, scene, parentIndex);
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

        scene.SetSelectedObjectIndex(objectIndex);
        DrawCreateObjectMenu(panel, scene, project, objectIndex);

        ImGui::Separator();
        if (ImGui::MenuItem("Rename"))
        {
            renameTargetIndex = objectIndex;
            beginRenameNextFrame = true;
            pendingRenameIndex = -1;
            std::snprintf(
                renameBuffer,
                renameBufferSize,
                "%s",
                scene.GetObject(static_cast<std::size_t>(objectIndex)).GetName().c_str());
            focusRenameInput = true;
        }

        if (ImGui::MenuItem("Duplicate", "Ctrl+D"))
        {
            panel.PushInsertMutation(scene, "Duplicate", [objectIndex](Scene& target) {
                const int duplicatedIndex = target.DuplicateObject(objectIndex);
                if (duplicatedIndex >= 0)
                {
                    target.SetSelectedObjectIndex(duplicatedIndex);
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
                scene.GetObject(static_cast<std::size_t>(objectIndex)).GetName();
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

    void DrawHierarchyDragDropSource(int objectIndex, const std::string& label, bool enabled)
    {
        if (!enabled)
        {
            return;
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
        {
            ImGui::SetDragDropPayload(kHierarchyDragDropPayload, &objectIndex, sizeof(int));
            ImGui::TextUnformatted(label.c_str());
            ImGui::EndDragDropSource();
        }
    }

    void DrawHierarchyInsertGapLine()
    {
        const ImVec2 gapMin = ImGui::GetItemRectMin();
        const ImVec2 gapMax = ImGui::GetItemRectMax();
        const float lineY = (gapMin.y + gapMax.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(gapMin.x, lineY),
            ImVec2(gapMax.x, lineY),
            IM_COL32(90, 150, 255, 255),
            2.0f);
    }

    void DrawHierarchyInsertGap(
        const SceneHierarchyPanel& panel,
        Scene& scene,
        int referenceIndex,
        HierarchyInsertMode mode)
    {
        ImGui::PushID(referenceIndex);
        ImGui::PushID(static_cast<int>(mode));
        ImGui::InvisibleButton("##HierarchyInsertGap", ImVec2(-FLT_MIN, kHierarchyInsertGapHeight));

        if (ImGui::BeginDragDropTarget())
        {
            const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
            if (activePayload != nullptr && activePayload->IsDataType(kHierarchyDragDropPayload))
            {
                const int draggedIndex = *static_cast<const int*>(activePayload->Data);
                if (scene.CanPlaceObjectInHierarchy(draggedIndex, referenceIndex, mode)
                    && scene.WouldPlaceObjectInHierarchyChange(draggedIndex, referenceIndex, mode))
                {
                    DrawHierarchyInsertGapLine();

                    const ImGuiDragDropFlags acceptFlags =
                        ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                    if (const ImGuiPayload* payload =
                            ImGui::AcceptDragDropPayload(kHierarchyDragDropPayload, acceptFlags))
                    {
                        if (payload->IsDelivery())
                        {
                            const SceneObjectId draggedId = GetObjectId(scene, draggedIndex);
                            const SceneObjectId referenceId = GetObjectId(scene, referenceIndex);
                            panel.PushReparentMutation(
                                scene,
                                "Reparent",
                                draggedId,
                                referenceId,
                                mode);
                        }
                    }
                }
            }

            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
        ImGui::PopID();
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
        if (activePayload != nullptr && activePayload->IsDataType(kHierarchyDragDropPayload))
        {
            const int draggedIndex = *static_cast<const int*>(activePayload->Data);
            const HierarchyInsertMode mode = HierarchyInsertMode::AsChild;
            if (scene.CanPlaceObjectInHierarchy(draggedIndex, referenceIndex, mode)
                && scene.WouldPlaceObjectInHierarchyChange(draggedIndex, referenceIndex, mode))
            {
                DrawHierarchyReparentIndicator();
                openStates[GetObjectId(scene, referenceIndex)] = true;

                const ImGuiDragDropFlags acceptFlags =
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kHierarchyDragDropPayload, acceptFlags))
                {
                    if (payload->IsDelivery())
                    {
                        const SceneObjectId draggedId = GetObjectId(scene, draggedIndex);
                        const SceneObjectId referenceId = GetObjectId(scene, referenceIndex);
                        panel.PushReparentMutation(
                            scene,
                            "Reparent",
                            draggedId,
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
        bool& renameInputEngaged)
    {
        DrawHierarchyInsertGap(panel, scene, objectIndex, HierarchyInsertMode::Before);

        const SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
        const std::vector<int> children = scene.GetChildren(objectIndex);
        const bool isRenaming = objectIndex == pendingRenameIndex;

        ImGui::PushID(objectIndex);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
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
                    flags | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanTextWidth;
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
                    const SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
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

            if (ImGui::IsItemClicked())
            {
                HandleHierarchyRowSelection(scene, objectIndex, openStates);
            }

            const bool allowDragDrop = true;
            DrawHierarchyDragDropSource(objectIndex, label, allowDragDrop);
            DrawHierarchyRowDropTarget(panel, scene, objectIndex, openStates);
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
                    renameInputEngaged);
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
    SceneObjectId objectId,
    SceneObjectId referenceId,
    HierarchyInsertMode mode) const
{
    if (m_drawUndoStack != nullptr)
    {
        PushReparentObjects(*m_drawUndoStack, scene, commandName, objectId, referenceId, mode);
        return;
    }

    const int objectIndex = scene.FindObjectIndex(objectId);
    const int referenceIndex = scene.FindObjectIndex(referenceId);
    if (objectIndex >= 0 && referenceIndex >= 0)
    {
        scene.PlaceObjectInHierarchy(objectIndex, referenceIndex, mode);
        scene.SetSelectedObjectIndex(objectIndex);
    }
}

void SceneHierarchyPanel::Draw(
    Scene& scene,
    ProjectSession& project,
    UndoStack& undoStack,
    EditorClipboard& clipboard) const
{
    m_drawUndoStack = &undoStack;
    m_drawClipboard = &clipboard;

    EditorPanelLayout::ApplyFirstUseLayout(EditorPanelLayout::Panel::Hierarchy);

    if (!ImGui::Begin("Hierarchy", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
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

    const float footerHeight = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    ImGui::BeginChild("HierarchyList", ImVec2(0.0f, -footerHeight), ImGuiChildFlags_Borders);

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
    }

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
            m_renameInputEngaged);
    }

    std::vector<int> visibleObjectIndices;
    BuildVisibleObjectOrder(scene, m_nodeOpenStates, visibleObjectIndices);
    if (!visibleObjectIndices.empty())
    {
        DrawHierarchyInsertGap(
            *this,
            scene,
            visibleObjectIndices.back(),
            HierarchyInsertMode::After);
    }

    const ImVec2 backgroundSpace = ImGui::GetContentRegionAvail();
    if (backgroundSpace.y > 0.0f)
    {
        ImGui::InvisibleButton("##HierarchyBackground", backgroundSpace);
        DrawHierarchyBackgroundContextMenu(*this, scene, project, undoStack, clipboard);
    }

    m_scrollSelectionIntoView = false;

    ImGui::EndChild();

    const std::string& importError = scene.GetLastImportError();
    if (!importError.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Import failed: %s", importError.c_str());
    }

    const std::string& importWarning = scene.GetLastImportWarning();
    if (!importWarning.empty())
    {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "Import warning: %s", importWarning.c_str());
    }

    ImGui::BeginDisabled(!scene.HasSelection());
    if (ImGui::Button("Delete"))
    {
        if (m_drawUndoStack != nullptr)
        {
            PushDeleteSelection(*m_drawUndoStack, scene, "Delete");
        }
        else
        {
            scene.RemoveSelectedObjects();
        }
    }
    ImGui::EndDisabled();

    m_drawUndoStack = nullptr;
    m_drawClipboard = nullptr;

    ImGui::End();
}
