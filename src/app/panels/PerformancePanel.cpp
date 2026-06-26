#include "app/panels/PerformancePanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/scene/Scene.h"
#include "engine/rhi/GfxContext.h"
#include "engine/scene/SceneObject.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace
{
    struct SceneCounts
    {
        int objects = 0;
        int meshes = 0;
        int lights = 0;
    };

    SceneCounts CountSceneObjects(const Scene& scene)
    {
        SceneCounts counts;
        for (const SceneObject& object : scene.GetObjects())
        {
            ++counts.objects;
            if (object.HasMesh())
            {
                ++counts.meshes;
            }
            if (object.HasLight())
            {
                ++counts.lights;
            }
        }
        return counts;
    }

    ImVec4 FpsColor(float fps)
    {
        if (fps >= 55.0f)
        {
            return ImVec4(0.45f, 1.0f, 0.55f, 1.0f);
        }
        if (fps >= 30.0f)
        {
            return ImVec4(1.0f, 0.85f, 0.35f, 1.0f);
        }
        return ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
    }

    float MsToFps(const float frameMs)
    {
        return frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;
    }
}

void PerformancePanel::OnFrame(const double deltaTimeSeconds)
{
    const float frameMs = std::clamp(static_cast<float>(deltaTimeSeconds * 1000.0), 0.01f, 1000.0f);

    m_frameTimeHistory[m_historyWriteIndex] = frameMs;
    m_historyWriteIndex = (m_historyWriteIndex + 1) % kHistorySize;
    m_historyCount = std::min(m_historyCount + 1, kHistorySize);
    ++m_frameCounter;

    m_minFrameMs = frameMs;
    m_maxFrameMs = frameMs;
    m_sumFrameMs = 0.0f;
    for (int index = 0; index < m_historyCount; ++index)
    {
        const int sampleIndex =
            m_historyCount < kHistorySize
                ? index
                : (m_historyWriteIndex + index) % kHistorySize;
        const float sample = m_frameTimeHistory[sampleIndex];
        m_minFrameMs = std::min(m_minFrameMs, sample);
        m_maxFrameMs = std::max(m_maxFrameMs, sample);
        m_sumFrameMs += sample;
    }
}

void PerformancePanel::Draw(
    const Scene& scene,
    const int sceneViewWidth,
    const int sceneViewHeight,
    const int windowWidth,
    const int windowHeight,
    const bool playModeActive) const
{
    EditorPanelConstraints::ApplySideColumnPanel();
    if (!EditorPanelConstraints::BeginDockedPanel("Performance", m_showPanel))
    {
        return;
    }

    const int lastSampleIndex =
        m_historyCount > 0 ? (m_historyWriteIndex + kHistorySize - 1) % kHistorySize : 0;
    const float currentMs = m_historyCount > 0 ? m_frameTimeHistory[lastSampleIndex] : 0.0f;
    const float averageMs = m_historyCount > 0 ? m_sumFrameMs / static_cast<float>(m_historyCount) : 0.0f;
    const float currentFps = MsToFps(currentMs);
    const float averageFps = MsToFps(averageMs);
    const float minFps = MsToFps(m_maxFrameMs);
    const float maxFps = MsToFps(m_minFrameMs);

    if (ImGui::CollapsingHeader("Frame timing", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::PushStyleColor(ImGuiCol_Text, FpsColor(currentFps));
        ImGui::Text("%.1f FPS", currentFps);
        ImGui::PopStyleColor();
        ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(m_frameCounter));

        ImGui::Separator();

        if (ImGui::BeginTable("perf_frame_stats", 4, ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("Current");
            ImGui::TableSetupColumn("Average");
            ImGui::TableSetupColumn("Min / Max");
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("FPS");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", currentFps);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", averageFps);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f / %.1f", minFps, maxFps);

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Frame (ms)");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", currentMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", averageMs);
            ImGui::TableNextColumn();
            ImGui::Text("%.2f / %.2f", m_minFrameMs, m_maxFrameMs);

            ImGui::EndTable();
        }

        const ImGuiIO& io = ImGui::GetIO();
        ImGui::TextDisabled("ImGui framerate: %.1f FPS (%.3f ms)", io.Framerate, io.DeltaTime * 1000.0f);

        const ImVec2 plotSize = ImVec2(ImGui::GetContentRegionAvail().x, 80.0f);
        const char* overlay = currentMs > 0.0f ? "ms / frame" : "Collecting...";
        const int plotOffset = m_historyCount < kHistorySize ? 0 : m_historyWriteIndex;
        ImGui::PlotLines(
            "##frame_time_plot",
            m_frameTimeHistory,
            m_historyCount,
            plotOffset,
            overlay,
            0.0f,
            std::max(m_maxFrameMs, 33.3f),
            plotSize);
        ImGui::TextDisabled("Rolling window: last %d frames", kHistorySize);
    }

    if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const SceneCounts counts = CountSceneObjects(scene);
        ImGui::Text("Objects: %d", counts.objects);
        ImGui::Text("Meshes: %d", counts.meshes);
        ImGui::Text("Lights: %d", counts.lights);
        ImGui::Text("Selected: %d", static_cast<int>(scene.GetSelection().indices.size()));
    }

    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("Play mode: %s", playModeActive ? "active" : "editor");

        int outputWidth = 0;
        int outputHeight = 0;
        GfxContext::Get().GetOutputRenderSize(outputWidth, outputHeight);
        ImGui::Text("Scene view: %d x %d", sceneViewWidth, sceneViewHeight);
        ImGui::Text("Render output: %d x %d", outputWidth, outputHeight);
        ImGui::Text(
            "Swapchain: %d x %d",
            GfxContext::Get().GetWidth(),
            GfxContext::Get().GetHeight());
        ImGui::Text("Window: %d x %d", windowWidth, windowHeight);

        std::uint32_t srvUsed = 0;
        std::uint32_t srvCapacity = 0;
        GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
        const float srvUsage =
            srvCapacity > 0 ? static_cast<float>(srvUsed) / static_cast<float>(srvCapacity) : 0.0f;
        ImGui::Text("SRV descriptors: %u / %u", srvUsed, srvCapacity);
        ImGui::ProgressBar(srvUsage, ImVec2(-1.0f, 0.0f));

        const std::string& gpuError = GfxContext::GetLastGpuAllocationError();
        if (!gpuError.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextWrapped("Last GPU alloc error: %s", gpuError.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::End();
}
