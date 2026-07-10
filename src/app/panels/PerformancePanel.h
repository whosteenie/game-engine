#pragma once

#include "engine/platform/SystemResources.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class Scene;

class PerformancePanel
{
public:
    void OnFrame(double deltaTimeSeconds);

    void Draw(
        const Scene& scene,
        int sceneViewWidth,
        int sceneViewHeight,
        int windowWidth,
        int windowHeight,
        bool playModeActive) const;

    bool& ShowPanel() const { return m_showPanel; }

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
    mutable std::unordered_map<std::string, float> m_smoothedGpuPassMs;
    mutable SmoothedSystemResourceDisplay m_smoothedSystemResources;
    SystemResourcesMonitor m_systemResources;
};
