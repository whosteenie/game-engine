#include "app/LightingPanel.h"

#include "app/EditorPanelLayout.h"
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
    EditorPanelLayout::ApplyFirstUseLayout(EditorPanelLayout::Panel::RendererTuning);

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
            scene.MarkDirty();
        }
    }

    ScreenSpaceEffects& screenSpaceEffects = scene.GetScreenSpaceEffects();

    if (ImGui::CollapsingHeader("HDR", ImGuiTreeNodeFlags_DefaultOpen))
    {
        float exposure = screenSpaceEffects.GetExposure();
        if (ImGui::SliderFloat("Exposure (stops)", &exposure, -2.0f, 4.0f))
        {
            screenSpaceEffects.SetExposure(exposure);
            scene.MarkDirty();
        }

        int tonemapMode = static_cast<int>(screenSpaceEffects.GetTonemapMode());
        const char* tonemapModes[] = {"Gamma", "Reinhard", "ACES"};
        if (ImGui::Combo("Tonemap", &tonemapMode, tonemapModes, IM_ARRAYSIZE(tonemapModes)))
        {
            screenSpaceEffects.SetTonemapMode(static_cast<TonemapMode>(tonemapMode));
            scene.MarkDirty();
        }

        bool bloomEnabled = screenSpaceEffects.IsBloomEnabled();
        if (ImGui::Checkbox("Bloom", &bloomEnabled))
        {
            screenSpaceEffects.SetBloomEnabled(bloomEnabled);
            scene.MarkDirty();
        }

        if (bloomEnabled)
        {
            float bloomThreshold = screenSpaceEffects.GetBloomThreshold();
            if (ImGui::SliderFloat("Bloom threshold", &bloomThreshold, 0.0f, 3.0f))
            {
                screenSpaceEffects.SetBloomThreshold(bloomThreshold);
                scene.MarkDirty();
            }

            float bloomSoftKnee = screenSpaceEffects.GetBloomSoftKnee();
            if (ImGui::SliderFloat("Bloom soft knee", &bloomSoftKnee, 0.0f, 1.0f))
            {
                screenSpaceEffects.SetBloomSoftKnee(bloomSoftKnee);
                scene.MarkDirty();
            }

            float bloomIntensity = screenSpaceEffects.GetBloomIntensity();
            if (ImGui::SliderFloat("Bloom intensity", &bloomIntensity, 0.0f, 2.0f))
            {
                screenSpaceEffects.SetBloomIntensity(bloomIntensity);
                scene.MarkDirty();
            }

            float bloomBlurRadius = screenSpaceEffects.GetBloomBlurRadius();
            if (ImGui::SliderFloat("Bloom blur radius", &bloomBlurRadius, 0.25f, 4.0f))
            {
                screenSpaceEffects.SetBloomBlurRadius(bloomBlurRadius);
                scene.MarkDirty();
            }
        }
    }

    if (ImGui::CollapsingHeader("Screen Space", ImGuiTreeNodeFlags_DefaultOpen))
    {
        bool enabled = screenSpaceEffects.IsEnabled();
        if (ImGui::Checkbox("Enable HDR post-processing", &enabled))
        {
            screenSpaceEffects.SetEnabled(enabled);
            scene.MarkDirty();
        }

        bool ssaoEnabled = screenSpaceEffects.IsSsaoEnabled();
        if (ImGui::Checkbox("SSAO", &ssaoEnabled))
        {
            screenSpaceEffects.SetSsaoEnabled(ssaoEnabled);
            scene.MarkDirty();
        }

        float ssaoRadius = screenSpaceEffects.GetSsaoRadius();
        if (ImGui::SliderFloat("SSAO radius", &ssaoRadius, 0.05f, 1.0f))
        {
            screenSpaceEffects.SetSsaoRadius(ssaoRadius);
            scene.MarkDirty();
        }

        float ssaoBias = screenSpaceEffects.GetSsaoBias();
        if (ImGui::SliderFloat("SSAO bias", &ssaoBias, 0.0f, 0.1f))
        {
            screenSpaceEffects.SetSsaoBias(ssaoBias);
            scene.MarkDirty();
        }

        float ssaoPower = screenSpaceEffects.GetSsaoPower();
        if (ImGui::SliderFloat("SSAO intensity", &ssaoPower, 0.5f, 4.0f))
        {
            screenSpaceEffects.SetSsaoPower(ssaoPower);
            scene.MarkDirty();
        }

        float aoStrength = screenSpaceEffects.GetAoStrength();
        if (ImGui::SliderFloat("SSAO blend strength", &aoStrength, 0.0f, 1.0f))
        {
            screenSpaceEffects.SetAoStrength(aoStrength);
            scene.MarkDirty();
        }

        bool contactShadowsEnabled = screenSpaceEffects.IsContactShadowsEnabled();
        if (ImGui::Checkbox("Contact shadows", &contactShadowsEnabled))
        {
            screenSpaceEffects.SetContactShadowsEnabled(contactShadowsEnabled);
            scene.MarkDirty();
        }

        if (contactShadowsEnabled)
        {
            float contactStrength = screenSpaceEffects.GetContactStrength();
            if (ImGui::SliderFloat("Contact shadow strength", &contactStrength, 0.0f, 1.0f))
            {
                screenSpaceEffects.SetContactStrength(contactStrength);
                scene.MarkDirty();
            }

            float contactDistance = screenSpaceEffects.GetContactShadowDistance();
            if (ImGui::SliderFloat("Contact shadow distance", &contactDistance, 0.02f, 0.5f))
            {
                screenSpaceEffects.SetContactShadowDistance(contactDistance);
                scene.MarkDirty();
            }

            int contactSteps = screenSpaceEffects.GetContactShadowSteps();
            if (ImGui::SliderInt("Contact shadow steps", &contactSteps, 4, 32))
            {
                screenSpaceEffects.SetContactShadowSteps(contactSteps);
                scene.MarkDirty();
            }
        }
    }

    ImGui::End();
}
