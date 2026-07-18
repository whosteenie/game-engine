#include "engine/rhi/context/Detail.h"

void GfxContext::ExecuteImmediate(const std::function<void(void* commandList)>& recordCommands)
{
    if (m_impl == nullptr || !recordCommands)
    {
        return;
    }

    if (m_impl->ImmediateUploadDepth > 0)
    {
        throw std::runtime_error(
            "Nested GPU upload (ExecuteImmediate called while another upload is in progress)");
    }

    if (m_frameRecording)
    {
        throw std::runtime_error(
            "ExecuteImmediate called during active frame recording; upload on the current command list instead");
    }

    struct ImmediateUploadScope
    {
        Impl* impl;
        explicit ImmediateUploadScope(Impl* implIn) : impl(implIn) { ++impl->ImmediateUploadDepth; }
        ~ImmediateUploadScope() { --impl->ImmediateUploadDepth; }
    };
    ImmediateUploadScope uploadScope(m_impl);

    ComPtr<ID3D12CommandAllocator> uploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
    ThrowIfFailed(
        m_impl->Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAllocator)),
        "Create upload command allocator failed");
    ThrowIfFailed(
        m_impl->Device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            uploadAllocator.Get(),
            nullptr,
            IID_PPV_ARGS(&uploadCommandList)),
        "Create upload command list failed");

    recordCommands(uploadCommandList.Get());
    ThrowIfFailed(uploadCommandList->Close(), "Close upload command list failed");

    ID3D12CommandList* commandLists[] = {uploadCommandList.Get()};
    m_impl->CommandQueue->ExecuteCommandLists(1, commandLists);

    std::uint64_t maxFenceValue = m_impl->Fence->GetCompletedValue();
    maxFenceValue = std::max(maxFenceValue, m_submissionFenceValue);
    for (const FrameContext& frameContext : m_impl->Frames)
    {
        maxFenceValue = std::max(maxFenceValue, frameContext.FenceValue);
    }

    const std::uint64_t fenceValue = AllocateNextFenceValue(maxFenceValue);
    ThrowIfFailed(m_impl->CommandQueue->Signal(m_impl->Fence.Get(), fenceValue), "Upload signal failed");
    if (FrameDiagnostics::IsEnabled())
    {
        FrameDiagnostics::Log(
            m_frameRecording
                ? "ExecuteImmediate: upload during active frame recording"
                : "ExecuteImmediate: upload outside frame recording");
    }
    WaitForFenceValue(fenceValue);
    m_submissionFenceValue = fenceValue;
}

void GfxContext::ResetDrawSrvTable()
{
    if (m_impl != nullptr)
    {
        m_impl->DrawSrvTableNextIndex = DrawTextureDescriptorStart;
    }
}

std::uint32_t GfxContext::AllocateDrawSrvTable()
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return UINT32_MAX;
    }

    const std::uint32_t tableStart = m_impl->DrawSrvTableNextIndex;
    const std::uint32_t nextIndex = tableStart + DrawTextureSlotsPerTable;
    if (nextIndex > OffscreenSrvDescriptorStart)
    {
        GfxContextDetail::SetGpuAllocationError("Draw SRV descriptor table exhausted");
        return UINT32_MAX;
    }

    m_impl->DrawSrvTableNextIndex = nextIndex;
    return tableStart;
}

GfxContext::TransientDescriptorRange GfxContext::AllocateTransientSrvRange(const std::uint32_t count)
{
    TransientDescriptorRange range{};
    if (m_impl == nullptr || count == 0)
    {
        return range;
    }

    const std::uint32_t baseIndex = m_impl->DrawSrvTableNextIndex;
    const std::uint32_t nextIndex = baseIndex + count;
    if (nextIndex > OffscreenSrvDescriptorStart)
    {
        GfxContextDetail::SetGpuAllocationError("Transient SRV descriptor region exhausted");
        return range;
    }

    m_impl->DrawSrvTableNextIndex = nextIndex;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = m_impl->SrvHeap->GetGPUDescriptorHandleForHeapStart();
    range.baseIndex = baseIndex;
    range.descriptorSize = m_impl->SrvDescriptorSize;
    range.cpuHandle = cpuStart.ptr + static_cast<SIZE_T>(baseIndex) * m_impl->SrvDescriptorSize;
    range.gpuHandle = gpuStart.ptr + static_cast<UINT64>(baseIndex) * m_impl->SrvDescriptorSize;
    return range;
}

GfxContext::TransientUploadAllocation GfxContext::AllocateTransientUpload(
    const void* data,
    const std::uint32_t byteSize)
{
    TransientUploadAllocation allocation{};
    if (m_impl == nullptr || data == nullptr || byteSize == 0)
    {
        return allocation;
    }

    TransientUploadArena& arena = m_impl->TransientUploadArenas[m_frameIndex];
    if (arena.Resource == nullptr || arena.Mapped == nullptr)
    {
        EngineDiagnostics::SetLastGpuAllocationError("Transient upload arena is not initialized");
        return allocation;
    }

    const std::uint32_t alignedSize = (byteSize + 255u) & ~255u;
    if (arena.Offset + alignedSize > TransientUploadCapacityBytes)
    {
        EngineDiagnostics::SetLastGpuAllocationError("Transient upload arena exhausted");
        return allocation;
    }

    std::memcpy(static_cast<std::uint8_t*>(arena.Mapped) + arena.Offset, data, byteSize);
    allocation.gpuAddress =
        arena.Resource->GetGPUVirtualAddress() + static_cast<std::uint64_t>(arena.Offset);
    allocation.byteSize = byteSize;
    arena.Offset += alignedSize;
    return allocation;
}

void GfxContext::WaitForFenceValue(const std::uint64_t fenceValue, const bool pumpWindowEvents)
{
    if (m_impl == nullptr || fenceValue == 0)
    {
        return;
    }

    if (m_impl->Fence->GetCompletedValue() >= fenceValue)
    {
        return;
    }

    // Register the completion notification exactly ONCE. The previous code re-registered on every
    // loop iteration, which queues a separate notification per iteration for the same value. When
    // the fence completes they ALL fire: our WaitForSingleObject consumes one (auto-reset event),
    // but the leftovers re-signal the event with no waiter, leaving it stuck signaled. The next
    // WaitForFenceValue (for a higher, still-pending value) then returns WAIT_OBJECT_0 immediately
    // and the caller proceeds while the GPU is still running — freeing/resetting PSOs and command
    // allocators mid-flight (the "referenced by GPU operations in-flight" CORRUPTION and the
    // "allocator reset before previous executions completed" errors). It only triggered when a wait
    // looped more than once (op > ~16ms, e.g. IBL cubemap generation), hence the intermittency.
    {
        const HRESULT setEventResult =
            m_impl->Fence->SetEventOnCompletion(fenceValue, m_impl->FenceEvent);
        if (FAILED(setEventResult))
        {
            std::string deviceRemovedReason;
            if (IsDeviceRemoved(&deviceRemovedReason))
            {
                throw std::runtime_error(
                    "D3D12 device removed while waiting for fence: " + deviceRemovedReason);
            }

            ThrowIfFailed(setEventResult, "SetEventOnCompletion failed");
        }
    }

    const auto waitStart = std::chrono::steady_clock::now();
    long long lastLoggedMs = -1;

    while (m_impl->Fence->GetCompletedValue() < fenceValue)
    {
        if (pumpWindowEvents)
        {
            PumpWindowEvents(m_window);
        }

        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - waitStart)
                                   .count();
        if (FrameDiagnostics::IsEnabled() && elapsedMs >= 500 && elapsedMs / 500 != lastLoggedMs / 500)
        {
            lastLoggedMs = elapsedMs;
            FrameDiagnostics::LogFenceWait(
                fenceValue,
                m_impl->Fence->GetCompletedValue(),
                elapsedMs);
        }

        std::string deviceRemovedReason;
        if (IsDeviceRemoved(&deviceRemovedReason))
        {
            throw std::runtime_error("D3D12 device removed while waiting for fence: " + deviceRemovedReason);
        }

        // Poll with a short timeout so we can keep pumping window events / checking device-removed,
        // but do NOT re-register the completion notification (see above). The while-condition on
        // GetCompletedValue() remains the source of truth, so a spurious wake just re-checks.
        const DWORD waitResult = WaitForSingleObject(m_impl->FenceEvent, pumpWindowEvents ? 16 : INFINITE);
        if (waitResult == WAIT_OBJECT_0 && m_impl->Fence->GetCompletedValue() >= fenceValue)
        {
            break;
        }

        // A close request must not weaken the fence guarantee. Shutdown destroys PSOs and
        // resources immediately after this wait, so returning before the requested value has
        // completed would release objects that the GPU can still be using.
    }
}

bool GfxContext::IsDeviceRemoved(std::string* outReason) const
{
    if (m_impl == nullptr || m_impl->Device == nullptr)
    {
        return false;
    }

    const HRESULT removedReason = m_impl->Device->GetDeviceRemovedReason();
    if (removedReason == S_OK)
    {
        return false;
    }

    if (outReason != nullptr)
    {
        *outReason = "HRESULT=" + HresultFormat::Format(removedReason);
    }

    return true;
}

void GfxContext::WaitForGpuIdle()
{
    WaitForGpu();
    WaitForImGuiUploadQueueIdle();
    ProcessDeferredDestroys(false);
}

void GfxContext::WaitForSwapchainFrames(const bool pumpWindowEvents)
{
    if (m_impl == nullptr)
    {
        return;
    }

    std::uint64_t maxFenceValue = m_submissionFenceValue;
    for (const FrameContext& frame : m_impl->Frames)
    {
        maxFenceValue = std::max(maxFenceValue, frame.FenceValue);
    }

    FrameDiagnostics::LogPhase("WaitForSwapchainFrames");
    WaitForFenceValue(maxFenceValue, pumpWindowEvents);
}

void GfxContext::WaitForGpu()
{
    if (m_impl == nullptr)
    {
        return;
    }

    std::uint64_t maxFenceValue = 0;
    for (const FrameContext& frame : m_impl->Frames)
    {
        maxFenceValue = std::max(maxFenceValue, frame.FenceValue);
    }

    maxFenceValue = std::max(maxFenceValue, m_submissionFenceValue);

    for (const std::uint64_t fenceValue : m_fenceValues)
    {
        maxFenceValue = std::max(maxFenceValue, fenceValue);
    }

    WaitForFenceValue(maxFenceValue);
}

void GfxContext::SignalFrameSubmission()
{
    FrameContext& frame = m_impl->Frames[m_frameIndex];
    const std::uint64_t nextFenceValue = AllocateNextFenceValue(frame.FenceValue);
    ThrowIfFailed(m_impl->CommandQueue->Signal(m_impl->Fence.Get(), nextFenceValue), "Signal failed");
    frame.FenceValue = nextFenceValue;
    m_fenceValues[m_frameIndex] = nextFenceValue;
    m_submissionFenceValue = std::max(m_submissionFenceValue, nextFenceValue);
}

void GfxContext::AdvanceSwapchainFrameIndex()
{
    m_frameIndex = m_impl->SwapChain->GetCurrentBackBufferIndex();
}

void GfxContext::MoveToNextFrame()
{
    SignalFrameSubmission();
    AdvanceSwapchainFrameIndex();
}

std::uint64_t GfxContext::AllocateNextFenceValue(const std::uint64_t frameFenceValue) const
{
    std::uint64_t nextFenceValue = frameFenceValue + 1;
    nextFenceValue = std::max(nextFenceValue, m_submissionFenceValue + 1);
    if (m_impl != nullptr && m_impl->Fence != nullptr)
    {
        nextFenceValue = std::max(nextFenceValue, m_impl->Fence->GetCompletedValue() + 1);
    }

    return nextFenceValue;
}

