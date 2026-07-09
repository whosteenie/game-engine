#include "app/panels/PerformancePanel.h"

#include "app/editor/EditorPanelConstraints.h"
#include "app/editor/EditorWidgets.h"
#include "app/scene/Scene.h"
#include "engine/rhi/GfxContext.h"
#include "engine/scene/SceneObject.h"

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    struct SceneCounts
    {
        int objects = 0;
        int meshes = 0;
        int lights = 0;
    };

    struct GpuPassNode
    {
        std::string label;
        float milliseconds = 0.0f;
        std::vector<GpuPassNode> children;
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
        return EditorWidgets::ErrorTextColor();
    }

    float MsToFps(const float frameMs)
    {
        return frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;
    }

    void FormatByteSize(const std::uint64_t bytes, char* buffer, const std::size_t bufferSize)
    {
        if (bytes >= 1024ull * 1024ull * 1024ull)
        {
            std::snprintf(
                buffer,
                bufferSize,
                "%.2f GiB",
                static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
        }
        else if (bytes >= 1024ull * 1024ull)
        {
            std::snprintf(
                buffer,
                bufferSize,
                "%.1f MiB",
                static_cast<double>(bytes) / (1024.0 * 1024.0));
        }
        else if (bytes >= 1024ull)
        {
            std::snprintf(
                buffer,
                bufferSize,
                "%.1f KiB",
                static_cast<double>(bytes) / 1024.0);
        }
        else
        {
            std::snprintf(buffer, bufferSize, "%llu B", static_cast<unsigned long long>(bytes));
        }
    }

    GpuPassNode* FindOrCreateChild(std::vector<GpuPassNode>& nodes, const std::string& label)
    {
        for (GpuPassNode& node : nodes)
        {
            if (node.label == label)
            {
                return &node;
            }
        }
        nodes.push_back(GpuPassNode{label, 0.0f, {}});
        return &nodes.back();
    }

    void InsertGpuTiming(std::vector<GpuPassNode>& roots, const std::string& path, const float ms)
    {
        const std::size_t slash = path.find('/');
        if (slash == std::string::npos)
        {
            GpuPassNode* node = FindOrCreateChild(roots, path);
            node->milliseconds = ms;
            return;
        }

        GpuPassNode* parent = FindOrCreateChild(roots, path.substr(0, slash));
        InsertGpuTiming(parent->children, path.substr(slash + 1), ms);
    }

    float DisplayGpuPassMilliseconds(const GpuPassNode& node)
    {
        if (!node.children.empty())
        {
            if (node.milliseconds > 0.0f)
            {
                return node.milliseconds;
            }
            float total = 0.0f;
            for (const GpuPassNode& child : node.children)
            {
                total += DisplayGpuPassMilliseconds(child);
            }
            return total;
        }
        return node.milliseconds;
    }

    int RootSortRank(const std::string& label)
    {
        static const char* kOrder[] = {
            "Shadow maps",
            "Scene raster",
            "Path tracer",
            "Post-process",
            "DLSS",
            "UI (ImGui)",
        };
        for (int index = 0; index < IM_ARRAYSIZE(kOrder); ++index)
        {
            if (label == kOrder[index])
            {
                return index;
            }
        }
        return IM_ARRAYSIZE(kOrder);
    }

    void SortGpuPassRoots(std::vector<GpuPassNode>& roots)
    {
        std::sort(
            roots.begin(),
            roots.end(),
            [](const GpuPassNode& left, const GpuPassNode& right) {
                const int leftRank = RootSortRank(left.label);
                const int rightRank = RootSortRank(right.label);
                if (leftRank != rightRank)
                {
                    return leftRank < rightRank;
                }
                return left.label < right.label;
            });
    }

    void SortGpuPassChildren(std::vector<GpuPassNode>& nodes)
    {
        std::sort(
            nodes.begin(),
            nodes.end(),
            [](const GpuPassNode& left, const GpuPassNode& right) {
                return DisplayGpuPassMilliseconds(left) > DisplayGpuPassMilliseconds(right);
            });
        for (GpuPassNode& node : nodes)
        {
            SortGpuPassChildren(node.children);
        }
    }

    std::vector<GpuPassNode> BuildGpuPassTree(const std::vector<GpuProfiler::Entry>& timings)
    {
        std::vector<GpuPassNode> roots;
        for (const GpuProfiler::Entry& entry : timings)
        {
            InsertGpuTiming(roots, entry.name, entry.milliseconds);
        }
        SortGpuPassRoots(roots);
        SortGpuPassChildren(roots);
        return roots;
    }

    float ComputeGpuRootTotalMs(const std::vector<GpuPassNode>& roots)
    {
        float total = 0.0f;
        for (const GpuPassNode& node : roots)
        {
            total += DisplayGpuPassMilliseconds(node);
        }
        return total;
    }

    float MaxGpuRootMilliseconds(const std::vector<GpuPassNode>& roots)
    {
        float maxMs = 0.0f;
        for (const GpuPassNode& node : roots)
        {
            maxMs = std::max(maxMs, DisplayGpuPassMilliseconds(node));
        }
        return maxMs;
    }

    void DrawGpuPassBar(
        const float milliseconds,
        const float maxPassMs,
        const float gpuTotalMs)
    {
        const float barFrac = maxPassMs > 0.0f ? milliseconds / maxPassMs : 0.0f;
        const float sharePct = gpuTotalMs > 0.0f ? milliseconds / gpuTotalMs * 100.0f : 0.0f;
        char overlay[16];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", sharePct);
        ImGui::ProgressBar(barFrac, ImVec2(-1.0f, 0.0f), overlay);
    }

    void DrawGpuPassRow(
        const GpuPassNode& node,
        const float maxPassMs,
        const float gpuTotalMs,
        const int depth)
    {
        const bool hasChildren = !node.children.empty();
        const float displayMs = DisplayGpuPassMilliseconds(node);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        ImGui::PushID(node.label.c_str());
        ImGui::Indent(static_cast<float>(depth) * 12.0f);

        ImGuiTreeNodeFlags treeFlags = ImGuiTreeNodeFlags_SpanAvailWidth;
        bool open = false;
        if (hasChildren)
        {
            treeFlags |= ImGuiTreeNodeFlags_DefaultOpen;
            open = ImGui::TreeNodeEx(node.label.c_str(), treeFlags);
        }
        else
        {
            treeFlags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet | ImGuiTreeNodeFlags_NoTreePushOnOpen;
            ImGui::TreeNodeEx(node.label.c_str(), treeFlags);
        }

        ImGui::TableNextColumn();
        ImGui::Text("%.3f", displayMs);
        ImGui::TableNextColumn();
        DrawGpuPassBar(displayMs, maxPassMs, gpuTotalMs);

        if (open)
        {
            for (const GpuPassNode& child : node.children)
            {
                DrawGpuPassRow(child, maxPassMs, gpuTotalMs, depth + 1);
            }
            ImGui::TreePop();
        }

        ImGui::Unindent(static_cast<float>(depth) * 12.0f);
        ImGui::PopID();
    }

    void DrawUsageRow(const char* label, const std::uint64_t usedBytes, const std::uint64_t totalBytes)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();

        char usedText[32];
        char totalText[32];
        FormatByteSize(usedBytes, usedText, sizeof(usedText));
        FormatByteSize(totalBytes, totalText, sizeof(totalText));
        ImGui::Text("%s / %s", usedText, totalText);

        ImGui::TableNextColumn();
        const float usage =
            totalBytes > 0 ? static_cast<float>(usedBytes) / static_cast<float>(totalBytes) : 0.0f;
        char overlay[16];
        std::snprintf(overlay, sizeof(overlay), "%.0f%%", usage * 100.0f);
        ImGui::ProgressBar(usage, ImVec2(-1.0f, 0.0f), overlay);
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

    m_systemResources.OnFrame(deltaTimeSeconds);
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

    if (ImGui::CollapsingHeader("System resources", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const SystemResourceSnapshot& resources = m_systemResources.GetSnapshot();

        if (ImGui::BeginTable(
                "perf_system_resources",
                3,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Metric");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Usage");
            ImGui::TableHeadersRow();

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("CPU (process)");
            ImGui::TableNextColumn();
            ImGui::Text("%.1f%%", resources.processCpuPercent);
            ImGui::TableNextColumn();
            ImGui::ProgressBar(
                std::clamp(resources.processCpuPercent / 100.0f, 0.0f, 1.0f),
                ImVec2(-1.0f, 0.0f));

            DrawUsageRow(
                "RAM (process)",
                resources.processWorkingSetBytes,
                resources.systemTotalRamBytes);
            DrawUsageRow(
                "RAM (system)",
                resources.systemUsedRamBytes,
                resources.systemTotalRamBytes);

            if (resources.gpuMemoryValid)
            {
                const std::uint64_t vramTotal =
                    resources.gpuDedicatedTotalBytes > 0
                        ? resources.gpuDedicatedTotalBytes
                        : resources.gpuLocalBudgetBytes;
                DrawUsageRow("VRAM (process)", resources.gpuLocalUsageBytes, vramTotal);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("D3D12 allocated");
                ImGui::TableNextColumn();
                char allocatedText[32];
                FormatByteSize(resources.d3d12LocalAllocatedBytes, allocatedText, sizeof(allocatedText));
                ImGui::TextUnformatted(allocatedText);
                ImGui::TableNextColumn();
                const float d3d12Share =
                    vramTotal > 0
                        ? static_cast<float>(resources.d3d12LocalAllocatedBytes)
                            / static_cast<float>(vramTotal)
                        : 0.0f;
                ImGui::ProgressBar(d3d12Share, ImVec2(-1.0f, 0.0f));
            }
            else
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("VRAM");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("GPU not ready");
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("GPU load");
            ImGui::TableNextColumn();
            if (resources.gpuUtilizationAvailable)
            {
                ImGui::Text("%.0f%%", resources.gpuUtilizationPercent);
                ImGui::TableNextColumn();
                ImGui::ProgressBar(
                    std::clamp(resources.gpuUtilizationPercent / 100.0f, 0.0f, 1.0f),
                    ImVec2(-1.0f, 0.0f));
            }
            else
            {
                ImGui::TextDisabled("N/A");
                ImGui::TableNextColumn();
                ImGui::TextDisabled("-");
            }

            ImGui::EndTable();
        }

        if (GfxContext::Get().IsInitialized())
        {
            ImGui::TextDisabled("GPU: %s", GfxContext::Get().GetAdapterDescription().c_str());
        }
        ImGui::TextDisabled(
            "VRAM usage is per-process (DXGI). GPU load uses Windows perf counters when available.");
    }

    if (ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const std::vector<GpuProfiler::Entry>& timings = GfxContext::Get().GetGpuTimings();
        if (timings.empty())
        {
            ImGui::TextDisabled("No GPU timing data yet.");
            ImGui::TextDisabled("(timestamp queries unavailable or first frames)");
        }
        else
        {
            const std::vector<GpuPassNode> passTree = BuildGpuPassTree(timings);
            const float gpuTotalMs = ComputeGpuRootTotalMs(passTree);
            const float maxPassMs = MaxGpuRootMilliseconds(passTree);

            if (ImGui::BeginTable(
                    "perf_gpu_passes",
                    3,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Pass");
                ImGui::TableSetupColumn("ms");
                ImGui::TableSetupColumn("Share");
                ImGui::TableHeadersRow();

                for (const GpuPassNode& root : passTree)
                {
                    DrawGpuPassRow(root, maxPassMs, gpuTotalMs, 0);
                }

                ImGui::EndTable();
            }

            ImGui::Text("GPU total (top-level passes): %.3f ms", gpuTotalMs);
            ImGui::TextDisabled(
                "Expand categories for sub-pass breakdown. ~1-2 frame latency; nested scopes are not double-counted.");
        }
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
            EditorWidgets::TextColoredError("Last GPU alloc error: %s", gpuError.c_str());
        }
    }

    ImGui::End();
}
