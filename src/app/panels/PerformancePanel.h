#pragma once

#include "engine/platform/SystemResources.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class Scene;
class SceneRenderer;

struct ApplicationFrameDiagnostics
{
    double updateCpuMs = 0.0;
    double renderCpuMs = 0.0;
    double frameCpuMs = 0.0;
    double imguiBeginCpuMs = 0.0;
    double projectChooserUiCpuMs = 0.0;
    double viewportUiCpuMs = 0.0;
    double hierarchyUiCpuMs = 0.0;
    double inspectorUiCpuMs = 0.0;
    double projectFilesUiCpuMs = 0.0;
    double lightingUiCpuMs = 0.0;
    double performanceUiCpuMs = 0.0;
    double sceneEditorCpuMs = 0.0;
};

class PerformancePanel
{
public:
    void OnFrame(double deltaTimeSeconds);
    void SetApplicationFrameDiagnostics(const ApplicationFrameDiagnostics& diagnostics)
    {
        m_applicationFrameDiagnostics = diagnostics;
    }

    void Draw(
        const Scene& scene,
        const SceneRenderer& renderer,
        int sceneViewWidth,
        int sceneViewHeight,
        int windowWidth,
        int windowHeight,
        bool playModeActive) const;

    bool& ShowPanel() const { return m_showPanel; }
    bool& GpuPassSmoothingEnabled() const { return m_gpuPassSmoothingEnabled; }
    bool& CpuPassSmoothingEnabled() const { return m_cpuPassSmoothingEnabled; }

private:
    static void SmoothResourceValue(
        float& smoothed,
        float raw,
        float alpha,
        bool initialized);

    void RefreshSmoothedSystemResources(const SystemResourceSnapshot& snapshot, float alpha);
    SystemResourceSnapshot BuildDisplaySystemResources(const SystemResourceSnapshot& snapshot) const;

    struct SmoothedSystemResourceDisplay
    {
        bool initialized = false;
        float processCpuPercent = 0.0f;
        float processWorkingSetBytes = 0.0f;
        float systemUsedRamBytes = 0.0f;
        float gpuLocalUsageBytes = 0.0f;
        float d3d12LocalAllocatedBytes = 0.0f;
        float gpuSystemUtilizationPercent = -1.0f;
        bool gpuSystemUtilizationAvailable = false;
        float gpuInstrumentedFramePercent = -1.0f;
    };

    static constexpr int kHistorySize = 120;
    static constexpr int kPerfSampleInterval = 8;
    static constexpr float kPerfSmoothAlpha = 0.35f;

    mutable bool m_showPanel = true;
    mutable float m_frameTimeHistory[kHistorySize] = {};
    mutable int m_historyWriteIndex = 0;
    mutable int m_historyCount = 0;
    mutable float m_minFrameMs = 0.0f;
    mutable float m_maxFrameMs = 0.0f;
    mutable float m_sumFrameMs = 0.0f;
    mutable std::uint64_t m_frameCounter = 0;
    mutable int m_gpuTimingSampleCounter = 0;
    mutable bool m_cpuTimingSamplePending = true;
    mutable bool m_gpuPassSmoothingEnabled = false;
    mutable bool m_cpuPassSmoothingEnabled = false;
    mutable std::unordered_map<std::string, float> m_smoothedGpuPassMs;
    mutable std::unordered_map<std::string, float> m_smoothedCpuPassMs;
    ApplicationFrameDiagnostics m_applicationFrameDiagnostics{};
    mutable SmoothedSystemResourceDisplay m_smoothedSystemResources;
    SystemResourcesMonitor m_systemResources;
};
