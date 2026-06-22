#include "app/LightingPanel.h"

#include "app/EditorWidgets.h"
#include "app/Scene.h"
#include "engine/Camera.h"
#include "engine/IBL.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/SceneLighting.h"

#include <imgui.h>

#include <glm/glm.hpp>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    const char* LightTypeLabel(LightType type)
    {
        switch (type)
        {
        case LightType::Directional:
            return "Directional";
        case LightType::Point:
            return "Point";
        case LightType::Spot:
            return "Spot";
        }

        return "Unknown";
    }
}

void LightingPanel::Draw(
    Scene& scene,
    const Camera& camera) const
{
    ImGui::SetNextWindowSize(ImVec2(440.0f, 620.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Renderer Tuning", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    const glm::vec3 cameraPosition = camera.GetPosition();
    ImGui::Text("Camera: (%.1f, %.1f, %.1f)", cameraPosition.x, cameraPosition.y, cameraPosition.z);

    SceneLighting& lighting = scene.GetLighting();
    IBL& ibl = scene.GetIBL();

    if (ImGui::CollapsingHeader("Lights", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool showGizmos = scene.GetShowLightGizmos();
        if (ImGui::Checkbox("Show light gizmos", &showGizmos))
        {
            scene.SetShowLightGizmos(showGizmos);
        }

        const std::size_t lightCount = lighting.GetLightCount();
        if (lightCount == 0)
        {
            ImGui::TextUnformatted("No lights in scene.");
        }
        else
        {
            int selectedLightIndex = scene.GetSelectedLightIndex();
            if (selectedLightIndex >= static_cast<int>(lightCount))
            {
                selectedLightIndex = static_cast<int>(lightCount) - 1;
                scene.SetSelectedLightIndex(selectedLightIndex);
            }
            else if (selectedLightIndex < 0)
            {
                selectedLightIndex = 0;
            }

            std::vector<const char*> lightLabels;
            std::vector<std::string> lightLabelStorage;
            lightLabelStorage.reserve(lightCount);
            lightLabels.reserve(lightCount);

            for (std::size_t lightIndex = 0; lightIndex < lightCount; ++lightIndex)
            {
                const Light& light = lighting.GetLights()[lightIndex];
                char buffer[64];
                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "Light %zu (%s)",
                    lightIndex,
                    LightTypeLabel(light.GetType()));
                lightLabelStorage.emplace_back(buffer);
                lightLabels.push_back(lightLabelStorage.back().c_str());
            }

            if (ImGui::Combo("Active light", &selectedLightIndex, lightLabels.data(), static_cast<int>(lightLabels.size())))
            {
                scene.SetSelectedLightIndex(selectedLightIndex);
            }

            Light& activeLight = lighting.GetLight(static_cast<std::size_t>(selectedLightIndex));
            ImGui::Text("Type: %s", LightTypeLabel(activeLight.GetType()));

            glm::vec3 color = activeLight.GetColor();
            if (EditorWidgets::ColorEditVec3("Color", color))
            {
                activeLight.SetColor(color);
            }

            float intensity = activeLight.GetIntensity();
            if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 8.0f))
            {
                activeLight.SetIntensity(intensity);
            }

            switch (activeLight.GetType())
            {
            case LightType::Directional:
            {
                glm::vec3 direction = activeLight.GetDirection();
                if (EditorWidgets::SliderVec3("Direction (toward source)", direction, -1.0f, 1.0f))
                {
                    activeLight.SetDirection(direction);
                }

                float gizmoDistance = glm::length(activeLight.GetPosition());
                if (gizmoDistance < 0.001f)
                {
                    gizmoDistance = 14.0f;
                }

                if (ImGui::SliderFloat("Gizmo distance", &gizmoDistance, 6.0f, 30.0f))
                {
                    activeLight.SetPosition(glm::normalize(activeLight.GetDirection()) * gizmoDistance);
                }

                ImGui::TextUnformatted("Wireframe: disc at gizmo position with rays along shine direction.");
                break;
            }
            case LightType::Point:
            {
                glm::vec3 position = activeLight.GetPosition();
                if (EditorWidgets::SliderVec3("Position", position, -20.0f, 20.0f))
                {
                    activeLight.SetPosition(position);
                }

                float range = activeLight.GetRange();
                if (ImGui::SliderFloat("Range (0 = infinite)", &range, 0.0f, 25.0f))
                {
                    activeLight.SetRange(range);
                }

                ImGui::TextUnformatted("Wireframe: sphere at light position.");
                break;
            }
            case LightType::Spot:
            {
                glm::vec3 position = activeLight.GetPosition();
                if (EditorWidgets::SliderVec3("Position", position, -20.0f, 20.0f))
                {
                    activeLight.SetPosition(position);
                }

                glm::vec3 direction = activeLight.GetDirection();
                if (EditorWidgets::SliderVec3("Direction (toward source)", direction, -1.0f, 1.0f))
                {
                    activeLight.SetDirection(direction);
                }

                float outerCutoffDegrees = glm::degrees(std::acos(glm::clamp(activeLight.GetOuterCutoffCos(), -1.0f, 1.0f)));
                float innerCutoffDegrees = glm::degrees(std::acos(glm::clamp(activeLight.GetInnerCutoffCos(), -1.0f, 1.0f)));
                if (ImGui::SliderFloat("Outer angle", &outerCutoffDegrees, 1.0f, 89.0f))
                {
                    innerCutoffDegrees = glm::min(innerCutoffDegrees, outerCutoffDegrees - 1.0f);
                    activeLight.SetSpotCutoffDegrees(innerCutoffDegrees, outerCutoffDegrees);
                }

                if (ImGui::SliderFloat("Inner angle", &innerCutoffDegrees, 0.0f, outerCutoffDegrees - 1.0f))
                {
                    activeLight.SetSpotCutoffDegrees(innerCutoffDegrees, outerCutoffDegrees);
                }

                float range = activeLight.GetRange();
                if (ImGui::SliderFloat("Range (0 = default gizmo length)", &range, 0.0f, 25.0f))
                {
                    activeLight.SetRange(range);
                }

                ImGui::TextUnformatted("Wireframe: cone from position along shine direction.");
                break;
            }
            }

            if (lightCount < static_cast<std::size_t>(SceneLighting::MaxLights))
            {
                ImGui::Separator();
                if (ImGui::Button("Add point light"))
                {
                    lighting.AddLight(Light::MakePoint(
                        glm::vec3(2.0f, 3.0f, 1.0f),
                        glm::vec3(1.0f, 0.85f, 0.65f),
                        2.0f));
                    scene.SetSelectedLightIndex(static_cast<int>(lighting.GetLightCount()) - 1);
                }

                ImGui::SameLine();
                if (ImGui::Button("Add spot light"))
                {
                    lighting.AddLight(Light::MakeSpot(
                        glm::vec3(-2.5f, 4.0f, 0.5f),
                        glm::vec3(0.2f, -1.0f, -0.1f),
                        glm::vec3(0.75f, 0.9f, 1.0f),
                        3.0f,
                        10.0f,
                        18.0f,
                        1.0f,
                        0.07f,
                        0.017f,
                        8.0f));
                    scene.SetSelectedLightIndex(static_cast<int>(lighting.GetLightCount()) - 1);
                }
            }
        }
    }

    if (ImGui::CollapsingHeader("Image-Based Lighting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float environmentIntensity = ibl.GetEnvironmentIntensity();
        if (ImGui::SliderFloat("Environment intensity", &environmentIntensity, 0.0f, 2.0f))
        {
            ibl.SetEnvironmentIntensity(environmentIntensity);
        }
    }

    ImGui::End();
}
