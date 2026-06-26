#include "engine/rendering/Framebuffer.h"



#include "engine/rhi/GfxContext.h"



#include <D3D12MemAlloc.h>

#include <d3d12.h>

#include <dxgiformat.h>



#include <array>

#include <algorithm>

#include <algorithm>

#include <cstring>

#include <stdexcept>

#include <string>



namespace

{

    DXGI_FORMAT ColorFormatForAttachment(int attachmentIndex, FramebufferColorMode colorMode)

    {

        if (colorMode == FramebufferColorMode::Single)

        {

            // LDR color is required for ImGui viewport compositing until tonemap/post-process is enabled on D3D12.

            return DXGI_FORMAT_R8G8B8A8_UNORM;

        }



        if (attachmentIndex == 4)
        {
            return DXGI_FORMAT_R16G16_FLOAT;
        }

        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    }



    int ColorAttachmentCount(FramebufferColorMode colorMode)
    {
        return colorMode == FramebufferColorMode::SplitDirectIndirect ? 7 : 1;
    }

    constexpr std::uint32_t kShaderResourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    constexpr std::uint32_t kRenderTargetState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET);
    constexpr std::uint32_t kDepthWriteState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE);

    void TransitionResourceState(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        std::uint32_t& trackedState,
        const std::uint32_t newState)
    {
        if (resource == nullptr)
        {
            return;
        }

        if (trackedState == 0)
        {
            trackedState = kShaderResourceState;
        }

        if (trackedState == newState)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(trackedState);
        barrier.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(newState);
        commandList->ResourceBarrier(1, &barrier);
        trackedState = newState;
    }

}



Framebuffer::~Framebuffer()

{

    Destroy();

}



void Framebuffer::Destroy()

{

    if (!GfxContext::Get().IsInitialized())

    {

        m_width = 0;

        m_height = 0;

        return;

    }



    for (int attachmentIndex = 0; attachmentIndex < MaxColorAttachments; ++attachmentIndex)

    {

        if (m_colorSrvIndices[attachmentIndex] != UINT32_MAX)

        {

            GfxContext::Get().FreeOffscreenSrv(m_colorSrvIndices[attachmentIndex]);

            m_colorSrvIndices[attachmentIndex] = UINT32_MAX;

        }



        if (m_colorAllocations[attachmentIndex] != nullptr)

        {

            static_cast<D3D12MA::Allocation*>(m_colorAllocations[attachmentIndex])->Release();

            m_colorAllocations[attachmentIndex] = nullptr;

        }



        m_colorResources[attachmentIndex] = nullptr;

    }



    if (m_depthSrvIndex != UINT32_MAX)

    {

        GfxContext::Get().FreeOffscreenSrv(m_depthSrvIndex);

        m_depthSrvIndex = UINT32_MAX;

    }



    if (m_depthAllocation != nullptr)

    {

        static_cast<D3D12MA::Allocation*>(m_depthAllocation)->Release();

        m_depthAllocation = nullptr;

    }



    m_depthResource = nullptr;



    if (m_rtvBaseIndex != UINT32_MAX)

    {

        GfxContext::Get().FreeOffscreenRtvBlock(m_rtvBaseIndex, static_cast<std::uint32_t>(m_colorAttachmentCount));

        m_rtvBaseIndex = UINT32_MAX;

    }



    if (m_dsvIndex != UINT32_MAX)

    {

        GfxContext::Get().FreeOffscreenDsv(m_dsvIndex);

        m_dsvIndex = UINT32_MAX;

    }



    m_width = 0;

    m_height = 0;

    m_colorMode = FramebufferColorMode::Single;

    m_colorAttachmentCount = 1;

    for (int attachmentIndex = 0; attachmentIndex < MaxColorAttachments; ++attachmentIndex)
    {
        m_colorStates[attachmentIndex] = 0;
    }

    m_depthState = 0;

}



void Framebuffer::Create(const int width, const int height)

{

    try

    {

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    m_colorAttachmentCount = ColorAttachmentCount(m_colorMode);

    m_rtvBaseIndex = GfxContext::Get().AllocateOffscreenRtvBlock(static_cast<std::uint32_t>(m_colorAttachmentCount));

    const bool needsDepth = true;
    m_dsvIndex = GfxContext::Get().AllocateOffscreenDsv();

    if (m_rtvBaseIndex == UINT32_MAX || (needsDepth && m_dsvIndex == UINT32_MAX))
    {
        const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        throw std::runtime_error(gpuError.empty() ? "GPU descriptor/SRV allocation failed" : gpuError);
    }
    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)

    {

        const DXGI_FORMAT format = ColorFormatForAttachment(attachmentIndex, m_colorMode);



        D3D12_RESOURCE_DESC resourceDesc{};

        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        resourceDesc.Width = static_cast<UINT64>(width);

        resourceDesc.Height = static_cast<UINT>(height);

        resourceDesc.DepthOrArraySize = 1;

        resourceDesc.MipLevels = 1;

        resourceDesc.Format = format;

        resourceDesc.SampleDesc.Count = 1;

        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;



        D3D12_CLEAR_VALUE clearValue{};

        clearValue.Format = format;

        clearValue.Color[0] = 0.08f;

        clearValue.Color[1] = 0.09f;

        clearValue.Color[2] = 0.15f;

        clearValue.Color[3] = 1.0f;



        D3D12MA::ALLOCATION_DESC allocationDesc{};

        allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;



        ID3D12Resource* resource = nullptr;

        D3D12MA::Allocation* allocation = nullptr;

        const HRESULT createHr = allocator->CreateResource(

                &allocationDesc,

                &resourceDesc,

                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,

                &clearValue,

                &allocation,

                IID_PPV_ARGS(&resource));

        if (FAILED(createHr))

        {

            throw std::runtime_error(
                "Failed to create framebuffer color attachment (HRESULT=0x" +
                std::to_string(static_cast<unsigned long>(createHr)) + ")");

        }



        m_colorResources[attachmentIndex] = resource;

        m_colorAllocations[attachmentIndex] = allocation;

        m_colorStates[attachmentIndex] = kShaderResourceState;

        m_colorSrvIndices[attachmentIndex] = GfxContext::Get().AllocateOffscreenSrv();
        if (m_colorSrvIndices[attachmentIndex] == UINT32_MAX)
        {
            const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        throw std::runtime_error(gpuError.empty() ? "GPU descriptor/SRV allocation failed" : gpuError);
        }

        GfxContext::Get().CreateSrvForTexture(

            resource,

            static_cast<int>(format),

            m_colorSrvIndices[attachmentIndex],

            width,

            height);



        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};

        rtvHandle.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));

        device->CreateRenderTargetView(resource, nullptr, rtvHandle);

    }



    if (needsDepth)
    {

        D3D12_RESOURCE_DESC depthDesc{};

        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        depthDesc.Width = static_cast<UINT64>(width);

        depthDesc.Height = static_cast<UINT>(height);

        depthDesc.DepthOrArraySize = 1;

        depthDesc.MipLevels = 1;

        depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

        depthDesc.SampleDesc.Count = 1;

        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;



        D3D12_CLEAR_VALUE depthClear{};

        depthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

        depthClear.DepthStencil.Depth = 1.0f;

        depthClear.DepthStencil.Stencil = 0;



        D3D12MA::ALLOCATION_DESC allocationDesc{};

        allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;



        ID3D12Resource* resource = nullptr;

        D3D12MA::Allocation* allocation = nullptr;

        if (FAILED(allocator->CreateResource(

                &allocationDesc,

                &depthDesc,

                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,

                &depthClear,

                &allocation,

                IID_PPV_ARGS(&resource))))

        {

            throw std::runtime_error("Failed to create framebuffer depth attachment");

        }



        m_depthResource = resource;

        m_depthAllocation = allocation;

        m_depthState = kShaderResourceState;

        m_depthSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
        if (m_depthSrvIndex == UINT32_MAX)
        {
            const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        throw std::runtime_error(gpuError.empty() ? "GPU descriptor/SRV allocation failed" : gpuError);
        }

        GfxContext::Get().CreateSrvForTexture(

            resource,

            static_cast<int>(DXGI_FORMAT_R24_UNORM_X8_TYPELESS),

            m_depthSrvIndex,

            width,

            height);



        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};

        dsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(m_dsvIndex);

        device->CreateDepthStencilView(resource, nullptr, dsvHandle);

    }

    }

    catch (...)

    {

        Destroy();

        throw;

    }

}



bool Framebuffer::Resize(const int width, const int height, const FramebufferColorMode colorMode)
{
    if (width <= 0 || height <= 0)
    {
        Destroy();
        return false;
    }

    if (m_width == width && m_height == height && m_colorMode == colorMode && m_colorResources[0] != nullptr)
    {
        return true;
    }

    Destroy();
    m_colorMode = colorMode;

    try
    {
        Create(width, height);
    }
    catch (...)
    {
        Destroy();
        return false;
    }

    m_width = width;
    m_height = height;
    return true;
}



void Framebuffer::TransitionColorAttachment(const int attachmentIndex, const std::uint32_t newState) const
{
    if (attachmentIndex < 0 || attachmentIndex >= m_colorAttachmentCount)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    TransitionResourceState(
        commandList,
        static_cast<ID3D12Resource*>(m_colorResources[attachmentIndex]),
        const_cast<std::uint32_t&>(m_colorStates[attachmentIndex]),
        newState);
}

void Framebuffer::TransitionDepth(const std::uint32_t newState) const
{
    if (m_depthResource == nullptr)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    TransitionResourceState(
        commandList,
        static_cast<ID3D12Resource*>(m_depthResource),
        const_cast<std::uint32_t&>(m_depthState),
        newState);
}

void Framebuffer::EnsureShaderResourceState() const
{
    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        TransitionColorAttachment(attachmentIndex, kShaderResourceState);
    }

    TransitionDepth(kShaderResourceState);
}

void Framebuffer::ClearRenderTarget() const
{
    if (m_colorResources[0] == nullptr || m_rtvBaseIndex == UINT32_MAX)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    TransitionColorAttachment(0, kRenderTargetState);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
    rtvHandle.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    const float clearColor[] = {0.12f, 0.14f, 0.20f, 1.0f};
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    TransitionColorAttachment(0, kShaderResourceState);
}

void Framebuffer::BindDrawTarget(const bool clearAttachments, const float clearColor[4]) const
{
    if (m_colorResources[0] == nullptr || m_rtvBaseIndex == UINT32_MAX)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        TransitionColorAttachment(attachmentIndex, kRenderTargetState);
    }

    if (m_depthResource != nullptr)
    {
        TransitionDepth(kDepthWriteState);
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MaxColorAttachments> rtvs{};
    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        rtvs[static_cast<std::size_t>(attachmentIndex)].ptr =
            GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPointer = nullptr;
    if (m_depthResource != nullptr)
    {
        dsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(m_dsvIndex);
        dsvPointer = &dsvHandle;
    }

    commandList->OMSetRenderTargets(
        static_cast<UINT>(m_colorAttachmentCount),
        rtvs.data(),
        FALSE,
        dsvPointer);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    if (clearAttachments)
    {
        const float defaultClearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
        const float* resolvedClearColor = clearColor != nullptr ? clearColor : defaultClearColor;
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            commandList->ClearRenderTargetView(
                rtvs[static_cast<std::size_t>(attachmentIndex)],
                resolvedClearColor,
                0,
                nullptr);
        }

        if (dsvPointer != nullptr)
        {
            commandList->ClearDepthStencilView(
                dsvHandle,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                1.0f,
                0,
                0,
                nullptr);
        }
    }
}

void Framebuffer::Bind() const
{
    BindDrawTarget(true);
    GfxContext::Get().SetBoundOutputFramebuffer(this);
}

void Framebuffer::Unbind() const
{
    EnsureShaderResourceState();
}



std::uintptr_t Framebuffer::GetFramebuffer() const
{
    return reinterpret_cast<std::uintptr_t>(this);
}

std::uintptr_t Framebuffer::GetColorTexture() const
{
    if (m_colorSrvIndices[0] == UINT32_MAX)
    {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(GfxContext::Get().GetSrvHeapGpuHandle(m_colorSrvIndices[0]));
}

std::uintptr_t Framebuffer::GetIndirectColorTexture() const
{
    if (m_colorSrvIndices[1] == UINT32_MAX)
    {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(GfxContext::Get().GetSrvHeapGpuHandle(m_colorSrvIndices[1]));
}



std::uintptr_t Framebuffer::GetNormalColorTexture() const
{
    if (m_colorSrvIndices[2] == UINT32_MAX)
    {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(GfxContext::Get().GetSrvHeapGpuHandle(m_colorSrvIndices[2]));
}

std::uintptr_t Framebuffer::GetShadowFactorTexture() const
{
    if (m_colorSrvIndices[3] == UINT32_MAX)
    {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(GfxContext::Get().GetSrvHeapGpuHandle(m_colorSrvIndices[3]));
}

std::uintptr_t Framebuffer::GetDepthTexture() const
{
    if (m_depthSrvIndex == UINT32_MAX)
    {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(GfxContext::Get().GetSrvHeapGpuHandle(m_depthSrvIndex));
}

int Framebuffer::GetWidth() const

{

    return m_width;

}



int Framebuffer::GetHeight() const

{

    return m_height;

}



bool Framebuffer::IsValid() const

{

    return m_colorResources[0] != nullptr && m_width > 0 && m_height > 0;

}



bool Framebuffer::HasSplitLighting() const

{

    return m_colorMode == FramebufferColorMode::SplitDirectIndirect;

}



bool Framebuffer::HasGeometryNormals() const

{

    return m_colorMode == FramebufferColorMode::SplitDirectIndirect;

}



bool Framebuffer::HasShadowFactor() const

{

    return m_colorMode == FramebufferColorMode::SplitDirectIndirect;

}

bool Framebuffer::HasVelocity() const
{
    return m_colorMode == FramebufferColorMode::SplitDirectIndirect;
}

bool Framebuffer::HasMaterialGbuffer() const
{
    return m_colorMode == FramebufferColorMode::SplitDirectIndirect;
}

std::uintptr_t Framebuffer::GetVelocityTexture() const
{
    if (m_colorSrvIndices[4] == UINT32_MAX)
    {
        return 0;
    }

    return reinterpret_cast<std::uintptr_t>(GfxContext::Get().GetSrvHeapGpuHandle(m_colorSrvIndices[4]));
}



std::uintptr_t Framebuffer::GetColorSrvCpuHandle(const int attachmentIndex) const

{

    if (attachmentIndex < 0 || attachmentIndex >= MaxColorAttachments ||

        m_colorSrvIndices[attachmentIndex] == UINT32_MAX)

    {

        return 0;

    }



    return GfxContext::Get().GetSrvCpuHandle(m_colorSrvIndices[attachmentIndex]);

}



std::uintptr_t Framebuffer::GetDepthSrvCpuHandle() const

{

    if (m_depthSrvIndex == UINT32_MAX)

    {

        return 0;

    }



    return GfxContext::Get().GetSrvCpuHandle(m_depthSrvIndex);

}



void* Framebuffer::GetColorResource(const int attachmentIndex) const

{

    if (attachmentIndex < 0 || attachmentIndex >= MaxColorAttachments)

    {

        return nullptr;

    }



    return m_colorResources[attachmentIndex];

}



void* Framebuffer::GetDepthResource() const

{

    return m_depthResource;

}



std::uintptr_t Framebuffer::GetColorRtvCpuHandle(const int attachmentIndex) const

{

    if (attachmentIndex < 0 || attachmentIndex >= m_colorAttachmentCount || m_rtvBaseIndex == UINT32_MAX)

    {

        return 0;

    }



    return GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));

}



std::uintptr_t Framebuffer::GetDepthDsvCpuHandle() const

{

    if (m_dsvIndex == UINT32_MAX)

    {

        return 0;

    }



    return GfxContext::Get().GetOffscreenDsvCpuHandle(m_dsvIndex);

}

bool Framebuffer::ReadbackColorPixel(const int x, const int y, float outRgba[4]) const
{
    if (m_colorResources[0] == nullptr || outRgba == nullptr || m_width <= 0 || m_height <= 0)
    {
        return false;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* colorResource = static_cast<ID3D12Resource*>(m_colorResources[0]);
    const DXGI_FORMAT colorFormat = ColorFormatForAttachment(0, m_colorMode);
    const bool isUnorm8 = colorFormat == DXGI_FORMAT_R8G8B8A8_UNORM;
    const UINT64 kReadbackSize = isUnorm8 ? 4ull : sizeof(std::uint16_t) * 4ull;

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
    const std::uint32_t beforeState = m_colorStates[0];

    GfxContext::Get().ExecuteImmediate([&](void* commandListPtr) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

        D3D12_RESOURCE_BARRIER toCopy{};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = colorResource;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        toCopy.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(beforeState);
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        commandList->ResourceBarrier(1, &toCopy);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = colorResource;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readbackResource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint.Offset = 0;
        destination.PlacedFootprint.Footprint.Format = colorFormat;
        destination.PlacedFootprint.Footprint.Width = 1;
        destination.PlacedFootprint.Footprint.Height = 1;
        destination.PlacedFootprint.Footprint.Depth = 1;
        destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackSize);

        const UINT left = static_cast<UINT>(clampedX);
        const UINT top = static_cast<UINT>(clampedY);
        const D3D12_BOX sourceBox{left, top, 0, left + 1, top + 1, 1};
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

        D3D12_RESOURCE_BARRIER fromCopy{};
        fromCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        fromCopy.Transition.pResource = colorResource;
        fromCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        fromCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        fromCopy.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(beforeState);
        commandList->ResourceBarrier(1, &fromCopy);
    });

    D3D12_RANGE readRange{0, static_cast<SIZE_T>(kReadbackSize)};
    void* mapped = nullptr;
    if (FAILED(readbackResource->Map(0, &readRange, &mapped)))
    {
        readbackAllocation->Release();
        readbackResource->Release();
        return false;
    }

    if (isUnorm8)
    {
        const auto* channels = static_cast<const std::uint8_t*>(mapped);
        for (int channel = 0; channel < 4; ++channel)
        {
            outRgba[channel] = static_cast<float>(channels[channel]) / 255.0f;
        }
    }
    else
    {
        const auto* halfChannels = static_cast<const std::uint16_t*>(mapped);
        auto halfToFloat = [](const std::uint16_t half) -> float {
            const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
            const std::uint32_t exponent = (half & 0x7C00u) >> 10;
            const std::uint32_t mantissa = half & 0x03FFu;
            std::uint32_t bits = 0;
            if (exponent == 0)
            {
                if (mantissa == 0)
                {
                    bits = sign;
                }
                else
                {
                    int adjustedExponent = -1;
                    std::uint32_t adjustedMantissa = mantissa;
                    while ((adjustedMantissa & 0x0400u) == 0)
                    {
                        adjustedMantissa <<= 1;
                        --adjustedExponent;
                    }
                    adjustedMantissa &= 0x03FFu;
                    const std::uint32_t floatExponent = static_cast<std::uint32_t>(127 - 15 + adjustedExponent);
                    bits = sign | (floatExponent << 23) | (adjustedMantissa << 13);
                }
            }
            else if (exponent == 31)
            {
                bits = sign | 0x7F800000u | (mantissa << 13);
            }
            else
            {
                const std::uint32_t floatExponent = (exponent - 15 + 127) << 23;
                bits = sign | floatExponent | (mantissa << 13);
            }

            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };

        for (int channel = 0; channel < 4; ++channel)
        {
            outRgba[channel] = halfToFloat(halfChannels[channel]);
        }
    }

    readbackResource->Unmap(0, nullptr);
    readbackAllocation->Release();
    readbackResource->Release();
    return true;
}
