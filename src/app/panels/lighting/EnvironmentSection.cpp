#include "app/panels/lighting/LightingPanelSections.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/RendererSettingUi.h"
#include "app/editor/TuningSectionState.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"
#include "app/scene/document/Scene.h"
#include "engine/assets/FileDialog.h"
#include "engine/components/LightComponent.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/EnvironmentPresets.h"
#include "engine/lighting/IBL.h"
#include "engine/scene/Transform.h"

#include <imgui.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>


void DrawEnvironmentSection(const LightingPanelContext& ctx)
{
    Scene& scene = ctx.scene;
    RendererEditContext& editContext = ctx.editContext;
    EnvironmentMap& environmentMap = ctx.environmentMap;
    IBL& ibl = ctx.ibl;

    if (TuningSectionState::SectionHeader("Environment", true))
    {
        ImGui::SeparatorText("Sky & background");
        int backgroundMode = static_cast<int>(environmentMap.GetBackgroundMode());
        const char* backgroundModeLabels[] = {"Skybox (HDR)", "Solid color"};
        if (ImGui::Combo("Background", &backgroundMode, backgroundModeLabels, IM_ARRAYSIZE(backgroundModeLabels)))
        {
            environmentMap.SetBackgroundMode(
                static_cast<EnvironmentBackgroundMode>(backgroundMode));
            scene.MarkDirty();
        }
        RendererSettingUi::HandleFieldEdit("environment_background", editContext);
        RendererSettingUi::MarkRendered("environment_background");

        const bool skyboxBackground =
            environmentMap.GetBackgroundMode() == EnvironmentBackgroundMode::Skybox;
        if (!skyboxBackground)
        {
            glm::vec3 solidColor = environmentMap.GetSolidBackgroundColorSrgb();
            if (ImGui::ColorEdit3("Background color", glm::value_ptr(solidColor)))
            {
                environmentMap.SetSolidBackgroundColorSrgb(solidColor);
                scene.MarkDirty();
            }
            HandleRendererFieldEditEvents(editContext);
        }

        const std::string& skyboxPath = environmentMap.GetHdrPath();
        const std::string activeHdrLabel = skyboxPath.empty()
            ? "none"
            : std::filesystem::path(skyboxPath).filename().string();
        ImGui::Text("Active HDR: %s", activeHdrLabel.c_str());

        int environmentPreset = EnvironmentPresets::FindPresetIndex(skyboxPath);

        std::vector<const char*> presetLabels;
        presetLabels.reserve(EnvironmentPresets::kCount);
        for (std::size_t index = 0; index < EnvironmentPresets::kCount; ++index)
        {
            presetLabels.push_back(EnvironmentPresets::kEntries[index].label);
        }

        if (ImGui::Combo(
                "Environment preset",
                &environmentPreset,
                presetLabels.data(),
                static_cast<int>(presetLabels.size())))
        {
            if (environmentPreset > 0
                && static_cast<std::size_t>(environmentPreset) < EnvironmentPresets::kCount)
            {
                environmentMap.SetHdrPath(EnvironmentPresets::kEntries[environmentPreset].path);
                scene.MarkDirty();
            }
        }
        HandleRendererFieldEditEvents(editContext);

        static char skyboxPathBuffer[512] = {};
        if (skyboxPath.size() < sizeof(skyboxPathBuffer))
        {
            std::strncpy(skyboxPathBuffer, skyboxPath.c_str(), sizeof(skyboxPathBuffer) - 1);
        }

        ImGui::InputText("HDR path", skyboxPathBuffer, sizeof(skyboxPathBuffer));
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            environmentMap.SetHdrPath(skyboxPathBuffer);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);
        RendererSettingUi::MarkRendered("skybox_hdr_path");

        ImGui::SameLine();
        if (ImGui::Button("Browse##SkyboxHdr"))
        {
            std::string selectedPath;
            if (FileDialog::OpenImageFile(selectedPath))
            {
                environmentMap.SetHdrPath(selectedPath);
                scene.MarkDirty();
            }
        }

        float skyboxRotation = environmentMap.GetRotationDegrees();
        if (ImGui::SliderFloat("Rotation Y (deg)", &skyboxRotation, 0.0f, 360.0f))
        {
            const float previousRotation = environmentMap.GetRotationDegrees();
            environmentMap.SetRotationDegrees(skyboxRotation);
            // Environment lookup rotates world directions by +Y; the visible HDR (and linked
            // directional light) therefore moves through world space by the inverse angle.
            const float deltaRadians = -glm::radians(skyboxRotation - previousRotation);
            if (std::abs(deltaRadians) > 1e-6f)
            {
                for (std::size_t objectIndex = 0; objectIndex < scene.GetObjects().size(); ++objectIndex)
                {
                    const SceneObject& object = scene.GetObjects()[objectIndex];
                    if (!object.HasLight()
                        || object.GetLight().type != LightType::Directional
                        || !object.GetLight().autoAlignWithHdrSkybox)
                    {
                        continue;
                    }
                    Transform worldTransform = Transform::FromMatrix(
                        scene.GetWorldMatrix(static_cast<int>(objectIndex)));
                    worldTransform.rotation = glm::normalize(
                        glm::angleAxis(deltaRadians, glm::vec3(0.0f, 1.0f, 0.0f))
                        * worldTransform.rotation);
                    // Preserve the directional-light gizmo anchor: only its direction follows sky rotation.
                    scene.SetObjectWorldMatrix(static_cast<int>(objectIndex), worldTransform.ToMatrix());
                }
            }
            scene.MarkDirty();
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            environmentMap.CommitRotation();
        }
        RendererSettingUi::HandleFieldEdit("skybox_rotation", editContext);
        RendererSettingUi::MarkRendered("skybox_rotation");

        float skyboxExposure = environmentMap.GetExposure();
        if (ImGui::SliderFloat("Skybox exposure", &skyboxExposure, 0.1f, 4.0f))
        {
            environmentMap.SetExposure(skyboxExposure);
            scene.MarkDirty();
        }
        RendererSettingUi::HandleFieldEdit("skybox_exposure", editContext);
        RendererSettingUi::MarkRendered("skybox_exposure");

        int iblCubemapResolutionIndex =
            IblCubemapResolutionToComboIndex(environmentMap.GetIblCubemapResolution());
        const char* iblCubemapResolutionLabels[] = {
            "Auto (match HDR)",
            "512",
            "1024",
            "2048",
            "4096",
        };
        if (ImGui::Combo(
                "IBL cubemap resolution",
                &iblCubemapResolutionIndex,
                iblCubemapResolutionLabels,
                IM_ARRAYSIZE(iblCubemapResolutionLabels)))
        {
            environmentMap.SetIblCubemapResolution(
                IblCubemapResolutionFromComboIndex(iblCubemapResolutionIndex));
            scene.MarkDirty();
        }
        RendererSettingUi::HandleFieldEdit("ibl_cubemap_resolution", editContext);
        RendererSettingUi::MarkRendered("ibl_cubemap_resolution");
        LightingPanelUi::DrawTooltipForLastItem(
            "Resolution used for environment lighting and reflections. Higher values improve detail but use more GPU memory.");
        LightingPanelUi::DrawWrappedNote(
            "Sky background uses the HDR file at full resolution. IBL cubemap resolution affects reflections only.");

        if (environmentMap.IsLoaded())
        {
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.55f, 1.0f), "Status: loaded");
            const IBL& ibl = environmentMap.GetIBL();
            int hdrWidth = 0;
            int hdrHeight = 0;
            ibl.GetHdrDimensions(hdrWidth, hdrHeight);
            if (hdrWidth > 0 && hdrHeight > 0)
            {
                ImGui::Text("HDR resolution: %d x %d", hdrWidth, hdrHeight);
                if (hdrWidth <= 1024 && hdrHeight <= 512)
                {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.75f, 0.35f, 1.0f),
                        "Low-res 1K asset - re-pick preset or use a 2K/4K HDR for sharper sky.");
                }
            }

            ImGui::Text("IBL cubemap face size: %u px", ibl.GetCubemapFaceResolution());
        }
        else if (!environmentMap.GetLoadError().empty())
        {
            EditorWidgets::TextColoredError("Status: failed");
            ImGui::TextWrapped("%s", environmentMap.GetLoadError().c_str());
        }
        else
        {
            ImGui::TextDisabled("Status: pending");
        }

        LightingPanelUi::DrawWrappedNote(
            "For star fields and fine cloud detail, use 2K or 4K HDR files from Poly Haven.");

        ImGui::SeparatorText("Image-based lighting");
        float environmentIntensity = ibl.GetEnvironmentIntensity();
        if (ImGui::SliderFloat("Environment intensity", &environmentIntensity, 0.0f, 2.0f))
        {
            ibl.SetEnvironmentIntensity(environmentIntensity);
            scene.MarkDirty();
        }
        RendererSettingUi::HandleFieldEdit("environment_intensity", editContext);
        RendererSettingUi::MarkRendered("environment_intensity");
        LightingPanelUi::DrawWrappedNote(
            "Scales diffuse and specular IBL in the deferred composite. Lower this if RT GI or SSGI washes the scene out.");
    }
}
