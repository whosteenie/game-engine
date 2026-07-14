#include "app/editor/EditorReorderDragDrop.h"
#include "app/panels/SceneInspectorPanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorWidgets.h"
#include "app/inspector/InspectorEditMode.h"
#include "app/inspector/InspectorMultiEdit.h"
#include "app/inspector/InspectorTransform.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/scene/SceneComponentCatalog.h"
#include "app/undo/UndoCommand.h"
#include "app/editor/EditorMouseWrapping.h"
#include "engine/components/ColliderComponent.h"
#include "engine/assets/FileDialog.h"
#include "engine/components/CameraComponent.h"
#include "engine/scene/InspectorComponentOrder.h"
#include "engine/lighting/Light.h"
#include "engine/components/LightComponent.h"
#include "engine/rendering/ColorSpace.h"
#include "engine/rendering/Material.h"
#include "engine/components/RigidBodyComponent.h"
#include "engine/scene/SceneObject.h"
#include "engine/rendering/Texture.h"
#include "engine/assets/TextureCache.h"
#include "engine/scene/Transform.h"
#include "engine/scene/RotationUtils.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"

#include <imgui.h>
#include <imgui_internal.h>

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
    constexpr float kMinColliderDimension = 0.05f;

    constexpr float kTransformRowLabelWidth = 68.0f;

    const char* const kAxisLabels[] = {"X", "Y", "Z"};
    const ImVec4 kAxisColors[] = {
        ImVec4(0.86f, 0.33f, 0.33f, 1.0f),
        ImVec4(0.52f, 0.78f, 0.40f, 1.0f),
        ImVec4(0.42f, 0.58f, 0.92f, 1.0f),
    };

    bool DrawTransformRowLabel(
        const char* label,
        glm::vec3& value,
        const glm::vec3& resetValue,
        TransformEditContext* editContext)
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
                if (editContext != nullptr
                    && editContext->undoStack != nullptr
                    && editContext->scene != nullptr)
                {
                    PushTransformMutation(
                        *editContext->undoStack,
                        *editContext->scene,
                        editContext->objectIndices,
                        editContext->commandName,
                        [&](Scene& scene) {
                            (void)scene;
                            value = resetValue;
                        });
                }
                else
                {
                    value = resetValue;
                }

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
        const char* format,
        TransformEditContext* editContext)
    {
        ImGui::PushID(axis);
        ImGui::AlignTextToFramePadding();

        ImGui::PushStyleColor(ImGuiCol_Text, kAxisColors[axis]);
        ImGui::TextUnformatted(kAxisLabels[axis]);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        value[axis] = EditorWidgets::SanitizeSignedZero(value[axis]);
        const bool dragged = EditorWidgets::DragFloat("##value", &value[axis], dragSpeed, 0.0f, 0.0f, format);
        if (dragged)
        {
            value[axis] = EditorWidgets::SanitizeSignedZero(value[axis]);
        }
        EditorMouseWrapping::MarkCurrentItemForMouseWrap();
        if (editContext != nullptr)
        {
            HandleTransformFieldEditEvents(*editContext);
        }

        bool changed = dragged;
        if (ImGui::BeginPopupContextItem())
        {
            char menuLabel[64];
            std::snprintf(menuLabel, sizeof(menuLabel), "Reset %s %s", kAxisLabels[axis], label);
            if (ImGui::MenuItem(menuLabel))
            {
                if (editContext != nullptr
                    && editContext->undoStack != nullptr
                    && editContext->scene != nullptr)
                {
                    PushTransformMutation(
                        *editContext->undoStack,
                        *editContext->scene,
                        editContext->objectIndices,
                        editContext->commandName,
                        [&](Scene& scene) {
                            (void)scene;
                            value[axis] = resetValue[axis];
                        });
                }
                else
                {
                    value[axis] = resetValue[axis];
                }

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
        TransformEditContext* editContext,
        const char* format = "%.2f")
    {
        ImGui::PushID(label);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        bool changed = DrawTransformRowLabel(label, value, resetValue, editContext);

        for (int axis = 0; axis < 3; ++axis)
        {
            ImGui::TableSetColumnIndex(axis + 1);
            changed |= DrawTransformAxisField(
                axis,
                label,
                value,
                resetValue,
                dragSpeed,
                format,
                editContext);
        }

        ImGui::PopID();
        return changed;
    }

    void ApplyMultiTransformChange(
        Scene& scene,
        const std::vector<int>& selectedIndices,
        TransformEditContext& editContext,
        const std::function<void(Scene&)>& apply)
    {
        if (editContext.sessionOpen)
        {
            apply(scene);
            return;
        }

        if (editContext.undoStack != nullptr)
        {
            PushTransformMutation(
                *editContext.undoStack,
                scene,
                selectedIndices,
                editContext.commandName,
                apply);
            return;
        }

        apply(scene);
    }

    void DrawMaterialTextureSlot(
        const char* label,
        bool hasMap,
        const std::string& path,
        TextureColorSpace colorSpace,
        Scene& scene,
        UndoStack* undoStack,
        const std::vector<int>& objectIndices,
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
                    if (undoStack != nullptr)
                    {
                        PushMaterialMutation(
                            *undoStack,
                            scene,
                            objectIndices,
                            "Material",
                            [&](Scene& target) {
                                assign(texture, selectedPath);
                                target.MarkDirty();
                            });
                    }
                    else
                    {
                        assign(texture, selectedPath);
                        scene.MarkDirty();
                    }
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
            if (undoStack != nullptr)
            {
                PushMaterialMutation(
                    *undoStack,
                    scene,
                    objectIndices,
                    "Material",
                    [&](Scene& target) {
                        clear();
                        target.MarkDirty();
                    });
            }
            else
            {
                clear();
                scene.MarkDirty();
            }
        }
        ImGui::EndDisabled();

        ImGui::PopID();
    }

    void DrawMaterialSection(
        Material& material,
        Scene& scene,
        MaterialEditContext& editContext)
    {
        glm::vec3 albedoDisplay = ColorSpace::LinearToSrgb(material.GetAlbedo());
        if (EditorWidgets::ColorEditVec3("Albedo", albedoDisplay))
        {
            material.SetAlbedo(ColorSpace::SrgbToLinear(albedoDisplay));
            scene.MarkDirty();
        }
        HandleMaterialFieldEditEvents(editContext);

        float roughness = material.GetRoughness();
        if (ImGui::SliderFloat("Roughness", &roughness, 0.0f, 1.0f))
        {
            material.SetRoughness(roughness);
            scene.MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "0 = perfect mirror (stored in the G-buffer for RT/SSR). "
                "Direct sun specular still uses a small BRDF floor to avoid GGX singularities.");
        }
        HandleMaterialFieldEditEvents(editContext);

        float metallic = material.GetMetallic();
        if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f))
        {
            material.SetMetallic(metallic);
            scene.MarkDirty();
        }
        HandleMaterialFieldEditEvents(editContext);

        float transmission = material.GetTransmission();
        if (ImGui::SliderFloat("Transmission", &transmission, 0.0f, 1.0f))
        {
            material.SetTransmission(transmission);
            scene.MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "0 = opaque. 1 = clear glass.\n"
                "Window pane: use a Plane mesh + Thin walled + Double sided.\n"
                "Avoid scaled cubes — edge faces cause dark/rainbow rims.\n"
                "Bottle/cup/lens: leave Thin walled off (solid volume).");
        }
        HandleMaterialFieldEditEvents(editContext);

        bool thinWalled = material.IsThinWalled();
        if (ImGui::Checkbox("Thin walled (pane)", &thinWalled))
        {
            material.SetThinWalled(thinWalled);
            scene.MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "glTF thicknessFactor = 0: zero-thickness window glass.\n"
                "Enter + exit refraction in one bounce — use for flat panes only.\n"
                "Not for cups, bottles, or solid lenses (leave off).\n"
                "Pair with Double sided on single-plane meshes.");
        }
        HandleMaterialFieldEditEvents(editContext);

        float ior = material.GetIndexOfRefraction();
        if (ImGui::SliderFloat("IOR", &ior, 1.0f, 3.0f))
        {
            material.SetIndexOfRefraction(ior);
            scene.MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Index of refraction. Air = 1.0 (no bending). Window glass ~1.5. "
                "Diamond ~2.4. Edits reset path-tracer noise so the image reconverges.");
        }
        HandleMaterialFieldEditEvents(editContext);

        glm::vec3 emissive = material.GetEmissive();
        EditorWidgets::SanitizeSignedZero(emissive);
        if (EditorWidgets::DragFloat3("Emission", &emissive.x, 0.05f, 0.0f, 0.0f, "%.3f"))
        {
            EditorWidgets::SanitizeSignedZero(emissive);
            material.SetEmissive(emissive);
            scene.MarkDirty();
        }
        EditorMouseWrapping::MarkCurrentItemForMouseWrap();
        HandleMaterialFieldEditEvents(editContext);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Linear HDR emissive RGB. Values above 1.0 can bloom.");
        }

        bool doubleSided = material.IsDoubleSided();
        if (ImGui::Checkbox("Double sided", &doubleSided))
        {
            if (editContext.undoStack != nullptr)
            {
                PushMaterialMutation(
                    *editContext.undoStack,
                    scene,
                    editContext.objectIndices,
                    "Material",
                    [&](Scene& target) {
                        material.SetDoubleSided(doubleSided);
                        target.MarkDirty();
                    });
            }
            else
            {
                material.SetDoubleSided(doubleSided);
                scene.MarkDirty();
            }
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Maps");

        DrawMaterialTextureSlot(
            "Albedo",
            material.HasAlbedoMap(),
            material.GetAlbedoMapPath(),
            TextureColorSpace::SRGB,
            scene,
            editContext.undoStack,
            editContext.objectIndices,
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
            HandleMaterialFieldEditEvents(editContext);
        }

        DrawMaterialTextureSlot(
            "Normal",
            material.HasNormalMap(),
            material.GetNormalMapPath(),
            TextureColorSpace::Linear,
            scene,
            editContext.undoStack,
            editContext.objectIndices,
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
            HandleMaterialFieldEditEvents(editContext);
        }

        DrawMaterialTextureSlot(
            "Ambient occlusion",
            material.HasAoMap(),
            material.GetAoMapPath(),
            TextureColorSpace::Linear,
            scene,
            editContext.undoStack,
            editContext.objectIndices,
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
            HandleMaterialFieldEditEvents(editContext);
        }

        DrawMaterialTextureSlot(
            "Emissive",
            material.HasEmissiveMap(),
            material.GetEmissiveMapPath(),
            TextureColorSpace::SRGB,
            scene,
            editContext.undoStack,
            editContext.objectIndices,
            [&material](std::shared_ptr<Texture> texture, const std::string& path) {
                material.SetEmissiveMap(std::move(texture), path);
            },
            [&material]() { material.ClearEmissiveMap(); });

        if (material.HasEmissiveMap())
        {
            int emissiveTexCoordSet = material.GetEmissiveTexCoordSet();
            if (ImGui::Combo("Emissive UV set", &emissiveTexCoordSet, "0\0" "1\0"))
            {
                material.SetEmissiveTexCoordSet(emissiveTexCoordSet);
                scene.MarkDirty();
            }
            HandleMaterialFieldEditEvents(editContext);
            ImGui::TextDisabled("Emission color multiplies the emissive map.");
        }

        if (material.HasMetallicRoughnessMap())
        {
            DrawMaterialTextureSlot(
                "Metallic-roughness",
                true,
                material.GetRoughnessMapPath(),
                TextureColorSpace::Linear,
                scene,
                editContext.undoStack,
                editContext.objectIndices,
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
            HandleMaterialFieldEditEvents(editContext);

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
                editContext.undoStack,
                editContext.objectIndices,
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
                HandleMaterialFieldEditEvents(editContext);
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
                        if (editContext.undoStack != nullptr)
                        {
                            PushMaterialMutation(
                                *editContext.undoStack,
                                scene,
                                editContext.objectIndices,
                                "Material",
                                [&](Scene& target) {
                                    material.SetMetallicRoughnessMap(
                                        std::move(texture),
                                        material.GetRoughnessTexCoordSet(),
                                        selectedPath);
                                    target.MarkDirty();
                                });
                        }
                        else
                        {
                            material.SetMetallicRoughnessMap(
                                std::move(texture),
                                material.GetRoughnessTexCoordSet(),
                                selectedPath);
                            scene.MarkDirty();
                        }
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

    void DrawTransformSection(SceneObject& object, Scene& scene, TransformEditContext& editContext)
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

            if (DrawTransformRow(
                    "Position",
                    transform.position,
                    glm::vec3(0.0f),
                    0.1f,
                    &editContext))
            {
                scene.MarkDirty();
            }

            glm::vec3 rotationDegrees = transform.GetRotationDegrees();
            const bool rotationLockedByHdrAlignment = object.HasLight()
                && object.GetLight().type == LightType::Directional
                && object.GetLight().autoAlignWithHdrSkybox;
            if (rotationLockedByHdrAlignment)
            {
                ImGui::BeginDisabled();
            }
            if (DrawTransformRow(
                    "Rotation",
                    rotationDegrees,
                    glm::vec3(0.0f),
                    0.5f,
                    &editContext))
            {
                transform.SetRotationDegrees(rotationDegrees);
                scene.MarkDirty();
            }
            if (rotationLockedByHdrAlignment)
            {
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("Rotation is controlled by HDR skybox auto-alignment.");
                }
            }

            if (DrawTransformRow(
                    "Scale",
                    transform.scale,
                    glm::vec3(1.0f),
                    0.01f,
                    &editContext))
            {
                scene.MarkDirty();
            }

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    void DrawLightSection(Scene& scene, int objectIndex, LightEditContext& editContext)
    {
        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
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
                if (editContext.undoStack != nullptr)
                {
                    PushLightMutation(
                        *editContext.undoStack,
                        scene,
                        editContext.objectIndices,
                        "Light",
                        CaptureObjectLights,
                        [&](Scene& target) {
                            SceneObject& targetObject =
                                target.GetSceneObject(static_cast<std::size_t>(objectIndex));
                            LightComponent& targetLight = targetObject.GetLight();
                            const bool preserveShadow = targetLight.castsShadow;
                            targetLight = MakeDefaultLightComponent(newType);
                            targetLight.castsShadow =
                                preserveShadow && newType == LightType::Directional;
                            target.MarkDirty();
                        });
                }
                else
                {
                    const bool preserveShadow = light.castsShadow;
                    light = MakeDefaultLightComponent(newType);
                    light.castsShadow = preserveShadow && newType == LightType::Directional;
                    scene.MarkDirty();
                }
            }
        }

        if (EditorWidgets::ColorEditVec3("Color", light.color))
        {
            scene.MarkDirty();
        }
        HandleLightFieldEditEvents(editContext);

        if (ImGui::SliderFloat("Intensity", &light.intensity, 0.0f, 8.0f))
        {
            scene.MarkDirty();
        }
        HandleLightFieldEditEvents(editContext);

        if (light.type == LightType::Directional)
        {
            bool autoAlignWithHdrSkybox = light.autoAlignWithHdrSkybox;
            if (ImGui::Checkbox("Auto-align with HDR skybox", &autoAlignWithHdrSkybox))
            {
                light.autoAlignWithHdrSkybox = autoAlignWithHdrSkybox;
                if (autoAlignWithHdrSkybox)
                {
                    const IBL& ibl = scene.GetRenderer().GetEnvironmentMap().GetIBL();
                    if (ibl.HasDetectedSunDirection())
                    {
                        Transform worldTransform = Transform::FromMatrix(scene.GetWorldMatrix(objectIndex));
                        worldTransform.rotation = RotationUtils::QuatFromLocalYAxis(
                            ibl.GetDetectedSunDirection());
                        scene.SetObjectWorldMatrix(objectIndex, worldTransform.ToMatrix());
                    }
                }
                scene.MarkDirty();
            }
            HandleLightFieldEditEvents(editContext);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Keeps this light's direction synchronized when the HDR skybox is rotated. "
                    "It does not override manual scene-light edits.");
            }

            bool castsShadow = light.castsShadow;
            if (ImGui::Checkbox("Cast shadows", &castsShadow))
            {
                if (editContext.undoStack != nullptr)
                {
                    PushLightMutation(
                        *editContext.undoStack,
                        scene,
                        editContext.objectIndices,
                        "Light",
                        [](const Scene& targetScene, const std::vector<int>&) {
                            return CaptureAllObjectLights(targetScene);
                        },
                        [&](Scene& target) {
                            SceneObject& targetObject =
                                target.GetSceneObject(static_cast<std::size_t>(objectIndex));
                            LightComponent& targetLight = targetObject.GetLight();
                            if (castsShadow)
                            {
                                for (SceneObject& otherObject : target.GetObjects())
                                {
                                    if (&otherObject != &targetObject && otherObject.HasLight())
                                    {
                                        otherObject.GetLight().castsShadow = false;
                                    }
                                }
                            }

                            targetLight.castsShadow = castsShadow;
                            target.MarkDirty();
                        });
                }
                else
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
            HandleLightFieldEditEvents(editContext);

            if (light.type == LightType::Spot)
            {
                if (ImGui::SliderFloat(
                        "Inner angle",
                        &light.innerCutoffDegrees,
                        0.0f,
                        light.outerCutoffDegrees - 1.0f))
                {
                    scene.MarkDirty();
                }
                HandleLightFieldEditEvents(editContext);

                if (ImGui::SliderFloat("Outer angle", &light.outerCutoffDegrees, 1.0f, 89.0f))
                {
                    scene.MarkDirty();
                }
                HandleLightFieldEditEvents(editContext);
                if (light.innerCutoffDegrees >= light.outerCutoffDegrees)
                {
                    light.innerCutoffDegrees = light.outerCutoffDegrees - 1.0f;
                }
            }

            ImGui::TextUnformatted("Position sets the light source. Rotation aims local +Y toward the source.");
        }
    }

    void DrawMultiTransformSection(
        Scene& scene,
        const std::vector<int>& selectedIndices,
        TransformEditContext& editContext)
    {
        std::vector<glm::vec3> worldPositions;
        std::vector<glm::vec3> worldRotations;
        std::vector<glm::vec3> worldScales;
        worldPositions.reserve(selectedIndices.size());
        worldRotations.reserve(selectedIndices.size());
        worldScales.reserve(selectedIndices.size());

        for (int objectIndex : selectedIndices)
        {
            const WorldTransformState worldState = GetObjectWorldTransformState(scene, objectIndex);
            worldPositions.push_back(worldState.position);
            worldRotations.push_back(worldState.rotationDegrees);
            worldScales.push_back(worldState.scale);
        }

        MultiVec3 positionField = MultiVec3::Collect(worldPositions.data(), worldPositions.size());
        MultiVec3 rotationField = MultiVec3::Collect(worldRotations.data(), worldRotations.size());
        MultiVec3 scaleField = MultiVec3::Collect(worldScales.data(), worldScales.size());

        ImGui::TextDisabled("World space. Edited values apply to all selected objects.");
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

            if (DrawMultiVec3Row("Position", positionField, glm::vec3(0.0f), 0.1f, "%.2f", &editContext))
            {
                ApplyMultiTransformChange(
                    scene,
                    selectedIndices,
                    editContext,
                    [&](Scene& target) {
                        ApplyWorldPositionFieldToObjects(target, selectedIndices, positionField);
                    });
            }

            if (DrawMultiVec3Row("Rotation", rotationField, glm::vec3(0.0f), 0.5f, "%.2f", &editContext))
            {
                ApplyMultiTransformChange(
                    scene,
                    selectedIndices,
                    editContext,
                    [&](Scene& target) {
                        ApplyWorldRotationFieldToObjects(target, selectedIndices, rotationField);
                    });
            }

            if (DrawMultiVec3Row("Scale", scaleField, glm::vec3(1.0f), 0.01f, "%.2f", &editContext))
            {
                ApplyMultiTransformChange(
                    scene,
                    selectedIndices,
                    editContext,
                    [&](Scene& target) {
                        ApplyWorldScaleFieldToObjects(target, selectedIndices, scaleField);
                    });
            }

            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    void DrawCameraSection(Scene& scene, int objectIndex, CameraEditContext& editContext)
    {
        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        CameraComponent& camera = object.GetCamera();

        if (ImGui::SliderFloat("FOV", &camera.fovDegrees, 15.0f, 120.0f))
        {
            scene.MarkDirty();
        }
        HandleCameraFieldEditEvents(editContext);

        if (EditorWidgets::DragFloat("Near", &camera.nearPlane, 0.01f, 0.01f, camera.farPlane - 0.01f))
        {
            scene.MarkDirty();
        }
        HandleCameraFieldEditEvents(editContext);

        if (EditorWidgets::DragFloat("Far", &camera.farPlane, 1.0f, camera.nearPlane + 0.01f, 10000.0f))
        {
            scene.MarkDirty();
        }
        HandleCameraFieldEditEvents(editContext);

        if (ImGui::Checkbox("Enabled", &camera.enabled))
        {
            scene.MarkDirty();
        }
        HandleCameraFieldEditEvents(editContext);

        if (EditorWidgets::DragInt("Depth", &camera.depth, 1.0f, -100, 100))
        {
            scene.MarkDirty();
        }
        HandleCameraFieldEditEvents(editContext);

        bool isMain = camera.isMain;
        if (ImGui::Checkbox("Main Camera", &isMain))
        {
            if (isMain != camera.isMain)
            {
                if (editContext.undoStack != nullptr)
                {
                    PushCameraMutation(
                        *editContext.undoStack,
                        scene,
                        editContext.objectIndices,
                        "Camera",
                        [](const Scene& targetScene, const std::vector<int>&) {
                            return CaptureAllObjectCameras(targetScene);
                        },
                        [&](Scene& target) {
                            SceneObject& targetObject =
                                target.GetSceneObject(static_cast<std::size_t>(objectIndex));
                            CameraComponent& targetCamera = targetObject.GetCamera();
                            targetCamera.isMain = isMain;
                            if (targetCamera.isMain)
                            {
                                target.EnsureUniqueMainCamera(objectIndex);
                            }

                            target.MarkDirty();
                        });
                }
                else
                {
                    camera.isMain = isMain;
                    if (camera.isMain)
                    {
                        scene.EnsureUniqueMainCamera(objectIndex);
                    }

                    scene.MarkDirty();
                }
            }
        }
    }

    void DrawRigidBodySection(Scene& scene, int objectIndex, RigidBodyEditContext& editContext)
    {
        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        RigidBodyComponent& rigidBody = object.GetRigidBody();

        if (EditorWidgets::DragFloat("Mass", &rigidBody.mass, 0.1f, 0.001f, 10000.0f))
        {
            rigidBody.mass = std::max(rigidBody.mass, 0.001f);
            scene.MarkDirty();
        }
        HandleRigidBodyFieldEditEvents(editContext);

        if (ImGui::Checkbox("Use Gravity", &rigidBody.useGravity))
        {
            scene.MarkDirty();
        }
        HandleRigidBodyFieldEditEvents(editContext);

        if (ImGui::Checkbox("Is Kinematic", &rigidBody.isKinematic))
        {
            scene.MarkDirty();
        }
        HandleRigidBodyFieldEditEvents(editContext);

        int collisionDetectionIndex = static_cast<int>(rigidBody.collisionDetection);
        if (ImGui::Combo(
                "Collision Detection",
                &collisionDetectionIndex,
                "Discrete\0Continuous\0",
                2))
        {
            rigidBody.collisionDetection =
                static_cast<RigidBodyCollisionDetection>(collisionDetectionIndex);
            scene.MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Continuous uses Jolt linear casting to reduce tunneling on fast-moving bodies.");
        }
        HandleRigidBodyFieldEditEvents(editContext);

        if (EditorWidgets::DragFloat("Linear Damping", &rigidBody.linearDamping, 0.01f, 0.0f, 10.0f))
        {
            rigidBody.linearDamping = std::max(rigidBody.linearDamping, 0.0f);
            scene.MarkDirty();
        }
        HandleRigidBodyFieldEditEvents(editContext);

        if (EditorWidgets::DragFloat("Angular Damping", &rigidBody.angularDamping, 0.01f, 0.0f, 10.0f))
        {
            rigidBody.angularDamping = std::max(rigidBody.angularDamping, 0.0f);
            scene.MarkDirty();
        }
        HandleRigidBodyFieldEditEvents(editContext);

        if (ImGui::Checkbox("Allow Sleeping", &rigidBody.allowSleeping))
        {
            scene.MarkDirty();
        }
        HandleRigidBodyFieldEditEvents(editContext);
    }

    void DrawColliderSection(Scene& scene, int objectIndex, ColliderEditContext& editContext)
    {
        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        ColliderComponent& collider = object.GetCollider();

        int shapeIndex = static_cast<int>(collider.shape);
        if (ImGui::Combo("Shape", &shapeIndex, "Box\0Sphere\0", 2))
        {
            const ColliderShape newShape = static_cast<ColliderShape>(shapeIndex);
            if (newShape != collider.shape)
            {
                if (editContext.undoStack != nullptr)
                {
                    PushColliderMutation(
                        *editContext.undoStack,
                        scene,
                        editContext.objectIndices,
                        "Collider",
                        [&](Scene& target) {
                            target.GetSceneObject(static_cast<std::size_t>(objectIndex))
                                .GetCollider()
                                .shape = newShape;
                            target.MarkDirty();
                        });
                }
                else
                {
                    collider.shape = newShape;
                    scene.MarkDirty();
                }
            }
        }

        if (collider.shape == ColliderShape::Box)
        {
            if (EditorWidgets::DragFloat3(
                    "Half Extents",
                    &collider.halfExtents.x,
                    0.01f,
                    kMinColliderDimension,
                    1000.0f))
            {
                collider.halfExtents = glm::max(collider.halfExtents, glm::vec3(kMinColliderDimension));
                scene.MarkDirty();
            }
            HandleColliderFieldEditEvents(editContext);
        }
        else
        {
            if (EditorWidgets::DragFloat("Radius", &collider.radius, 0.01f, kMinColliderDimension, 1000.0f))
            {
                collider.radius = std::max(collider.radius, kMinColliderDimension);
                scene.MarkDirty();
            }
            HandleColliderFieldEditEvents(editContext);
        }

        if (EditorWidgets::DragFloat3("Offset", &collider.offset.x, 0.01f))
        {
            EditorWidgets::SanitizeSignedZero(collider.offset);
            scene.MarkDirty();
        }
        EditorMouseWrapping::MarkCurrentItemForMouseWrap();
        HandleColliderFieldEditEvents(editContext);

        if (ImGui::Checkbox("Is Trigger", &collider.isTrigger))
        {
            scene.MarkDirty();
        }
        HandleColliderFieldEditEvents(editContext);

        if (EditorWidgets::DragFloat("Friction", &collider.friction, 0.01f, 0.0f, 1.0f))
        {
            collider.friction = std::max(collider.friction, 0.0f);
            scene.MarkDirty();
        }
        HandleColliderFieldEditEvents(editContext);

        if (EditorWidgets::DragFloat("Restitution", &collider.restitution, 0.01f, 0.0f, 1.0f))
        {
            collider.restitution = std::max(collider.restitution, 0.0f);
            scene.MarkDirty();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Bounciness. 0 = inelastic, 1 = fully elastic.");
        }
        HandleColliderFieldEditEvents(editContext);
    }

    void ReorderInspectorComponents(
        Scene& scene,
        const int objectIndex,
        UndoStack* undoStack,
        const int fromIndex,
        const int beforeIndex)
    {
        SceneObject& object = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        const int slotCount = static_cast<int>(object.GetEffectiveInspectorComponentOrder().size());
        if (!WouldInspectorComponentMoveChangeOrder(fromIndex, beforeIndex, slotCount))
        {
            return;
        }

        if (undoStack != nullptr)
        {
            PushInspectorComponentOrderMutation(
                *undoStack,
                scene,
                objectIndex,
                "Reorder components",
                [&](std::vector<InspectorComponentType>& order) {
                    MoveInspectorComponentBefore(order, fromIndex, beforeIndex);
                });
            return;
        }

        std::vector<InspectorComponentType> order = object.GetEffectiveInspectorComponentOrder();
        if (MoveInspectorComponentBefore(order, fromIndex, beforeIndex))
        {
            object.SetInspectorComponentOrder(std::move(order));
            scene.MarkDirty();
        }
    }

    struct InspectorComponentDragPayload
    {
        int slotIndex = -1;
    };

    bool IsInspectorComponentDragActive()
    {
        const ImGuiPayload* payload = ImGui::GetDragDropPayload();
        return payload != nullptr
            && payload->IsDataType(EditorReorderDragDrop::kInspectorComponentDragPayload);
    }

    void DrawInspectorInsertGapLine()
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

    bool TryAcceptInspectorInsertGap(
        const SceneInspectorPanel& panel,
        Scene& scene,
        int objectIndex,
        int beforeIndex,
        UndoStack* undoStack,
        const ImGuiPayload* activePayload,
        bool drawLine,
        bool bottomStickyEligible,
        bool useBottomInsertLineY = false)
    {
        if (activePayload == nullptr
            || !activePayload->IsDataType(EditorReorderDragDrop::kInspectorComponentDragPayload))
        {
            return false;
        }

        const auto* dragPayload = static_cast<const InspectorComponentDragPayload*>(activePayload->Data);
        const int fromIndex = dragPayload->slotIndex;
        const int slotCount = static_cast<int>(
            scene.GetSceneObject(static_cast<std::size_t>(objectIndex)).GetEffectiveInspectorComponentOrder().size());
        if (!WouldInspectorComponentMoveChangeOrder(fromIndex, beforeIndex, slotCount))
        {
            return false;
        }

        if (drawLine)
        {
            if (bottomStickyEligible)
            {
                const ImVec2 itemMin = ImGui::GetItemRectMin();
                const ImVec2 itemMax = ImGui::GetItemRectMax();
                panel.UpdateDragInsertLatch(
                    objectIndex,
                    beforeIndex,
                    itemMin.x,
                    itemMin.y,
                    itemMax.x,
                    itemMax.y,
                    useBottomInsertLineY);
                panel.DrawDragInsertLatchLine();
            }
            else
            {
                panel.ClearDragInsertLatch();
                DrawInspectorInsertGapLine();
            }
        }

        const ImGuiDragDropFlags acceptFlags =
            ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(
                    EditorReorderDragDrop::kInspectorComponentDragPayload,
                    acceptFlags))
        {
            if (payload->IsDelivery())
            {
                ReorderInspectorComponents(scene, objectIndex, undoStack, fromIndex, beforeIndex);
            }

            return true;
        }

        return false;
    }

    void DrawInspectorComponentInsertGap(
        const SceneInspectorPanel& panel,
        Scene& scene,
        int objectIndex,
        int beforeIndex,
        UndoStack* undoStack,
        bool bottomStickyEligible = false)
    {
        ImGui::PushID(beforeIndex);
        ImGui::PushID("InspectorInsertGap");
        ImGui::InvisibleButton(
            "##InspectorInsertGap",
            ImVec2(-FLT_MIN, EditorReorderDragDrop::kInsertGapHitHeight));

        if (ImGui::BeginDragDropTarget())
        {
            TryAcceptInspectorInsertGap(
                panel,
                scene,
                objectIndex,
                beforeIndex,
                undoStack,
                ImGui::GetDragDropPayload(),
                true,
                bottomStickyEligible);
            ImGui::EndDragDropTarget();
        }

        ImGui::PopID();
        ImGui::PopID();
    }

    void DrawInspectorComponentBottomDropOverlay(
        const SceneInspectorPanel& panel,
        Scene& scene,
        int objectIndex,
        int beforeIndex,
        UndoStack* undoStack,
        float height)
    {
        if (height <= 0.0f)
        {
            return;
        }

        const ImVec2 savedCursor = ImGui::GetCursorPos();
        const ImVec2 overlayScreenPos = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;

        ImGui::SetCursorScreenPos(overlayScreenPos);
        ImGui::InvisibleButton(
            "##InspectorComponentBottomDropOverlay",
            ImVec2(width > 0.0f ? width : -FLT_MIN, height));

        if (ImGui::BeginDragDropTarget())
        {
            const bool hasActiveLatch = panel.HasDragInsertLatchFor(objectIndex, beforeIndex);
            if (hasActiveLatch)
            {
                const ImVec2 zoneMin = ImGui::GetItemRectMin();
                const ImVec2 zoneMax = ImGui::GetItemRectMax();
                panel.UpdateDragInsertLatch(
                    objectIndex,
                    beforeIndex,
                    zoneMin.x,
                    zoneMin.y,
                    zoneMax.x,
                    zoneMax.y);
                panel.DrawDragInsertLatchLine();
            }

            TryAcceptInspectorInsertGap(
                panel,
                scene,
                objectIndex,
                beforeIndex,
                undoStack,
                ImGui::GetDragDropPayload(),
                !hasActiveLatch,
                true,
                true);

            ImGui::EndDragDropTarget();
        }

        ImGui::SetCursorPos(savedCursor);
    }

    bool BeginReorderableInspectorSection(
        const char* label,
        const InspectorComponentType type,
        const int slotIndex,
        const int slotCount,
        Scene& scene,
        const int objectIndex,
        UndoStack* undoStack)
    {
        ImGui::PushID(slotIndex);
        ImGui::PushID(static_cast<int>(type));

        const bool open = ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen);

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
        {
            InspectorComponentDragPayload dragPayload{slotIndex};
            ImGui::SetDragDropPayload(
                EditorReorderDragDrop::kInspectorComponentDragPayload,
                &dragPayload,
                sizeof(dragPayload));
            ImGui::TextUnformatted(label);
            ImGui::EndDragDropSource();
        }

        if (ImGui::BeginPopupContextItem())
        {
            if (slotIndex > 0 && ImGui::MenuItem("Move Up"))
            {
                ReorderInspectorComponents(scene, objectIndex, undoStack, slotIndex, slotIndex - 1);
            }

            if (slotIndex + 1 < slotCount && ImGui::MenuItem("Move Down"))
            {
                ReorderInspectorComponents(scene, objectIndex, undoStack, slotIndex, slotIndex + 2);
            }

            SceneSystemComponentType systemType = SceneSystemComponentType::Light;
            if (TryInspectorComponentTypeToSystemType(type, systemType)
                && ImGui::MenuItem("Remove Component"))
            {
                const std::string commandName =
                    std::string("Remove ") + GetSceneSystemComponentLabel(systemType);
                if (undoStack != nullptr)
                {
                    PushSystemComponentMutation(
                        *undoStack,
                        scene,
                        objectIndex,
                        commandName,
                        [&](Scene& target) {
                            RemoveSceneSystemComponent(target, objectIndex, systemType);
                        });
                }
                else
                {
                    RemoveSceneSystemComponent(scene, objectIndex, systemType);
                }
            }

            ImGui::EndPopup();
        }

        ImGui::PopID();
        ImGui::PopID();
        return open;
    }

    void DrawObjectFlagsSection(
        Scene& scene,
        const int objectIndex,
        TransformEditContext& editContext)
    {
        SceneObject& selectedObject = scene.GetSceneObject(static_cast<std::size_t>(objectIndex));
        if (!selectedObject.IsRenderable())
        {
            ImGui::TextUnformatted("Empty object (transform container only).");
            return;
        }

        bool castShadow = selectedObject.CastsShadow();
        if (ImGui::Checkbox("Cast shadow", &castShadow))
        {
            if (editContext.undoStack != nullptr)
            {
                PushShadowFlagsMutation(
                    *editContext.undoStack,
                    scene,
                    {objectIndex},
                    "Cast shadow",
                    [&](Scene& target) {
                        target.GetSceneObject(static_cast<std::size_t>(objectIndex)).SetCastShadow(castShadow);
                        target.MarkDirty();
                    });
            }
            else
            {
                selectedObject.SetCastShadow(castShadow);
                scene.MarkDirty();
            }
        }

        bool receiveShadow = selectedObject.ReceivesShadow();
        if (ImGui::Checkbox("Receive shadow", &receiveShadow))
        {
            if (editContext.undoStack != nullptr)
            {
                PushShadowFlagsMutation(
                    *editContext.undoStack,
                    scene,
                    {objectIndex},
                    "Receive shadow",
                    [&](Scene& target) {
                        target.GetSceneObject(static_cast<std::size_t>(objectIndex))
                            .SetReceiveShadow(receiveShadow);
                        target.MarkDirty();
                    });
            }
            else
            {
                selectedObject.SetReceiveShadow(receiveShadow);
                scene.MarkDirty();
            }
        }
    }

    void DrawOrderedInspectorComponents(
        const SceneInspectorPanel& panel,
        Scene& scene,
        const int selectedIndex,
        TransformEditContext& editContext,
        MaterialEditContext& materialEditContext,
        LightEditContext& lightEditContext,
        CameraEditContext& cameraEditContext,
        RigidBodyEditContext& rigidBodyEditContext,
        ColliderEditContext& colliderEditContext)
    {
        SceneObject& selectedObject = scene.GetSceneObject(static_cast<std::size_t>(selectedIndex));
        const std::vector<InspectorComponentType> componentOrder =
            selectedObject.GetEffectiveInspectorComponentOrder();
        const int slotCount = static_cast<int>(componentOrder.size());

        if (!IsInspectorComponentDragActive())
        {
            panel.ClearDragInsertLatch();
        }
        else
        {
            panel.ClearDragInsertLatchUnlessObject(selectedIndex);
        }

        for (int slotIndex = 0; slotIndex < slotCount; ++slotIndex)
        {
            const InspectorComponentType componentType = componentOrder[static_cast<std::size_t>(slotIndex)];
            if (!SceneObjectHasInspectorComponent(selectedObject, componentType))
            {
                continue;
            }

            DrawInspectorComponentInsertGap(
                panel,
                scene,
                selectedIndex,
                slotIndex,
                editContext.undoStack);

            const char* const label = GetInspectorComponentLabel(componentType);
            const bool sectionOpen = BeginReorderableInspectorSection(
                label,
                componentType,
                slotIndex,
                slotCount,
                scene,
                selectedIndex,
                editContext.undoStack);

            if (!sectionOpen)
            {
                continue;
            }

            switch (componentType)
            {
            case InspectorComponentType::Material:
                ImGui::PushID("MaterialSection");
                DrawMaterialSection(selectedObject.GetMaterial(), scene, materialEditContext);
                ImGui::PopID();
                break;
            case InspectorComponentType::ObjectFlags:
                ImGui::PushID("ObjectFlagsSection");
                DrawObjectFlagsSection(scene, selectedIndex, editContext);
                ImGui::PopID();
                break;
            case InspectorComponentType::Light:
                ImGui::PushID("LightSection");
                DrawLightSection(scene, selectedIndex, lightEditContext);
                ImGui::PopID();
                break;
            case InspectorComponentType::Camera:
                ImGui::PushID("CameraSection");
                DrawCameraSection(scene, selectedIndex, cameraEditContext);
                ImGui::PopID();
                break;
            case InspectorComponentType::RigidBody:
                ImGui::PushID("RigidBodySection");
                DrawRigidBodySection(scene, selectedIndex, rigidBodyEditContext);
                ImGui::PopID();
                break;
            case InspectorComponentType::Collider:
                ImGui::PushID("ColliderSection");
                DrawColliderSection(scene, selectedIndex, colliderEditContext);
                ImGui::PopID();
                break;
            }
        }

        DrawInspectorComponentInsertGap(
            panel,
            scene,
            selectedIndex,
            slotCount,
            editContext.undoStack,
            true);

        const ImVec2 backgroundSpace = ImGui::GetContentRegionAvail();
        if (backgroundSpace.y > 0.0f && IsInspectorComponentDragActive())
        {
            DrawInspectorComponentBottomDropOverlay(
                panel,
                scene,
                selectedIndex,
                slotCount,
                editContext.undoStack,
                backgroundSpace.y);
        }

        if (IsInspectorComponentDragActive())
        {
            const ImGuiWindow* inspectorWindow = ImGui::GetCurrentWindow();
            const ImVec2 mousePos = ImGui::GetIO().MousePos;
            const bool mouseInsideInspector =
                mousePos.x >= inspectorWindow->InnerRect.Min.x
                && mousePos.x <= inspectorWindow->InnerRect.Max.x
                && mousePos.y >= inspectorWindow->InnerRect.Min.y
                && mousePos.y <= inspectorWindow->InnerRect.Max.y;
            if (!mouseInsideInspector)
            {
                panel.ClearDragInsertLatch();
            }
        }
    }

    void DrawAddComponentFooter(Scene& scene, int objectIndex, UndoStack* undoStack)
    {
        std::vector<SceneSystemComponentType> addableComponents;
        GetAddableSceneSystemComponents(scene.GetSceneObject(static_cast<std::size_t>(objectIndex)), addableComponents);
        if (addableComponents.empty())
        {
            return;
        }

        ImGui::Separator();
        if (ImGui::Button("Add Component"))
        {
            ImGui::OpenPopup("AddComponentPopup");
        }

        if (ImGui::BeginPopup("AddComponentPopup"))
        {
            for (SceneSystemComponentType type : addableComponents)
            {
                if (ImGui::MenuItem(GetSceneSystemComponentLabel(type)))
                {
                    const std::string commandName =
                        std::string("Add ") + GetSceneSystemComponentLabel(type);
                    if (undoStack != nullptr)
                    {
                        PushSystemComponentMutation(
                            *undoStack,
                            scene,
                            objectIndex,
                            commandName,
                            [&](Scene& target) {
                                AddSceneSystemComponent(target, objectIndex, type);
                            });
                    }
                    else
                    {
                        AddSceneSystemComponent(scene, objectIndex, type);
                    }
                }
            }

            ImGui::EndPopup();
        }
    }

    void DrawMultiObjectSection(
        Scene& scene,
        const std::vector<int>& selectedIndices,
        UndoStack* undoStack)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();

        bool allRenderable = true;
        bool allEmpty = true;
        std::vector<bool> castShadowValues;
        std::vector<bool> receiveShadowValues;
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
            else if (!object.HasLight() && !object.HasCamera()
                && !object.HasRigidBody() && !object.HasCollider())
            {
                allRenderable = false;
            }
            else
            {
                allRenderable = false;
                allEmpty = false;
            }
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
                if (undoStack != nullptr)
                {
                    PushShadowFlagsMutation(
                        *undoStack,
                        scene,
                        selectedIndices,
                        "Cast shadow",
                        [&](Scene& target) {
                            for (int objectIndex : selectedIndices)
                            {
                                target.GetSceneObject(static_cast<std::size_t>(objectIndex))
                                    .SetCastShadow(castShadowField.value);
                            }
                            target.MarkDirty();
                        });
                }
                else
                {
                    for (int objectIndex : selectedIndices)
                    {
                        scene.GetSceneObject(static_cast<std::size_t>(objectIndex))
                            .SetCastShadow(castShadowField.value);
                    }

                    scene.MarkDirty();
                }
            }

            MultiBool receiveShadowField = MultiBool::Collect(receiveShadowValues);
            if (DrawMultiCheckbox("Receive shadow", receiveShadowField))
            {
                if (undoStack != nullptr)
                {
                    PushShadowFlagsMutation(
                        *undoStack,
                        scene,
                        selectedIndices,
                        "Receive shadow",
                        [&](Scene& target) {
                            for (int objectIndex : selectedIndices)
                            {
                                target.GetSceneObject(static_cast<std::size_t>(objectIndex))
                                    .SetReceiveShadow(receiveShadowField.value);
                            }
                            target.MarkDirty();
                        });
                }
                else
                {
                    for (int objectIndex : selectedIndices)
                    {
                        scene.GetSceneObject(static_cast<std::size_t>(objectIndex))
                            .SetReceiveShadow(receiveShadowField.value);
                    }

                    scene.MarkDirty();
                }
            }
        }
    }

    void DrawSingleObjectInspector(
        const SceneInspectorPanel& panel,
        Scene& scene,
        int selectedIndex,
        TransformEditContext& editContext,
        MaterialEditContext& materialEditContext,
        LightEditContext& lightEditContext,
        CameraEditContext& cameraEditContext,
        RigidBodyEditContext& rigidBodyEditContext,
        ColliderEditContext& colliderEditContext,
        SceneObjectId& nameEditObjectId,
        std::string& nameEditOldName)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        SceneObject& selectedObject = scene.GetSceneObject(static_cast<std::size_t>(selectedIndex));

        ImGui::Text("Inspector: %s", selectedObject.GetName().c_str());

        char nameBuffer[64];
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", selectedObject.GetName().c_str());
        const std::string nameBeforeEdit = selectedObject.GetName();
        if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer)))
        {
            selectedObject.SetName(nameBuffer);
            scene.MarkDirty();
        }

        if (ImGui::IsItemActivated())
        {
            nameEditObjectId = selectedObject.GetId();
            nameEditOldName = nameBeforeEdit;
        }

        if (ImGui::IsItemDeactivatedAfterEdit()
            && editContext.undoStack != nullptr
            && nameEditObjectId == selectedObject.GetId())
        {
            const std::string& newName = selectedObject.GetName();
            if (newName != nameEditOldName)
            {
                PushSetObjectName(
                    *editContext.undoStack,
                    scene,
                    nameEditObjectId,
                    nameEditOldName,
                    newName);
            }

            nameEditObjectId = kInvalidSceneObjectId;
            nameEditOldName.clear();
        }

        const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem())
        {
            if (ImGui::MenuItem("Reset Transform"))
            {
                if (editContext.undoStack != nullptr)
                {
                    PushTransformMutation(
                        *editContext.undoStack,
                        scene,
                        {selectedIndex},
                        "Reset Transform",
                        [&](Scene& target) {
                            ResetTransformsOnObjects(target, {selectedIndex});
                        });
                }
                else
                {
                    ResetTransformsOnObjects(scene, {selectedIndex});
                }
            }

            ImGui::EndPopup();
        }

        if (transformOpen)
        {
            DrawTransformSection(selectedObject, scene, editContext);
        }

        DrawOrderedInspectorComponents(
            panel,
            scene,
            selectedIndex,
            editContext,
            materialEditContext,
            lightEditContext,
            cameraEditContext,
            rigidBodyEditContext,
            colliderEditContext);

        DrawAddComponentFooter(scene, selectedIndex, editContext.undoStack);
    }

    void DrawMultiObjectInspector(
        Scene& scene,
        const std::vector<int>& selectedIndices,
        TransformEditContext& editContext,
        UndoStack* undoStack)
    {
        const std::size_t selectionCount = selectedIndices.size();
        ImGui::Text("%zu objects selected", selectionCount);

        const int primaryIndex = scene.GetPrimarySelection();
        if (primaryIndex >= 0 && static_cast<std::size_t>(primaryIndex) < scene.GetObjects().size())
        {
            ImGui::TextDisabled("Primary: %s", scene.GetSceneObject(static_cast<std::size_t>(primaryIndex)).GetName().c_str());
        }

        if (ShouldShowInspectorSection(InspectorSectionKind::Transform, scene, selectedIndices))
        {
            const bool transformOpen = ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Reset Transform"))
                {
                    if (editContext.undoStack != nullptr)
                    {
                        PushTransformMutation(
                            *editContext.undoStack,
                            scene,
                            selectedIndices,
                            "Reset Transform",
                            [&](Scene& target) {
                                ResetTransformsOnObjects(target, selectedIndices);
                            });
                    }
                    else
                    {
                        ResetTransformsOnObjects(scene, selectedIndices);
                    }
                }

                ImGui::EndPopup();
            }

            if (transformOpen)
            {
                DrawMultiTransformSection(scene, selectedIndices, editContext);
            }
        }

        if (ShouldShowInspectorSection(InspectorSectionKind::Object, scene, selectedIndices)
            && ImGui::CollapsingHeader("Object", ImGuiTreeNodeFlags_DefaultOpen))
        {
            DrawMultiObjectSection(scene, selectedIndices, undoStack);
        }
    }
}

void SceneInspectorPanel::ClearDragInsertLatch() const
{
    m_dragInsertLatchObjectIndex = -1;
    m_dragInsertLatchBeforeIndex = -1;
}

void SceneInspectorPanel::ClearDragInsertLatchUnlessObject(int objectIndex) const
{
    if (m_dragInsertLatchObjectIndex >= 0 && m_dragInsertLatchObjectIndex != objectIndex)
    {
        ClearDragInsertLatch();
    }
}

void SceneInspectorPanel::UpdateDragInsertLatch(
    int objectIndex,
    int beforeIndex,
    float itemMinX,
    float itemMinY,
    float itemMaxX,
    float itemMaxY,
    bool useBottomInsertLineY) const
{
    const bool isNewLatch =
        m_dragInsertLatchObjectIndex != objectIndex || m_dragInsertLatchBeforeIndex != beforeIndex;
    m_dragInsertLatchObjectIndex = objectIndex;
    m_dragInsertLatchBeforeIndex = beforeIndex;
    m_dragInsertLatchLineMinX = itemMinX;
    m_dragInsertLatchLineMaxX = itemMaxX;
    if (isNewLatch)
    {
        m_dragInsertLatchLineY = useBottomInsertLineY
            ? itemMinY - EditorReorderDragDrop::kInsertGapHitHeight * 0.5f
            : (itemMinY + itemMaxY) * 0.5f;
    }
}

bool SceneInspectorPanel::HasDragInsertLatchFor(int objectIndex, int beforeIndex) const
{
    return m_dragInsertLatchObjectIndex == objectIndex && m_dragInsertLatchBeforeIndex == beforeIndex;
}

void SceneInspectorPanel::DrawDragInsertLatchLine() const
{
    if (m_dragInsertLatchObjectIndex < 0)
    {
        return;
    }

    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(m_dragInsertLatchLineMinX, m_dragInsertLatchLineY),
        ImVec2(m_dragInsertLatchLineMaxX, m_dragInsertLatchLineY),
        IM_COL32(90, 150, 255, 255),
        2.0f);
}

void SceneInspectorPanel::Draw(Scene& scene, UndoStack* undoStack) const
{
    EditorPanelConstraints::ApplySideColumnPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Inspector", m_showPanel))
    {
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

    if (selectedIndices != m_transformEditSelection)
    {
        m_transformEditContext.sessionOpen = false;
        m_transformEditContext.pendingBefore.clear();
        m_transformEditSelection = selectedIndices;
    }

    if (selectedIndices != m_materialEditSelection)
    {
        m_materialEditContext.sessionOpen = false;
        m_materialEditContext.pendingBefore.clear();
        m_materialEditSelection = selectedIndices;
    }

    if (selectedIndices != m_lightEditSelection)
    {
        m_lightEditContext.sessionOpen = false;
        m_lightEditContext.pendingBefore.clear();
        m_lightEditSelection = selectedIndices;
    }

    if (selectedIndices != m_cameraEditSelection)
    {
        m_cameraEditContext.sessionOpen = false;
        m_cameraEditContext.pendingBefore.clear();
        m_cameraEditSelection = selectedIndices;
    }

    if (selectedIndices != m_rigidBodyEditSelection)
    {
        m_rigidBodyEditContext.sessionOpen = false;
        m_rigidBodyEditContext.pendingBefore.clear();
        m_rigidBodyEditSelection = selectedIndices;
    }

    if (selectedIndices != m_colliderEditSelection)
    {
        m_colliderEditContext.sessionOpen = false;
        m_colliderEditContext.pendingBefore.clear();
        m_colliderEditSelection = selectedIndices;
    }

    if (selectedIndices.size() == 1
        && selectedIndices.front() >= 0
        && selectedIndices.front() < static_cast<int>(objects.size())
        && objects[static_cast<std::size_t>(selectedIndices.front())].GetId() != m_nameEditObjectId)
    {
        m_nameEditObjectId = kInvalidSceneObjectId;
        m_nameEditOldName.clear();
    }
    else if (selectedIndices.size() != 1)
    {
        m_nameEditObjectId = kInvalidSceneObjectId;
        m_nameEditOldName.clear();
    }

    TransformEditContext& editContext = m_transformEditContext;
    editContext.undoStack = undoStack;
    editContext.scene = &scene;
    editContext.objectIndices = selectedIndices;
    editContext.commandName = "Transform";

    m_materialEditContext.undoStack = undoStack;
    m_materialEditContext.scene = &scene;
    m_materialEditContext.objectIndices = selectedIndices;
    m_materialEditContext.commandName = "Material";

    m_lightEditContext.undoStack = undoStack;
    m_lightEditContext.scene = &scene;
    m_lightEditContext.objectIndices = selectedIndices;
    m_lightEditContext.commandName = "Light";

    m_cameraEditContext.undoStack = undoStack;
    m_cameraEditContext.scene = &scene;
    m_cameraEditContext.objectIndices = selectedIndices;
    m_cameraEditContext.commandName = "Camera";

    m_rigidBodyEditContext.undoStack = undoStack;
    m_rigidBodyEditContext.scene = &scene;
    m_rigidBodyEditContext.objectIndices = selectedIndices;
    m_rigidBodyEditContext.commandName = "Rigid Body";

    m_colliderEditContext.undoStack = undoStack;
    m_colliderEditContext.scene = &scene;
    m_colliderEditContext.objectIndices = selectedIndices;
    m_colliderEditContext.commandName = "Collider";

    // Snapshot each context's pre-edit state BEFORE any inspector widget runs this frame, so a
    // slider drag that begins with a track click (value jumps + IsItemActivated on the same frame)
    // captures the true prior value for undo instead of the post-jump value.
    BeginMaterialEditFrame(m_materialEditContext);
    BeginLightFieldEditFrame(m_lightEditContext);
    BeginCameraFieldEditFrame(m_cameraEditContext);
    BeginRigidBodyFieldEditFrame(m_rigidBodyEditContext);
    BeginColliderFieldEditFrame(m_colliderEditContext);

    if (selectedIndices.size() == 1)
    {
        DrawSingleObjectInspector(
            *this,
            scene,
            selectedIndices.front(),
            editContext,
            m_materialEditContext,
            m_lightEditContext,
            m_cameraEditContext,
            m_rigidBodyEditContext,
            m_colliderEditContext,
            m_nameEditObjectId,
            m_nameEditOldName);
    }
    else
    {
        DrawMultiObjectInspector(scene, selectedIndices, editContext, undoStack);
    }

    ImGui::End();
}
