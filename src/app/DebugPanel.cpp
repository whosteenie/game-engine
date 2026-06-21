#include "app/DebugPanel.h"

#include "app/DemoScene.h"
#include "engine/Camera.h"
#include "engine/IBL.h"
#include "engine/Light.h"
#include "engine/Material.h"
#include "engine/SceneLighting.h"

#include <imgui.h>

#include <glm/glm.hpp>

namespace
{
    bool SliderVec3(const char* label, glm::vec3& value, float min, float max)
    {
        const bool changed = ImGui::SliderFloat3(label, &value.x, min, max);
        return changed;
    }

    bool ColorEditVec3(const char* label, glm::vec3& value)
    {
        return ImGui::ColorEdit3(label, &value.x);
    }
}

void DebugPanel::Draw(
    DemoScene& scene,
    Material& cubeMaterial,
    const Camera& camera,
    bool paused) const
{
    ImGui::SetNextWindowSize(ImVec2(440.0f, 620.0f), ImGuiCond_FirstUseEver);

    if (!ImGui::Begin("Renderer Tuning", &m_showPanel, ImGuiWindowFlags_None))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Pause animation: Space");
    ImGui::Text("Paused: %s", paused ? "yes" : "no");

    const glm::vec3 cameraPosition = camera.GetPosition();
    ImGui::Text("Camera: (%.1f, %.1f, %.1f)", cameraPosition.x, cameraPosition.y, cameraPosition.z);

    SceneLighting& lighting = scene.GetLighting();
    IBL& ibl = scene.GetIBL();

    if (lighting.GetLightCount() > 0)
    {
        Light& sun = lighting.GetLight(0);

        if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen))
        {
            glm::vec3 direction = sun.GetDirection();
            if (SliderVec3("Direction", direction, -1.0f, 1.0f))
            {
                sun.SetDirection(direction);
            }

            float intensity = sun.GetIntensity();
            if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 8.0f))
            {
                sun.SetIntensity(intensity);
            }

            glm::vec3 color = sun.GetColor();
            if (ColorEditVec3("Color", color))
            {
                sun.SetColor(color);
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

    if (ImGui::CollapsingHeader("Cube Material", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushID("CubeMaterial");

        glm::vec3 albedo = cubeMaterial.GetAlbedo();
        if (ColorEditVec3("Albedo", albedo))
        {
            cubeMaterial.SetAlbedo(albedo);
        }

        float roughness = cubeMaterial.GetRoughness();
        if (ImGui::SliderFloat("Roughness", &roughness, 0.04f, 1.0f))
        {
            cubeMaterial.SetRoughness(roughness);
        }

        float metallic = cubeMaterial.GetMetallic();
        if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f))
        {
            cubeMaterial.SetMetallic(metallic);
        }

        ImGui::PopID();
    }

    Material& floorMaterial = scene.GetFloorMaterial();
    if (ImGui::CollapsingHeader("Floor Material"))
    {
        ImGui::PushID("FloorMaterial");

        glm::vec3 albedo = floorMaterial.GetAlbedo();
        if (ColorEditVec3("Albedo", albedo))
        {
            floorMaterial.SetAlbedo(albedo);
        }

        float roughness = floorMaterial.GetRoughness();
        if (ImGui::SliderFloat("Roughness", &roughness, 0.04f, 1.0f))
        {
            floorMaterial.SetRoughness(roughness);
        }

        float metallic = floorMaterial.GetMetallic();
        if (ImGui::SliderFloat("Metallic", &metallic, 0.0f, 1.0f))
        {
            floorMaterial.SetMetallic(metallic);
        }

        ImGui::PopID();
    }

    ImGui::End();
}
