#pragma once

#include <cstdint>
#include <functional>
#include <string>

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
    void WaitForSwapchainFrames();

    void Resize(int width, int height);
    void BeginFrame();
    void CancelFrame();
    void EndFrame();
    void SubmitCommandList();

    std::uint32_t AllocateOffscreenSrv();
    void FreeOffscreenSrv(std::uint32_t descriptorIndex);
    void* GetSrvHeapGpuHandle(std::uint32_t descriptorIndex) const;

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
        int height) const;
    void ClearOffscreenTarget(
        void* resource,
        std::uint32_t rtvIndex,
        const float clearColor[4],
        int width,
        int height);

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

private:
    GfxContext() = default;

    void WaitForGpu();
    void WaitForFenceValue(std::uint64_t fenceValue);
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
};
