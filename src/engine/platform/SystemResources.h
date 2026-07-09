#pragma once

#include <cstdint>

struct SystemResourceSnapshot
{
    float processCpuPercent = 0.0f;
    float gpuUtilizationPercent = -1.0f;
    bool gpuUtilizationAvailable = false;

    std::uint64_t processWorkingSetBytes = 0;
    std::uint64_t processPrivateBytes = 0;
    std::uint64_t systemUsedRamBytes = 0;
    std::uint64_t systemTotalRamBytes = 0;

    std::uint64_t gpuDedicatedTotalBytes = 0;
    std::uint64_t gpuLocalUsageBytes = 0;
    std::uint64_t gpuLocalBudgetBytes = 0;
    std::uint64_t d3d12LocalAllocatedBytes = 0;
    bool gpuMemoryValid = false;
};

// Lightweight process / system resource sampler (Windows). GPU memory comes from GfxContext;
// GPU utilization uses DXGI/PDH when available.
class SystemResourcesMonitor
{
public:
    SystemResourcesMonitor();
    ~SystemResourcesMonitor();

    SystemResourcesMonitor(const SystemResourcesMonitor&) = delete;
    SystemResourcesMonitor& operator=(const SystemResourcesMonitor&) = delete;

    void OnFrame(double deltaTimeSeconds);
    const SystemResourceSnapshot& GetSnapshot() const { return m_snapshot; }

private:
    SystemResourceSnapshot m_snapshot{};
    struct Impl;
    Impl* m_impl = nullptr;
};
