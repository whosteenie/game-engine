#include "app/SceneInspectorPanel.h"

#include "app/EditorPanelLayout.h"
#include "app/EditorWidgets.h"
#include "app/InspectorEditMode.h"
#include "app/InspectorMultiEdit.h"
#include "app/InspectorTransform.h"
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
#include <string>
#include <vector>

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
        Scene& scene,
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
                    scene.MarkDirty();
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
            scene.MarkDirty();
        }
        ImGui::EndDisabled();

        ImGui::PopID();
    }

    void DrawMaterialSection(Material& material, Scene& scene)
    {
        glm::vec3 albedo = material.GetAlbedo();
        if (EditorWidgets::ColorEditVec3("Albedo", albedo))
        {
            material.SetAlbedo(albedo);
            scene.MarkDirty();
        }

        float roughness = material.GetRoughness();
        if (ImGui::SliderFloat("Roughness", &roughness, 0.04f, 1.0f))
        {
            material.SetRoughness(roughness);
            scene.MarkDirty();
        }

        float metallic = material.GetMetallic();
        if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f))
        {
            material.SetMetallic(metallic);
            scene.MarkDirty();
        }

        bool doubleSided = material.IsDoubleSided();
        if (ImGui::Checkbox("Double sided", &doubleSided))
        {
            material.SetDoubleSided(doubleSided);
            scene.MarkDirty();
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Maps");

        DrawMaterialTextureSlot(
            "Albedo",
            material.HasAlbedoMap(),
            material.GetAlbedoMapPath(),
            TextureColorSpace::SRGB,
            scene,
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
                scene.MarkDirty();
            }
        }

        DrawMaterialTextureSlot(
            "Normal",
            material.HasNormalMap(),
            material.GetNormalMapPath(),
            TextureColorSpace::Linear,
            scene,
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
                scene.MarkDirty();
            }
        }

        DrawMaterialTextureSlot(
            "Ambient occlusion",
            material.HasAoMap(),
            material.GetAoMapPath(),
            TextureColorSpace::Linear,
            scene,
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
                scene.MarkDirty();
            }
        }

        if (material.HasMetallicRoughnessMap())
        {
            DrawMaterialTextureSlot(
                "Metallic-roughness",
                true,
                material.GetRoughnessMapPath(),
                TextureColorSpace::Linear,
                scene,
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
                scene.MarkDirty();
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
                scene,
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
                    scene.MarkDirty();
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
                        scene.MarkDirty();
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

    void DrawTransformSection(SceneObject& object, Scene& scene)
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

            if (DrawTransformRow("Position", transform.position, glm::vec3(0.0f), 0.1f))
            {
                scene.MarkDirty();
            }

            glm::vec3 rotationDegrees = transform.GetRotationDegrees();
            if (DrawTransformRow("Rotation", rotationDegrees, glm::vec3(0.0f), 0.5f))
            {
                transform.SetRotationDegrees(rotationDegrees);
                scene.MarkDirty();
            }

            if (DrawTransformRow("Scale", transform.scale, glm::vec3(1.0f), 0.01f))
            {
                scene.MarkDirty();
            }

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
                scene.MarkDirty();
            }
        }

        if (EditorWidgets::ColorEditVec3("Color", light.color))
        {
            scene.MarkDirty();
        }

        if (ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 8.0f))
        {
            scene.MarkDirty();
        }

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
                scene.MarkDirty();
            }

            ImGui::TextUnformatted("Rotation aims local +Y toward the light source.");
            ImGui::TextUnformatted("Position sets the gizmo anchor in the scene.");
        }
        else
        {
            light.castsShadow = false;
            if (ImGui::SliderFloat("Range", &light.range, 0.0f, 25.0f))
            {
                scene.MarkDirty();
            }

            if (light.type == LightType::Spot)
            {
                if (ImGui::SliderFloat("Inner angle", &light.innerCutoffDegrees, 0.0f, light.outerCutoffDegrees - 1.0f))
                {
                    scene.MarkDirty();
                }

                if (ImGui::SliderFloat("Outer angle", &light.outerCutoffDegrees, 1.0f, 89.0f))
                {
                    scene.MarkDirty();
                }
                if (light.innerCutoffDegrees >= light.outerCutoffDegrees)
                {
                    light.innerCutoffDegrees = light.outerCutoffDegrees - 1.0f;
                }
            }

            ImGui::TextUnformatted("Position sets the light source. Rotation aims local +Y toward the source.");
        }
    }

    void DrawMultiTransformSection(Scene& scene, const std::vector<int>& movableIndices)
    {
        std::vector<glm::vec3> worldPositions;
        std::vector<glm::vec3> worldRotations;
        std::vector<glm::vec3> worldScales;
        worldPositions.reserve(movableIndices.size());
        worldRotations.reserve(movableIndices.size());
        worldScales.reserve(movableIndices.size());

        for (int objectIndex : movableIndices)
        {
            const WorldTransformState worldState = GetObjectWorldTransformState(scene, objectIndex);
            worldPositions.push_back(worldState.position);
            worldRotations.push_back(worldState.rotationDegrees);
            worldScales.push_back(worldState.scale);
        }

        MultiVec3 positionField = MultiVec3::Collect(worldPositions.data(), worldPositions.size());
        MultiVec3 rotationField = MultiVec3::Collect(worldRotations.data(), worldRotations.size());
        MultiVec3 scaleField = MultiVec3::Collect(worldScales.data(), worldScales.size());

        ImGui::TextDisabled("World space. Edited values apply to all selected movables.");
        ImGui::PushID("MultiTransformTable");
        if (ImGui::BeginTable(
                "##fields",
                4,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoBordersInBody))
        {
            ImGui::TableSetupColumn("##row", ImGuiTableColumnFlags_WidthFixed, kTransformRowLabelWidth);
            ImGui::TableSetupColumn("##x", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##y", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("##z", ImGuiTableColumnFlags_WidthStretch, 1.0f);

            if (DrawMultiVec3Row("Position", positionField, glm::vec3(0.0f), 0.1f))
            {
                ApplyWorldPositionFieldToObjects(scene, movableIndices, positionField);
            }

            if (DrawMultiVec3Row("Rotation", rotationField, glm::vec3(0.0f), 0.5f))
            {
                ApplyWorldRotationFieldToObjects(scene, movableIndices, rotationField);
            }

            if (DrawMultiVec3Row("Scale", scaleField, glm::vec3(1.0f), 0.01f))
            {
                ApplyWorldScaleFieldToObjects(scene, movableIndices, scaleField);
            }

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    void DrawMultiObjectSection(Scene& scene, const std::vector<int>& selectedIndices)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();

        bool allRenderable = true;
        bool allEmpty = true;
        std::vector<bool> movableValues;
        std::vector<bool> castShadowValues;
        std::vector<bool> receiveShadowValues;
        movableValues.reserve(selectedIndices.size());
        castShadowValues.reserve(selectedIndices.size());
        receiveShadowValues.reserve(selectedIndices.size());

        for (int objectIndex : selectedIndices)
        {
            const SceneObject& object = objects[static_cast<std::size_t>(objectIndex)];
            if (object.IsRenderable())
            {
                allEmpty = false;
                castShadowValues.push_back(object.CastsShadow());
                receiveShadowValues.push_back(object.ReceivesShadow());
            }
            else if (!object.HasLight())
            {
                allRenderable = false;
            }
            else
            {
                allRenderable = false;
                allEmpty = false;
            }

            movableValues.push_back(object.IsMovable());
        }

        MultiBool movableField = MultiBool::Collect(movableValues);
        if (DrawMultiCheckbox("Movable", movableField))
        {
            for (int objectIndex : selectedIndices)
            {
                scene.GetObject(static_cast<std::size_t>(objectIndex)).SetMovable(movableField.value);
            }

            scene.MarkDirty();
        }

        if (allEmpty && !allRenderable)
        {
            ImGui::TextUnformatted("Selection includes empty objects and/or lights.");
        }
        else if (allEmpty)
        {
            ImGui::TextUnformatted("Empty objects (transform containers only).");
        }
        else if (!allRenderable)
        {
            ImGui::TextDisabled("Shadow flags apply only when every selected object is renderable.");
        }

        if (allRenderable)
        {
            MultiBool castShadowField = MultiBool::Collect(castShadowValues);
            if (DrawMultiCheckbox("Cast shadow", castShadowField))
            {
                for (int objectIndex : selectedIndices)
                {
                    scene.GetObject(static_cast<std::size_t>(objectIndex)).SetCastShadow(castShadowField.value);
                }

                scene.MarkDirty();
            }

            MultiBool receiveShadowField = MultiBool::Collect(receiveShadowValues);
            if (DrawMultiCheckbox("Receive shadow", receiveShadowField))
            {
                for (int objectIndex : selectedIndices)
                {
                    scene.GetObject(static_cast<std::size_t>(objectIndex)).SetReceiveShadow(
                        receiveShadowField.value);
                }

                scene.MarkDirty();
            }
        }
    }

    void DrawSingleObjectInspector(Scene& scene, int selectedIndex)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        SceneObject& selectedObject = scene.GetObject(static_cast<std::size_t>(selectedIndex));

        ImGui::Text("Inspector: %s", selectedObject.GetName().c_str());

        char nameBuffer[64];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", selectedObject.GetName().c_str());
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            selectedObject.SetName(nameBuffer);
            scene.MarkDirty();
        }

        const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Reset Transform"))
            {
                selectedObject.GetTransform().Reset();
                scene.MarkDirty();
            }

            ImGui::EndPopup();
        }

        if (transformOpen && selectedObject.IsMovable())
        {
            DrawTransformSection(selectedObject, scene);
        }
        else if (transformOpen && !selectedObject.IsMovable())
        {
            ImGui::TextDisabled("Transform is read-only for non-movable objects.");
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
                    scene.MarkDirty();
                }

                bool castShadow = selectedObject.CastsShadow();
                if (ImGui::Checkbox("Cast shadow", &castShadow))
                {
                    selectedObject.SetCastShadow(castShadow);
                    scene.MarkDirty();
                }

                bool receiveShadow = selectedObject.ReceivesShadow();
                if (ImGui::Checkbox("Receive shadow", &receiveShadow))
                {
                    selectedObject.SetReceiveShadow(receiveShadow);
                    scene.MarkDirty();
                }
            }
            else
            {
                ImGui::TextUnformatted("Empty object (transform container only).");
                bool movable = selectedObject.IsMovable();
                if (ImGui::Checkbox("Movable", &movable))
                {
                    selectedObject.SetMovable(movable);
                    scene.MarkDirty();
                }
            }
        }

        if (selectedObject.HasMaterial() && ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::PushID(selectedIndex);
            DrawMaterialSection(selectedObject.GetMaterial(), scene);
            ImGui::PopID();
        }
    }

    void DrawMultiObjectInspector(Scene& scene, const std::vector<int>& selectedIndices)
    {
        const std::size_t selectionCount = selectedIndices.size();
        ImGui::Text("%zu objects selected", selectionCount);

        const int primaryIndex = scene.GetPrimarySelection();
        if (primaryIndex >= 0 && static_cast<std::size_t>(primaryIndex) < scene.GetObjects().size())
        {
            ImGui::TextDisabled("Primary: %s", scene.GetObject(static_cast<std::size_t>(primaryIndex)).GetName().c_str());
        }

        const std::vector<int> movableIndices = scene.GetMovableSelectedIndices();
        if (!movableIndices.empty()
            && movableIndices.size() < selectedIndices.size())
        {
            ImGui::TextDisabled(
                "Transform edits apply to %zu movable object(s).",
                movableIndices.size());
        }

        if (ShouldShowInspectorSection(InspectorSectionKind::Transform, scene, selectedIndices))
        {
            const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Reset Transform"))
                {
                    ResetTransformsOnObjects(scene, movableIndices);
                }

                ImGui::EndPopup();
            }

            if (transformOpen)
            {
                DrawMultiTransformSection(scene, movableIndices);
            }
        }

        if (ShouldShowInspectorSection(InspectorSectionKind::Object, scene, selectedIndices)
            && ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawMultiObjectSection(scene, selectedIndices);
        }
    }
}

void SceneInspectorPanel::Draw(Scene& scene) const
{
    EditorPanelLayout::ApplyFirstUseLayout(EditorPanelLayout::Panel::Inspector);

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

    const std::vector<int>& selectedIndices = scene.GetSelection().indices;
    if (selectedIndices.empty())
    {
        ImGui::TextUnformatted("No object selected.");
        ImGui::End();
        return;
    }

    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (int selectedIndex : selectedIndices)
    {
        if (selectedIndex < 0 || selectedIndex >= static_cast<int>(objects.size()))
        {
            ImGui::TextUnformatted("No object selected.");
            ImGui::End();
            return;
        }
    }

    if (selectedIndices.size() == 1)
    {
        DrawSingleObjectInspector(scene, selectedIndices.front());
    }
    else
    {
        DrawMultiObjectInspector(scene, selectedIndices);
    }

    ImGui::End();
}
