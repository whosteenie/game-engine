#include "app/SceneHierarchyPanel.h"

#include "app/DemoScene.h"
#include "engine/Material.h"
#include "engine/SceneObject.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <cstdio>

namespace
{
    bool SliderVec3(const char* label, glm::vec3& value, float min, float max)
    {
        return ImGui::SliderFloat3(label, &value.x, min, max);
    }

    bool ColorEditVec3(const char* label, glm::vec3& value)
    {
        return ImGui::ColorEdit3(label, &value.x);
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

    void DrawTransformSection(SceneObject& object)
    {
        Transform& transform = object.GetTransform();

        SliderVec3("Position", transform.position, -20.0f, 20.0f);
        if (!object.HasAutoSpin())
        {
            SliderVec3("Rotation", transform.rotationDegrees, -180.0f, 180.0f);
        }
        else
        {
            ImGui::TextUnformatted("Rotation driven by auto spin.");
        }

        SliderVec3("Scale", transform.scale, 0.1f, 5.0f);
    }
}

void SceneHierarchyPanel::Draw(DemoScene& scene) const
{
    ImGui::SetNextWindowSize(ImVec2(360.0f, 520.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Scene", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Arrow keys / Page Up-Down move selected object.");
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

    if (ImGui::Button("Add cube"))
    {
        scene.AddCubeObject();
        selectedIndex = static_cast<int>(scene.GetObjects().size()) - 1;
        scene.SetSelectedObjectIndex(selectedIndex);
    }

    ImGui::Separator();

    SceneObject& selectedObject = scene.GetObject(static_cast<std::size_t>(selectedIndex));
    ImGui::Text("Inspector: %s", selectedObject.GetName().c_str());

    char nameBuffer[64];
    std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", selectedObject.GetName().c_str());
    if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
    {
        selectedObject.SetName(nameBuffer);
    }

    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
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

        bool autoSpin = selectedObject.HasAutoSpin();
        if (ImGui::Checkbox("Auto spin", &autoSpin))
        {
            selectedObject.SetAutoSpin(autoSpin);
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
