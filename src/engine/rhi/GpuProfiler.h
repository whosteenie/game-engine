#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12QueryHeap;
struct ID3D12Resource;

// Per-pass GPU timing via D3D12 timestamp queries. Double-buffered against the swapchain frame
// count so results are read back only after the covering fence has completed (no GPU stall): the
// numbers shown come from the last submission that used the recycled slice (~1-2 frames latency,
// which is invisible for a rolling perf readout).
//
// Usage per frame: BeginFrame -> [BeginScope/EndScope]* -> Resolve (once, right before the
// command list carrying the scopes is closed).
class GpuProfiler
{
public:
    struct Entry
    {
        std::string name;
        float milliseconds = 0.0f;
    };

    bool Initialize(ID3D12Device* device, std::uint32_t frameCount, std::uint64_t timestampFrequency);
    void Shutdown();
    bool IsReady() const { return m_queryHeap != nullptr; }

    // Start-of-frame: compute results from the previous submission that used this slice (its fence
    // has already completed) and reset the slice for this frame's scopes.
    void BeginFrame(std::uint32_t frameIndex);

    // Opens a timed GPU scope on the command list; returns a scope id for EndScope, or -1 when the
    // per-frame scope budget is exhausted or the profiler is unavailable.
    int BeginScope(ID3D12GraphicsCommandList* commandList, const char* name);
    void EndScope(ID3D12GraphicsCommandList* commandList, int scopeId);

    // Resolves this frame's queries into the readback buffer. Call exactly once, immediately before
    // the command list carrying the scopes is closed.
    void Resolve(ID3D12GraphicsCommandList* commandList, std::uint32_t frameIndex);

    const std::vector<Entry>& GetResults() const { return m_results; }
    float GetTotalMs() const { return m_totalMs; }

private:
    static constexpr std::uint32_t kMaxScopesPerFrame = 48;
    static constexpr std::uint32_t kQueriesPerFrame = kMaxScopesPerFrame * 2;

    struct Slice
    {
        std::vector<std::string> names;
        bool resolved = false;
    };

    std::uint32_t QueryBase(std::uint32_t frameIndex) const { return frameIndex * kQueriesPerFrame; }

    ID3D12QueryHeap* m_queryHeap = nullptr;
    ID3D12Resource* m_readback = nullptr;
    const std::uint64_t* m_mapped = nullptr;
    std::uint64_t m_timestampFrequency = 0;
    std::uint32_t m_frameCount = 0;
    std::uint32_t m_currentFrame = 0;
    std::vector<Slice> m_slices;
    std::vector<Entry> m_results;
    float m_totalMs = 0.0f;
};
