#include "app/SceneHierarchyPanel.h"

#include "app/Scene.h"
#include "engine/FileDialog.h"
#include "engine/SceneObject.h"
#include "engine/SceneHierarchy.h"
#include "engine/ScenePrimitive.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace
{
    constexpr const char* kHierarchyDragDropPayload = "SCENE_HIERARCHY_ITEM";
    constexpr float kHierarchyInsertGapHeight = 4.0f;

    bool IsNodeExpanded(int objectIndex, const std::unordered_map<int, bool>& openStates)
    {
        const auto iterator = openStates.find(objectIndex);
        return iterator != openStates.end() && iterator->second;
    }

    void BuildVisibleObjectOrder(
        const Scene& scene,
        int objectIndex,
        const std::unordered_map<int, bool>& openStates,
        std::vector<int>& outVisibleIndices)
    {
        outVisibleIndices.push_back(objectIndex);

        if (!IsNodeExpanded(objectIndex, openStates))
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
        const std::unordered_map<int, bool>& openStates,
        std::vector<int>& outVisibleIndices)
    {
        outVisibleIndices.clear();
        for (int rootIndex : scene.GetRootObjectIndices())
        {
            BuildVisibleObjectOrder(scene, rootIndex, openStates, outVisibleIndices);
        }
    }

    void HandleHierarchyKeyboardNavigation(
        Scene& scene,
        int& selectedIndex,
        std::unordered_map<int, bool>& openStates,
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

        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        {
            if (selectedIndex < 0)
            {
                selectedIndex = visibleIndices.front();
            }
            else
            {
                const auto currentIterator =
                    std::find(visibleIndices.begin(), visibleIndices.end(), selectedIndex);
                if (currentIterator != visibleIndices.end() && currentIterator + 1 != visibleIndices.end())
                {
                    selectedIndex = *(currentIterator + 1);
                }
            }

            scene.SetSelectedObjectIndex(selectedIndex);
            scrollSelectionIntoView = true;
        }
        else if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        {
            if (selectedIndex < 0)
            {
                selectedIndex = visibleIndices.back();
            }
            else
            {
                const auto currentIterator =
                    std::find(visibleIndices.begin(), visibleIndices.end(), selectedIndex);
                if (currentIterator != visibleIndices.end() && currentIterator != visibleIndices.begin())
                {
                    selectedIndex = *(currentIterator - 1);
                }
            }

            scene.SetSelectedObjectIndex(selectedIndex);
            scrollSelectionIntoView = true;
        }
        else if (selectedIndex >= 0)
        {
            const std::vector<int> children = scene.GetChildren(selectedIndex);
            if (children.empty())
            {
                return;
            }

            if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && !IsNodeExpanded(selectedIndex, openStates))
            {
                openStates[selectedIndex] = true;
                scrollSelectionIntoView = true;
            }
            else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && IsNodeExpanded(selectedIndex, openStates))
            {
                openStates[selectedIndex] = false;
                scrollSelectionIntoView = true;
            }
        }
    }

    bool AddPrimitiveFromMenu(Scene& scene, ScenePrimitive primitive, int parentIndex)
    {
        if (ImGui::MenuItem(GetScenePrimitiveDisplayName(primitive)))
        {
            const int newIndex = scene.AddObject(primitive, parentIndex);
            scene.SetSelectedObjectIndex(newIndex);
            return true;
        }

        return false;
    }

    bool AddEmptyFromMenu(Scene& scene, int parentIndex)
    {
        if (ImGui::MenuItem("Empty"))
        {
            const int newIndex = scene.AddEmptyObject(parentIndex);
            scene.SetSelectedObjectIndex(newIndex);
            return true;
        }

        return false;
    }

    void Draw3DObjectMenu(Scene& scene, int parentIndex)
    {
        if (ImGui::BeginMenu("3D Object"))
        {
            AddPrimitiveFromMenu(scene, ScenePrimitive::Cube, parentIndex);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Sphere, parentIndex);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Cylinder, parentIndex);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Capsule, parentIndex);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Plane, parentIndex);
            ImGui::EndMenu();
        }
    }

    void ImportModelFromDialog(Scene& scene, int parentIndex)
    {
        std::string modelPath;
        if (!FileDialog::OpenModelFile(modelPath))
        {
            return;
        }

        const std::vector<int> importedIndices = scene.ImportModel(modelPath, parentIndex);
        if (importedIndices.empty())
        {
            return;
        }

        scene.SetSelectedObjectIndex(importedIndices.front());
    }

    void DrawCreateObjectMenu(Scene& scene, int parentIndex)
    {
        AddEmptyFromMenu(scene, parentIndex);
        Draw3DObjectMenu(scene, parentIndex);
        if (ImGui::MenuItem("Import Model..."))
        {
            ImportModelFromDialog(scene, parentIndex);
        }
    }

    void DrawObjectContextMenu(
        Scene& scene,
        int objectIndex,
        int& pendingDeleteIndex,
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
        DrawCreateObjectMenu(scene, objectIndex);

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

        if (ImGui::MenuItem("Delete"))
        {
            pendingDeleteIndex = objectIndex;
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

    void DrawHierarchyInsertGap(Scene& scene, int referenceIndex, HierarchyInsertMode mode)
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
                            scene.PlaceObjectInHierarchy(draggedIndex, referenceIndex, mode);
                            scene.SetSelectedObjectIndex(draggedIndex);
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
        Scene& scene,
        int referenceIndex,
        std::unordered_map<int, bool>& openStates)
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
                openStates[referenceIndex] = true;

                const ImGuiDragDropFlags acceptFlags =
                    ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
                if (const ImGuiPayload* payload =
                        ImGui::AcceptDragDropPayload(kHierarchyDragDropPayload, acceptFlags))
                {
                    if (payload->IsDelivery())
                    {
                        scene.PlaceObjectInHierarchy(draggedIndex, referenceIndex, mode);
                        scene.SetSelectedObjectIndex(draggedIndex);
                    }
                }
            }
        }

        ImGui::EndDragDropTarget();
    }

    void DrawHierarchyBackgroundContextMenu(Scene& scene)
    {
        if (ImGui::BeginPopupContextItem())
        {
            DrawCreateObjectMenu(scene, -1);
            ImGui::EndPopup();
        }
    }

    void DrawHierarchyNode(
        Scene& scene,
        int objectIndex,
        int selectedIndex,
        std::unordered_map<int, bool>& openStates,
        bool& scrollSelectionIntoView,
        int& pendingDeleteIndex,
        int& pendingRenameIndex,
        int& renameTargetIndex,
        bool& beginRenameNextFrame,
        char* renameBuffer,
        std::size_t renameBufferSize,
        bool& focusRenameInput,
        bool& renameInputEngaged)
    {
        DrawHierarchyInsertGap(scene, objectIndex, HierarchyInsertMode::Before);

        const SceneObject& object = scene.GetObject(static_cast<std::size_t>(objectIndex));
        const std::vector<int> children = scene.GetChildren(objectIndex);
        const bool isRenaming = objectIndex == pendingRenameIndex;

        ImGui::PushID(objectIndex);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (objectIndex == selectedIndex && !isRenaming)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        if (children.empty())
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        else
        {
            ImGui::SetNextItemOpen(IsNodeExpanded(objectIndex, openStates), ImGuiCond_Always);
        }

        const std::string label = object.IsRenderable() ? object.GetName() : "[Empty] " + object.GetName();
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
                    scene.GetObject(static_cast<std::size_t>(objectIndex)).SetName(renameBuffer);
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
                openStates[objectIndex] = opened;
            }

            if (objectIndex == selectedIndex && scrollSelectionIntoView)
            {
                ImGui::SetScrollHereY(0.5f);
            }

            if (ImGui::IsItemClicked())
            {
                scene.SetSelectedObjectIndex(objectIndex);
            }

            const bool allowDragDrop = true;
            DrawHierarchyDragDropSource(objectIndex, label, allowDragDrop);
            DrawHierarchyRowDropTarget(scene, objectIndex, openStates);
        }

        if (!isRenaming)
        {
            DrawObjectContextMenu(
                scene,
                objectIndex,
                pendingDeleteIndex,
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
                    scene,
                    childIndex,
                    selectedIndex,
                    openStates,
                    scrollSelectionIntoView,
                    pendingDeleteIndex,
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

    void DrawAddObjectPopup(Scene& scene)
    {
        if (ImGui::BeginPopup("AddObjectPopup"))
        {
            DrawCreateObjectMenu(scene, -1);
            ImGui::EndPopup();
        }
    }
}

void SceneHierarchyPanel::Draw(Scene& scene) const
{
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280.0f, 700.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Hierarchy", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    const std::vector<SceneObject>& objects = scene.GetObjects();
    int selectedIndex = scene.GetSelectedObjectIndex();

    if (objects.empty())
    {
        ImGui::TextUnformatted("No scene objects.");
        ImGui::End();
        return;
    }

    if (selectedIndex >= static_cast<int>(objects.size()))
    {
        selectedIndex = static_cast<int>(objects.size()) - 1;
        scene.SetSelectedObjectIndex(selectedIndex);
    }

    if (ImGui::SmallButton("+ Create"))
    {
        ImGui::OpenPopup("AddObjectPopup");
    }
    DrawAddObjectPopup(scene);
    selectedIndex = scene.GetSelectedObjectIndex();

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
        HandleHierarchyKeyboardNavigation(scene, selectedIndex, m_nodeOpenStates, m_scrollSelectionIntoView);
    }

    for (int objectIndex : scene.GetRootObjectIndices())
    {
        DrawHierarchyNode(
            scene,
            objectIndex,
            selectedIndex,
            m_nodeOpenStates,
            m_scrollSelectionIntoView,
            m_pendingDeleteIndex,
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
            scene,
            visibleObjectIndices.back(),
            HierarchyInsertMode::After);
    }

    const ImVec2 backgroundSpace = ImGui::GetContentRegionAvail();
    if (backgroundSpace.y > 0.0f)
    {
        ImGui::InvisibleButton("##HierarchyBackground", backgroundSpace);
        DrawHierarchyBackgroundContextMenu(scene);
    }

    m_scrollSelectionIntoView = false;

    if (m_pendingDeleteIndex >= 0)
    {
        scene.RemoveObject(static_cast<std::size_t>(m_pendingDeleteIndex));
        m_nodeOpenStates.clear();
        m_pendingDeleteIndex = -1;
        m_pendingRenameIndex = -1;
        m_renameTargetIndex = -1;
        m_beginRenameNextFrame = false;
        m_focusRenameInput = false;
        m_renameInputEngaged = false;
        selectedIndex = scene.GetSelectedObjectIndex();
    }

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
        scene.RemoveObject(static_cast<std::size_t>(scene.GetSelectedObjectIndex()));
        m_nodeOpenStates.clear();
    }
    ImGui::EndDisabled();

    ImGui::End();
}
