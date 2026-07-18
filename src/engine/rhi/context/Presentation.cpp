#include "engine/rhi/context/Detail.h"

void GfxContext::CreateRenderTargets()
{
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_impl->RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (std::uint32_t frameIndex = 0; frameIndex < FrameCount; ++frameIndex)
    {
        FrameContext& frame = m_impl->Frames[frameIndex];
        ThrowIfFailed(m_impl->SwapChain->GetBuffer(frameIndex, IID_PPV_ARGS(&frame.RenderTarget)), "GetBuffer failed");
        m_impl->Device->CreateRenderTargetView(frame.RenderTarget.Get(), nullptr, rtvHandle);
        frame.RtvCpuHandle = rtvHandle;
        rtvHandle.ptr += m_impl->RtvDescriptorSize;
    }

    m_frameIndex = m_impl->SwapChain->GetCurrentBackBufferIndex();
}

void GfxContext::ReleaseRenderTargets()
{
    for (FrameContext& frame : m_impl->Frames)
    {
        frame.RenderTarget.Reset();
    }
}

void GfxContext::RenderImGui(ImDrawData* drawData)
{
    ImGui_ImplDX12_RenderDrawData(drawData, m_impl->CommandList.Get());
}

bool GfxContext::RequestPresentedImageCapture()
{
    if (m_impl == nullptr)
    {
        return false;
    }

    Impl::PresentedImageCaptureState& capture = m_impl->PresentedImageCapture;
    if (capture.Requested || capture.Submitted || capture.Resource != nullptr)
    {
        return false;
    }

    capture.Requested = true;
    return true;
}

bool GfxContext::HasPendingPresentedImageCapture() const
{
    return m_impl != nullptr
        && (m_impl->PresentedImageCapture.Requested || m_impl->PresentedImageCapture.Submitted);
}

void GfxContext::RecordPresentedImageCapture(void* commandListPtr, void* renderTargetPtr)
{
    if (m_impl == nullptr || !m_impl->PresentedImageCapture.Requested || commandListPtr == nullptr
        || renderTargetPtr == nullptr)
    {
        return;
    }

    Impl::PresentedImageCaptureState& capture = m_impl->PresentedImageCapture;
    auto* const commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);
    auto* const renderTarget = static_cast<ID3D12Resource*>(renderTargetPtr);
    const D3D12_RESOURCE_DESC sourceDesc = renderTarget->GetDesc();
    if (sourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D
        || sourceDesc.SampleDesc.Count != 1
        || sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        EngineLog::Error("s0p5-capture", "Final output is not a single-sample RGBA8 swapchain texture.");
        capture.Requested = false;
        return;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0;
    UINT64 totalBytes = 0;
    m_impl->Device->GetCopyableFootprints(
        &sourceDesc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
    if (totalBytes == 0 || rows != sourceDesc.Height || rowBytes != sourceDesc.Width * 4ull)
    {
        EngineLog::Error("s0p5-capture", "Could not derive a canonical RGBA8 readback footprint.");
        capture.Requested = false;
        return;
    }

    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = totalBytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(m_impl->MemoryAllocator->CreateResource(
            &allocationDesc,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            &capture.Allocation,
            IID_PPV_ARGS(&capture.Resource))))
    {
        capture.Allocation = nullptr;
        capture.Resource = nullptr;
        capture.Requested = false;
        EngineLog::Error("s0p5-capture", "Could not allocate the final-output readback buffer.");
        return;
    }

    D3D12_RESOURCE_BARRIER toCopy{};
    toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toCopy.Transition.pResource = renderTarget;
    toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commandList->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = renderTarget;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = capture.Resource;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    std::swap(toCopy.Transition.StateBefore, toCopy.Transition.StateAfter);
    commandList->ResourceBarrier(1, &toCopy);

    capture.Footprint = footprint;
    capture.Width = static_cast<int>(sourceDesc.Width);
    capture.Height = static_cast<int>(sourceDesc.Height);
    capture.Requested = false;
    capture.Submitted = true;
}

bool GfxContext::TryConsumePresentedImageCapture(PresentedImageCapture& outCapture)
{
    outCapture = {};
    if (m_impl == nullptr)
    {
        return false;
    }

    Impl::PresentedImageCaptureState& capture = m_impl->PresentedImageCapture;
    if (!capture.Submitted || capture.FenceValue == 0 || m_impl->Fence->GetCompletedValue() < capture.FenceValue)
    {
        return false;
    }

    const std::size_t rowBytes = static_cast<std::size_t>(capture.Width) * 4u;
    std::vector<std::uint8_t> rgba8(rowBytes * static_cast<std::size_t>(capture.Height));
    D3D12_RANGE readRange{0, capture.Footprint.Footprint.RowPitch * static_cast<SIZE_T>(capture.Height)};
    void* mapped = nullptr;
    if (FAILED(capture.Resource->Map(0, &readRange, &mapped)) || mapped == nullptr)
    {
        ReleasePresentedImageCapture();
        EngineLog::Error("s0p5-capture", "Could not map completed final-output readback buffer.");
        return false;
    }
    const auto* const source = static_cast<const std::uint8_t*>(mapped) + capture.Footprint.Offset;
    for (int row = 0; row < capture.Height; ++row)
    {
        std::memcpy(
            rgba8.data() + rowBytes * static_cast<std::size_t>(row),
            source + capture.Footprint.Footprint.RowPitch * static_cast<std::size_t>(row),
            rowBytes);
    }
    capture.Resource->Unmap(0, nullptr);

    outCapture.width = capture.Width;
    outCapture.height = capture.Height;
    outCapture.rgba8 = std::move(rgba8);
    ReleasePresentedImageCapture();
    return true;
}

void GfxContext::ReleasePresentedImageCapture()
{
    if (m_impl == nullptr)
    {
        return;
    }

    Impl::PresentedImageCaptureState& capture = m_impl->PresentedImageCapture;
    if (capture.Allocation != nullptr)
    {
        capture.Allocation->Release();
    }
    if (capture.Resource != nullptr)
    {
        capture.Resource->Release();
    }
    capture = {};
}

bool GfxContext::ReadbackPresentedColorPixel(const int x, const int y, float outRgba[4]) const
{
    if (m_impl == nullptr || outRgba == nullptr || m_width <= 0 || m_height <= 0)
    {
        return false;
    }

    const std::uint32_t presentedIndex = (m_frameIndex + FrameCount - 1u) % FrameCount;
    ID3D12Resource* swapchainBuffer = m_impl->Frames[presentedIndex].RenderTarget.Get();
    if (swapchainBuffer == nullptr)
    {
        return false;
    }

    D3D12MA::Allocator* allocator = m_impl->MemoryAllocator;
    constexpr UINT64 kReadbackSize = sizeof(std::uint8_t) * 4;

    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = kReadbackSize;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC readbackAllocationDesc{};
    readbackAllocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

    ID3D12Resource* readbackResource = nullptr;
    D3D12MA::Allocation* readbackAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &readbackAllocationDesc,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            &readbackAllocation,
            IID_PPV_ARGS(&readbackResource))))
    {
        return false;
    }

    const int clampedX = std::clamp(x, 0, m_width - 1);
    const int clampedY = std::clamp(y, 0, m_height - 1);

    const_cast<GfxContext*>(this)->ExecuteImmediate([&](void* commandListPtr) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = swapchainBuffer;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commandList->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = swapchainBuffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readbackResource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset = 0;
        destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        destination.PlacedFootprint.Footprint.Width = 1;
        destination.PlacedFootprint.Footprint.Height = 1;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackSize);

        const UINT left = static_cast<UINT>(clampedX);
        const UINT top = static_cast<UINT>(clampedY);
        const D3D12_BOX sourceBox{left, top, 0, left + 1, top + 1, 1};
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

        D3D12_RESOURCE_BARRIER toPresent{};
        toPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toPresent.Transition.pResource = swapchainBuffer;
        toPresent.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &toPresent);
    });

    D3D12_RANGE readRange{0, static_cast<SIZE_T>(kReadbackSize)};
    void* mapped = nullptr;
    if (FAILED(readbackResource->Map(0, &readRange, &mapped)))
    {
        readbackAllocation->Release();
        readbackResource->Release();
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(mapped);
    for (int channel = 0; channel < 4; ++channel)
    {
        outRgba[channel] = static_cast<float>(bytes[channel]) / 255.0f;
    }

    readbackResource->Unmap(0, nullptr);
    readbackAllocation->Release();
    readbackResource->Release();
    return true;
}

std::string GfxContext::GetLastGpuAllocationError()
{
    return EngineDiagnostics::GetLastGpuAllocationError();
}

void GfxContext::GetSrvDescriptorUsage(std::uint32_t& outUsed, std::uint32_t& outCapacity) const
{
    outUsed = 0;
    outCapacity = 0;
    if (m_impl == nullptr)
    {
        return;
    }

    outUsed = m_impl->SrvAllocator.UsedCount();
    outCapacity = m_impl->SrvAllocator.Capacity();
}

void GfxContext::LogD3D12InfoQueueMessages(const char* context)
{
    if (m_impl == nullptr || m_impl->Device == nullptr || context == nullptr)
    {
        return;
    }

    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(m_impl->Device.As(&infoQueue)))
    {
        return;
    }

    const UINT64 messageCount = infoQueue->GetNumStoredMessages();
    if (messageCount == 0)
    {
        return;
    }

    EngineLog::Info("d3d12-info-queue", std::string(context) + ": draining " + std::to_string(messageCount) + " message(s)");
    for (UINT64 messageIndex = 0; messageIndex < messageCount; ++messageIndex)
    {
        SIZE_T messageLength = 0;
        if (FAILED(infoQueue->GetMessage(static_cast<UINT>(messageIndex), nullptr, &messageLength))
            || messageLength == 0)
        {
            continue;
        }

        std::vector<std::uint8_t> messageBuffer(messageLength);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(messageBuffer.data());
        if (FAILED(infoQueue->GetMessage(static_cast<UINT>(messageIndex), message, &messageLength)))
        {
            continue;
        }

        const char* description = message->pDescription != nullptr ? message->pDescription : "(no description)";
        const std::string logLine =
            std::string(context) + " [" + std::to_string(message->Severity) + "]: " + description;
        if (message->Severity >= D3D12_MESSAGE_SEVERITY_ERROR)
        {
            EngineLog::Error("d3d12-info-queue", logLine);
        }
        else if (message->Severity >= D3D12_MESSAGE_SEVERITY_WARNING)
        {
            EngineLog::Warn("d3d12-info-queue", logLine);
        }
        else
        {
            EngineLog::Info("d3d12-info-queue", logLine);
        }
    }

    infoQueue->ClearStoredMessages();
}
