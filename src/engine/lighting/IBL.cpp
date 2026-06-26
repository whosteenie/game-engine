#include "engine/lighting/IBL.h"
#include "engine/lighting/IrradianceSh.h"

#include "engine/rendering/Constants.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    const float kCubeVertices[] = {
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
    };

    const float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
    };

    std::array<glm::mat4, 6> BuildCaptureViews()
    {
        return {
            glm::lookAtLH(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAtLH(glm::vec3(0.0f), glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAtLH(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f)),
            glm::lookAtLH(glm::vec3(0.0f), glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f)),
            glm::lookAtLH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
            glm::lookAtLH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f)),
        };
    }

    D3D12_RESOURCE_STATES TransitionResource(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        if (before == after)
        {
            return after;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commandList->ResourceBarrier(1, &barrier);
        return after;
    }

    void CreateCubemapSrv(ID3D12Device* device, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, std::uint32_t mipLevels)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.TextureCube.MipLevels = mipLevels;
        device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);
    }

    void CreateTexture2DSrv(
        ID3D12Device* device,
        ID3D12Resource* resource,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        DXGI_FORMAT format,
        std::uint32_t mipLevels)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = mipLevels;
        device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);
    }
}

IBL::GpuTexture IBL::CreateCubemapTextureResource(
    const std::uint32_t resolution,
    const std::uint32_t mipLevels,
    const std::uint32_t initialState)
{
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    GpuTexture texture{};
    texture.mipLevels = mipLevels;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = resolution;
    resourceDesc.Height = resolution;
    resourceDesc.DepthOrArraySize = 6;
    resourceDesc.MipLevels = mipLevels;
    resourceDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            static_cast<D3D12_RESOURCE_STATES>(initialState),
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error("Failed to create IBL cubemap texture");
    }

    texture.resource = resource;
    texture.allocation = allocation;
    texture.srvDescriptorIndex = GfxContext::Get().AllocateOffscreenSrv();
    texture.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(texture.srvDescriptorIndex);
    CreateCubemapSrv(
        static_cast<ID3D12Device*>(GfxContext::Get().GetDevice()),
        resource,
        {texture.srvCpuHandle},
        mipLevels);
    return texture;
}

IBL::GpuTexture IBL::CreateRenderTargetTexture2DResource(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t format,
    const std::uint32_t initialState)
{
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    GpuTexture texture{};
    texture.mipLevels = 1;

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = width;
    resourceDesc.Height = height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = static_cast<DXGI_FORMAT>(format);
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            static_cast<D3D12_RESOURCE_STATES>(initialState),
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error("Failed to create IBL render target texture");
    }

    texture.resource = resource;
    texture.allocation = allocation;
    texture.srvDescriptorIndex = GfxContext::Get().AllocateOffscreenSrv();
    texture.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(texture.srvDescriptorIndex);
    CreateTexture2DSrv(
        static_cast<ID3D12Device*>(GfxContext::Get().GetDevice()),
        resource,
        {texture.srvCpuHandle},
        static_cast<DXGI_FORMAT>(format),
        1);
    return texture;
}

IBL::IBL(const char* hdrPath)
    : m_hdrPath(hdrPath != nullptr ? hdrPath : "")
{
}

IBL::~IBL()
{
    DestroyResources();
}

IBL::IBL(IBL&& other) noexcept
    : m_hdrGpu(other.m_hdrGpu),
      m_environmentCubemapGpu(other.m_environmentCubemapGpu),
      m_irradianceSh(other.m_irradianceSh),
      m_prefilterMapGpu(other.m_prefilterMapGpu),
      m_brdfLutGpu(other.m_brdfLutGpu),
      m_cubeVb(std::move(other.m_cubeVb)),
      m_quadVb(std::move(other.m_quadVb)),
      m_captureDepthResource(other.m_captureDepthResource),
      m_captureDepthAllocation(other.m_captureDepthAllocation),
      m_captureDepthDsvIndex(other.m_captureDepthDsvIndex),
      m_captureRtvIndex(other.m_captureRtvIndex),
      m_activeCaptureTarget(other.m_activeCaptureTarget),
      m_gpuGenerated(other.m_gpuGenerated),
      m_hdrPath(std::move(other.m_hdrPath)),
      m_loadError(std::move(other.m_loadError)),
      m_rotationYRadians(other.m_rotationYRadians),
      m_maxPrefilterMipLevel(other.m_maxPrefilterMipLevel),
      m_environmentIntensity(other.m_environmentIntensity),
      m_hdrWidth(other.m_hdrWidth),
      m_hdrHeight(other.m_hdrHeight),
      m_cubemapResolutionMode(other.m_cubemapResolutionMode),
      m_captureDepthWidth(other.m_captureDepthWidth)
{
    other.m_hdrGpu = {};
    other.m_environmentCubemapGpu = {};
    other.m_irradianceSh = {};
    other.m_prefilterMapGpu = {};
    other.m_brdfLutGpu = {};
    other.m_captureDepthResource = nullptr;
    other.m_captureDepthAllocation = nullptr;
    other.m_captureDepthDsvIndex = UINT32_MAX;
    other.m_captureRtvIndex = UINT32_MAX;
    other.m_activeCaptureTarget = nullptr;
    other.m_gpuGenerated = false;
    other.m_hdrPath.clear();
    other.m_loadError.clear();
    other.m_rotationYRadians = 0.0f;
    other.m_hdrWidth = 0;
    other.m_hdrHeight = 0;
    other.m_cubemapResolutionMode = EnvironmentIblCubemapResolution::Auto;
    other.m_captureDepthWidth = 0;
}

IBL& IBL::operator=(IBL&& other) noexcept
{
    if (this != &other)
    {
        DestroyResources();
        m_hdrGpu = other.m_hdrGpu;
        m_environmentCubemapGpu = other.m_environmentCubemapGpu;
        m_irradianceSh = other.m_irradianceSh;
        m_prefilterMapGpu = other.m_prefilterMapGpu;
        m_brdfLutGpu = other.m_brdfLutGpu;
        m_cubeVb = std::move(other.m_cubeVb);
        m_quadVb = std::move(other.m_quadVb);
        m_captureDepthResource = other.m_captureDepthResource;
        m_captureDepthAllocation = other.m_captureDepthAllocation;
        m_captureDepthDsvIndex = other.m_captureDepthDsvIndex;
        m_captureRtvIndex = other.m_captureRtvIndex;
        m_activeCaptureTarget = other.m_activeCaptureTarget;
        m_gpuGenerated = other.m_gpuGenerated;
        m_hdrPath = std::move(other.m_hdrPath);
        m_loadError = std::move(other.m_loadError);
        m_rotationYRadians = other.m_rotationYRadians;
        m_maxPrefilterMipLevel = other.m_maxPrefilterMipLevel;
        m_environmentIntensity = other.m_environmentIntensity;
        m_hdrWidth = other.m_hdrWidth;
        m_hdrHeight = other.m_hdrHeight;
        m_cubemapResolutionMode = other.m_cubemapResolutionMode;
        m_captureDepthWidth = other.m_captureDepthWidth;

        other.m_hdrGpu = {};
        other.m_environmentCubemapGpu = {};
        other.m_irradianceSh = {};
        other.m_prefilterMapGpu = {};
        other.m_brdfLutGpu = {};
        other.m_captureDepthResource = nullptr;
        other.m_captureDepthAllocation = nullptr;
        other.m_captureDepthDsvIndex = UINT32_MAX;
        other.m_captureRtvIndex = UINT32_MAX;
        other.m_activeCaptureTarget = nullptr;
        other.m_gpuGenerated = false;
        other.m_hdrPath.clear();
        other.m_loadError.clear();
        other.m_rotationYRadians = 0.0f;
    }

    return *this;
}

void IBL::DestroyGpuTexture(GpuTexture& texture)
{
    if (!GfxContext::Get().IsInitialized())
    {
        texture = {};
        return;
    }

    if (texture.srvDescriptorIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(texture.srvDescriptorIndex);
    }

    if (texture.allocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(texture.allocation)->Release();
    }

    texture = {};
}

void IBL::DestroyEnvironmentTextures()
{
    DestroyGpuTexture(m_hdrGpu);
    DestroyGpuTexture(m_environmentCubemapGpu);
    DestroyGpuTexture(m_prefilterMapGpu);
    m_irradianceSh = {};
    m_gpuGenerated = false;
    m_loadError.clear();
}

void IBL::DestroyResources()
{
    DestroyEnvironmentTextures();
    DestroyGpuTexture(m_brdfLutGpu);

    if (GfxContext::Get().IsInitialized())
    {
        if (m_captureRtvIndex != UINT32_MAX)
        {
            GfxContext::Get().FreeOffscreenRtvBlock(m_captureRtvIndex, 1);
            m_captureRtvIndex = UINT32_MAX;
        }

        if (m_captureDepthDsvIndex != UINT32_MAX)
        {
            GfxContext::Get().FreeOffscreenDsv(m_captureDepthDsvIndex);
            m_captureDepthDsvIndex = UINT32_MAX;
        }
    }

    if (m_captureDepthAllocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(m_captureDepthAllocation)->Release();
        m_captureDepthAllocation = nullptr;
    }

    m_captureDepthResource = nullptr;
    m_captureDepthWidth = 0;
    m_gpuGenerated = false;
}

void IBL::CreateCaptureResources()
{
    m_cubeVb.Create(GpuBuffer::Type::Vertex, kCubeVertices, static_cast<std::uint32_t>(sizeof(kCubeVertices)));
    m_quadVb.Create(GpuBuffer::Type::Vertex, kQuadVertices, static_cast<std::uint32_t>(sizeof(kQuadVertices)));

    m_captureRtvIndex = GfxContext::Get().AllocateOffscreenRtvBlock(1);
    m_captureDepthDsvIndex = GfxContext::Get().AllocateOffscreenDsv();
}

void IBL::EnsureCaptureDepthBuffer(const std::uint32_t resolution)
{
    if (m_captureDepthResource != nullptr && m_captureDepthWidth >= resolution)
    {
        return;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    if (m_captureDepthAllocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(m_captureDepthAllocation)->Release();
        m_captureDepthAllocation = nullptr;
    }

    m_captureDepthResource = nullptr;
    m_captureDepthWidth = 0;

    D3D12_RESOURCE_DESC depthDesc{};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = resolution;
    depthDesc.Height = resolution;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12MA::ALLOCATION_DESC depthAllocationDesc{};
    depthAllocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* depthResource = nullptr;
    D3D12MA::Allocation* depthAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &depthAllocationDesc,
            &depthDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            nullptr,
            &depthAllocation,
            IID_PPV_ARGS(&depthResource))))
    {
        throw std::runtime_error("Failed to create IBL capture depth texture");
    }

    m_captureDepthResource = depthResource;
    m_captureDepthAllocation = depthAllocation;
    m_captureDepthWidth = resolution;

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(
        depthResource,
        &dsvDesc,
        {GfxContext::Get().GetOffscreenDsvCpuHandle(m_captureDepthDsvIndex)});
}

void IBL::LoadHdrEquirectangular(const char* hdrPath)
{
    stbi_set_flip_vertically_on_load(true);

    int width = 0;
    int height = 0;
    int channels = 0;
    float* imageData = stbi_loadf(hdrPath, &width, &height, &channels, 0);
    if (imageData == nullptr)
    {
        throw std::runtime_error(std::string("Failed to load HDR environment map: ") + hdrPath);
    }

    const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> rgba(pixelCount * 4);
    const int sourceChannels = channels > 0 ? channels : 3;
    for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
    {
        const float* source = imageData + pixelIndex * static_cast<std::size_t>(sourceChannels);
        float* destination = rgba.data() + pixelIndex * 4;
        destination[0] = source[0];
        destination[1] = sourceChannels > 1 ? source[1] : source[0];
        destination[2] = sourceChannels > 2 ? source[2] : source[0];
        destination[3] = 1.0f;
    }

    stbi_image_free(imageData);

    m_hdrWidth = width;
    m_hdrHeight = height;

    m_irradianceSh = ProjectIrradianceSh9FromEquirect(rgba, width, height);

    float maxHdrChannel = 0.0f;
    for (const float channel : rgba)
    {
        maxHdrChannel = std::max(maxHdrChannel, channel);
    }
    if (maxHdrChannel <= 0.0f)
    {
        throw std::runtime_error("HDR environment map contains no positive radiance data");
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT64>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12MA::ALLOCATION_DESC textureAllocationDesc{};
    textureAllocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* textureResource = nullptr;
    D3D12MA::Allocation* textureAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &textureAllocationDesc,
            &textureDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            &textureAllocation,
            IID_PPV_ARGS(&textureResource))))
    {
        throw std::runtime_error("Failed to create HDR texture resource");
    }

    UINT rowCount = 0;
    UINT64 rowPitch = 0;
    UINT64 imageSize = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT textureFootprint{};
    device->GetCopyableFootprints(
        &textureDesc,
        0,
        1,
        0,
        &textureFootprint,
        &rowCount,
        &rowPitch,
        &imageSize);

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = imageSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC uploadAllocationDesc{};
    uploadAllocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

    ID3D12Resource* uploadResource = nullptr;
    D3D12MA::Allocation* uploadAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &uploadAllocationDesc,
            &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            &uploadAllocation,
            IID_PPV_ARGS(&uploadResource))))
    {
        textureAllocation->Release();
        textureResource->Release();
        throw std::runtime_error("Failed to create HDR upload buffer");
    }

    void* mapped = nullptr;
    if (FAILED(uploadResource->Map(0, nullptr, &mapped)))
    {
        uploadAllocation->Release();
        uploadResource->Release();
        textureAllocation->Release();
        textureResource->Release();
        throw std::runtime_error("Failed to map HDR upload buffer");
    }

    for (int row = 0; row < height; ++row)
    {
        std::memcpy(
            static_cast<unsigned char*>(mapped) + static_cast<std::size_t>(row) * rowPitch,
            rgba.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4,
            static_cast<std::size_t>(width) * 4 * sizeof(float));
    }
    uploadResource->Unmap(0, nullptr);

    ID3D12Resource* textureResourcePtr = textureResource;
    GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPointer);

        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = uploadResource;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint = textureFootprint;

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = textureResourcePtr;
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;
        commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

        TransitionResource(
            commandList,
            textureResourcePtr,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });

    uploadAllocation->Release();
    uploadResource->Release();

    m_hdrGpu.resource = textureResource;
    m_hdrGpu.allocation = textureAllocation;
    m_hdrGpu.mipLevels = 1;
    m_hdrGpu.srvDescriptorIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_hdrGpu.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(m_hdrGpu.srvDescriptorIndex);
    CreateTexture2DSrv(
        device,
        textureResource,
        {m_hdrGpu.srvCpuHandle},
        DXGI_FORMAT_R32G32B32A32_FLOAT,
        1);
}

void IBL::CaptureCubemapFaces(
    unsigned int /*targetCubemap*/,
    Shader& shader,
    unsigned int resolution,
    unsigned int mipLevel,
    bool /*generateMipmapsAfter*/)
{
    if (m_activeCaptureTarget == nullptr)
    {
        return;
    }

    GpuTexture& target = *m_activeCaptureTarget;
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    const glm::mat4 captureProjection = glm::perspectiveLH_ZO(glm::radians(90.0f), 1.0f, 0.1f, 10.0f);
    const auto captureViews = BuildCaptureViews();

    const D3D12_CPU_DESCRIPTOR_HANDLE captureRtvHandle{
        GfxContext::Get().GetOffscreenRtvCpuHandle(m_captureRtvIndex)};
    const D3D12_CPU_DESCRIPTOR_HANDLE captureDsvHandle{
        GfxContext::Get().GetOffscreenDsvCpuHandle(m_captureDepthDsvIndex)};

    ID3D12Resource* targetResource = static_cast<ID3D12Resource*>(target.resource);
    ID3D12Resource* depthResource = static_cast<ID3D12Resource*>(m_captureDepthResource);

    GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPointer);

        TransitionResource(
            commandList,
            targetResource,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        TransitionResource(
            commandList,
            depthResource,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            D3D12_RESOURCE_STATE_DEPTH_WRITE);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(resolution);
        viewport.Height = static_cast<float>(resolution);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{0, 0, static_cast<LONG>(resolution), static_cast<LONG>(resolution)};

        shader.UseOnCommandList(commandList);
        shader.SetMat4("uProjection", captureProjection);

        const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};

        for (unsigned int face = 0; face < 6; ++face)
        {
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.ArraySize = 1;
            rtvDesc.Texture2DArray.FirstArraySlice = face;
            rtvDesc.Texture2DArray.MipSlice = mipLevel;
            device->CreateRenderTargetView(targetResource, &rtvDesc, captureRtvHandle);

            commandList->OMSetRenderTargets(1, &captureRtvHandle, FALSE, &captureDsvHandle);
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->ClearRenderTargetView(captureRtvHandle, clearColor, 0, nullptr);
            commandList->ClearDepthStencilView(captureDsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            shader.SetMat4("uView", captureViews[face]);
            shader.FlushUniformsOnCommandList(commandList);

            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            m_cubeVb.BindVertexToCommandList(commandList, 0, 3 * sizeof(float));
            commandList->DrawInstanced(36, 1, 0, 0);
        }

        TransitionResource(
            commandList,
            targetResource,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });
}

void IBL::CreateEnvironmentCubemap()
{
    const std::uint32_t faceResolution = ResolveCubemapFaceResolution();
    EnsureCaptureDepthBuffer(faceResolution);

    m_environmentCubemapGpu = CreateCubemapTextureResource(
        faceResolution,
        1,
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    Shader equirectShader(
        EngineConstants::IblCubemapVertexShader,
        EngineConstants::IblEquirectToCubemapFragmentShader);
    equirectShader.BindTextureSlot(0, m_hdrGpu.srvCpuHandle);
    equirectShader.SetInt("uEquirectangularMap", 0);
    equirectShader.SetFloat("uRotationY", m_rotationYRadians);

    m_activeCaptureTarget = &m_environmentCubemapGpu;
    CaptureCubemapFaces(0, equirectShader, faceResolution, 0, false);
    m_activeCaptureTarget = nullptr;
}

void IBL::CreatePrefilterMap()
{
    const std::uint32_t cubemapResolution = ResolveCubemapFaceResolution();
    const unsigned int prefilterResolution =
        std::clamp(cubemapResolution / 4u, 128u, 512u);
    const unsigned int mipLevels =
        static_cast<unsigned int>(std::log2(static_cast<double>(prefilterResolution))) + 1u;
    m_maxPrefilterMipLevel = static_cast<float>(mipLevels - 1u);

    m_prefilterMapGpu = CreateCubemapTextureResource(
        prefilterResolution,
        mipLevels,
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    Shader prefilterShader(
        EngineConstants::IblCubemapVertexShader,
        EngineConstants::IblPrefilterFragmentShader);
    prefilterShader.BindTextureSlot(0, m_environmentCubemapGpu.srvCpuHandle);
    prefilterShader.SetInt("uEnvironmentMap", 0);

    for (unsigned int mip = 0; mip < mipLevels; ++mip)
    {
        const unsigned int mipWidth =
            prefilterResolution * static_cast<unsigned int>(std::pow(0.5, static_cast<double>(mip)));
        const float roughness = mipLevels <= 1
            ? 0.0f
            : static_cast<float>(mip) / static_cast<float>(mipLevels - 1);
        prefilterShader.SetFloat("uRoughness", roughness);
        m_activeCaptureTarget = &m_prefilterMapGpu;
        CaptureCubemapFaces(0, prefilterShader, mipWidth, mip, false);
    }

    m_activeCaptureTarget = nullptr;
}

void IBL::CreateBrdfLut()
{
    const unsigned int lutSize = 512;

    m_brdfLutGpu = CreateRenderTargetTexture2DResource(
        lutSize,
        lutSize,
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    const D3D12_CPU_DESCRIPTOR_HANDLE captureRtvHandle{
        GfxContext::Get().GetOffscreenRtvCpuHandle(m_captureRtvIndex)};
    ID3D12Resource* brdfResource = static_cast<ID3D12Resource*>(m_brdfLutGpu.resource);

    Shader brdfShader(EngineConstants::IblBrdfVertexShader, EngineConstants::IblBrdfFragmentShader);

    GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPointer);

        TransitionResource(
            commandList,
            brdfResource,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);

        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
        rtvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(brdfResource, &rtvDesc, captureRtvHandle);

        D3D12_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(lutSize);
        viewport.Height = static_cast<float>(lutSize);
        viewport.MaxDepth = 1.0f;
        D3D12_RECT scissor{0, 0, static_cast<LONG>(lutSize), static_cast<LONG>(lutSize)};

        commandList->OMSetRenderTargets(1, &captureRtvHandle, FALSE, nullptr);
        commandList->RSSetViewports(1, &viewport);
        commandList->RSSetScissorRects(1, &scissor);
        const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
        commandList->ClearRenderTargetView(captureRtvHandle, clearColor, 0, nullptr);

        brdfShader.UseOnCommandList(commandList);
        brdfShader.FlushUniformsOnCommandList(commandList);
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_quadVb.BindVertexToCommandList(commandList, 0, 4 * sizeof(float));
        commandList->DrawInstanced(6, 1, 0, 0);

        TransitionResource(
            commandList,
            brdfResource,
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });
}

void IBL::GenerateGpuResources()
{
    if (m_gpuGenerated)
    {
        return;
    }

    try
    {
        if (m_captureDepthResource == nullptr)
        {
            CreateCaptureResources();
        }

        if (m_hdrGpu.resource == nullptr)
        {
            LoadHdrEquirectangular(m_hdrPath.c_str());
        }

        CreateEnvironmentCubemap();
        CreatePrefilterMap();
        if (m_brdfLutGpu.resource == nullptr)
        {
            CreateBrdfLut();
        }
        m_gpuGenerated = true;
        m_loadError.clear();
    }
    catch (const std::exception& exception)
    {
        m_loadError = exception.what();
        DestroyEnvironmentTextures();
        throw;
    }
    catch (...)
    {
        m_loadError = "unknown IBL generation error";
        DestroyEnvironmentTextures();
        throw;
    }
}

void IBL::ReloadFromHdr(const char* hdrPath, const float rotationYRadians)
{
    const std::string nextPath = hdrPath != nullptr ? hdrPath : "";
    if (nextPath.empty())
    {
        throw std::runtime_error("HDR environment path is empty");
    }

    if (m_gpuGenerated && nextPath == m_hdrPath && rotationYRadians == m_rotationYRadians)
    {
        return;
    }

    m_hdrPath = nextPath;
    m_rotationYRadians = rotationYRadians;
    DestroyEnvironmentTextures();

    try
    {
        if (m_captureDepthResource == nullptr)
        {
            CreateCaptureResources();
        }

        LoadHdrEquirectangular(m_hdrPath.c_str());
        CreateEnvironmentCubemap();
        CreatePrefilterMap();
        if (m_brdfLutGpu.resource == nullptr)
        {
            CreateBrdfLut();
        }
        m_gpuGenerated = true;
        m_loadError.clear();
    }
    catch (const std::exception& exception)
    {
        m_loadError = exception.what();
        DestroyEnvironmentTextures();
        throw;
    }
    catch (...)
    {
        m_loadError = "unknown IBL reload error";
        DestroyEnvironmentTextures();
        throw;
    }
}

bool IBL::IsReady() const
{
    return m_gpuGenerated;
}

const std::string& IBL::GetLoadError() const
{
    return m_loadError;
}

const std::string& IBL::GetHdrPath() const
{
    return m_hdrPath;
}

float IBL::GetRotationYRadians() const
{
    return m_rotationYRadians;
}

std::uintptr_t IBL::GetEnvironmentCubemapSrvCpuHandle() const
{
    if (!m_gpuGenerated)
    {
        try
        {
            const_cast<IBL*>(this)->GenerateGpuResources();
        }
        catch (...)
        {
            return 0;
        }
    }

    if (!m_gpuGenerated)
    {
        return 0;
    }

    return m_environmentCubemapGpu.srvCpuHandle;
}

std::uintptr_t IBL::GetHdrEquirectSrvCpuHandle() const
{
    if (!m_gpuGenerated)
    {
        try
        {
            const_cast<IBL*>(this)->GenerateGpuResources();
        }
        catch (...)
        {
            return 0;
        }
    }

    if (!m_gpuGenerated || m_hdrGpu.srvCpuHandle == 0)
    {
        return 0;
    }

    return m_hdrGpu.srvCpuHandle;
}

void IBL::BindTextures(Shader& shader) const
{
    if (!m_gpuGenerated)
    {
        try
        {
            const_cast<IBL*>(this)->GenerateGpuResources();
        }
        catch (...)
        {
            return;
        }
    }

    if (!m_gpuGenerated)
    {
        return;
    }

    shader.SetVec4Array(
        "uIrradianceSh",
        m_irradianceSh.coefficients.data(),
        static_cast<int>(m_irradianceSh.coefficients.size()));

    shader.BindTextureSlot(2, m_prefilterMapGpu.srvCpuHandle);
    shader.SetInt("uPrefilterMap", 2);

    shader.BindTextureSlot(3, m_brdfLutGpu.srvCpuHandle);
    shader.SetInt("uBrdfLut", 3);

    shader.SetFloat("uMaxReflectionLod", m_maxPrefilterMipLevel);
    shader.SetFloat("uEnvironmentIntensity", m_environmentIntensity);
}

float IBL::GetMaxReflectionLod() const
{
    return m_maxPrefilterMipLevel;
}

float IBL::GetEnvironmentIntensity() const
{
    return m_environmentIntensity;
}

void IBL::SetEnvironmentIntensity(float intensity)
{
    m_environmentIntensity = intensity;
}

std::uint32_t IBL::ResolveCubemapFaceResolution() const
{
    return ResolveEnvironmentCubemapFaceResolution(
        m_cubemapResolutionMode,
        m_hdrWidth,
        m_hdrHeight);
}

EnvironmentIblCubemapResolution IBL::GetCubemapResolutionMode() const
{
    return m_cubemapResolutionMode;
}

void IBL::SetCubemapResolutionMode(const EnvironmentIblCubemapResolution mode)
{
    if (mode == m_cubemapResolutionMode)
    {
        return;
    }

    m_cubemapResolutionMode = mode;
    if (!m_gpuGenerated || m_hdrGpu.resource == nullptr)
    {
        return;
    }

    try
    {
        if (m_captureDepthResource == nullptr)
        {
            CreateCaptureResources();
        }

        DestroyGpuTexture(m_environmentCubemapGpu);
        DestroyGpuTexture(m_prefilterMapGpu);
        CreateEnvironmentCubemap();
        CreatePrefilterMap();
        m_loadError.clear();
    }
    catch (const std::exception& exception)
    {
        m_loadError = exception.what();
        DestroyEnvironmentTextures();
        throw;
    }
}

std::uint32_t IBL::GetCubemapFaceResolution() const
{
    if (m_hdrWidth > 0 || m_hdrHeight > 0)
    {
        return ResolveCubemapFaceResolution();
    }

    if (m_cubemapResolutionMode != EnvironmentIblCubemapResolution::Auto)
    {
        return static_cast<std::uint32_t>(m_cubemapResolutionMode);
    }

    return 1024;
}

void IBL::GetHdrDimensions(int& width, int& height) const
{
    width = m_hdrWidth;
    height = m_hdrHeight;
}
