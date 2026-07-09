#include "engine/raytracing/NrdCommon.h"

#include "engine/raytracing/DxrGpuResource.h"

#include <NRD.h>

#include <d3d12.h>

namespace NrdCommon
{
    std::uint32_t NrdFormatToDxgi(const nrd::Format format)
    {
        switch (format)
        {
        case nrd::Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case nrd::Format::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
        case nrd::Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
        case nrd::Format::R8_SINT: return DXGI_FORMAT_R8_SINT;
        case nrd::Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
        case nrd::Format::RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
        case nrd::Format::RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
        case nrd::Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
        case nrd::Format::RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
        case nrd::Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case nrd::Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case nrd::Format::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
        case nrd::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case nrd::Format::R16_SINT: return DXGI_FORMAT_R16_SINT;
        case nrd::Format::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
        case nrd::Format::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
        case nrd::Format::RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
        case nrd::Format::RG16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case nrd::Format::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
        case nrd::Format::RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
        case nrd::Format::RGBA16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case nrd::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case nrd::Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
        case nrd::Format::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
        case nrd::Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
        case nrd::Format::RG32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case nrd::Format::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
        case nrd::Format::RGB32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
        case nrd::Format::RGB32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
        case nrd::Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGBA32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case nrd::Format::R10_G10_B10_A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
        case nrd::Format::R11_G11_B10_UFLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case nrd::Format::R9_G9_B9_E5_UFLOAT: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    void TransitionTracked(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        std::uint32_t& state,
        const std::uint32_t newState)
    {
        const auto current = static_cast<D3D12_RESOURCE_STATES>(state);
        const auto target = static_cast<D3D12_RESOURCE_STATES>(newState);
        if (resource == nullptr || current == target)
        {
            return;
        }

        TransitionResource(commandList, resource, current, target);
        state = newState;
    }
}
