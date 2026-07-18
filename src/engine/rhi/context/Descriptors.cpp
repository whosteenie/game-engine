#include "engine/rhi/context/Detail.h"

bool GfxContext::IsShaderVisibleSrvCpuHandle(const std::uintptr_t cpuHandle) const
{
    if (cpuHandle == 0 || m_impl == nullptr || m_impl->SrvDescriptorSize == 0)
    {
        return false;
    }

    if (m_impl->RtvHeap != nullptr)
    {
        const SIZE_T rtvStart = m_impl->RtvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        const SIZE_T rtvEnd = rtvStart + static_cast<SIZE_T>(FrameCount) * m_impl->RtvDescriptorSize;
        if (cpuHandle >= rtvStart && cpuHandle < rtvEnd)
        {
            return false;
        }
    }

    if (m_impl->OffscreenRtvHeap != nullptr)
    {
        const SIZE_T rtvStart = m_impl->OffscreenRtvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        const SIZE_T rtvEnd = rtvStart + static_cast<SIZE_T>(OffscreenRtvCount) * m_impl->RtvDescriptorSize;
        if (cpuHandle >= rtvStart && cpuHandle < rtvEnd)
        {
            return false;
        }
    }

    if (m_impl->OffscreenDsvHeap != nullptr)
    {
        const SIZE_T dsvStart = m_impl->OffscreenDsvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
        const SIZE_T dsvEnd = dsvStart + static_cast<SIZE_T>(OffscreenDsvCount) * m_impl->DsvDescriptorSize;
        if (cpuHandle >= dsvStart && cpuHandle < dsvEnd)
        {
            return false;
        }
    }

    const SIZE_T heapStart = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart().ptr;
    const SIZE_T heapEnd = heapStart + static_cast<SIZE_T>(SrvDescriptorCount)
        * static_cast<SIZE_T>(m_impl->SrvDescriptorSize);
    if (cpuHandle < heapStart || cpuHandle >= heapEnd)
    {
        return false;
    }

    const SIZE_T offset = cpuHandle - heapStart;
    if (offset % static_cast<SIZE_T>(m_impl->SrvDescriptorSize) != 0)
    {
        return false;
    }

    const std::uint32_t index =
        static_cast<std::uint32_t>(offset / static_cast<SIZE_T>(m_impl->SrvDescriptorSize));
    D3D12_CPU_DESCRIPTOR_HANDLE expected = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    expected.ptr += static_cast<SIZE_T>(index) * m_impl->SrvDescriptorSize;
    return expected.ptr == cpuHandle;
}

std::uint32_t GfxContext::AllocateOffscreenRtvBlock(const std::uint32_t count)
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return FixedDescriptorHeap::kInvalid;
    }

    const std::uint32_t index = m_impl->OffscreenRtvAllocator.AllocateBlock(count);
    if (index == FixedDescriptorHeap::kInvalid)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext out of offscreen RTV descriptors");
    }

    return index;
}

void GfxContext::FreeOffscreenRtvBlock(const std::uint32_t baseIndex, const std::uint32_t count)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->OffscreenRtvAllocator.FreeBlock(baseIndex, count);
}

std::uint32_t GfxContext::AllocateOffscreenDsv()
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return FixedDescriptorHeap::kInvalid;
    }

    const std::uint32_t index = m_impl->OffscreenDsvAllocator.AllocateOne();
    if (index == FixedDescriptorHeap::kInvalid)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext out of offscreen DSV descriptors");
    }

    return index;
}

void GfxContext::FreeOffscreenDsv(const std::uint32_t descriptorIndex)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->OffscreenDsvAllocator.FreeOne(descriptorIndex);
}

std::uintptr_t GfxContext::GetOffscreenRtvCpuHandle(const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->OffscreenRtvAllocator.IsValidIndex(descriptorIndex))
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->OffscreenRtvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->RtvDescriptorSize;
    return handle.ptr;
}

std::uintptr_t GfxContext::GetOffscreenDsvCpuHandle(const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->OffscreenDsvAllocator.IsValidIndex(descriptorIndex))
    {
        return 0;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = m_impl->OffscreenDsvHeap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->DsvDescriptorSize;
    return handle.ptr;
}

void* GfxContext::GetSrvHeapGpuHandle(std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->SrvAllocator.IsValidIndex(descriptorIndex))
    {
        return nullptr;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE handle = m_impl->SrvHeap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(descriptorIndex) * m_impl->SrvDescriptorSize;
    return reinterpret_cast<void*>(handle.ptr);
}

std::uint32_t GfxContext::AllocateOffscreenSrv()
{
    if (m_impl == nullptr)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext is not initialized");
        return FixedDescriptorHeap::kInvalid;
    }

    const std::uint32_t index = m_impl->SrvAllocator.AllocateOne();
    if (index == FixedDescriptorHeap::kInvalid)
    {
        GfxContextDetail::SetGpuAllocationError("GfxContext out of SRV descriptors");
    }

    return index;
}

void GfxContext::FreeOffscreenSrv(std::uint32_t descriptorIndex)
{
    if (m_impl == nullptr)
    {
        return;
    }

    m_impl->SrvAllocator.FreeOne(descriptorIndex);
}

std::uint64_t GfxContext::DeferredDestroyFenceValue() const
{
    // While recording, the next signal (>= m_submissionFenceValue + 1) covers the command list
    // currently being built; otherwise everything that can reference the resource has already
    // been submitted and is covered by the last signaled value.
    return m_frameRecording ? m_submissionFenceValue + 1 : m_submissionFenceValue;
}

void GfxContext::DeferredReleaseResource(void* d3d12maAllocation, void* d3d12Resource)
{
    auto* allocation = static_cast<D3D12MA::Allocation*>(d3d12maAllocation);
    auto* resource = static_cast<ID3D12Resource*>(d3d12Resource);
    if (allocation == nullptr && resource == nullptr)
    {
        return;
    }

    if (m_impl == nullptr)
    {
        // Context already shut down: nothing can be in flight, release immediately.
        if (resource != nullptr)
        {
            resource->Release();
        }
        if (allocation != nullptr)
        {
            allocation->Release();
        }
        return;
    }

    Impl::DeferredDestroy entry{};
    entry.FenceValue = DeferredDestroyFenceValue();
    entry.Allocation = allocation;
    entry.Resource = resource;
    m_impl->DeferredDestroys.push_back(entry);
}

void GfxContext::DeferredReleaseGpuObject(void* d3d12Object)
{
    auto* object = static_cast<IUnknown*>(d3d12Object);
    if (object == nullptr)
    {
        return;
    }

    if (m_impl == nullptr)
    {
        // Context shutdown has already waited for the queue.
        object->Release();
        return;
    }

    Impl::DeferredDestroy entry{};
    entry.FenceValue = DeferredDestroyFenceValue();
    entry.GpuObject = object;
    m_impl->DeferredDestroys.push_back(entry);
}

void GfxContext::DeferredFreeOffscreenSrv(const std::uint32_t descriptorIndex)
{
    if (m_impl == nullptr || descriptorIndex == UINT32_MAX)
    {
        return;
    }

    Impl::DeferredDestroy entry{};
    entry.FenceValue = DeferredDestroyFenceValue();
    entry.SrvIndex = descriptorIndex;
    m_impl->DeferredDestroys.push_back(entry);
}

void GfxContext::DeferredFreeOffscreenRtvBlock(const std::uint32_t baseIndex, const std::uint32_t count)
{
    if (m_impl == nullptr || baseIndex == UINT32_MAX || count == 0)
    {
        return;
    }

    Impl::DeferredDestroy entry{};
    entry.FenceValue = DeferredDestroyFenceValue();
    entry.RtvBaseIndex = baseIndex;
    entry.RtvCount = count;
    m_impl->DeferredDestroys.push_back(entry);
}

void GfxContext::DeferredFreeOffscreenDsv(const std::uint32_t descriptorIndex)
{
    if (m_impl == nullptr || descriptorIndex == UINT32_MAX)
    {
        return;
    }

    Impl::DeferredDestroy entry{};
    entry.FenceValue = DeferredDestroyFenceValue();
    entry.DsvIndex = descriptorIndex;
    m_impl->DeferredDestroys.push_back(entry);
}

void GfxContext::ProcessDeferredDestroys(const bool flushAll)
{
    if (m_impl == nullptr || m_impl->DeferredDestroys.empty())
    {
        return;
    }

    const std::uint64_t completedValue =
        m_impl->Fence != nullptr ? m_impl->Fence->GetCompletedValue() : 0;

    std::vector<Impl::DeferredDestroy>& pending = m_impl->DeferredDestroys;
    std::size_t writeIndex = 0;
    for (std::size_t readIndex = 0; readIndex < pending.size(); ++readIndex)
    {
        Impl::DeferredDestroy& entry = pending[readIndex];
        if (!flushAll && entry.FenceValue > completedValue)
        {
            pending[writeIndex++] = entry;
            continue;
        }

        if (entry.Resource != nullptr)
        {
            entry.Resource->Release();
        }
        if (entry.GpuObject != nullptr)
        {
            entry.GpuObject->Release();
        }
        if (entry.Allocation != nullptr)
        {
            entry.Allocation->Release();
        }
        if (entry.SrvIndex != UINT32_MAX)
        {
            m_impl->SrvAllocator.FreeOne(entry.SrvIndex);
        }
        if (entry.RtvBaseIndex != UINT32_MAX && entry.RtvCount > 0)
        {
            m_impl->OffscreenRtvAllocator.FreeBlock(entry.RtvBaseIndex, entry.RtvCount);
        }
        if (entry.DsvIndex != UINT32_MAX)
        {
            m_impl->OffscreenDsvAllocator.FreeOne(entry.DsvIndex);
        }
    }

    pending.resize(writeIndex);
}

void GfxContext::AllocSrvDescriptorForImGui(void* out_cpu_handle, void* out_gpu_handle)
{
    const std::uint32_t index = AllocateOffscreenSrv();
    auto* cpu = static_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(out_cpu_handle);
    auto* gpu = static_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(out_gpu_handle);
    if (index == FixedDescriptorHeap::kInvalid)
    {
        *cpu = {};
        *gpu = {};
        return;
    }

    *cpu = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu->ptr += static_cast<SIZE_T>(index) * m_impl->SrvDescriptorSize;
    *gpu = m_impl->SrvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu->ptr += static_cast<UINT64>(index) * m_impl->SrvDescriptorSize;
}

void GfxContext::FreeSrvDescriptorFromCpuHandle(const std::uintptr_t cpu_handle_ptr)
{
    if (m_impl == nullptr || cpu_handle_ptr == 0 || !IsShaderVisibleSrvCpuHandle(cpu_handle_ptr))
    {
        return;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    const std::uint32_t index = static_cast<std::uint32_t>(
        (cpu_handle_ptr - heapStart.ptr) / m_impl->SrvDescriptorSize);
    DeferredFreeOffscreenSrv(index);
}

void GfxContext::CreateSrvForTexture(
    void* resource,
    int formatRgba_UNORM,
    std::uint32_t descriptorIndex,
    int width,
    int height,
    const std::uint32_t mipLevels) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = static_cast<DXGI_FORMAT>(formatRgba_UNORM);
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mipLevels;
    CreateShaderResourceView(resource, &srvDesc, descriptorIndex);
    (void)width;
    (void)height;
}

void GfxContext::CreateMsaaDepthSrv(void* resource, const std::uint32_t descriptorIndex) const
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    CreateShaderResourceView(resource, &srvDesc, descriptorIndex);
}

void GfxContext::CreateShaderResourceView(
    void* resource,
    const void* srvDesc,
    const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->SrvAllocator.IsValidIndex(descriptorIndex))
    {
        return;
    }

    auto* device = static_cast<ID3D12Device*>(GetDevice());
    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    const auto* typedDesc = static_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(srvDesc);
    if (device == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE visibleHandle = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    visibleHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->SrvDescriptorSize;
    device->CreateShaderResourceView(d3dResource, typedDesc, visibleHandle);

    if (m_impl->SrvCpuHeap != nullptr)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE copyHandle = m_impl->SrvCpuHeap->GetCPUDescriptorHandleForHeapStart();
        copyHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->SrvDescriptorSize;
        device->CreateShaderResourceView(d3dResource, typedDesc, copyHandle);
    }
}

void GfxContext::CreateUnorderedAccessView(
    void* resource,
    void* counterResource,
    const void* uavDesc,
    const std::uint32_t descriptorIndex) const
{
    if (m_impl == nullptr || !m_impl->SrvAllocator.IsValidIndex(descriptorIndex))
    {
        return;
    }

    auto* device = static_cast<ID3D12Device*>(GetDevice());
    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    auto* d3dCounterResource = static_cast<ID3D12Resource*>(counterResource);
    const auto* typedDesc = static_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(uavDesc);
    if (device == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE visibleHandle = m_impl->SrvHeap->GetCPUDescriptorHandleForHeapStart();
    visibleHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->SrvDescriptorSize;
    device->CreateUnorderedAccessView(d3dResource, d3dCounterResource, typedDesc, visibleHandle);

    if (m_impl->SrvCpuHeap != nullptr)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE copyHandle = m_impl->SrvCpuHeap->GetCPUDescriptorHandleForHeapStart();
        copyHandle.ptr += static_cast<SIZE_T>(descriptorIndex) * m_impl->SrvDescriptorSize;
        device->CreateUnorderedAccessView(d3dResource, d3dCounterResource, typedDesc, copyHandle);
    }
}

void GfxContext::ClearOffscreenTarget(
    void* resource,
    const std::uint32_t rtvIndex,
    const float clearColor[4],
    const int width,
    const int height)
{
    if (m_impl == nullptr || resource == nullptr || rtvIndex == UINT32_MAX)
    {
        return;
    }

    auto* d3dResource = static_cast<ID3D12Resource*>(resource);
    auto* commandList = m_impl->CommandList.Get();

    D3D12_CPU_DESCRIPTOR_HANDLE offscreenRtv{};
    offscreenRtv.ptr = GetOffscreenRtvCpuHandle(rtvIndex);

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = d3dResource;
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commandList->ResourceBarrier(1, &toRenderTarget);

    commandList->OMSetRenderTargets(1, &offscreenRtv, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, width, height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(offscreenRtv, clearColor, 0, nullptr);

    D3D12_RESOURCE_BARRIER toShaderResource{};
    toShaderResource.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toShaderResource.Transition.pResource = d3dResource;
    toShaderResource.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toShaderResource.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toShaderResource.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    commandList->ResourceBarrier(1, &toShaderResource);

    FrameContext& frame = m_impl->Frames[m_frameIndex];
    commandList->OMSetRenderTargets(1, &frame.RtvCpuHandle, FALSE, nullptr);
    D3D12_VIEWPORT swapViewport{};
    swapViewport.Width = static_cast<float>(m_width);
    swapViewport.Height = static_cast<float>(m_height);
    swapViewport.MaxDepth = 1.0f;
    D3D12_RECT swapScissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &swapViewport);
    commandList->RSSetScissorRects(1, &swapScissor);
}

