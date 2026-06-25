#include "engine/rendering/Texture.h"

#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <stb_image.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
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

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    const UINT64 uploadPitch = (static_cast<UINT64>(width) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);
    const UINT64 uploadSize = uploadPitch * static_cast<UINT64>(height);

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = static_cast<UINT64>(width);
    textureDesc.Height = static_cast<UINT>(height);
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = format;
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

    for (int row = 0; row < height; ++row)
    {
        std::memcpy(
            static_cast<unsigned char*>(mapped) + static_cast<std::size_t>(row) * uploadPitch,
            rgba.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4,
            static_cast<std::size_t>(width) * 4);
    }
    uploadResource->Unmap(0, nullptr);

    ID3D12Resource* textureResourcePtr = textureResource;
    GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
        auto* uploadCommandList = static_cast<ID3D12GraphicsCommandList*>(commandListPointer);

        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = uploadResource;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &sourceLocation.PlacedFootprint, nullptr, nullptr, nullptr);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = textureResourcePtr;
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;
        uploadCommandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = textureResourcePtr;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        uploadCommandList->ResourceBarrier(1, &barrier);
    });

    uploadAllocation->Release();
    uploadResource->Release();

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
        height);
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
