#include "app/SceneHierarchyPanel.h"

#include "app/Scene.h"
#include "app/SceneEditor.h"
#include "engine/Material.h"
#include "engine/SceneObject.h"
#include "engine/ScenePrimitive.h"

#include <imgui.h>

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <cstdio>

namespace
{
    constexpr float kTransformRowLabelWidth = 68.0f;

    const char* const kAxisLabels[] = {"X", "Y", "Z"};
    const ImVec4 kAxisColors[] = {
        ImVec4(0.86f, 0.33f, 0.33f, 1.0f),
        ImVec4(0.52f, 0.78f, 0.40f, 1.0f),
        ImVec4(0.42f, 0.58f, 0.92f, 1.0f),
    };

    bool ColorEditVec3(const char* label, glm::vec3& value)
    {
        return ImGui::ColorEdit3(label, &value.x);
    }

    bool DrawTransformRowLabel(const char* label, glm::vec3& value, const glm::vec3& resetValue)
    {
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
        ImGui::Selectable(label, false, ImGuiSelectableFlags_None);

        bool changed = false;
        if (ImGui::BeginPopupContextItem())
        {
            char menuLabel[64];
            std::snprintf(menuLabel, sizeof(menuLabel), "Reset %s", label);
            if (ImGui::MenuItem(menuLabel))
            {
                value = resetValue;
                changed = true;
            }

            ImGui::EndPopup();
        }

        ImGui::PopStyleColor(3);
        return changed;
    }

    bool DrawTransformAxisField(
        int axis,
        const char* label,
        glm::vec3& value,
        const glm::vec3& resetValue,
        float dragSpeed,
        const char* format)
    {
        ImGui::PushID(axis);
        ImGui::AlignTextToFramePadding();

        ImGui::PushStyleColor(ImGuiCol_Text, kAxisColors[axis]);
        ImGui::TextUnformatted(kAxisLabels[axis]);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool dragged = ImGui::DragFloat("##value", &value[axis], dragSpeed, 0.0f, 0.0f, format);

        bool changed = dragged;
        if (ImGui::BeginPopupContextItem())
        {
            char menuLabel[64];
            std::snprintf(menuLabel, sizeof(menuLabel), "Reset %s %s", kAxisLabels[axis], label);
            if (ImGui::MenuItem(menuLabel))
            {
                value[axis] = resetValue[axis];
                changed = true;
            }

            ImGui::EndPopup();
        }

        ImGui::PopID();
        return changed;
    }

    bool DrawTransformRow(
        const char* label,
        glm::vec3& value,
        const glm::vec3& resetValue,
        float dragSpeed,
        const char* format = "%.2f")
    {
        ImGui::PushID(label);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        bool changed = DrawTransformRowLabel(label, value, resetValue);

        for (int axis = 0; axis < 3; ++axis)
        {
            ImGui::TableSetColumnIndex(axis + 1);
            changed |= DrawTransformAxisField(axis, label, value, resetValue, dragSpeed, format);
        }

        ImGui::PopID();
        return changed;
    }

    void DrawMaterialSection(Material& material)
    {
        glm::vec3 albedo = material.GetAlbedo();
        if (ColorEditVec3("Albedo", albedo))
        {
            material.SetAlbedo(albedo);
        }

        float roughness = material.GetRoughness();
        if (ImGui::SliderFloat("Roughness", &roughness, 0.04f, 1.0f))
        {
            material.SetRoughness(roughness);
        }

        float metallic = material.GetMetallic();
        if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f))
        {
            material.SetMetallic(metallic);
        }

        if (material.HasAlbedoMap())
        {
            ImGui::TextUnformatted("Albedo tints the albedo map.");
        }
    }

    void ResetTransform(Transform& transform)
    {
        transform.Reset();
    }

    void DrawTransformSection(SceneObject& object)
    {
        Transform& transform = object.GetTransform();

        ImGui::PushID("TransformTable");
        if (ImGui::BeginTable(
                "##fields",
                4,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody))
        {
            ImGui::TableSetupColumn("##row", ImGuiTableColumnFlags_WidthFixed, kTransformRowLabelWidth);
            ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##y", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##z", ImGuiTableColumnFlags_WidthStretch, 1.0f);

            DrawTransformRow("Position", transform.position, glm::vec3(0.0f), 0.1f);

            glm::vec3 rotationDegrees = transform.GetRotationDegrees();
            if (DrawTransformRow("Rotation", rotationDegrees, glm::vec3(0.0f), 0.5f))
            {
                transform.SetRotationDegrees(rotationDegrees);
            }

            DrawTransformRow("Scale", transform.scale, glm::vec3(1.0f), 0.01f);

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    bool DrawToolButton(const char* label, TransformTool tool, TransformTool activeTool, Scene& scene)
    {
        const bool isActive = tool == activeTool;
        if (isActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
        }

        const bool clicked = ImGui::Button(label);
        if (isActive)
        {
            ImGui::PopStyleColor();
        }

        if (clicked)
        {
            scene.GetSceneEditor().SetTool(tool);
        }

        return clicked;
    }

    bool DrawSpaceButton(const char* label, TransformSpace space, TransformSpace activeSpace, Scene& scene)
    {
        const bool isActive = space == activeSpace;
        if (isActive)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.55f, 0.85f, 1.0f));
        }

        const bool clicked = ImGui::Button(label);
        if (isActive)
        {
            ImGui::PopStyleColor();
        }

        if (clicked)
        {
            scene.GetSceneEditor().SetTransformSpace(space);
        }

        return clicked;
    }

    bool AddPrimitiveFromMenu(Scene& scene, ScenePrimitive primitive)
    {
        if (ImGui::MenuItem(GetScenePrimitiveDisplayName(primitive)))
        {
            const int newIndex = scene.AddObject(primitive);
            scene.SetSelectedObjectIndex(newIndex);
            return true;
        }

        return false;
    }

    void Draw3DObjectMenu(Scene& scene)
    {
        if (ImGui::BeginMenu("3D Object"))
        {
            AddPrimitiveFromMenu(scene, ScenePrimitive::Cube);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Sphere);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Cylinder);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Capsule);
            AddPrimitiveFromMenu(scene, ScenePrimitive::Plane);
            ImGui::EndMenu();
        }
    }

    void DrawAddObjectPopup(Scene& scene)
    {
        if (ImGui::BeginPopup("AddObjectPopup"))
        {
            Draw3DObjectMenu(scene);
            ImGui::EndPopup();
        }
    }
}

void SceneHierarchyPanel::Draw(Scene& scene) const
{
    ImGui::SetNextWindowSize(ImVec2(400.0f, 560.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Scene", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    SceneEditor& editor = scene.GetSceneEditor();
    const TransformTool activeTool = editor.GetTool();
    const TransformSpace activeSpace = editor.GetTransformSpace();

    DrawToolButton("Move (W)", TransformTool::Translate, activeTool, scene);
    ImGui::SameLine();
    DrawToolButton("Rotate (E)", TransformTool::Rotate, activeTool, scene);
    ImGui::SameLine();
    DrawToolButton("Scale (R)", TransformTool::Scale, activeTool, scene);

    if (activeTool != TransformTool::Scale)
    {
        DrawSpaceButton("Local (L)", TransformSpace::Local, activeSpace, scene);
        ImGui::SameLine();
        DrawSpaceButton("World (G)", TransformSpace::World, activeSpace, scene);
    }
    else
    {
        ImGui::TextUnformatted("Scale gizmo uses local axes.");
    }

    ImGui::TextUnformatted("LMB: select/deselect or drag gizmo.");
    ImGui::TextUnformatted("RMB + WASD/Q/E: fly camera. Shift: move faster.");
    ImGui::Separator();

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

    ImGui::TextUnformatted("Hierarchy");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Create"))
    {
        ImGui::OpenPopup("AddObjectPopup");
    }
    DrawAddObjectPopup(scene);

    ImGui::BeginChild("HierarchyList", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders);
    if (ImGui::Selectable("(none)", selectedIndex < 0))
    {
        scene.ClearSelection();
        selectedIndex = -1;
    }

    for (int objectIndex = 0; objectIndex < static_cast<int>(objects.size()); ++objectIndex)
    {
        const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
        const bool isSelected = objectIndex == selectedIndex;

        if (ImGui::Selectable(object.GetName().c_str(), isSelected))
        {
            scene.SetSelectedObjectIndex(objectIndex);
            selectedIndex = objectIndex;
        }
    }

    if (ImGui::BeginPopupContextWindow("HierarchyContextMenu", ImGuiPopupFlags_MouseButtonRight))
    {
        Draw3DObjectMenu(scene);
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    ImGui::BeginDisabled(!scene.HasSelection());
    if (ImGui::Button("Delete"))
    {
        scene.RemoveObject(static_cast<std::size_t>(selectedIndex));
        selectedIndex = scene.GetSelectedObjectIndex();
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    if (!scene.HasSelection())
    {
        ImGui::TextUnformatted("No object selected.");
        ImGui::End();
        return;
    }

    SceneObject& selectedObject = scene.GetObject(static_cast<std::size_t>(selectedIndex));
    ImGui::Text("Inspector: %s", selectedObject.GetName().c_str());

    char nameBuffer[64];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", selectedObject.GetName().c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        selectedObject.SetName(nameBuffer);
    }

    const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Reset Transform"))
        {
            ResetTransform(selectedObject.GetTransform());
        }

        ImGui::EndPopup();
    }

    if (transformOpen)
    {
        DrawTransformSection(selectedObject);
    }

    if (ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool movable = selectedObject.IsMovable();
        if (ImGui::Checkbox("Movable", &movable))
        {
            selectedObject.SetMovable(movable);
        }

        bool castShadow = selectedObject.CastsShadow();
        if (ImGui::Checkbox("Cast shadow", &castShadow))
        {
            selectedObject.SetCastShadow(castShadow);
        }

        bool receiveShadow = selectedObject.ReceivesShadow();
        if (ImGui::Checkbox("Receive shadow", &receiveShadow))
        {
            selectedObject.SetReceiveShadow(receiveShadow);
        }
    }

    if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID(selectedIndex);
        DrawMaterialSection(selectedObject.GetMaterial());
        ImGui::PopID();
    }

    ImGui::End();
}
