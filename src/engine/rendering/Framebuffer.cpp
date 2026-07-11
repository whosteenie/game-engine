#include "engine/rendering/Framebuffer.h"



#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/HresultFormat.h"



#include <D3D12MemAlloc.h>

#include <d3d12.h>

#include <dxgiformat.h>



#include <array>

#include <algorithm>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <string>



namespace

{
    thread_local std::string g_lastFramebufferError;

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

    std::string FormatDescriptorUsageContext()
    {
        std::uint32_t srvUsed = 0;
        std::uint32_t srvCapacity = 0;
        GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
        return " (SRV " + std::to_string(srvUsed) + "/" + std::to_string(srvCapacity) + ", last GPU error: "
            + GfxContext::GetLastGpuAllocationError() + ")";
    }

    [[noreturn]] void ThrowFramebufferError(const std::string& message)
    {
        g_lastFramebufferError = message;
        EngineLog::Error("framebuffer", message);
        throw std::runtime_error(g_lastFramebufferError);
    }

    void FramebufferTraceStep(const std::string& message)
    {
        EngineLog::Breadcrumb("framebuffer", message);
    }

    constexpr std::uint32_t kShaderResourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    constexpr std::uint32_t kRenderTargetState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_RENDER_TARGET);
    constexpr std::uint32_t kDepthWriteState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    constexpr std::uint32_t kDepthReadState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_READ);
    constexpr std::uint32_t kCopySourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE);
    constexpr std::uint32_t kCopyDestState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST);
    constexpr std::uint32_t kResolveSourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    constexpr std::uint32_t kResolveDestState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_RESOLVE_DEST);

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



    // CRASH-01: a framebuffer can be resized/destroyed while a recorded-but-unsubmitted (or
    // in-flight) command list still references its textures and descriptor slots. All releases
    // and descriptor-slot frees are deferred until the covering fence completes.
    for (int attachmentIndex = 0; attachmentIndex < MaxColorAttachments; ++attachmentIndex)
    {
        if (m_colorSrvIndices[attachmentIndex] != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(m_colorSrvIndices[attachmentIndex]);
            m_colorSrvIndices[attachmentIndex] = UINT32_MAX;
        }

        if (m_colorAllocations[attachmentIndex] != nullptr || m_colorResources[attachmentIndex] != nullptr)
        {
            GfxContext::Get().DeferredReleaseResource(
                m_colorAllocations[attachmentIndex],
                m_colorResources[attachmentIndex]);
            m_colorAllocations[attachmentIndex] = nullptr;
        }

        m_colorResources[attachmentIndex] = nullptr;
    }

    for (int attachmentIndex = 0; attachmentIndex < MaxColorAttachments; ++attachmentIndex)
    {
        if (m_msaaColorAllocations[attachmentIndex] != nullptr || m_msaaColorResources[attachmentIndex] != nullptr)
        {
            GfxContext::Get().DeferredReleaseResource(
                m_msaaColorAllocations[attachmentIndex],
                m_msaaColorResources[attachmentIndex]);
            m_msaaColorAllocations[attachmentIndex] = nullptr;
        }

        m_msaaColorResources[attachmentIndex] = nullptr;
        m_msaaColorStates[attachmentIndex] = 0;
        m_msaaColorInitialized[attachmentIndex] = false;
    }

    if (m_msaaDepthAllocation != nullptr || m_msaaDepthResource != nullptr)
    {
        GfxContext::Get().DeferredReleaseResource(m_msaaDepthAllocation, m_msaaDepthResource);
        m_msaaDepthAllocation = nullptr;
    }

    m_msaaDepthResource = nullptr;
    m_msaaDepthState = 0;
    m_msaaDepthInitialized = false;

    if (m_msaaRtvBaseIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenRtvBlock(
            m_msaaRtvBaseIndex,
            static_cast<std::uint32_t>(m_colorAttachmentCount));
        m_msaaRtvBaseIndex = UINT32_MAX;
    }

    if (m_msaaDsvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenDsv(m_msaaDsvIndex);
        m_msaaDsvIndex = UINT32_MAX;
    }

    if (m_depthSrvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenSrv(m_depthSrvIndex);
        m_depthSrvIndex = UINT32_MAX;
    }

    if (m_msaaDepthSrvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenSrv(m_msaaDepthSrvIndex);
        m_msaaDepthSrvIndex = UINT32_MAX;
    }

    if (m_depthAllocation != nullptr || m_depthResource != nullptr)
    {
        GfxContext::Get().DeferredReleaseResource(m_depthAllocation, m_depthResource);
        m_depthAllocation = nullptr;
    }

    m_depthResource = nullptr;

    if (m_rtvBaseIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenRtvBlock(m_rtvBaseIndex, m_resolvedRtvCount);
        m_rtvBaseIndex = UINT32_MAX;
    }

    if (m_dsvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenDsv(m_dsvIndex);
        m_dsvIndex = UINT32_MAX;
    }



    m_width = 0;

    m_height = 0;

    m_colorMode = FramebufferColorMode::Single;

    m_colorAttachmentCount = 1;

    m_sampleCount = 1;

    m_resolvedRtvCount = 0;

    for (int attachmentIndex = 0; attachmentIndex < MaxColorAttachments; ++attachmentIndex)
    {
        m_colorStates[attachmentIndex] = 0;
        m_colorInitialized[attachmentIndex] = false;
    }

    m_depthState = 0;
    m_depthInitialized = false;

}



void Framebuffer::Create(const int width, const int height)

{

    try

    {

    g_lastFramebufferError.clear();

    std::string deviceRemovedReason;
    if (GfxContext::Get().IsDeviceRemoved(&deviceRemovedReason))
    {
        ThrowFramebufferError(
            "D3D12 device already removed before framebuffer create: " + deviceRemovedReason);
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    m_colorAttachmentCount = ColorAttachmentCount(m_colorMode);

    if (m_sampleCount <= 1)
    {
        m_sampleCount = 1;
    }

    const bool usesMsaa = m_sampleCount > 1;

    FramebufferTraceStep(
        std::string("framebuffer create ") + std::to_string(width) + "x" + std::to_string(height)
        + " attachments=" + std::to_string(m_colorAttachmentCount)
        + " samples=" + std::to_string(m_sampleCount));

    const std::uint32_t resolvedRtvCount = static_cast<std::uint32_t>(m_colorAttachmentCount);

    m_resolvedRtvCount = resolvedRtvCount;

    m_rtvBaseIndex = GfxContext::Get().AllocateOffscreenRtvBlock(resolvedRtvCount);

    const bool needsDepth = true;

    m_dsvIndex = GfxContext::Get().AllocateOffscreenDsv();

    if (usesMsaa)

    {

        m_msaaRtvBaseIndex =
            GfxContext::Get().AllocateOffscreenRtvBlock(static_cast<std::uint32_t>(m_colorAttachmentCount));

        m_msaaDsvIndex = GfxContext::Get().AllocateOffscreenDsv();

    }

    if (m_rtvBaseIndex == UINT32_MAX || (needsDepth && m_dsvIndex == UINT32_MAX)

        || (usesMsaa && (m_msaaRtvBaseIndex == UINT32_MAX || m_msaaDsvIndex == UINT32_MAX)))
    {
        const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        ThrowFramebufferError(
            (gpuError.empty() ? std::string("GPU descriptor allocation failed for framebuffer")
                              : gpuError)
            + FormatDescriptorUsageContext());
    }

    FramebufferTraceStep("framebuffer descriptors allocated");
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

            const std::string hresultText = HresultFormat::Format(createHr);
            const std::string hresultName = HresultFormat::Describe(createHr);
            ThrowFramebufferError(
                "Failed to create framebuffer color attachment " + std::to_string(attachmentIndex)
                + " (HRESULT=" + hresultText
                + (hresultName.empty() ? "" : ", " + hresultName) + ")");

        }



        m_colorResources[attachmentIndex] = resource;

        m_colorAllocations[attachmentIndex] = allocation;

        m_colorStates[attachmentIndex] = kShaderResourceState;
        m_colorInitialized[attachmentIndex] = false;

        m_colorSrvIndices[attachmentIndex] = GfxContext::Get().AllocateOffscreenSrv();
        if (m_colorSrvIndices[attachmentIndex] == UINT32_MAX)
        {
            ThrowFramebufferError(
                "Failed to allocate SRV for framebuffer color attachment "
                + std::to_string(attachmentIndex) + FormatDescriptorUsageContext());
        }

        GfxContext::Get().CreateSrvForTexture(

            resource,

            static_cast<int>(format),

            m_colorSrvIndices[attachmentIndex],

            width,

            height);



        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};

        rtvHandle.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(
            m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));

        device->CreateRenderTargetView(resource, nullptr, rtvHandle);

    }



    FramebufferTraceStep("framebuffer resolve color attachments created");

    if (usesMsaa)

    {

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

            resourceDesc.SampleDesc.Count = static_cast<UINT>(m_sampleCount);

            resourceDesc.SampleDesc.Quality = 0;

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

                D3D12_RESOURCE_STATE_RENDER_TARGET,

                &clearValue,

                &allocation,

                IID_PPV_ARGS(&resource));

            if (FAILED(createHr))

            {

                ThrowFramebufferError(
                    "Failed to create MSAA framebuffer color attachment "
                    + std::to_string(attachmentIndex) + " (HRESULT=" + HresultFormat::Format(createHr)
                    + (HresultFormat::Describe(createHr).empty() ? "" : ", " + HresultFormat::Describe(createHr))
                    + ")");

            }



            m_msaaColorResources[attachmentIndex] = resource;

            m_msaaColorAllocations[attachmentIndex] = allocation;

            m_msaaColorStates[attachmentIndex] = kRenderTargetState;
            m_msaaColorInitialized[attachmentIndex] = false;



            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};

            rtvHandle.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(

                m_msaaRtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));

            device->CreateRenderTargetView(resource, nullptr, rtvHandle);

        }

    }



    FramebufferTraceStep("framebuffer msaa color attachments created");

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

            ThrowFramebufferError("Failed to create framebuffer depth attachment");

        }



        m_depthResource = resource;

        m_depthAllocation = allocation;

        m_depthState = kShaderResourceState;
        m_depthInitialized = false;

        m_depthSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
        if (m_depthSrvIndex == UINT32_MAX)
        {
            ThrowFramebufferError(
                "Failed to allocate SRV for framebuffer depth" + FormatDescriptorUsageContext());
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



    if (usesMsaa)

    {

        D3D12_RESOURCE_DESC msaaDepthDesc{};

        msaaDepthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

        msaaDepthDesc.Width = static_cast<UINT64>(width);

        msaaDepthDesc.Height = static_cast<UINT>(height);

        msaaDepthDesc.DepthOrArraySize = 1;

        msaaDepthDesc.MipLevels = 1;

        msaaDepthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

        msaaDepthDesc.SampleDesc.Count = static_cast<UINT>(m_sampleCount);

        msaaDepthDesc.SampleDesc.Quality = 0;

        msaaDepthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        msaaDepthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;



        D3D12_CLEAR_VALUE msaaDepthClear{};

        msaaDepthClear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;

        msaaDepthClear.DepthStencil.Depth = 1.0f;

        msaaDepthClear.DepthStencil.Stencil = 0;



        D3D12MA::ALLOCATION_DESC allocationDesc{};

        allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;



        ID3D12Resource* msaaResource = nullptr;

        D3D12MA::Allocation* msaaAllocation = nullptr;

        if (FAILED(allocator->CreateResource(

                &allocationDesc,

                &msaaDepthDesc,

                D3D12_RESOURCE_STATE_DEPTH_WRITE,

                &msaaDepthClear,

                &msaaAllocation,

                IID_PPV_ARGS(&msaaResource))))

        {

            ThrowFramebufferError("Failed to create MSAA framebuffer depth attachment");

        }



        m_msaaDepthResource = msaaResource;

        m_msaaDepthAllocation = msaaAllocation;

        m_msaaDepthState = kDepthWriteState;
        m_msaaDepthInitialized = false;



        D3D12_CPU_DESCRIPTOR_HANDLE msaaDsvHandle{};

        msaaDsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(m_msaaDsvIndex);

        device->CreateDepthStencilView(msaaResource, nullptr, msaaDsvHandle);

        m_msaaDepthSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
        if (m_msaaDepthSrvIndex == UINT32_MAX)
        {
            ThrowFramebufferError(
                "Failed to allocate SRV for MSAA depth" + FormatDescriptorUsageContext());
        }

        GfxContext::Get().CreateMsaaDepthSrv(msaaResource, m_msaaDepthSrvIndex);

    }

    FramebufferTraceStep("framebuffer create ok");

    }

    catch (const std::exception& exception)

    {

        const std::string detail = !g_lastFramebufferError.empty()
            ? g_lastFramebufferError
            : SafeExceptionMessage(exception);
        Destroy();
        throw std::runtime_error(std::string("Framebuffer create failed: ") + detail);

    }

    catch (...)

    {

        Destroy();

        throw std::runtime_error("Framebuffer create failed: unknown error");

    }

}



bool Framebuffer::Resize(
    const int width,
    const int height,
    const FramebufferColorMode colorMode,
    const int sampleCount)
{
    if (width <= 0 || height <= 0)
    {
        Destroy();
        return false;
    }

    int normalizedSampleCount = sampleCount > 1 ? sampleCount : 1;
    if (normalizedSampleCount > 1
        && (!GfxContext::Get().IsInitialized()
            || !GfxContext::Get().IsMsaaSampleCountSupported(normalizedSampleCount)))
    {
        normalizedSampleCount = 1;
    }

    if (m_width == width && m_height == height && m_colorMode == colorMode
        && m_sampleCount == normalizedSampleCount && m_colorResources[0] != nullptr)
    {
        return true;
    }

    Destroy();
    m_colorMode = colorMode;
    m_sampleCount = normalizedSampleCount;

    try
    {
        Create(width, height);
    }
    catch (const std::exception& exception)
    {
        const std::string detail = !g_lastFramebufferError.empty()
            ? g_lastFramebufferError
            : SafeExceptionMessage(exception);
        Destroy();
        const std::string message = std::string("Framebuffer resize failed (") + std::to_string(width) + "x"
            + std::to_string(height) + ", samples=" + std::to_string(normalizedSampleCount)
            + ", attachments=" + std::to_string(ColorAttachmentCount(colorMode)) + "): "
            + detail;
        EngineLog::Error("framebuffer", message);
        throw std::runtime_error(message);
    }
    catch (...)
    {
        Destroy();
        const std::string message = std::string("Framebuffer resize failed (") + std::to_string(width) + "x"
            + std::to_string(height) + ", samples=" + std::to_string(normalizedSampleCount)
            + ", attachments=" + std::to_string(ColorAttachmentCount(colorMode))
            + "): unknown error";
        EngineLog::Error("framebuffer", message);
        throw std::runtime_error(message);
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

void Framebuffer::TransitionMsaaColorAttachment(const int attachmentIndex, const std::uint32_t newState) const
{
    if (attachmentIndex < 0 || attachmentIndex >= m_colorAttachmentCount
        || m_msaaColorResources[attachmentIndex] == nullptr)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    TransitionResourceState(
        commandList,
        static_cast<ID3D12Resource*>(m_msaaColorResources[attachmentIndex]),
        const_cast<std::uint32_t&>(m_msaaColorStates[attachmentIndex]),
        newState);
}

void Framebuffer::TransitionMsaaDepth(const std::uint32_t newState) const
{
    if (m_msaaDepthResource == nullptr)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    TransitionResourceState(
        commandList,
        static_cast<ID3D12Resource*>(m_msaaDepthResource),
        const_cast<std::uint32_t&>(m_msaaDepthState),
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

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
    if (UsesMsaa())
    {
        TransitionMsaaColorAttachment(0, kRenderTargetState);
        rtvHandle.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(m_msaaRtvBaseIndex);
    }
    else
    {
        TransitionColorAttachment(0, kRenderTargetState);
        rtvHandle.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex);
    }

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
    if (UsesMsaa())
    {
        m_msaaColorInitialized[0] = true;
    }
    else
    {
        m_colorInitialized[0] = true;
    }

    if (UsesMsaa())
    {
        TransitionMsaaColorAttachment(0, kRenderTargetState);
    }
    else
    {
        TransitionColorAttachment(0, kShaderResourceState);
    }
}

void Framebuffer::BindColorRenderTarget(const bool clearAttachments, const float clearColor[4]) const
{
    if (m_colorResources[0] == nullptr || m_rtvBaseIndex == UINT32_MAX)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    if (UsesMsaa())
    {
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            TransitionMsaaColorAttachment(attachmentIndex, kRenderTargetState);
        }

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MaxColorAttachments> rtvs{};
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            rtvs[static_cast<std::size_t>(attachmentIndex)].ptr =
                GfxContext::Get().GetOffscreenRtvCpuHandle(
                    m_msaaRtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));
        }

        commandList->OMSetRenderTargets(
            static_cast<UINT>(m_colorAttachmentCount),
            rtvs.data(),
            FALSE,
            nullptr);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(m_width);
        viewport.Height = static_cast<float>(m_height);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{0, 0, m_width, m_height};
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);

        if (clearAttachments || [&]() {
                for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
                {
                    if (!m_msaaColorInitialized[attachmentIndex])
                    {
                        return true;
                    }
                }
                return false;
            }())
        {
            const float defaultClearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
            const float* resolvedClearColor = clearColor != nullptr ? clearColor : defaultClearColor;
            for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
            {
                if (clearAttachments || !m_msaaColorInitialized[attachmentIndex])
                {
                    commandList->ClearRenderTargetView(
                        rtvs[static_cast<std::size_t>(attachmentIndex)],
                        resolvedClearColor,
                        0,
                        nullptr);
                    m_msaaColorInitialized[attachmentIndex] = true;
                }
            }
        }

        return;
    }

    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        TransitionColorAttachment(attachmentIndex, kRenderTargetState);
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MaxColorAttachments> rtvs{};
    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        rtvs[static_cast<std::size_t>(attachmentIndex)].ptr =
            GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));
    }

    commandList->OMSetRenderTargets(
        static_cast<UINT>(m_colorAttachmentCount),
        rtvs.data(),
        FALSE,
        nullptr);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    if (clearAttachments || [&]() {
            for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
            {
                if (!m_colorInitialized[attachmentIndex])
                {
                    return true;
                }
            }
            return false;
        }())
    {
        const float defaultClearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
        const float* resolvedClearColor = clearColor != nullptr ? clearColor : defaultClearColor;
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            if (clearAttachments || !m_colorInitialized[attachmentIndex])
            {
                commandList->ClearRenderTargetView(
                    rtvs[static_cast<std::size_t>(attachmentIndex)],
                    resolvedClearColor,
                    0,
                    nullptr);
                m_colorInitialized[attachmentIndex] = true;
            }
        }
    }
}

void Framebuffer::PrepareDepthForDepthTestPass() const
{
    if (UsesMsaa())
    {
        TransitionMsaaDepth(kDepthWriteState);
    }
    else
    {
        TransitionDepth(kDepthWriteState);
    }
}

void Framebuffer::PrepareResolvedDepthForDepthTestPass() const
{
    TransitionDepth(kDepthWriteState);
}

bool Framebuffer::BindGizmoDrawTarget() const
{
    if (m_colorResources[0] == nullptr || m_rtvBaseIndex == UINT32_MAX || m_depthResource == nullptr)
    {
        return false;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    TransitionColorAttachment(0, kRenderTargetState);
    if (UsesMsaa())
    {
        PrepareResolvedDepthForDepthTestPass();
    }
    else
    {
        PrepareDepthForDepthTestPass();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE colorRtv{};
    colorRtv.ptr = GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex);

    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    depthDsv.ptr = UsesMsaa()
        ? GetResolvedDepthDsvCpuHandle()
        : GetDepthDsvCpuHandle();

    commandList->OMSetRenderTargets(1, &colorRtv, FALSE, &depthDsv);
    if (!m_colorInitialized[0])
    {
        const float clearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
        commandList->ClearRenderTargetView(colorRtv, clearColor, 0, nullptr);
        m_colorInitialized[0] = true;
    }
    if (!m_depthInitialized)
    {
        commandList->ClearDepthStencilView(
            depthDsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
        m_depthInitialized = true;
    }
    GfxContext::Get().SetBoundOutputFramebuffer(this);
    return true;
}

bool Framebuffer::BindSplitLightingOverlayDrawTarget() const
{
    if (!HasSplitLighting() || m_colorResources[0] == nullptr || m_rtvBaseIndex == UINT32_MAX
        || m_depthResource == nullptr)
    {
        return false;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        TransitionColorAttachment(attachmentIndex, kRenderTargetState);
    }

    if (UsesMsaa())
    {
        PrepareResolvedDepthForDepthTestPass();
    }
    else
    {
        PrepareDepthForDepthTestPass();
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MaxColorAttachments> rtvs{};
    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        rtvs[static_cast<std::size_t>(attachmentIndex)].ptr =
            GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));
    }

    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{};
    depthDsv.ptr = UsesMsaa() ? GetResolvedDepthDsvCpuHandle() : GetDepthDsvCpuHandle();

    commandList->OMSetRenderTargets(
        static_cast<UINT>(m_colorAttachmentCount),
        rtvs.data(),
        FALSE,
        &depthDsv);

    const float clearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        if (!m_colorInitialized[attachmentIndex])
        {
            commandList->ClearRenderTargetView(
                rtvs[static_cast<std::size_t>(attachmentIndex)],
                clearColor,
                0,
                nullptr);
            m_colorInitialized[attachmentIndex] = true;
        }
    }
    if (!m_depthInitialized)
    {
        commandList->ClearDepthStencilView(
            depthDsv,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
        m_depthInitialized = true;
    }

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    GfxContext::Get().SetBoundOutputFramebuffer(this);
    return true;
}

bool Framebuffer::CopyDepthFrom(const Framebuffer& source) const
{
    if (this == &source || m_depthResource == nullptr || source.m_depthResource == nullptr)
    {
        return false;
    }

    if (m_width != source.m_width || m_height != source.m_height)
    {
        return false;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    source.TransitionDepth(kCopySourceState);
    TransitionDepth(kCopyDestState);

    commandList->CopyResource(
        static_cast<ID3D12Resource*>(m_depthResource),
        static_cast<ID3D12Resource*>(source.m_depthResource));
    m_depthInitialized = true;

    source.TransitionDepth(kShaderResourceState);
    return true;
}

void Framebuffer::RestoreDepthShaderResource() const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    TransitionDepth(kShaderResourceState);
}

void Framebuffer::BindDrawTarget(const bool clearAttachments, const float clearColor[4]) const
{
    if (m_colorResources[0] == nullptr || m_rtvBaseIndex == UINT32_MAX)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    if (UsesMsaa())
    {
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            TransitionMsaaColorAttachment(attachmentIndex, kRenderTargetState);
        }

        if (m_msaaDepthResource != nullptr)
        {
            TransitionMsaaDepth(kDepthWriteState);
        }

        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, MaxColorAttachments> rtvs{};
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            rtvs[static_cast<std::size_t>(attachmentIndex)].ptr =
                GfxContext::Get().GetOffscreenRtvCpuHandle(
                    m_msaaRtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));
        }

        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
        const D3D12_CPU_DESCRIPTOR_HANDLE* dsvPointer = nullptr;
        if (m_msaaDepthResource != nullptr)
        {
            dsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(m_msaaDsvIndex);
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

        const bool needsColorInit = [&]() {
            for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
            {
                if (!m_msaaColorInitialized[attachmentIndex])
                {
                    return true;
                }
            }
            return false;
        }();
        const bool needsDepthInit = dsvPointer != nullptr && !m_msaaDepthInitialized;
        if (clearAttachments || needsColorInit || needsDepthInit)
        {
            const float defaultClearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
            const float* resolvedClearColor = clearColor != nullptr ? clearColor : defaultClearColor;
            for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
            {
                if (clearAttachments || !m_msaaColorInitialized[attachmentIndex])
                {
                    commandList->ClearRenderTargetView(
                        rtvs[static_cast<std::size_t>(attachmentIndex)],
                        resolvedClearColor,
                        0,
                        nullptr);
                    m_msaaColorInitialized[attachmentIndex] = true;
                }
            }

            if (dsvPointer != nullptr && (clearAttachments || !m_msaaDepthInitialized))
            {
                commandList->ClearDepthStencilView(
                    dsvHandle,
                    D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                    1.0f,
                    0,
                    0,
                    nullptr);
                m_msaaDepthInitialized = true;
            }
        }

        return;
    }

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

    const bool needsColorInit = [&]() {
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            if (!m_colorInitialized[attachmentIndex])
            {
                return true;
            }
        }
        return false;
    }();
    const bool needsDepthInit = dsvPointer != nullptr && !m_depthInitialized;
    if (clearAttachments || needsColorInit || needsDepthInit)
    {
        const float defaultClearColor[] = {0.08f, 0.09f, 0.15f, 1.0f};
        const float* resolvedClearColor = clearColor != nullptr ? clearColor : defaultClearColor;
        for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
        {
            if (clearAttachments || !m_colorInitialized[attachmentIndex])
            {
                commandList->ClearRenderTargetView(
                    rtvs[static_cast<std::size_t>(attachmentIndex)],
                    resolvedClearColor,
                    0,
                    nullptr);
                m_colorInitialized[attachmentIndex] = true;
            }
        }

        if (dsvPointer != nullptr && (clearAttachments || !m_depthInitialized))
        {
            commandList->ClearDepthStencilView(
                dsvHandle,
                D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                1.0f,
                0,
                0,
                nullptr);
            m_depthInitialized = true;
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
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    EnsureShaderResourceState();
}

void Framebuffer::ResolveMsaa() const
{
    if (!UsesMsaa())
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    for (int attachmentIndex = 0; attachmentIndex < m_colorAttachmentCount; ++attachmentIndex)
    {
        if (m_msaaColorResources[attachmentIndex] == nullptr || m_colorResources[attachmentIndex] == nullptr)
        {
            continue;
        }

        TransitionMsaaColorAttachment(attachmentIndex, kResolveSourceState);
        TransitionColorAttachment(attachmentIndex, kResolveDestState);

        commandList->ResolveSubresource(
            static_cast<ID3D12Resource*>(m_colorResources[attachmentIndex]),
            0,
            static_cast<ID3D12Resource*>(m_msaaColorResources[attachmentIndex]),
            0,
            ColorFormatForAttachment(attachmentIndex, m_colorMode));
        m_colorInitialized[attachmentIndex] = true;

        TransitionColorAttachment(attachmentIndex, kShaderResourceState);
        TransitionMsaaColorAttachment(attachmentIndex, kRenderTargetState);
    }
}

void Framebuffer::BeginMsaaDepthResolvePass() const
{
    if (!UsesMsaa() || m_msaaDepthResource == nullptr || m_depthResource == nullptr)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    TransitionMsaaDepth(kDepthReadState);
    TransitionDepth(kDepthWriteState);

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    dsvHandle.ptr = GfxContext::Get().GetOffscreenDsvCpuHandle(m_dsvIndex);
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    if (!m_depthInitialized)
    {
        commandList->ClearDepthStencilView(
            dsvHandle,
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f,
            0,
            0,
            nullptr);
        m_depthInitialized = true;
    }
}

void Framebuffer::FinishMsaaDepthResolvePass() const
{
    if (!UsesMsaa() || m_msaaDepthResource == nullptr || m_depthResource == nullptr)
    {
        return;
    }

    TransitionDepth(kShaderResourceState);
    TransitionMsaaDepth(kDepthWriteState);
}

std::uintptr_t Framebuffer::GetMsaaDepthSrvCpuHandle() const
{
    if (m_msaaDepthSrvIndex == UINT32_MAX)
    {
        return 0;
    }

    return GfxContext::Get().GetSrvCpuHandle(m_msaaDepthSrvIndex);
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

std::uintptr_t Framebuffer::GetGBufferSrvCpuHandle(const GBufferSlot slot) const
{
    return GetColorSrvCpuHandle(ToGBufferAttachmentIndex(slot));
}

void* Framebuffer::GetGBufferColorResource(const GBufferSlot slot) const
{
    return GetColorResource(ToGBufferAttachmentIndex(slot));
}

void Framebuffer::TransitionGBufferSlot(const GBufferSlot slot, const std::uint32_t newState) const
{
    TransitionColorAttachment(ToGBufferAttachmentIndex(slot), newState);
}

void Framebuffer::TransitionDepthForDxrRead() const
{
    constexpr std::uint32_t kAllShaderRead = static_cast<std::uint32_t>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    TransitionDepth(kAllShaderRead);
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

    if (UsesMsaa())
    {
        if (m_msaaRtvBaseIndex == UINT32_MAX)
        {
            return 0;
        }

        return GfxContext::Get().GetOffscreenRtvCpuHandle(
            m_msaaRtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));
    }

    return GfxContext::Get().GetOffscreenRtvCpuHandle(m_rtvBaseIndex + static_cast<std::uint32_t>(attachmentIndex));

}



std::uintptr_t Framebuffer::GetDepthDsvCpuHandle() const

{

    if (UsesMsaa())
    {
        if (m_msaaDsvIndex == UINT32_MAX)
        {
            return 0;
        }

        return GfxContext::Get().GetOffscreenDsvCpuHandle(m_msaaDsvIndex);
    }

    if (m_dsvIndex == UINT32_MAX)

    {

        return 0;

    }



    return GfxContext::Get().GetOffscreenDsvCpuHandle(m_dsvIndex);

}

std::uintptr_t Framebuffer::GetResolvedDepthDsvCpuHandle() const
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
    const UINT64 kPixelByteSize = isUnorm8 ? 4ull : sizeof(std::uint16_t) * 4ull;
    constexpr UINT64 kPlacedFootprintAlignment = 512u;
    const UINT64 readbackBufferSize =
        ((kPixelByteSize + kPlacedFootprintAlignment - 1) / kPlacedFootprintAlignment)
        * kPlacedFootprintAlignment;
    const UINT rowPitch = static_cast<UINT>(readbackBufferSize);

    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = readbackBufferSize;
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
        destination.PlacedFootprint.Footprint.RowPitch = rowPitch;

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

    D3D12_RANGE readRange{0, static_cast<SIZE_T>(kPixelByteSize)};
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
