#include "app/SceneInspectorPanel.h"

#include "app/EditorWidgets.h"
#include "app/Scene.h"
#include "engine/FileDialog.h"
#include "engine/Light.h"
#include "engine/LightComponent.h"
#include "engine/Material.h"
#include "engine/SceneObject.h"
#include "engine/Texture.h"
#include "engine/TextureCache.h"
#include "engine/Transform.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <stdexcept>

namespace
{
    constexpr float kTransformRowLabelWidth = 68.0f;

    const char* const kAxisLabels[] = {"X", "Y", "Z"};
    const ImVec4 kAxisColors[] = {
        ImVec4(0.86f, 0.33f, 0.33f, 1.0f),
        ImVec4(0.52f, 0.78f, 0.40f, 1.0f),
        ImVec4(0.42f, 0.58f, 0.92f, 1.0f),
    };

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

    void DrawMaterialTextureSlot(
        const char* label,
        bool hasMap,
        const std::string& path,
        TextureColorSpace colorSpace,
        const std::function<void(std::shared_ptr<Texture>, const std::string&)>& assign,
        const std::function<void()>& clear)
    {
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();

        if (hasMap)
        {
            if (path.empty())
            {
                ImGui::TextUnformatted("(embedded)");
            }
            else
            {
                const std::string fileName = std::filesystem::path(path).filename().string();
                ImGui::TextUnformatted(fileName.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", path.c_str());
                }
            }
        }
        else
        {
            ImGui::TextDisabled("None");
        }

        if (ImGui::Button("Browse"))
        {
            std::string selectedPath;
            if (FileDialog::OpenImageFile(selectedPath))
            {
                try
                {
                    std::shared_ptr<Texture> texture = TextureCache::Get().Load(
                        selectedPath.c_str(),
                        colorSpace);
                    assign(texture, selectedPath);
                }
                catch (const std::exception&)
                {
                    ImGui::OpenPopup("TextureLoadError");
                }
            }
        }

        if (ImGui::BeginPopup("TextureLoadError"))
        {
            ImGui::TextUnformatted("Failed to load texture.");
            if (ImGui::Button("OK"))
            {
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasMap);
        if (ImGui::Button("Clear"))
        {
            clear();
        }
        ImGui::EndDisabled();

        ImGui::PopID();
    }

    void DrawMaterialSection(Material& material)
    {
        glm::vec3 albedo = material.GetAlbedo();
        if (EditorWidgets::ColorEditVec3("Albedo", albedo))
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

        bool doubleSided = material.IsDoubleSided();
        if (ImGui::Checkbox("Double sided", &doubleSided))
        {
            material.SetDoubleSided(doubleSided);
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Maps");

        DrawMaterialTextureSlot(
            "Albedo",
            material.HasAlbedoMap(),
            material.GetAlbedoMapPath(),
            TextureColorSpace::SRGB,
            [&material](std::shared_ptr<Texture> texture, const std::string& path) {
                material.SetAlbedoMap(std::move(texture), path);
            },
            [&material]() { material.ClearAlbedoMap(); });

        if (material.HasAlbedoMap())
        {
            int albedoTexCoordSet = material.GetAlbedoTexCoordSet();
            if (ImGui::Combo("Albedo UV set", &albedoTexCoordSet, "0\0" "1\0"))
            {
                material.SetAlbedoTexCoordSet(albedoTexCoordSet);
            }
        }

        DrawMaterialTextureSlot(
            "Normal",
            material.HasNormalMap(),
            material.GetNormalMapPath(),
            TextureColorSpace::Linear,
            [&material](std::shared_ptr<Texture> texture, const std::string& path) {
                material.SetNormalMap(std::move(texture), path);
            },
            [&material]() { material.ClearNormalMap(); });

        if (material.HasNormalMap())
        {
            int normalTexCoordSet = material.GetNormalTexCoordSet();
            if (ImGui::Combo("Normal UV set", &normalTexCoordSet, "0\0" "1\0"))
            {
                material.SetNormalTexCoordSet(normalTexCoordSet);
            }
        }

        DrawMaterialTextureSlot(
            "Ambient occlusion",
            material.HasAoMap(),
            material.GetAoMapPath(),
            TextureColorSpace::Linear,
            [&material](std::shared_ptr<Texture> texture, const std::string& path) {
                material.SetAoMap(std::move(texture), path);
            },
            [&material]() { material.ClearAoMap(); });

        if (material.HasAoMap())
        {
            int aoTexCoordSet = material.GetAoTexCoordSet();
            if (ImGui::Combo("AO UV set", &aoTexCoordSet, "0\0" "1\0"))
            {
                material.SetAoTexCoordSet(aoTexCoordSet);
            }
        }

        if (material.HasMetallicRoughnessMap())
        {
            DrawMaterialTextureSlot(
                "Metallic-roughness",
                true,
                material.GetRoughnessMapPath(),
                TextureColorSpace::Linear,
                [&material](std::shared_ptr<Texture> texture, const std::string& path) {
                    material.SetMetallicRoughnessMap(
                        std::move(texture),
                        material.GetRoughnessTexCoordSet(),
                        path);
                },
                [&material]() { material.ClearRoughnessMap(); });

            int roughnessTexCoordSet = material.GetRoughnessTexCoordSet();
            if (ImGui::Combo("Metallic-roughness UV set", &roughnessTexCoordSet, "0\0" "1\0"))
            {
                material.SetRoughnessTexCoordSet(roughnessTexCoordSet);
            }

            ImGui::TextDisabled("Uses glTF packing: green = roughness, blue = metallic.");
        }
        else
        {
            DrawMaterialTextureSlot(
                "Roughness",
                material.HasRoughnessMap(),
                material.GetRoughnessMapPath(),
                TextureColorSpace::Linear,
                [&material](std::shared_ptr<Texture> texture, const std::string& path) {
                    material.SetRoughnessMap(std::move(texture), path);
                },
                [&material]() { material.ClearRoughnessMap(); });

            if (material.HasRoughnessMap())
            {
                int roughnessTexCoordSet = material.GetRoughnessTexCoordSet();
                if (ImGui::Combo("Roughness UV set", &roughnessTexCoordSet, "0\0" "1\0"))
                {
                    material.SetRoughnessTexCoordSet(roughnessTexCoordSet);
                }
            }

            ImGui::PushID("MetallicRoughnessAssign");
            if (ImGui::Button("Assign metallic-roughness map"))
            {
                std::string selectedPath;
                if (FileDialog::OpenImageFile(selectedPath))
                {
                    try
                    {
                        std::shared_ptr<Texture> texture = TextureCache::Get().Load(
                            selectedPath.c_str(),
                            TextureColorSpace::Linear);
                        material.SetMetallicRoughnessMap(
                            std::move(texture),
                            material.GetRoughnessTexCoordSet(),
                            selectedPath);
                    }
                    catch (const std::exception&)
                    {
                        ImGui::OpenPopup("TextureLoadError");
                    }
                }
            }
            ImGui::PopID();
        }

        if (material.HasAlbedoMap())
        {
            ImGui::TextUnformatted("Albedo tints the albedo map.");
        }
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

    void DrawLightSection(SceneObject& object, Scene& scene)
    {
        LightComponent& light = object.GetLight();

        int lightTypeIndex = static_cast<int>(light.type);
        if (ImGui::Combo(
                "Type",
                &lightTypeIndex,
                "Directional\0Point\0Spot\0",
                3))
        {
            const LightType newType = static_cast<LightType>(lightTypeIndex);
            if (newType != light.type)
            {
                const bool preserveShadow = light.castsShadow;
                light = MakeDefaultLightComponent(newType);
                light.castsShadow = preserveShadow && newType == LightType::Directional;
            }
        }

        if (EditorWidgets::ColorEditVec3("Color", light.color))
        {
        }

        ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 8.0f);

        if (light.type == LightType::Directional)
        {
            bool castsShadow = light.castsShadow;
            if (ImGui::Checkbox("Cast shadows", &castsShadow))
            {
                if (castsShadow)
                {
                    for (SceneObject& otherObject : scene.GetObjects())
                    {
                        if (&otherObject != &object && otherObject.HasLight())
                        {
                            otherObject.GetLight().castsShadow = false;
                        }
                    }
                }

                light.castsShadow = castsShadow;
            }

            ImGui::TextUnformatted("Rotation aims local +Y toward the light source.");
            ImGui::TextUnformatted("Position sets the gizmo anchor in the scene.");
        }
        else
        {
            light.castsShadow = false;
            ImGui::SliderFloat("Range", &light.range, 0.0f, 25.0f);

            if (light.type == LightType::Spot)
            {
                ImGui::SliderFloat("Inner angle", &light.innerCutoffDegrees, 0.0f, light.outerCutoffDegrees - 1.0f);
                ImGui::SliderFloat("Outer angle", &light.outerCutoffDegrees, 1.0f, 89.0f);
                if (light.innerCutoffDegrees >= light.outerCutoffDegrees)
                {
                    light.innerCutoffDegrees = light.outerCutoffDegrees - 1.0f;
                }
            }

            ImGui::TextUnformatted("Position sets the light source. Rotation aims local +Y toward the source.");
        }
    }
}

void SceneInspectorPanel::Draw(Scene& scene) const
{
    const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(displaySize.x - 288.0f, 8.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(280.0f, 700.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Inspector", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    if (!scene.HasSelection())
    {
        ImGui::TextUnformatted("No object selected.");
        ImGui::End();
        return;
    }

    const int selectedIndex = scene.GetSelectedObjectIndex();
    const std::vector<SceneObject>& objects = scene.GetObjects();
    if (selectedIndex < 0 || selectedIndex >= static_cast<int>(objects.size()))
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
            selectedObject.GetTransform().Reset();
        }

        ImGui::EndPopup();
    }

    if (transformOpen)
    {
        DrawTransformSection(selectedObject);
    }

    if (selectedObject.HasLight() && ImGui::CollapsingHeader("Light", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID("LightSection");
        DrawLightSection(selectedObject, scene);
        ImGui::PopID();
    }

    if (!selectedObject.HasLight() && ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (selectedObject.IsRenderable())
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
        else
        {
            ImGui::TextUnformatted("Empty object (transform container only).");
            bool movable = selectedObject.IsMovable();
            if (ImGui::Checkbox("Movable", &movable))
            {
                selectedObject.SetMovable(movable);
            }
        }
    }

    if (selectedObject.HasMaterial() && ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID(selectedIndex);
        DrawMaterialSection(selectedObject.GetMaterial());
        ImGui::PopID();
    }

    ImGui::End();
}
