#include "engine/rhi/GpuProfiler.h"

#include "engine/platform/diagnostics/EngineLog.h"

#include <d3d12.h>

#include <cstring>

bool GpuProfiler::Initialize(
    ID3D12Device* device,
    const std::uint32_t frameCount,
    const std::uint64_t timestampFrequency)
{
    if (device == nullptr || frameCount == 0 || timestampFrequency == 0)
    {
        return false;
    }

    m_frameCount = frameCount;
    m_timestampFrequency = timestampFrequency;
    m_slices.assign(frameCount, Slice{});

    const std::uint32_t totalQueries = frameCount * kQueriesPerFrame;

    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = totalQueries;
    heapDesc.NodeMask = 0;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&m_queryHeap))))
    {
        EngineLog::Warn("gpu-profiler", "CreateQueryHeap(TIMESTAMP) failed; GPU timings disabled");
        m_queryHeap = nullptr;
        return false;
    }

    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = static_cast<std::uint64_t>(totalQueries) * sizeof(std::uint64_t);
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateCommittedResource(
            &readbackHeap,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_readback))))
    {
        EngineLog::Warn("gpu-profiler", "Readback buffer creation failed; GPU timings disabled");
        Shutdown();
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readEverything{0, static_cast<SIZE_T>(bufferDesc.Width)};
    if (FAILED(m_readback->Map(0, &readEverything, &mapped)))
    {
        EngineLog::Warn("gpu-profiler", "Readback map failed; GPU timings disabled");
        Shutdown();
        return false;
    }
    m_mapped = static_cast<const std::uint64_t*>(mapped);

    EngineLog::Info("gpu-profiler", "GPU timestamp profiler ready");
    return true;
}

void GpuProfiler::Shutdown()
{
    if (m_readback != nullptr)
    {
        m_readback->Unmap(0, nullptr);
        m_readback->Release();
        m_readback = nullptr;
    }
    if (m_queryHeap != nullptr)
    {
        m_queryHeap->Release();
        m_queryHeap = nullptr;
    }
    m_mapped = nullptr;
    m_slices.clear();
    m_results.clear();
    m_totalMs = 0.0f;
}

void GpuProfiler::BeginFrame(const std::uint32_t frameIndex)
{
    if (m_queryHeap == nullptr || frameIndex >= m_frameCount)
    {
        return;
    }

    m_currentFrame = frameIndex;
    Slice& slice = m_slices[frameIndex];

    // The slice's covering fence has completed before BeginFrame is reached, so the readback for
    // the last submission that used this slice is valid to read now.
    if (slice.resolved && m_mapped != nullptr && m_timestampFrequency != 0)
    {
        m_results.clear();
        m_totalMs = 0.0f;
        const std::uint32_t base = QueryBase(frameIndex);
        const double toMs = 1000.0 / static_cast<double>(m_timestampFrequency);
        for (std::size_t scope = 0; scope < slice.names.size(); ++scope)
        {
            const std::uint64_t begin = m_mapped[base + scope * 2];
            const std::uint64_t end = m_mapped[base + scope * 2 + 1];
            const float ms = end > begin ? static_cast<float>(static_cast<double>(end - begin) * toMs) : 0.0f;
            m_results.push_back(Entry{slice.names[scope], ms});
            m_totalMs += ms;
        }
    }

    slice.names.clear();
    slice.resolved = false;
}

int GpuProfiler::BeginScope(ID3D12GraphicsCommandList* commandList, const char* name)
{
    if (m_queryHeap == nullptr || commandList == nullptr)
    {
        return -1;
    }

    Slice& slice = m_slices[m_currentFrame];
    if (slice.names.size() >= kMaxScopesPerFrame)
    {
        return -1;
    }

    const int scopeId = static_cast<int>(slice.names.size());
    const std::uint32_t queryIndex = QueryBase(m_currentFrame) + static_cast<std::uint32_t>(scopeId) * 2;
    commandList->EndQuery(m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
    slice.names.emplace_back(name != nullptr ? name : "scope");
    return scopeId;
}

void GpuProfiler::EndScope(ID3D12GraphicsCommandList* commandList, const int scopeId)
{
    if (m_queryHeap == nullptr || commandList == nullptr || scopeId < 0)
    {
        return;
    }

    const std::uint32_t queryIndex =
        QueryBase(m_currentFrame) + static_cast<std::uint32_t>(scopeId) * 2 + 1;
    commandList->EndQuery(m_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, queryIndex);
}

void GpuProfiler::Resolve(ID3D12GraphicsCommandList* commandList, const std::uint32_t frameIndex)
{
    if (m_queryHeap == nullptr || commandList == nullptr || frameIndex >= m_frameCount)
    {
        return;
    }

    Slice& slice = m_slices[frameIndex];
    const std::uint32_t scopeCount = static_cast<std::uint32_t>(slice.names.size());
    if (scopeCount == 0)
    {
        slice.resolved = false;
        return;
    }

    const std::uint32_t base = QueryBase(frameIndex);
    commandList->ResolveQueryData(
        m_queryHeap,
        D3D12_QUERY_TYPE_TIMESTAMP,
        base,
        scopeCount * 2,
        m_readback,
        static_cast<std::uint64_t>(base) * sizeof(std::uint64_t));
    slice.resolved = true;
}
