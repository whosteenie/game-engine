#include "app/panels/lighting/LightingPanelSections.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorUndoWidgets.h"
#include "app/editor/EditorWidgets.h"
#include "app/editor/TuningSectionState.h"
#include "app/scene/RenderDiagnostics.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneRenderer.h"
#include "app/undo/UndoCommand.h"
#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/EnvironmentIblSettings.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/EnvironmentPresets.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/platform/EngineLog.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/ScreenSpaceEffects.h"
#include "engine/rendering/DxrCapabilities.h"
#include "engine/rendering/DxrSettings.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/assets/FileDialog.h"
#include "app/panels/lighting/LightingPanelUi.h"
#include "app/panels/lighting/LightingPanelShared.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <filesystem>
#include <cmath>
#include <cstring>
#include <vector>

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
        HandleRendererFieldEditEvents(editContext);

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
            environmentMap.SetRotationDegrees(skyboxRotation);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

        float skyboxExposure = environmentMap.GetExposure();
        if (ImGui::SliderFloat("Skybox exposure", &skyboxExposure, 0.1f, 4.0f))
        {
            environmentMap.SetExposure(skyboxExposure);
            scene.MarkDirty();
        }
        HandleRendererFieldEditEvents(editContext);

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
        HandleRendererFieldEditEvents(editContext);
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
                        "Low-res 1K asset — re-pick preset or use a 2K/4K HDR for sharper sky.");
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
        HandleRendererFieldEditEvents(editContext);
        LightingPanelUi::DrawWrappedNote(
            "Scales diffuse and specular IBL in the deferred composite. Lower this if RT GI or SSGI washes the scene out.");
    }
}
