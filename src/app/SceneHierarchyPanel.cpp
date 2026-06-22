#include "app/SceneHierarchyPanel.h"

#include "app/Scene.h"
#include "engine/FileDialog.h"
#include "engine/SceneObject.h"
#include "engine/ScenePrimitive.h"

#include <imgui.h>

#include <cstdio>

namespace
{
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

    void DrawHierarchyNode(
        Scene& scene,
        int objectIndex,
        int selectedIndex,
        int& pendingDeleteIndex,
        int& pendingRenameIndex,
        int& renameTargetIndex,
        bool& beginRenameNextFrame,
        char* renameBuffer,
        std::size_t renameBufferSize,
        bool& focusRenameInput,
        bool& renameInputEngaged)
    {
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
            if (ImGui::IsItemClicked())
            {
                scene.SetSelectedObjectIndex(objectIndex);
            }
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

    if (ImGui::Selectable("(none)", selectedIndex < 0))
    {
        scene.ClearSelection();
        selectedIndex = -1;
    }

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        if (objects[static_cast<std::size_t>(objectIndex)].GetParentIndex() >= 0)
        {
            continue;
        }

        DrawHierarchyNode(
            scene,
            objectIndex,
            selectedIndex,
            m_pendingDeleteIndex,
            m_pendingRenameIndex,
            m_renameTargetIndex,
            m_beginRenameNextFrame,
            m_renameBuffer,
            sizeof(m_renameBuffer),
            m_focusRenameInput,
            m_renameInputEngaged);
    }

    if (m_pendingDeleteIndex >= 0)
    {
        scene.RemoveObject(static_cast<std::size_t>(m_pendingDeleteIndex));
        m_pendingDeleteIndex = -1;
        m_pendingRenameIndex = -1;
        m_renameTargetIndex = -1;
        m_beginRenameNextFrame = false;
        m_focusRenameInput = false;
        m_renameInputEngaged = false;
        selectedIndex = scene.GetSelectedObjectIndex();
    }

    if (ImGui::BeginPopupContextWindow(
            "HierarchyContextMenu",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        DrawCreateObjectMenu(scene, -1);
        ImGui::EndPopup();
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
    }
    ImGui::EndDisabled();

    ImGui::End();
}
