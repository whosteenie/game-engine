#include "app/LightingPanel.h"

#include "app/Scene.h"
#include "engine/Camera.h"
#include "engine/IBL.h"
#include "engine/ScreenSpaceEffects.h"

#include <imgui.h>

#include <glm/glm.hpp>

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

    IBL& ibl = scene.GetIBL();

    if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool showGizmos = scene.GetShowLightGizmos();
        if (ImGui::Checkbox("Show light gizmos", &showGizmos))
        {
            scene.SetShowLightGizmos(showGizmos);
        }

        ImGui::TextUnformatted("Create and edit lights from the Hierarchy and Inspector.");
    }

    if (ImGui::CollapsingHeader("Image-Based Lighting", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float environmentIntensity = ibl.GetEnvironmentIntensity();
        if (ImGui::SliderFloat("Environment intensity", &environmentIntensity, 0.0f, 2.0f))
        {
            ibl.SetEnvironmentIntensity(environmentIntensity);
        }
    }

    ScreenSpaceEffects& screenSpaceEffects = scene.GetScreenSpaceEffects();
    if (ImGui::CollapsingHeader("Screen Space", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool enabled = screenSpaceEffects.IsEnabled();
        if (ImGui::Checkbox("Enable screen-space effects", &enabled))
        {
            screenSpaceEffects.SetEnabled(enabled);
        }

        bool ssaoEnabled = screenSpaceEffects.IsSsaoEnabled();
        if (ImGui::Checkbox("SSAO", &ssaoEnabled))
        {
            screenSpaceEffects.SetSsaoEnabled(ssaoEnabled);
        }

        float ssaoRadius = screenSpaceEffects.GetSsaoRadius();
        if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.05f, 1.0f))
        {
            screenSpaceEffects.SetSsaoRadius(ssaoRadius);
        }

        float ssaoBias = screenSpaceEffects.GetSsaoBias();
        if (ImGui::SliderFloat("SSAO bias", &ssaoBias, 0.0f, 0.1f))
        {
            screenSpaceEffects.SetSsaoBias(ssaoBias);
        }

        float ssaoPower = screenSpaceEffects.GetSsaoPower();
        if (ImGui::SliderFloat("SSAO intensity", &ssaoPower, 0.5f, 4.0f))
        {
            screenSpaceEffects.SetSsaoPower(ssaoPower);
        }

        float aoStrength = screenSpaceEffects.GetAoStrength();
        if (ImGui::SliderFloat("SSAO blend strength", &aoStrength, 0.0f, 1.0f))
        {
            screenSpaceEffects.SetAoStrength(aoStrength);
        }

        bool contactShadowsEnabled = screenSpaceEffects.IsContactShadowsEnabled();
        if (ImGui::Checkbox("Contact shadows", &contactShadowsEnabled))
        {
            screenSpaceEffects.SetContactShadowsEnabled(contactShadowsEnabled);
        }

        if (contactShadowsEnabled)
        {
            float contactStrength = screenSpaceEffects.GetContactStrength();
            if (ImGui::SliderFloat("Contact shadow strength", &contactStrength, 0.0f, 1.0f))
            {
                screenSpaceEffects.SetContactStrength(contactStrength);
            }

            float contactDistance = screenSpaceEffects.GetContactShadowDistance();
            if (ImGui::SliderFloat("Contact shadow distance", &contactDistance, 0.02f, 0.5f))
            {
                screenSpaceEffects.SetContactShadowDistance(contactDistance);
            }

            int contactSteps = screenSpaceEffects.GetContactShadowSteps();
            if (ImGui::SliderInt("Contact shadow steps", &contactSteps, 4, 32))
            {
                screenSpaceEffects.SetContactShadowSteps(contactSteps);
            }
        }
    }

    ImGui::End();
}
