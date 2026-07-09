#include "engine/platform/SystemResources.h"

#include "engine/rhi/GfxContext.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Psapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "Psapi.lib")
#pragma comment(lib, "Pdh.lib")
#endif

struct SystemResourcesMonitor::Impl
{
#ifdef _WIN32
    std::uint64_t lastWallTicks = 0;
    std::uint64_t lastProcessTicks = 0;
    int cpuSampleCount = 0;
    float cpuEma = 0.0f;
    int processorCount = 1;

    PDH_HQUERY gpuQuery = nullptr;
    PDH_HCOUNTER gpuCounter = nullptr;
    bool gpuCounterReady = false;
    bool gpuCounterPrimed = false;
#endif
};

SystemResourcesMonitor::SystemResourcesMonitor()
    : m_impl(new Impl())
{
#ifdef _WIN32
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    m_impl->processorCount =
        systemInfo.dwNumberOfProcessors > 0 ? static_cast<int>(systemInfo.dwNumberOfProcessors) : 1;

    if (PdhOpenQueryW(nullptr, 0, &m_impl->gpuQuery) == ERROR_SUCCESS)
    {
        const PDH_STATUS addStatus = PdhAddEnglishCounterW(
            m_impl->gpuQuery,
            L"\\GPU Engine(*)\\Utilization Percentage",
            0,
            &m_impl->gpuCounter);
        if (addStatus == ERROR_SUCCESS)
        {
            m_impl->gpuCounterReady = true;
            PdhCollectQueryData(m_impl->gpuQuery);
        }
        else
        {
            PdhCloseQuery(m_impl->gpuQuery);
            m_impl->gpuQuery = nullptr;
            m_impl->gpuCounter = nullptr;
        }
    }
#endif
}

SystemResourcesMonitor::~SystemResourcesMonitor()
{
#ifdef _WIN32
    if (m_impl->gpuQuery != nullptr)
    {
        PdhCloseQuery(m_impl->gpuQuery);
    }
#endif
    delete m_impl;
}

void SystemResourcesMonitor::OnFrame(const double /*deltaTimeSeconds*/)
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX processMemory{};
    processMemory.cb = sizeof(processMemory);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&processMemory),
            sizeof(processMemory)))
    {
        m_snapshot.processWorkingSetBytes = processMemory.WorkingSetSize;
        m_snapshot.processPrivateBytes = processMemory.PrivateUsage;
    }

    MEMORYSTATUSEX systemMemory{};
    systemMemory.dwLength = sizeof(systemMemory);
    if (GlobalMemoryStatusEx(&systemMemory))
    {
        m_snapshot.systemTotalRamBytes = systemMemory.ullTotalPhys;
        m_snapshot.systemUsedRamBytes = systemMemory.ullTotalPhys - systemMemory.ullAvailPhys;
    }

    FILETIME createTime{};
    FILETIME exitTime{};
    FILETIME kernelTime{};
    FILETIME userTime{};
    if (GetProcessTimes(GetCurrentProcess(), &createTime, &exitTime, &kernelTime, &userTime))
    {
        const auto fileTimeToUint64 = [](const FILETIME& fileTime) -> std::uint64_t {
            return (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
        };

        const std::uint64_t processTicks =
            fileTimeToUint64(kernelTime) + fileTimeToUint64(userTime);

        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        if (m_impl->lastWallTicks != 0)
        {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            if (frequency.QuadPart > 0)
            {
                const double wallSeconds =
                    static_cast<double>(counter.QuadPart - m_impl->lastWallTicks)
                    / static_cast<double>(frequency.QuadPart);
                const double processSeconds =
                    static_cast<double>(processTicks - m_impl->lastProcessTicks) / 10000000.0;
                if (wallSeconds > 0.0)
                {
                    const float instantCpu = static_cast<float>(
                        100.0 * processSeconds / wallSeconds
                        / static_cast<double>(m_impl->processorCount));
                    constexpr float kSmoothing = 0.12f;
                    if (m_impl->cpuSampleCount == 0)
                    {
                        m_impl->cpuEma = instantCpu;
                    }
                    else
                    {
                        m_impl->cpuEma =
                            m_impl->cpuEma * (1.0f - kSmoothing) + instantCpu * kSmoothing;
                    }
                    m_snapshot.processCpuPercent = m_impl->cpuEma;
                    ++m_impl->cpuSampleCount;
                }
            }
        }

        m_impl->lastWallTicks = static_cast<std::uint64_t>(counter.QuadPart);
        m_impl->lastProcessTicks = processTicks;
    }

    if (m_impl->gpuCounterReady && m_impl->gpuQuery != nullptr)
    {
        if (PdhCollectQueryData(m_impl->gpuQuery) == ERROR_SUCCESS)
        {
            if (m_impl->gpuCounterPrimed)
            {
                PDH_FMT_COUNTERVALUE counterValue{};
                if (PdhGetFormattedCounterValue(
                        m_impl->gpuCounter,
                        PDH_FMT_DOUBLE,
                        nullptr,
                        &counterValue)
                    == ERROR_SUCCESS
                    && (counterValue.CStatus == PDH_CSTATUS_VALID_DATA
                        || counterValue.CStatus == PDH_CSTATUS_NEW_DATA))
                {
                    m_snapshot.gpuUtilizationPercent =
                        static_cast<float>(counterValue.doubleValue);
                    m_snapshot.gpuUtilizationAvailable = true;
                }
            }
            else
            {
                m_impl->gpuCounterPrimed = true;
            }
        }
    }

    m_snapshot.gpuMemoryValid = false;
    if (GfxContext::Get().IsInitialized())
    {
        const GfxContext::GpuMemoryInfo gpuMemory = GfxContext::Get().QueryGpuMemoryInfo();
        m_snapshot.gpuDedicatedTotalBytes = gpuMemory.dedicatedTotalBytes;
        m_snapshot.gpuLocalUsageBytes = gpuMemory.localUsageBytes;
        m_snapshot.gpuLocalBudgetBytes = gpuMemory.localBudgetBytes;
        m_snapshot.d3d12LocalAllocatedBytes = gpuMemory.d3d12LocalAllocatedBytes;
        m_snapshot.gpuMemoryValid = gpuMemory.valid;
    }
#else
    (void)m_impl;
#endif
}
