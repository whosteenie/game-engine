#include "engine/rendering/resources/Texture.h"

#include "engine/platform/system/BackgroundWork.h"

#include "engine/rendering/core/Constants.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
    };

    std::unique_ptr<GpuBuffer> g_mipmapQuadBuffer;
    std::unique_ptr<Shader> g_mipmapGenShader;

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    GpuBuffer& GetMipmapQuadBuffer()
    {
        if (g_mipmapQuadBuffer == nullptr)
        {
            g_mipmapQuadBuffer = std::make_unique<GpuBuffer>();
            g_mipmapQuadBuffer->Create(
                GpuBuffer::Type::Vertex,
                kQuadVertices,
                static_cast<std::uint32_t>(sizeof(kQuadVertices)));
        }

        return *g_mipmapQuadBuffer;
    }

    Shader& GetMipmapGenShader()
    {
        if (g_mipmapGenShader == nullptr)
        {
            g_mipmapGenShader = std::make_unique<Shader>(
                EngineConstants::FullscreenVertexShader,
                EngineConstants::MipmapGenFragmentShader);
        }

        return *g_mipmapGenShader;
    }

    void TransitionResource(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES before,
        D3D12_RESOURCE_STATES after)
    {
        if (before == after || resource == nullptr)
        {
            return;
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        commandList->ResourceBarrier(1, &barrier);
    }

    void RecordTextureCopyAndTransition(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* textureResource,
        ID3D12Resource* uploadResource,
        const std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>& uploadLayouts,
        const std::uint32_t mipLevels)
    {
        for (std::uint32_t mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
        {
            D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
            sourceLocation.pResource = uploadResource;
            sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sourceLocation.PlacedFootprint = uploadLayouts[static_cast<std::size_t>(mipLevel)];

            D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
            destinationLocation.pResource = textureResource;
            destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destinationLocation.SubresourceIndex = mipLevel;
            commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);
        }

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = textureResource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = kAllShaderRead;
        commandList->ResourceBarrier(1, &barrier);
    }

    std::uint32_t CalculateMipLevelCount(int width, int height)
    {
        const int largestDimension = std::max(width, height);
        if (largestDimension <= 1)
        {
            return 1;
        }

        return 1 + static_cast<std::uint32_t>(std::floor(std::log2(static_cast<double>(largestDimension))));
    }

    struct CpuMipLevel
    {
        int width = 1;
        int height = 1;
        std::vector<unsigned char> pixels;
    };

    std::vector<CpuMipLevel> GenerateCpuMipChain(
        const std::vector<unsigned char>& basePixels,
        const int baseWidth,
        const int baseHeight,
        const std::uint32_t mipLevels)
    {
        std::vector<CpuMipLevel> mipChain;
        mipChain.reserve(mipLevels);
        mipChain.push_back(CpuMipLevel{baseWidth, baseHeight, basePixels});

        for (std::uint32_t mipLevel = 1; mipLevel < mipLevels; ++mipLevel)
        {
            const CpuMipLevel& source = mipChain.back();
            CpuMipLevel dest;
            dest.width = std::max(1, source.width / 2);
            dest.height = std::max(1, source.height / 2);
            dest.pixels.resize(static_cast<std::size_t>(dest.width) * static_cast<std::size_t>(dest.height) * 4);

            const auto downsampleRows = [&source, &dest](const int beginY, const int endY) {
                for (int y = beginY; y < endY; ++y)
                {
                    for (int x = 0; x < dest.width; ++x)
                    {
                        unsigned int sum[4] = {};
                        for (int offsetY = 0; offsetY < 2; ++offsetY)
                        {
                            for (int offsetX = 0; offsetX < 2; ++offsetX)
                            {
                                const int sourceX = std::min(source.width - 1, x * 2 + offsetX);
                                const int sourceY = std::min(source.height - 1, y * 2 + offsetY);
                                const std::size_t sourceIndex =
                                    (static_cast<std::size_t>(sourceY) * static_cast<std::size_t>(source.width)
                                    + static_cast<std::size_t>(sourceX)) * 4;
                                for (int channel = 0; channel < 4; ++channel)
                                {
                                    sum[channel] += source.pixels[sourceIndex + static_cast<std::size_t>(channel)];
                                }
                            }
                        }

                        const std::size_t destIndex =
                            (static_cast<std::size_t>(y) * static_cast<std::size_t>(dest.width)
                            + static_cast<std::size_t>(x)) * 4;
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            dest.pixels[destIndex + static_cast<std::size_t>(channel)] =
                                static_cast<unsigned char>((sum[channel] + 2) / 4);
                        }
                    }
                }
            };

            // Each output scanline writes to a disjoint range. The larger top mip levels dominate
            // texture import time, so spread only those across a bounded number of CPU workers;
            // tiny tail levels stay serial to avoid thread-launch overhead.
            constexpr std::size_t kParallelMipPixelThreshold = 64u * 1024u;
            const std::size_t destPixelCount =
                static_cast<std::size_t>(dest.width) * static_cast<std::size_t>(dest.height);
            const unsigned int workerCount = static_cast<unsigned int>(
                BackgroundWork::ResponsiveWorkerCount(
                    static_cast<std::size_t>(dest.height),
                    std::thread::hardware_concurrency()));
            if (destPixelCount < kParallelMipPixelThreshold || workerCount <= 1)
            {
                downsampleRows(0, dest.height);
            }
            else
            {
                std::vector<std::thread> workers;
                workers.reserve(workerCount);
                for (unsigned int workerIndex = 0; workerIndex < workerCount; ++workerIndex)
                {
                    const int beginY = static_cast<int>(
                        (static_cast<long long>(dest.height) * workerIndex) / workerCount);
                    const int endY = static_cast<int>(
                        (static_cast<long long>(dest.height) * (workerIndex + 1)) / workerCount);
                    workers.emplace_back([&, beginY, endY]() {
                        BackgroundWork::LowerCurrentThreadPriority();
                        downsampleRows(beginY, endY);
                    });
                }
                for (std::thread& worker : workers)
                {
                    worker.join();
                }
            }

            mipChain.push_back(std::move(dest));
        }

        return mipChain;
    }

    void RecordGenerateMipmapChain(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Device* device,
        ID3D12Resource* textureResource,
        DXGI_FORMAT format,
        int width,
        int height,
        std::uint32_t mipLevels,
        const D3D12_CPU_DESCRIPTOR_HANDLE tempRtvHandle,
        const D3D12_CPU_DESCRIPTOR_HANDLE tempSrvHandle,
        const std::uint32_t tempSrvIndex,
        Shader& mipShader,
        GpuBuffer& quadBuffer)
    {
        int sourceWidth = width;
        int sourceHeight = height;

        for (std::uint32_t mipLevel = 1; mipLevel < mipLevels; ++mipLevel)
        {
            const int destWidth = std::max(1, sourceWidth / 2);
            const int destHeight = std::max(1, sourceHeight / 2);

            D3D12_SHADER_RESOURCE_VIEW_DESC sourceSrvDesc{};
            sourceSrvDesc.Format = format;
            sourceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sourceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sourceSrvDesc.Texture2D.MostDetailedMip = mipLevel - 1;
            sourceSrvDesc.Texture2D.MipLevels = 1;
            GfxContext::Get().CreateShaderResourceView(textureResource, &sourceSrvDesc, tempSrvIndex);

            D3D12_RENDER_TARGET_VIEW_DESC destRtvDesc{};
            destRtvDesc.Format = format;
            destRtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            destRtvDesc.Texture2D.MipSlice = mipLevel;
            device->CreateRenderTargetView(textureResource, &destRtvDesc, tempRtvHandle);

            TransitionResource(
                commandList,
                textureResource,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_VIEWPORT viewport{};
            viewport.Width = static_cast<float>(destWidth);
            viewport.Height = static_cast<float>(destHeight);
            viewport.MaxDepth = 1.0f;
            D3D12_RECT scissor{0, 0, destWidth, destHeight};
            commandList->RSSetViewports(1, &viewport);
            commandList->RSSetScissorRects(1, &scissor);
            commandList->OMSetRenderTargets(1, &tempRtvHandle, FALSE, nullptr);

            mipShader.BindPipeline(false, true);
            mipShader.SetFloat("uTexelSizeX", 1.0f / static_cast<float>(sourceWidth));
            mipShader.SetFloat("uTexelSizeY", 1.0f / static_cast<float>(sourceHeight));
            mipShader.BindTextureSlot(0, tempSrvHandle.ptr);
            mipShader.FlushUniforms();

            ID3D12DescriptorHeap* heaps[] = {
                static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap())};
            commandList->SetDescriptorHeaps(1, heaps);
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            quadBuffer.BindVertexToCommandList(commandList, 0, 4 * static_cast<std::uint32_t>(sizeof(float)));
            commandList->DrawInstanced(6, 1, 0, 0);

            TransitionResource(
                commandList,
                textureResource,
                D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            sourceWidth = destWidth;
            sourceHeight = destHeight;
        }
    }

    void GenerateMipmapChain(
        ID3D12Device* device,
        ID3D12Resource* textureResource,
        DXGI_FORMAT format,
        int width,
        int height,
        std::uint32_t mipLevels)
    {
        if (mipLevels <= 1)
        {
            return;
        }

        const std::uint32_t tempSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
        const std::uint32_t tempRtvIndex = GfxContext::Get().AllocateOffscreenRtvBlock(1);
        if (tempSrvIndex == UINT32_MAX || tempRtvIndex == UINT32_MAX)
        {
            if (tempSrvIndex != UINT32_MAX)
            {
                GfxContext::Get().FreeOffscreenSrv(tempSrvIndex);
            }
            if (tempRtvIndex != UINT32_MAX)
            {
                GfxContext::Get().FreeOffscreenRtvBlock(tempRtvIndex, 1);
            }
            const std::string gpuError = GfxContext::GetLastGpuAllocationError();
            throw std::runtime_error(
                gpuError.empty() ? "Mipmap generation failed: out of GPU descriptors" : gpuError);
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE tempRtvHandle{
            GfxContext::Get().GetOffscreenRtvCpuHandle(tempRtvIndex)};
        const D3D12_CPU_DESCRIPTOR_HANDLE tempSrvHandle{GfxContext::Get().GetSrvCpuHandle(tempSrvIndex)};

        // Initialize upload resources before GPU work — GpuBuffer::Create also uploads.
        Shader& mipShader = GetMipmapGenShader();
        GpuBuffer& quadBuffer = GetMipmapQuadBuffer();

        if (GfxContext::Get().IsFrameRecording())
        {
            auto* commandList =
                static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
            RecordGenerateMipmapChain(
                commandList,
                device,
                textureResource,
                format,
                width,
                height,
                mipLevels,
                tempRtvHandle,
                tempSrvHandle,
                tempSrvIndex,
                mipShader,
                quadBuffer);
        }
        else
        {
            GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
                RecordGenerateMipmapChain(
                    static_cast<ID3D12GraphicsCommandList*>(commandListPointer),
                    device,
                    textureResource,
                    format,
                    width,
                    height,
                    mipLevels,
                    tempRtvHandle,
                    tempSrvHandle,
                    tempSrvIndex,
                    mipShader,
                    quadBuffer);
            });
        }

        GfxContext::Get().FreeOffscreenSrv(tempSrvIndex);
        GfxContext::Get().FreeOffscreenRtvBlock(tempRtvIndex, 1);
    }

    std::vector<unsigned char> ExpandToRgba(
        const unsigned char* pixels,
        int width,
        int height,
        int channelCount)
    {
        const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        std::vector<unsigned char> rgba(pixelCount * 4);
        for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
        {
            const unsigned char* source = pixels + pixelIndex * static_cast<std::size_t>(channelCount);
            unsigned char* destination = rgba.data() + pixelIndex * 4;
            if (channelCount == 1)
            {
                destination[0] = source[0];
                destination[1] = source[0];
                destination[2] = source[0];
                destination[3] = 255;
            }
            else if (channelCount == 3)
            {
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
                destination[3] = 255;
            }
            else
            {
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
                destination[3] = source[3];
            }
        }

        return rgba;
    }

    std::vector<unsigned char> FlipRowsVertically(
        const std::vector<unsigned char>& pixels,
        int width,
        int height)
    {
        const int rowBytes = width * 4;
        std::vector<unsigned char> flipped(pixels.size());
        for (int row = 0; row < height; ++row)
        {
            const int destinationRow = height - 1 - row;
            std::copy(
                pixels.begin() + static_cast<std::size_t>(row) * rowBytes,
                pixels.begin() + static_cast<std::size_t>(row + 1) * rowBytes,
                flipped.begin() + static_cast<std::size_t>(destinationRow) * rowBytes);
        }

        return flipped;
    }
}

Texture::Texture(const char* path, TextureColorSpace colorSpace, bool flipVertically)
    : Texture(path, colorSpace, TextureSamplerSettings{}, flipVertically)
{
}

Texture::Texture(
    const char* path,
    TextureColorSpace colorSpace,
    const TextureSamplerSettings& /*samplerSettings*/,
    bool flipVertically)
{
    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0;
    int height = 0;
    int channelCount = 0;
    unsigned char* pixels = stbi_load(path, &width, &height, &channelCount, 0);
    if (pixels == nullptr)
    {
        throw std::runtime_error(std::string("Failed to load texture: ") + path);
    }

    UploadPixels(pixels, width, height, channelCount, colorSpace);
    stbi_image_free(pixels);
    m_valid = true;
}

std::shared_ptr<Texture> Texture::CreateFromPixels(
    const unsigned char* pixels,
    int width,
    int height,
    int channelCount,
    TextureColorSpace colorSpace,
    const TextureSamplerSettings& /*samplerSettings*/,
    bool flipVertically)
{
    auto texture = std::shared_ptr<Texture>(new Texture());
    std::vector<unsigned char> rgba = ExpandToRgba(pixels, width, height, channelCount);
    if (flipVertically)
    {
        rgba = FlipRowsVertically(rgba, width, height);
    }

    texture->UploadPixels(rgba.data(), width, height, 4, colorSpace);
    texture->m_valid = true;
    return texture;
}

void Texture::ReleaseUploadResources()
{
    g_mipmapGenShader.reset();
    if (g_mipmapQuadBuffer != nullptr)
    {
        g_mipmapQuadBuffer->Destroy();
        g_mipmapQuadBuffer.reset();
    }
}

Texture::~Texture()
{
    if (!GfxContext::Get().IsInitialized())
    {
        return;
    }

    if (m_srvDescriptorIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(m_srvDescriptorIndex);
        m_srvDescriptorIndex = UINT32_MAX;
    }

    if (m_uploadAllocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(m_uploadAllocation)->Release();
        m_uploadAllocation = nullptr;
    }

    m_uploadResource = nullptr;

    if (m_allocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(m_allocation)->Release();
        m_allocation = nullptr;
    }

    m_resource = nullptr;
}

void Texture::UploadPixels(
    const unsigned char* pixels,
    int width,
    int height,
    int channelCount,
    TextureColorSpace colorSpace)
{
    std::vector<unsigned char> rgba = ExpandToRgba(pixels, width, height, channelCount);
    const DXGI_FORMAT format = (colorSpace == TextureColorSpace::SRGB)
        ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        : DXGI_FORMAT_R8G8B8A8_UNORM;
    const std::uint32_t mipLevels = CalculateMipLevelCount(width, height);
    const std::vector<CpuMipLevel> mipChain = GenerateCpuMipChain(rgba, width, height, mipLevels);

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT64>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = mipLevels;
    textureDesc.Format = format;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> uploadLayouts(mipLevels);
    std::vector<UINT> uploadRowCounts(mipLevels);
    std::vector<UINT64> uploadRowSizes(mipLevels);
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(
        &textureDesc,
        0,
        mipLevels,
        0,
        uploadLayouts.data(),
        uploadRowCounts.data(),
        uploadRowSizes.data(),
        &uploadSize);

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
        throw std::runtime_error("Failed to create texture resource");
    }

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
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
        throw std::runtime_error("Failed to create texture upload buffer");
    }

    void* mapped = nullptr;
    if (FAILED(uploadResource->Map(0, nullptr, &mapped)))
    {
        uploadAllocation->Release();
        uploadResource->Release();
        textureAllocation->Release();
        textureResource->Release();
        throw std::runtime_error("Failed to map texture upload buffer");
    }

    for (std::uint32_t mipLevel = 0; mipLevel < mipLevels; ++mipLevel)
    {
        const CpuMipLevel& mip = mipChain[static_cast<std::size_t>(mipLevel)];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = uploadLayouts[static_cast<std::size_t>(mipLevel)];
        unsigned char* destination =
            static_cast<unsigned char*>(mapped) + static_cast<std::size_t>(footprint.Offset);
        const std::size_t sourceRowBytes = static_cast<std::size_t>(mip.width) * 4;

        for (int row = 0; row < mip.height; ++row)
        {
            std::memcpy(
                destination + static_cast<std::size_t>(row) * footprint.Footprint.RowPitch,
                mip.pixels.data() + static_cast<std::size_t>(row) * sourceRowBytes,
                sourceRowBytes);
        }
    }
    uploadResource->Unmap(0, nullptr);

    if (GfxContext::Get().IsFrameRecording())
    {
        auto* commandList =
            static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        RecordTextureCopyAndTransition(
            commandList,
            textureResource,
            uploadResource,
            uploadLayouts,
            mipLevels);
        m_uploadResource = uploadResource;
        m_uploadAllocation = uploadAllocation;
    }
    else
    {
        GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
            RecordTextureCopyAndTransition(
                static_cast<ID3D12GraphicsCommandList*>(commandListPointer),
                textureResource,
                uploadResource,
                uploadLayouts,
                mipLevels);
        });

        uploadAllocation->Release();
        uploadResource->Release();
    }

    m_resource = textureResource;
    m_allocation = textureAllocation;
    m_srvDescriptorIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (m_srvDescriptorIndex == UINT32_MAX)
    {
        static_cast<D3D12MA::Allocation*>(m_allocation)->Release();
        m_allocation = nullptr;
        m_resource = nullptr;
        const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        throw std::runtime_error(gpuError.empty() ? "GPU descriptor/SRV allocation failed" : gpuError);
    }

    m_srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(m_srvDescriptorIndex);
    GfxContext::Get().CreateSrvForTexture(
        m_resource,
        static_cast<int>(format),
        m_srvDescriptorIndex,
        width,
        height,
        mipLevels);
    m_id = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(
        GfxContext::Get().GetSrvHeapGpuHandle(m_srvDescriptorIndex)));
}

void Texture::Bind(const unsigned int textureUnit) const
{
    if (!m_valid || m_srvCpuHandle == 0)
    {
        return;
    }

    if (const Shader* shader = Shader::GetActiveShader())
    {
        shader->BindTextureSlot(static_cast<int>(textureUnit), m_srvCpuHandle);
    }
}

unsigned int Texture::GetId() const
{
    return m_id;
}

bool Texture::IsValid() const
{
    return m_valid;
}
