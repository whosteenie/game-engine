#include "engine/rhi/context/Detail.h"

void GfxContext::TryDeferredStreamlineSwapChainUpgrade()
{
    EnsureStreamlineSwapChainUpgraded();
}

void GfxContext::EnsureStreamlineSwapChainUpgraded()
{
    if (m_impl == nullptr || m_impl->SwapChain == nullptr)
    {
        return;
    }

    if (!DlssContext::Get().IsRuntimeInitialized())
    {
        return;
    }

    void* swapChain = m_impl->SwapChain.Get();
    void* const before = swapChain;
    if (!DlssContext::Get().UpgradeSwapChain(&swapChain))
    {
        return;
    }

    if (swapChain == before)
    {
        return;
    }

    ComPtr<IDXGISwapChain3> upgraded;
    ThrowIfFailed(
        static_cast<IDXGISwapChain*>(swapChain)->QueryInterface(IID_PPV_ARGS(&upgraded)),
        "IDXGISwapChain3 QI after Streamline swapchain upgrade failed");
    m_impl->SwapChain = upgraded;
}

void GfxContext::ResizeInternal(const int width, const int height)
{
    WaitForGpu();
    EnsureStreamlineSwapChainUpgraded();
    ReleaseRenderTargets();

    ThrowIfFailed(
        m_impl->SwapChain->ResizeBuffers(FrameCount, width, height, DXGI_FORMAT_UNKNOWN, 0),
        "ResizeBuffers failed");
    m_width = width;
    m_height = height;

    for (FrameContext& frame : m_impl->Frames)
    {
        frame.FenceValue = 0;
    }

    m_fenceValues[0] = 0;
    m_fenceValues[1] = 0;
    m_submissionFenceValue = 0;
    m_frameRecording = false;
    m_frameCommandsSubmitted = false;

    CreateRenderTargets();
}

void GfxContext::ProcessPendingResize()
{
    if (m_impl == nullptr || m_frameRecording)
    {
        return;
    }

    if (m_pendingResizeWidth <= 0 || m_pendingResizeHeight <= 0)
    {
        return;
    }

    const int width = m_pendingResizeWidth;
    const int height = m_pendingResizeHeight;
    m_pendingResizeWidth = 0;
    m_pendingResizeHeight = 0;

    if (width == m_width && height == m_height)
    {
        return;
    }

    ResizeInternal(width, height);
}

void GfxContext::Resize(int width, int height)
{
    if (m_impl == nullptr || width <= 0 || height <= 0)
    {
        return;
    }

    if (width == m_width && height == m_height)
    {
        m_pendingResizeWidth = 0;
        m_pendingResizeHeight = 0;
        return;
    }

    // Always defer swapchain resize to BeginFrame so rapid window drags coalesce into one
    // WaitForGpu + ResizeBuffers instead of stalling on every GLFW size callback.
    m_pendingResizeWidth = width;
    m_pendingResizeHeight = height;
}

void GfxContext::BeginFrame()
{
    if (m_impl == nullptr || m_width <= 0 || m_height <= 0)
    {
        return;
    }

    std::string deviceRemovedReason;
    if (IsDeviceRemoved(&deviceRemovedReason))
    {
        throw std::runtime_error("D3D12 device removed before frame begin: " + deviceRemovedReason);
    }

    if (m_frameRecording)
    {
        CancelFrame();
    }

    ProcessPendingResize();
    ++m_submissionFrameNumber;
    FrameContext& frame = m_impl->Frames[m_frameIndex];
    FrameDiagnostics::LogPhase("BeginFrame-wait");
    const std::uint64_t frameWaitFence =
        std::max({frame.FenceValue, m_submissionFenceValue, m_fenceValues[m_frameIndex]});
    const auto fenceWaitStart = std::chrono::steady_clock::now();
    WaitForFenceValue(frameWaitFence);
    m_framePacingDiagnostics.previousFrameFenceWaitMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fenceWaitStart).count();

    // Drain deferred destroys whose covering fence has completed (CRASH-01/CRASH-03).
    ProcessDeferredDestroys(false);

    // This slice's fence has completed, so the previous submission's timestamps are readable now.
    m_gpuProfiler.BeginFrame(m_frameIndex);

    const HRESULT allocatorResetResult = frame.CommandAllocator->Reset();
    if (FAILED(allocatorResetResult))
    {
        std::string message = "CommandAllocator reset failed (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(allocatorResetResult)) + ")";
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            message += "; device removed: " + deviceRemovedReason;
        }

        throw std::runtime_error(message);
    }

    const HRESULT commandListResetResult =
        m_impl->CommandList->Reset(frame.CommandAllocator.Get(), nullptr);
    if (FAILED(commandListResetResult))
    {
        std::string message = "CommandList reset failed (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(commandListResetResult)) + ")";
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            message += "; device removed: " + deviceRemovedReason;
        }

        throw std::runtime_error(message);
    }

    m_frameRecording = true;
    m_frameGpuScopeId = GpuScopeBegin("Frame GPU span");
    m_impl->TransientUploadArenas[m_frameIndex].Offset = 0;
    m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
    EngineDiagnostics::ClearLastGpuAllocationError();
    m_frameCommandsSubmitted = false;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    m_impl->CommandList->ResourceBarrier(1, &barrier);

    const float clearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
    m_impl->CommandList->ClearRenderTargetView(frame.RtvCpuHandle, clearColor, 0, nullptr);
    m_impl->CommandList->OMSetRenderTargets(1, &frame.RtvCpuHandle, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    m_impl->CommandList->RSSetViewports(1, &viewport);
    m_impl->CommandList->RSSetScissorRects(1, &scissor);

    ID3D12DescriptorHeap* heaps[] = {m_impl->SrvHeap.Get()};
    m_impl->CommandList->SetDescriptorHeaps(1, heaps);
}

void GfxContext::CancelFrame()
{
    if (m_impl == nullptr || !m_frameRecording)
    {
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();
    if (!m_frameCommandsSubmitted && commandList != nullptr)
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.RenderTarget.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &barrier);

        const HRESULT closeResult = commandList->Close();
        if (SUCCEEDED(closeResult))
        {
            ID3D12CommandList* commandLists[] = {commandList};
            m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);
            SignalFrameSubmission();
            m_frameCommandsSubmitted = true;
        }
    }

    if (m_frameCommandsSubmitted)
    {
        WaitForFenceValue(frame.FenceValue);
    }

    m_frameRecording = false;
    m_frameCommandsSubmitted = false;
    m_frameGpuScopeId = -1;
    m_boundOutputFramebuffer = nullptr;
    m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
}

void GfxContext::ResetCommandListForTeardown()
{
    if (m_impl == nullptr || m_frameRecording)
    {
        return;
    }

    // The command list is closed here (post-EndFrame) and still tracks the objects it recorded last
    // frame. Reset the allocator + list and immediately close them: this drops those references so
    // releasing pipelines/resources next won't trip the debug layer, and leaves the list closed as
    // BeginFrame expects. Safe only because callers WaitForGpuIdle() first.
    FrameContext& frame = m_impl->Frames[m_frameIndex];
    if (FAILED(frame.CommandAllocator->Reset()))
    {
        return;
    }
    if (FAILED(m_impl->CommandList->Reset(frame.CommandAllocator.Get(), nullptr)))
    {
        return;
    }
    m_impl->CommandList->Close();
    m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
}

void GfxContext::SubmitCommandList()
{
    if (m_impl == nullptr)
    {
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();

    // BeginFrame() transitions the swapchain buffer to RENDER_TARGET. Offscreen-only
    // direct-submit paths must restore PRESENT before the next BeginFrame().
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &barrier);

    GpuScopeEnd(m_frameGpuScopeId);
    m_frameGpuScopeId = -1;
    m_gpuProfiler.Resolve(commandList, m_frameIndex);

    ThrowIfFailed(m_impl->CommandList->Close(), "CommandList close failed");

    ID3D12CommandList* commandLists[] = {m_impl->CommandList.Get()};
    m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);
    SignalFrameSubmission();
    m_frameCommandsSubmitted = true;
    m_frameRecording = false;
    AdvanceSwapchainFrameIndex();
}

void GfxContext::EndFrame()
{
    if (m_impl == nullptr)
    {
        return;
    }

    if (!m_frameRecording || m_width <= 0 || m_height <= 0)
    {
        CancelFrame();
        return;
    }

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    auto* commandList = m_impl->CommandList.Get();

    BindSwapChainRenderTarget(false);

    const int uiScopeId = m_gpuProfiler.BeginScope(commandList, "UI (ImGui)");
    if (ImDrawData* drawData = ImGui::GetDrawData())
    {
        RenderImGui(drawData);
    }
    m_gpuProfiler.EndScope(commandList, uiScopeId);

    if (m_impl->PresentedImageCapture.Requested)
    {
        GpuTimerScope captureScope("S0-P5/Capture final output");
        RecordPresentedImageCapture(commandList, frame.RenderTarget.Get());
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = frame.RenderTarget.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commandList->ResourceBarrier(1, &barrier);

    GpuScopeEnd(m_frameGpuScopeId);
    m_frameGpuScopeId = -1;
    m_gpuProfiler.Resolve(commandList, m_frameIndex);

    ThrowIfFailed(m_impl->CommandList->Close(), "CommandList close failed");

    ID3D12CommandList* commandLists[] = {m_impl->CommandList.Get()};
    m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);
    m_frameCommandsSubmitted = true;

    PumpWindowEvents(m_window);

    const auto presentStart = std::chrono::steady_clock::now();
    const HRESULT presentResult = m_impl->SwapChain->Present(m_vsyncEnabled ? 1u : 0u, 0);
    m_framePacingDiagnostics.presentCallMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - presentStart).count();

    // Signal AFTER Present so this frame's fence covers the present's GPU work, not just the
    // command list. Present() enqueues its flip work on the command queue *after* the command
    // list; signalling before it (the previous order) left the back buffer referenced by an
    // in-flight present that WaitForGpu() never waited on. That untracked present is exactly why
    // releasing the swapchain at shutdown faulted the debug layer with "resource ... referenced
    // by GPU operations in-flight". m_frameIndex still refers to the frame we just rendered here
    // (AdvanceSwapchainFrameIndex() runs below), so the fence value is attributed correctly.
    SignalFrameSubmission();
    if (m_impl->PresentedImageCapture.Submitted
        && m_impl->PresentedImageCapture.FenceValue == 0)
    {
        m_impl->PresentedImageCapture.FenceValue = frame.FenceValue;
    }

    m_frameRecording = false;
    m_boundOutputFramebuffer = nullptr;

    if (FAILED(presentResult))
    {
        std::string message =
            "Present failed (HRESULT=" + HresultFormat::Format(presentResult) + ")";
        std::string deviceRemovedReason;
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            message += "; device removed: " + deviceRemovedReason;
            LogD3D12InfoQueueMessages("present-device-removed");
        }
        GfxContextDetail::SetGpuAllocationError(message.c_str());
        EngineLog::Error("gfx", message);
        m_frameCommandsSubmitted = false;
        throw std::runtime_error(message);
    }

    m_frameCommandsSubmitted = false;
    AdvanceSwapchainFrameIndex();
    ProcessPendingResize();
}

void* GfxContext::GetDevice() const
{
    return m_impl != nullptr ? m_impl->Device.Get() : nullptr;
}

void* GfxContext::GetCommandList() const
{
    return m_impl != nullptr ? m_impl->CommandList.Get() : nullptr;
}

int GfxContext::GpuScopeBegin(const char* name)
{
    if (m_impl == nullptr || !m_frameRecording)
    {
        return -1;
    }
    return m_gpuProfiler.BeginScope(m_impl->CommandList.Get(), name);
}

void GfxContext::GpuScopeEnd(const int scopeId)
{
    if (m_impl == nullptr || scopeId < 0)
    {
        return;
    }
    m_gpuProfiler.EndScope(m_impl->CommandList.Get(), scopeId);
}

GfxContext::GpuTimerScope::GpuTimerScope(const char* name)
    : m_scopeId(GfxContext::Get().GpuScopeBegin(name))
{
}

GfxContext::GpuTimerScope::~GpuTimerScope()
{
    if (m_scopeId >= 0)
    {
        GfxContext::Get().GpuScopeEnd(m_scopeId);
    }
}

void* GfxContext::GetSrvHeap() const
{
    return m_impl != nullptr ? m_impl->SrvHeap.Get() : nullptr;
}

D3D12MA::Allocator* GfxContext::GetMemoryAllocator() const
{
    return m_impl != nullptr ? m_impl->MemoryAllocator : nullptr;
}

GfxContext::GpuMemoryInfo GfxContext::QueryGpuMemoryInfo() const
{
    GpuMemoryInfo info{};
    info.dedicatedTotalBytes = m_adapterDedicatedVideoMemory;
    if (m_impl == nullptr || m_impl->MemoryAllocator == nullptr)
    {
        return info;
    }

    D3D12MA::Budget localBudget{};
    D3D12MA::Budget nonLocalBudget{};
    m_impl->MemoryAllocator->GetBudget(&localBudget, &nonLocalBudget);
    info.localUsageBytes = localBudget.UsageBytes;
    info.localBudgetBytes = localBudget.BudgetBytes;
    info.d3d12LocalAllocatedBytes = localBudget.Stats.AllocationBytes;
    info.d3d12LocalBlockBytes = localBudget.Stats.BlockBytes;
    info.valid = true;
    return info;
}

std::uint32_t GfxContext::GetSrvDescriptorSize() const
{
    return m_impl != nullptr ? m_impl->SrvDescriptorSize : 0;
}

std::uintptr_t GfxContext::GetSrvCpuHandle(const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->SrvAllocator.IsValidIndex(descriptorIndex))
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->SrvDescriptorSize;
    return handle.ptr;
}

std::uintptr_t GfxContext::GetSrvCopySourceCpuHandle(const std::uintptr_t srvCpuHandle) const
{
    if (srvCpuHandle == 0 || m_impl == nullptr || m_impl->SrvDescriptorSize == 0
        || m_impl->SrvCpuHeap == nullptr)
    {
        return 0;
    }

    const SIZE_T visibleStart = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T visibleEnd = visibleStart + static_cast<SIZE_T>(SrvDescriptorCount)
        * static_cast<SIZE_T>(m_impl->SrvDescriptorSize);
    const SIZE_T copyStart = m_impl->SrvCpuHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T copyEnd = copyStart + static_cast<SIZE_T>(SrvDescriptorCount)
        * static_cast<SIZE_T>(m_impl->SrvDescriptorSize);

    if (srvCpuHandle >= copyStart && srvCpuHandle < copyEnd)
    {
        const SIZE_T offset = srvCpuHandle - copyStart;
        if (offset % static_cast<SIZE_T>(m_impl->SrvDescriptorSize) != 0)
        {
            return 0;
        }
        return srvCpuHandle;
    }

    if (srvCpuHandle < visibleStart || srvCpuHandle >= visibleEnd)
    {
        return 0;
    }

    const SIZE_T offset = srvCpuHandle - visibleStart;
    if (offset % static_cast<SIZE_T>(m_impl->SrvDescriptorSize) != 0)
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE copyHandle = m_impl->SrvCpuHeap->GetCPUDescriptorHandleForHeapStart();
    copyHandle.ptr += offset;
    return copyHandle.ptr;
}

