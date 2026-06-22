#include "app/EditorPanelLayout.h"

#include <imgui.h>

#include <algorithm>

namespace
{
    struct LayoutMetrics
    {
        ImVec2 workPos = {};
        ImVec2 workSize = {};
        float margin = 8.0f;
        float leftColumnWidth = 280.0f;
        float projectPanelWidth = 720.0f;
        float inspectorWidth = 280.0f;
        float rendererPanelHeight = 420.0f;
        float projectPanelHeight = 220.0f;
        float hierarchyPanelHeight = 280.0f;
    };

    float Clamp(float value, float minValue, float maxValue)
    {
        return std::max(minValue, std::min(value, maxValue));
    }

    LayoutMetrics ComputeLayoutMetrics()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();

        LayoutMetrics metrics;
        metrics.workPos = viewport->WorkPos;
        metrics.workSize = viewport->WorkSize;

        const float workWidth = std::max(metrics.workSize.x, 1.0f);
        const float workHeight = std::max(metrics.workSize.y, 1.0f);
        const float aspect = workWidth / workHeight;
        const float wideFactor = Clamp((aspect - 1.2f) / 1.0f, 0.0f, 1.0f);

        metrics.leftColumnWidth = Clamp(workWidth * (0.20f + 0.04f * wideFactor), 260.0f, 440.0f);
        metrics.inspectorWidth = Clamp(workWidth * (0.16f + 0.04f * wideFactor), 260.0f, 340.0f);

        const float maxProjectWidth = workWidth - metrics.inspectorWidth - metrics.margin * 3.0f;
        metrics.projectPanelWidth =
            Clamp(workWidth * (0.34f + 0.10f * wideFactor), metrics.leftColumnWidth + 120.0f, maxProjectWidth);

        metrics.projectPanelHeight = Clamp(workHeight * (0.18f + 0.04f * wideFactor), 170.0f, 280.0f);
        metrics.rendererPanelHeight = Clamp(workHeight * (0.40f - 0.05f * wideFactor), 280.0f, 520.0f);

        constexpr float kMinHierarchyHeight = 140.0f;
        const float verticalGaps = metrics.margin * 3.0f;
        metrics.hierarchyPanelHeight =
            workHeight - metrics.rendererPanelHeight - metrics.projectPanelHeight - verticalGaps;

        if (metrics.hierarchyPanelHeight < kMinHierarchyHeight)
        {
            const float deficit = kMinHierarchyHeight - metrics.hierarchyPanelHeight;
            metrics.rendererPanelHeight = std::max(240.0f, metrics.rendererPanelHeight - deficit);
            metrics.hierarchyPanelHeight =
                workHeight - metrics.rendererPanelHeight - metrics.projectPanelHeight - verticalGaps;
        }

        metrics.hierarchyPanelHeight = Clamp(metrics.hierarchyPanelHeight, kMinHierarchyHeight, 640.0f);
        return metrics;
    }
}

void EditorPanelLayout::ApplyFirstUseLayout(Panel panel)
{
    const LayoutMetrics metrics = ComputeLayoutMetrics();
    const float leftX = metrics.workPos.x + metrics.margin;
    const float topY = metrics.workPos.y + metrics.margin;
    const float rightX = metrics.workPos.x + metrics.workSize.x - metrics.inspectorWidth - metrics.margin;
    const float bottomY = metrics.workPos.y + metrics.workSize.y - metrics.projectPanelHeight - metrics.margin;
    const float hierarchyY = topY + metrics.rendererPanelHeight + metrics.margin;

    switch (panel)
    {
    case Panel::RendererTuning:
        ImGui::SetNextWindowPos(ImVec2(leftX, topY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            ImVec2(metrics.leftColumnWidth, metrics.rendererPanelHeight),
            ImGuiCond_FirstUseEver);
        break;

    case Panel::Toolbar:
    {
        const float centerX = metrics.workPos.x + metrics.workSize.x * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(centerX, topY), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
        break;
    }

    case Panel::Hierarchy:
        ImGui::SetNextWindowPos(ImVec2(leftX, hierarchyY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            ImVec2(metrics.leftColumnWidth, metrics.hierarchyPanelHeight),
            ImGuiCond_FirstUseEver);
        break;

    case Panel::ProjectFiles:
        ImGui::SetNextWindowPos(ImVec2(leftX, bottomY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            ImVec2(metrics.projectPanelWidth, metrics.projectPanelHeight),
            ImGuiCond_FirstUseEver);
        break;

    case Panel::Inspector:
        ImGui::SetNextWindowPos(ImVec2(rightX, topY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            ImVec2(metrics.inspectorWidth, metrics.workSize.y - metrics.margin * 2.0f),
            ImGuiCond_FirstUseEver);
        break;
    }
}
