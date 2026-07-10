#include "engine/platform/SystemResources.h"

#include "engine/rhi/GfxContext.h"

#include <algorithm>
#include <cmath>
#include <vector>

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

namespace
{
#ifdef _WIN32
    bool ReadPdhCounterMaxPercent(PDH_HCOUNTER counter, float& outMaxPercent)
    {
        DWORD bufferSize = 0;
        DWORD itemCount = 0;
        PDH_STATUS status = PdhGetFormattedCounterArrayW(
            counter,
            PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
            &bufferSize,
            &itemCount,
            nullptr);
        if (status != PDH_MORE_DATA)
        {
            PDH_FMT_COUNTERVALUE singleValue{};
            status = PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &singleValue);
            if (status != ERROR_SUCCESS
                || (singleValue.CStatus != PDH_CSTATUS_VALID_DATA
                    && singleValue.CStatus != PDH_CSTATUS_NEW_DATA))
            {
                return false;
            }

            outMaxPercent = static_cast<float>(singleValue.doubleValue);
            return true;
        }

        std::vector<std::uint8_t> buffer(bufferSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        status = PdhGetFormattedCounterArrayW(
            counter,
            PDH_FMT_DOUBLE | PDH_FMT_NOCAP100,
            &bufferSize,
            &itemCount,
            items);
        if (status != ERROR_SUCCESS)
        {
            return false;
        }

        double maxPercent = 0.0;
        bool found = false;
        for (DWORD index = 0; index < itemCount; ++index)
        {
            const PDH_FMT_COUNTERVALUE& value = items[index].FmtValue;
            if (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA)
            {
                continue;
            }

            maxPercent = std::max(maxPercent, value.doubleValue);
            found = true;
        }

        if (!found)
        {
            return false;
        }

        outMaxPercent = static_cast<float>(maxPercent);
        return true;
    }

    bool TryAddGpuUtilizationCounter(PDH_HQUERY query, PDH_HCOUNTER* outCounter)
    {
        static const wchar_t* kCounterPaths[] = {
            L"\\GPU Engine(*engtype_3D*)\\Utilization Percentage",
            L"\\GPU Engine(*)\\Utilization Percentage",
        };

        for (const wchar_t* path : kCounterPaths)
        {
            if (PdhAddEnglishCounterW(query, path, 0, outCounter) == ERROR_SUCCESS)
            {
                return true;
            }
        }

        return false;
    }
#endif
}

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
    float gpuSystemEma = 0.0f;
    int gpuSystemSampleCount = 0;
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
        if (TryAddGpuUtilizationCounter(m_impl->gpuQuery, &m_impl->gpuCounter))
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

void SystemResourcesMonitor::OnFrame(const double deltaTimeSeconds)
{
    m_snapshot.gpuSystemUtilizationAvailable = false;
    m_snapshot.gpuSystemUtilizationPercent = -1.0f;
    m_snapshot.gpuInstrumentedFramePercent = -1.0f;

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
                float maxPercent = 0.0f;
                if (ReadPdhCounterMaxPercent(m_impl->gpuCounter, maxPercent))
                {
                    constexpr float kSmoothing = 0.15f;
                    if (m_impl->gpuSystemSampleCount == 0)
                    {
                        m_impl->gpuSystemEma = maxPercent;
                    }
                    else
                    {
                        m_impl->gpuSystemEma =
                            m_impl->gpuSystemEma * (1.0f - kSmoothing) + maxPercent * kSmoothing;
                    }
                    ++m_impl->gpuSystemSampleCount;
                    m_snapshot.gpuSystemUtilizationPercent = m_impl->gpuSystemEma;
                    m_snapshot.gpuSystemUtilizationAvailable = true;
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

        const float gpuTotalMs = GfxContext::Get().GetGpuTotalMs();
        if (deltaTimeSeconds > 0.0 && gpuTotalMs > 0.0f)
        {
            m_snapshot.gpuInstrumentedFramePercent = std::clamp(
                gpuTotalMs / static_cast<float>(deltaTimeSeconds * 1000.0) * 100.0f,
                0.0f,
                100.0f);
        }
    }
#else
    (void)deltaTimeSeconds;
    (void)m_impl;
#endif
}
