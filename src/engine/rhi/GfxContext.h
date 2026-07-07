#pragma once

#include "engine/rendering/TextureSamplerSettings.h"
#include "engine/rhi/GpuProfiler.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;
struct ImDrawData;

class Framebuffer;

namespace D3D12MA
{
class Allocator;
}

// Global D3D12 device, swapchain, and per-frame command recording for the editor.
class GfxContext
{
public:
    static constexpr std::uint32_t FrameCount = 2;
    static constexpr std::uint32_t DrawTextureDescriptorStart = 32;
    static constexpr std::uint32_t DrawTextureSlotsPerTable = 16;

    static GfxContext& Get();

    bool Initialize(GLFWwindow* window, int width, int height);
    void Shutdown();
    bool IsInitialized() const { return m_impl != nullptr; }
    void WaitForGpuIdle();
    // Waits for submitted swapchain frames without stalling on upload/readback fences.
    void WaitForSwapchainFrames(bool pumpWindowEvents = true);

    void Resize(int width, int height);
    void BeginFrame();
    void CancelFrame();
    void EndFrame();
    void SubmitCommandList();

    // Resets the graphics command list to a clean, closed state so it no longer references any GPU
    // objects recorded during the previous frame. Call after WaitForGpuIdle() and before destroying
    // pipelines/resources OUTSIDE the normal BeginFrame path (e.g. the geometry-MSAA reload), so the
    // D3D12 debug layer doesn't fault on releasing an object the stale command list still tracks.
    // No-op while a frame is recording.
    void ResetCommandListForTeardown();

    std::uint32_t AllocateOffscreenSrv();
    void FreeOffscreenSrv(std::uint32_t descriptorIndex);
    void* GetSrvHeapGpuHandle(std::uint32_t descriptorIndex) const;

    // CRASH-01/CRASH-03: deferred destruction. GPU resources and descriptor slots must not be
    // destroyed/recycled while a submitted — or currently recording — command list can still
    // reference them (the GPU dereferences descriptors and memory at execution time, not at
    // record time). These enqueue the release against the fence value that covers all work
    // that could reference the resource; the queue is drained in BeginFrame/WaitForGpuIdle
    // once the fence has completed, and flushed unconditionally in Shutdown after a full wait.
    // Takes ownership of one resource reference (the one returned by D3D12MA::CreateResource,
    // which call sites previously leaked) and of the allocation reference.
    void DeferredReleaseResource(void* d3d12maAllocation, void* d3d12Resource);
    void DeferredFreeOffscreenSrv(std::uint32_t descriptorIndex);
    void DeferredFreeOffscreenRtvBlock(std::uint32_t baseIndex, std::uint32_t count);
    void DeferredFreeOffscreenDsv(std::uint32_t descriptorIndex);

    void* GetDevice() const;
    void* GetCommandList() const;
    void* GetSrvHeap() const;
    D3D12MA::Allocator* GetMemoryAllocator() const;
    std::uint32_t GetSrvDescriptorSize() const;
    std::uintptr_t GetSrvCpuHandle(std::uint32_t descriptorIndex) const;
    std::uint32_t AllocateOffscreenRtvBlock(std::uint32_t count);
    void FreeOffscreenRtvBlock(std::uint32_t baseIndex, std::uint32_t count);
    std::uint32_t AllocateOffscreenDsv();
    void FreeOffscreenDsv(std::uint32_t descriptorIndex);
    std::uintptr_t GetOffscreenRtvCpuHandle(std::uint32_t descriptorIndex) const;
    std::uintptr_t GetOffscreenDsvCpuHandle(std::uint32_t descriptorIndex) const;
    void CreateSrvForTexture(
        void* resource,
        int formatRgba_UNORM,
        std::uint32_t descriptorIndex,
        int width,
        int height,
        std::uint32_t mipLevels = 1) const;
    void CreateMsaaDepthSrv(void* resource, std::uint32_t descriptorIndex) const;
    void ClearOffscreenTarget(
        void* resource,
        std::uint32_t rtvIndex,
        const float clearColor[4],
        int width,
        int height);

    // Re-binds the frame's shader-visible SRV heap on the command list. BeginFrame sets it once, but
    // any external pass that records with its own descriptor heaps and does not restore ours (e.g.
    // Streamline/DLSS slEvaluateFeature, which runs with eDisableCLStateTracking) leaves the heap
    // clobbered — call this immediately afterward so subsequent draws bind descriptors correctly.
    void RebindFrameDescriptorHeaps();

    void BindSwapChainRenderTarget(bool clearColor = false);
    void SetBoundOutputFramebuffer(const Framebuffer* framebuffer);
    const Framebuffer* GetBoundOutputFramebuffer() const;

    void ExecuteImmediate(const std::function<void(void* commandList)>& recordCommands);

    struct TransientUploadAllocation
    {
        std::uint64_t gpuAddress = 0;
        std::uint32_t byteSize = 0;
    };

    TransientUploadAllocation AllocateTransientUpload(const void* data, std::uint32_t byteSize);

    // Per-draw SRV table base index in the global SRV heap (see Shader::FlushUniforms).
    std::uint32_t AllocateDrawSrvTable();
    void ResetDrawSrvTable();

    // Arbitrary-size transient shader-visible descriptor range, valid for the current frame
    // only (the region is reset in BeginFrame). Used by the NRD backend, which creates SRV/UAV
    // descriptors directly into the range per dispatch (no cross-heap copies needed).
    struct TransientDescriptorRange
    {
        std::uint32_t baseIndex = UINT32_MAX;
        std::uintptr_t cpuHandle = 0;   // write descriptors here (CreateShaderResourceView etc.)
        std::uint64_t gpuHandle = 0;    // bind this as the table base
        std::uint32_t descriptorSize = 0;
    };
    TransientDescriptorRange AllocateTransientSrvRange(std::uint32_t count);

    void AllocSrvDescriptorForImGui(void* out_cpu_handle, void* out_gpu_handle);
    void FreeSrvDescriptorFromCpuHandle(std::uintptr_t cpu_handle_ptr);

    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // Active scene-viewport size when an offscreen framebuffer is bound, otherwise swapchain size.
    void GetOutputRenderSize(int& outWidth, int& outHeight) const;

    // Reads a pixel from the swapchain buffer most recently presented by EndFrame().
    bool ReadbackPresentedColorPixel(int x, int y, float outRgba[4]) const;

    static std::string GetLastGpuAllocationError();
    void GetSrvDescriptorUsage(std::uint32_t& outUsed, std::uint32_t& outCapacity) const;
    bool IsDeviceRemoved(std::string* outReason = nullptr) const;

    void SetMaterialTextureFilterMode(TextureFilterMode mode);
    TextureFilterMode GetMaterialTextureFilterMode() const;

    void SetMaterialTextureAnisotropy(std::uint32_t anisotropy);
    std::uint32_t GetMaterialTextureAnisotropy() const;

    void SetMaterialTextureMipBias(float mipBias);
    float GetMaterialTextureMipBias() const;

    // Geometry MSAA (Phase 0: capability probe + active count; resolve pass in later phases).
    int GetActiveMsaaSampleCount() const { return m_activeMsaaSampleCount; }
    void SetActiveMsaaSampleCount(int sampleCount);
    std::uint8_t GetSupportedMsaaSampleCountsMask() const { return m_supportedMsaaSampleCountsMask; }
    bool IsMsaaSampleCountSupported(int sampleCount) const;
    bool IsFrameRecording() const { return m_frameRecording; }

    // DXR capability probe (D3D12_OPTIONS5 RaytracingTier). Tier 0 = not supported.
    bool IsRaytracingSupported() const;
    int GetRaytracingTier() const { return m_raytracingTier; }
    const std::string& GetAdapterDescription() const { return m_adapterDescription; }

    void LogD3D12InfoQueueMessages(const char* context);

    // Per-pass GPU timing (timestamp queries). Scopes are recorded on the active frame command
    // list; results lag by ~1-2 frames (read back after the covering fence). Use GpuTimerScope for
    // RAII pairing. GpuScopeBegin returns -1 when unavailable / budget exhausted.
    int GpuScopeBegin(const char* name);
    void GpuScopeEnd(int scopeId);
    const std::vector<GpuProfiler::Entry>& GetGpuTimings() const { return m_gpuProfiler.GetResults(); }
    float GetGpuTotalMs() const { return m_gpuProfiler.GetTotalMs(); }

    // RAII wrapper around GpuScopeBegin/End on the active frame command list.
    class GpuTimerScope
    {
    public:
        explicit GpuTimerScope(const char* name);
        ~GpuTimerScope();

        GpuTimerScope(const GpuTimerScope&) = delete;
        GpuTimerScope& operator=(const GpuTimerScope&) = delete;

    private:
        int m_scopeId = -1;
    };

private:
    GfxContext() = default;

    void WaitForGpu();
    void ProcessDeferredDestroys(bool flushAll);
    std::uint64_t DeferredDestroyFenceValue() const;
    void WaitForFenceValue(std::uint64_t fenceValue, bool pumpWindowEvents = true);
    std::uint64_t AllocateNextFenceValue(std::uint64_t frameFenceValue) const;
    void SignalFrameSubmission();
    void AdvanceSwapchainFrameIndex();
    void MoveToNextFrame();
    void ProcessPendingResize();
    void ResizeInternal(int width, int height);
    void CreateRenderTargets();
    void ReleaseRenderTargets();
    void RenderImGui(ImDrawData* drawData);

    GLFWwindow* m_window = nullptr;
    int m_width = 0;
    int m_height = 0;

    struct Impl;
    Impl* m_impl = nullptr;

    std::uint32_t m_frameIndex = 0;
    std::uint64_t m_fenceValues[FrameCount] = {};
    // Monotonic fence for immediate uploads; never aliased into swapchain frame fences.
    std::uint64_t m_submissionFenceValue = 0;
    const Framebuffer* m_boundOutputFramebuffer = nullptr;
    bool m_frameRecording = false;
    bool m_frameCommandsSubmitted = false;
    int m_pendingResizeWidth = 0;
    int m_pendingResizeHeight = 0;
    TextureFilterMode m_materialTextureFilterMode = TextureFilterMode::Trilinear;
    std::uint32_t m_materialTextureAnisotropy = 8;
    float m_materialTextureMipBias = 0.0f;
    int m_activeMsaaSampleCount = 1;
    std::uint8_t m_supportedMsaaSampleCountsMask = 0;
    int m_raytracingTier = 0;
    std::string m_adapterDescription;
    GpuProfiler m_gpuProfiler;
};
