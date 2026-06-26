#include "engine/rendering/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/platform/RenderPathDiagnostics.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    constexpr float kBackgroundSrgb[3] = {0.08f, 0.09f, 0.15f};

    // Fullscreen quad UVs for top-left texture origin (D3D12 convention).
    constexpr float kQuadVertices[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
    };

    float SrgbChannelToLinear(float channel)
    {
        return std::pow(channel, 2.2f);
    }

    bool IsPostProcessDebugMode(RenderDebugMode mode)
    {
        return mode == RenderDebugMode::Ssao ||
               mode == RenderDebugMode::CompositeOcclusion;
    }

    float HalfToFloat(const std::uint16_t half)
    {
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
                bits = sign |
                    static_cast<std::uint32_t>((15 + adjustedExponent) << 23) |
                    (adjustedMantissa << 13);
            }
        }
        else if (exponent == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            bits = sign |
                static_cast<std::uint32_t>((exponent + (127 - 15)) << 23) |
                (mantissa << 13);
        }

        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool ReadbackTextureCenterRgba16F(
        void* resource,
        const int width,
        const int height,
        const int x,
        const int y,
        float outRgba[4])
    {
        if (resource == nullptr || outRgba == nullptr || width <= 0 || height <= 0)
        {
            return false;
        }

        D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
        auto* textureResource = static_cast<ID3D12Resource*>(resource);
        constexpr UINT64 kReadbackSize = sizeof(std::uint16_t) * 4ull;

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

        const int clampedX = std::clamp(x, 0, width - 1);
        const int clampedY = std::clamp(y, 0, height - 1);

        GfxContext::Get().ExecuteImmediate([&](void* commandListPtr) {
            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition.pResource = textureResource;
            toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &toCopy);

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = textureResource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readbackResource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Offset = 0;
            destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            destination.PlacedFootprint.Footprint.Width = 1;
            destination.PlacedFootprint.Footprint.Height = 1;
            destination.PlacedFootprint.Footprint.Depth = 1;
            destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackSize);

            D3D12_BOX sourceBox{};
            sourceBox.left = static_cast<UINT>(clampedX);
            sourceBox.top = static_cast<UINT>(clampedY);
            sourceBox.front = 0;
            sourceBox.right = static_cast<UINT>(clampedX + 1);
            sourceBox.bottom = static_cast<UINT>(clampedY + 1);
            sourceBox.back = 1;
            commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

            D3D12_RESOURCE_BARRIER fromCopy{};
            fromCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fromCopy.Transition.pResource = textureResource;
            fromCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            fromCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            fromCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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

        const auto* halfChannels = static_cast<const std::uint16_t*>(mapped);
        for (int channel = 0; channel < 4; ++channel)
        {
            outRgba[channel] = HalfToFloat(halfChannels[channel]);
        }

        readbackResource->Unmap(0, nullptr);
        readbackAllocation->Release();
        readbackResource->Release();
        return true;
    }

    bool ReadbackDepthCenter(
        void* resource,
        const int width,
        const int height,
        const int x,
        const int y,
        float& outDepth)
    {
        if (resource == nullptr || width <= 0 || height <= 0)
        {
            return false;
        }

        D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
        auto* textureResource = static_cast<ID3D12Resource*>(resource);
        constexpr UINT64 kReadbackSize = sizeof(float);

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

        const int clampedX = std::clamp(x, 0, width - 1);
        const int clampedY = std::clamp(y, 0, height - 1);

        GfxContext::Get().ExecuteImmediate([&](void* commandListPtr) {
            auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

            D3D12_RESOURCE_BARRIER toCopy{};
            toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy.Transition.pResource = textureResource;
            toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            commandList->ResourceBarrier(1, &toCopy);

            D3D12_TEXTURE_COPY_LOCATION source{};
            source.pResource = textureResource;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;

            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = readbackResource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Offset = 0;
            destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
            destination.PlacedFootprint.Footprint.Width = 1;
            destination.PlacedFootprint.Footprint.Height = 1;
            destination.PlacedFootprint.Footprint.Depth = 1;
            destination.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(kReadbackSize);

            D3D12_BOX sourceBox{};
            sourceBox.left = static_cast<UINT>(clampedX);
            sourceBox.top = static_cast<UINT>(clampedY);
            sourceBox.front = 0;
            sourceBox.right = static_cast<UINT>(clampedX + 1);
            sourceBox.bottom = static_cast<UINT>(clampedY + 1);
            sourceBox.back = 1;
            commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

            D3D12_RESOURCE_BARRIER fromCopy{};
            fromCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fromCopy.Transition.pResource = textureResource;
            fromCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            fromCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            fromCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
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

        const auto rawDepth = *static_cast<const std::uint32_t*>(mapped);
        outDepth = static_cast<float>(rawDepth & 0x00FFFFFFu) / 16777215.0f;
        readbackResource->Unmap(0, nullptr);
        readbackAllocation->Release();
        readbackResource->Release();
        return true;
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

ScreenSpaceEffects::ScreenSpaceEffects()
    : m_sceneFramebuffer(std::make_unique<Framebuffer>()),
      m_ssaoShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoFragmentShader)),
      m_blurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoBlurFragmentShader)),
      m_compositeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ScreenCompositeFragmentShader)),
      m_bloomExtractShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomExtractFragmentShader)),
      m_bloomBlurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomBlurFragmentShader)),
      m_shadowBlurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ShadowBlurFragmentShader)),
      m_tonemapShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::TonemapFragmentShader)),
      m_fxaaShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::FxaaFragmentShader)),
      m_downsampleShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DownsampleFragmentShader)),
      m_taaShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::TaaFragmentShader)),
      m_smaaEdgeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SmaaEdgeFragmentShader)),
      m_smaaNeighborShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SmaaNeighborFragmentShader)),
      m_gridCompositeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::GridCompositeFragmentShader)),
      m_debugChannelShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DebugChannelFragmentShader))
{
    CreateFullscreenQuad();
    CreateKernel();
    CreateNoiseTexture();

    // HDR post-processing + SSAO defaults; selection overlay after tonemap (SceneRenderer).
    // Stage 3+: Bloom toggle, depth blit for gizmo occlusion, play-mode parity.
}

ScreenSpaceEffects::~ScreenSpaceEffects()
{
    DestroyInternalTarget(m_ssaoTarget);
    DestroyInternalTarget(m_ssaoBlurTarget);
    DestroyInternalTarget(m_shadowBlurTarget);
    DestroyInternalTarget(m_shadowBlur2Target);
    DestroyInternalTarget(m_hdrCompositeTarget);
    DestroyInternalTarget(m_bloomExtractTarget);
    DestroyInternalTarget(m_bloomBlurTarget);
    DestroyInternalTarget(m_bloomBlur2Target);
    DestroyInternalTarget(m_ldrTonemapTarget);
    DestroyInternalTarget(m_smaaEdgeTarget);
    DestroyInternalTarget(m_smaaOutputTarget);
    DestroyInternalTarget(m_taaHistoryTarget);
    DestroyInternalTarget(m_taaResolveTarget);
    DestroyInternalTarget(m_gridOverlayTarget);
    DestroyInternalTarget(m_noiseTexture);
}

void ScreenSpaceEffects::CreateFullscreenQuad()
{
    m_quadVb.Create(
        GpuBuffer::Type::Vertex,
        kQuadVertices,
        static_cast<std::uint32_t>(sizeof(kQuadVertices)));
}

void ScreenSpaceEffects::CreateInternalTarget(
    InternalTarget& target,
    const int width,
    const int height,
    const int format)
{
    DestroyInternalTarget(target);

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = static_cast<UINT64>(width);
    resourceDesc.Height = static_cast<UINT>(height);
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = static_cast<DXGI_FORMAT>(format);
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = static_cast<DXGI_FORMAT>(format);
    clearValue.Color[0] = 0.0f;
    clearValue.Color[1] = 0.0f;
    clearValue.Color[2] = 0.0f;
    clearValue.Color[3] = 1.0f;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        throw std::runtime_error("Failed to create post-process render target");
    }

    target.resource = resource;
    target.allocation = allocation;
    target.width = width;
    target.height = height;
    target.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    target.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(target.srvIndex);
    target.rtvIndex = GfxContext::Get().AllocateOffscreenRtvBlock(1);

    CreateTexture2DSrv(
        device,
        resource,
        {target.srvCpuHandle},
        static_cast<DXGI_FORMAT>(format),
        1);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex)};
    device->CreateRenderTargetView(resource, nullptr, rtvHandle);
}

void ScreenSpaceEffects::DestroyInternalTarget(InternalTarget& target) const
{
    if (!GfxContext::Get().IsInitialized())
    {
        target.srvIndex = UINT32_MAX;
        target.rtvIndex = UINT32_MAX;
        target.allocation = nullptr;
        target.resource = nullptr;
        target.srvCpuHandle = 0;
        target.width = 0;
        target.height = 0;
        return;
    }

    if (target.srvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenSrv(target.srvIndex);
        target.srvIndex = UINT32_MAX;
    }

    if (target.rtvIndex != UINT32_MAX)
    {
        GfxContext::Get().FreeOffscreenRtvBlock(target.rtvIndex, 1);
        target.rtvIndex = UINT32_MAX;
    }

    if (target.allocation != nullptr)
    {
        static_cast<D3D12MA::Allocation*>(target.allocation)->Release();
        target.allocation = nullptr;
    }

    target.resource = nullptr;
    target.srvCpuHandle = 0;
    target.width = 0;
    target.height = 0;
}

void ScreenSpaceEffects::ResizeInternalTarget(
    InternalTarget& target,
    const int width,
    const int height,
    const int format)
{
    CreateInternalTarget(target, width, height, format);
}

void ScreenSpaceEffects::CreateNoiseTexture()
{
    std::mt19937 generator(101);
    std::uniform_real_distribution<float> random(-1.0f, 1.0f);

    std::array<glm::vec3, 16> noiseValues{};
    for (glm::vec3& noiseValue : noiseValues)
    {
        glm::vec3 sample(random(generator), random(generator), random(generator));
        if (glm::dot(sample, sample) < 1e-6f)
        {
            sample = glm::vec3(1.0f, 0.0f, 0.0f);
        }
        noiseValue = glm::normalize(sample);
    }

    const int noiseWidth = 4;
    const int noiseHeight = 4;
    std::vector<float> rgbaPixels(static_cast<std::size_t>(noiseWidth * noiseHeight * 4));
    for (std::size_t pixelIndex = 0; pixelIndex < noiseValues.size(); ++pixelIndex)
    {
        rgbaPixels[pixelIndex * 4 + 0] = noiseValues[pixelIndex].x;
        rgbaPixels[pixelIndex * 4 + 1] = noiseValues[pixelIndex].y;
        rgbaPixels[pixelIndex * 4 + 2] = noiseValues[pixelIndex].z;
        rgbaPixels[pixelIndex * 4 + 3] = 1.0f;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    const DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = noiseWidth;
    textureDesc.Height = noiseHeight;
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
        throw std::runtime_error("Failed to create SSAO noise texture");
    }

    const UINT64 uploadPitch = (static_cast<UINT64>(noiseWidth) * sizeof(float) * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~(static_cast<UINT64>(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) - 1);
    const UINT64 uploadSize = uploadPitch * static_cast<UINT64>(noiseHeight);

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
        throw std::runtime_error("Failed to create SSAO noise upload buffer");
    }

    void* mapped = nullptr;
    uploadResource->Map(0, nullptr, &mapped);
    for (int row = 0; row < noiseHeight; ++row)
    {
        std::memcpy(
            static_cast<std::uint8_t*>(mapped) + static_cast<std::size_t>(row) * uploadPitch,
            rgbaPixels.data() + static_cast<std::size_t>(row) * noiseWidth * 4,
            static_cast<std::size_t>(noiseWidth) * 4 * sizeof(float));
    }
    uploadResource->Unmap(0, nullptr);

    GfxContext::Get().ExecuteImmediate([&](void* commandListPointer) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPointer);

        D3D12_TEXTURE_COPY_LOCATION destinationLocation{};
        destinationLocation.pResource = textureResource;
        destinationLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destinationLocation.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION sourceLocation{};
        sourceLocation.pResource = uploadResource;
        sourceLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        sourceLocation.PlacedFootprint.Offset = 0;
        sourceLocation.PlacedFootprint.Footprint.Format = format;
        sourceLocation.PlacedFootprint.Footprint.Width = noiseWidth;
        sourceLocation.PlacedFootprint.Footprint.Height = noiseHeight;
        sourceLocation.PlacedFootprint.Footprint.Depth = 1;
        sourceLocation.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(uploadPitch);

        commandList->CopyTextureRegion(&destinationLocation, 0, 0, 0, &sourceLocation, nullptr);

        TransitionResource(
            commandList,
            textureResource,
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    });

    uploadAllocation->Release();
    uploadResource->Release();

    m_noiseTexture.resource = textureResource;
    m_noiseTexture.allocation = textureAllocation;
    m_noiseTexture.width = noiseWidth;
    m_noiseTexture.height = noiseHeight;
    m_noiseTexture.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_noiseTexture.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(m_noiseTexture.srvIndex);
    CreateTexture2DSrv(device, textureResource, {m_noiseTexture.srvCpuHandle}, format, 1);
}

void ScreenSpaceEffects::CreateKernel()
{
    std::mt19937 generator(202);
    std::uniform_real_distribution<float> random(0.0f, 1.0f);

    m_kernelSamples.clear();
    m_kernelSamples.reserve(KernelSampleCount);

    for (int sampleIndex = 0; sampleIndex < KernelSampleCount; ++sampleIndex)
    {
        glm::vec3 sample(
            random(generator) * 2.0f - 1.0f,
            random(generator) * 2.0f - 1.0f,
            random(generator));
        sample = glm::normalize(sample);

        const float scale = static_cast<float>(sampleIndex) / static_cast<float>(KernelSampleCount);
        sample *= random(generator) * glm::mix(0.1f, 1.0f, scale * scale);
        m_kernelSamples.push_back(sample);
    }
}

void ScreenSpaceEffects::ResizeSingleChannelTargets(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResizeInternalTarget(m_ssaoTarget, width, height, format);
    ResizeInternalTarget(m_ssaoBlurTarget, width, height, format);
    ResizeInternalTarget(m_shadowBlurTarget, width, height, format);
    ResizeInternalTarget(m_shadowBlur2Target, width, height, format);
}

void ScreenSpaceEffects::ResizeHdrColorTarget(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResizeInternalTarget(m_hdrCompositeTarget, width, height, format);
}

void ScreenSpaceEffects::ResizeBloomTargets(const int width, const int height)
{
    const int bloomWidth = std::max(1, width / 2);
    const int bloomHeight = std::max(1, height / 2);
    const int format = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);

    ResizeInternalTarget(m_bloomExtractTarget, bloomWidth, bloomHeight, format);
    ResizeInternalTarget(m_bloomBlurTarget, bloomWidth, bloomHeight, format);
    ResizeInternalTarget(m_bloomBlur2Target, bloomWidth, bloomHeight, format);
}

void ScreenSpaceEffects::ResizeLdrTonemapTarget(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    ResizeInternalTarget(m_ldrTonemapTarget, width, height, format);
}

void ScreenSpaceEffects::ResizeGridOverlayTarget(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    ResizeInternalTarget(m_gridOverlayTarget, width, height, format);
}

void ScreenSpaceEffects::ResizeAntiAliasingTargets(const int width, const int height)
{
    const int ldrFormat = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    ResizeInternalTarget(m_smaaEdgeTarget, width, height, ldrFormat);
    ResizeInternalTarget(m_smaaOutputTarget, width, height, ldrFormat);
    ResizeInternalTarget(m_taaHistoryTarget, width, height, ldrFormat);
    ResizeInternalTarget(m_taaResolveTarget, width, height, ldrFormat);
}

float ScreenSpaceEffects::GetActiveRenderScale() const
{
    if (m_antiAliasingMode == AntiAliasingMode::SSAA)
    {
        return std::clamp(m_renderScale, 1.0f, 2.0f);
    }

    return 1.0f;
}

int ScreenSpaceEffects::GetRenderWidth() const
{
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(m_viewportWidth) * GetActiveRenderScale())));
}

int ScreenSpaceEffects::GetRenderHeight() const
{
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(m_viewportHeight) * GetActiveRenderScale())));
}

void ScreenSpaceEffects::ResetTaaHistory() const
{
    m_taaHistoryValid = false;
    m_taaFrameIndex = 0;
    m_prevViewProjection = glm::mat4(1.0f);
}

void ScreenSpaceEffects::Resize(const int viewportWidth, const int viewportHeight)
{
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        return;
    }

    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    const int renderWidth = GetRenderWidth();
    const int renderHeight = GetRenderHeight();

    if (m_width == renderWidth && m_height == renderHeight && m_sceneFramebuffer->IsValid())
    {
        return;
    }

    if (!m_sceneFramebuffer->Resize(renderWidth, renderHeight, FramebufferColorMode::SplitDirectIndirect))
    {
        return;
    }
    ResizeSingleChannelTargets(renderWidth, renderHeight);
    ResizeHdrColorTarget(renderWidth, renderHeight);
    ResizeBloomTargets(renderWidth, renderHeight);
    ResizeLdrTonemapTarget(renderWidth, renderHeight);
    ResizeAntiAliasingTargets(renderWidth, renderHeight);
    ResizeGridOverlayTarget(renderWidth, renderHeight);
    m_width = renderWidth;
    m_height = renderHeight;
    ResetTaaHistory();
}

namespace
{
    glm::vec2 HaltonJitter(const int frameIndex, const int width, const int height)
    {
        static constexpr float kHalton23[][2] = {
            {0.0f, 0.0f},         {0.5f, 0.333333f},    {0.25f, 0.666667f},   {0.75f, 0.111111f},
            {0.125f, 0.444444f},  {0.625f, 0.777778f},  {0.375f, 0.222222f},  {0.875f, 0.555556f},
            {0.0625f, 0.888889f}, {0.5625f, 0.037037f}, {0.3125f, 0.37037f},  {0.8125f, 0.703704f},
            {0.1875f, 0.148148f}, {0.6875f, 0.481481f}, {0.4375f, 0.814815f}, {0.9375f, 0.259259f},
        };

        const int sampleIndex = frameIndex % 16;
        const float jitterX = ((kHalton23[sampleIndex][0] - 0.5f) * 2.0f) / static_cast<float>(width);
        const float jitterY = ((kHalton23[sampleIndex][1] - 0.5f) * 2.0f) / static_cast<float>(height);
        return glm::vec2(jitterX, jitterY);
    }
}

void ScreenSpaceEffects::PrepareAntiAliasingFrame(Camera& camera) const
{
    camera.ClearProjectionJitter();
    if (m_antiAliasingMode == AntiAliasingMode::TAA && m_width > 0 && m_height > 0)
    {
        camera.SetProjectionJitter(HaltonJitter(m_taaFrameIndex, m_width, m_height));
    }
}

void ScreenSpaceEffects::FinalizeAntiAliasingFrame(const Camera& camera) const
{
    if (m_antiAliasingMode != AntiAliasingMode::TAA)
    {
        return;
    }

    m_prevViewProjection = camera.GetProjectionMatrix() * camera.GetViewMatrix();
    ++m_taaFrameIndex;
}

void ScreenSpaceEffects::BeginScenePass() const
{
    m_sceneFramebuffer->BindDrawTarget(false);

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    const float directClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float indirectClear[] = {
        SrgbChannelToLinear(kBackgroundSrgb[0]),
        SrgbChannelToLinear(kBackgroundSrgb[1]),
        SrgbChannelToLinear(kBackgroundSrgb[2]),
        1.0f};

    if (m_sceneFramebuffer->HasSplitLighting())
    {
        const float normalClear[] = {0.0f, 0.0f, 1.0f, 1.0f};
        const float shadowClear[] = {1.0f, 0.0f, 0.0f, 0.0f};

        D3D12_CPU_DESCRIPTOR_HANDLE directRtv{ m_sceneFramebuffer->GetColorRtvCpuHandle(0) };
        D3D12_CPU_DESCRIPTOR_HANDLE indirectRtv{ m_sceneFramebuffer->GetColorRtvCpuHandle(1) };
        D3D12_CPU_DESCRIPTOR_HANDLE normalRtv{ m_sceneFramebuffer->GetColorRtvCpuHandle(2) };
        D3D12_CPU_DESCRIPTOR_HANDLE shadowRtv{ m_sceneFramebuffer->GetColorRtvCpuHandle(3) };

        commandList->ClearRenderTargetView(directRtv, directClear, 0, nullptr);
        commandList->ClearRenderTargetView(indirectRtv, indirectClear, 0, nullptr);
        if (m_sceneFramebuffer->HasGeometryNormals())
        {
            commandList->ClearRenderTargetView(normalRtv, normalClear, 0, nullptr);
        }
        if (m_sceneFramebuffer->HasShadowFactor())
        {
            commandList->ClearRenderTargetView(shadowRtv, shadowClear, 0, nullptr);
        }
    }
    else
    {
        D3D12_CPU_DESCRIPTOR_HANDLE colorRtv{ m_sceneFramebuffer->GetColorRtvCpuHandle(0) };
        commandList->ClearRenderTargetView(colorRtv, indirectClear, 0, nullptr);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE depthDsv{ m_sceneFramebuffer->GetDepthDsvCpuHandle() };
    commandList->ClearDepthStencilView(
        depthDsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f,
        0,
        0,
        nullptr);
}

void ScreenSpaceEffects::EndScenePass() const
{
    m_sceneFramebuffer->Unbind();
}

void ScreenSpaceEffects::BeginGridOverlayPass() const
{
    if (m_gridOverlayTarget.resource == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    TransitionResource(
        commandList,
        static_cast<ID3D12Resource*>(m_gridOverlayTarget.resource),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{
        GfxContext::Get().GetOffscreenRtvCpuHandle(m_gridOverlayTarget.rtvIndex)};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{m_sceneFramebuffer->GetDepthDsvCpuHandle()};

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, m_width, m_height};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);

    const float clearColor[] = {0.0f, 0.0f, 0.0f, 0.0f};
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
}

void ScreenSpaceEffects::EndGridOverlayPass() const
{
    if (m_gridOverlayTarget.resource == nullptr)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    TransitionResource(
        commandList,
        static_cast<ID3D12Resource*>(m_gridOverlayTarget.resource),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void ScreenSpaceEffects::CompositeGridOverlay() const
{
    if (m_gridOverlayTarget.srvCpuHandle == 0 || m_width <= 0 || m_height <= 0)
    {
        return;
    }

    m_gridCompositeShader->Use(false, true);
    m_gridCompositeShader->SetInt("uGridOverlay", 0);
    m_gridCompositeShader->BindTextureSlot(0, m_gridOverlayTarget.srvCpuHandle);
    m_gridCompositeShader->FlushUniforms();
    DrawFullscreenPass(*m_gridCompositeShader, true);
}

void ScreenSpaceEffects::DrawFullscreenQuad() const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_quadVb.BindVertex(0, 4 * static_cast<std::uint32_t>(sizeof(float)));
    commandList->DrawInstanced(6, 1, 0, 0);
}

void ScreenSpaceEffects::DrawFullscreenPass(Shader& shader, const bool viewportLdr) const
{
    shader.BindPipeline(false, viewportLdr);
    shader.FlushUniforms();
    DrawFullscreenQuad();
}

void ScreenSpaceEffects::DrawFullscreenToTarget(
    Shader& shader,
    InternalTarget& target,
    const int width,
    const int height,
    const float clearColor[4],
    const bool viewportLdr) const
{
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* resource = static_cast<ID3D12Resource*>(target.resource);

    TransitionResource(
        commandList,
        resource,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{ GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex) };

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, width, height};

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    shader.BindPipeline(false, viewportLdr);
    shader.FlushUniforms();
    DrawFullscreenQuad();

    TransitionResource(
        commandList,
        resource,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void ScreenSpaceEffects::BindOutputTarget(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    int outputWidth = viewportWidth;
    int outputHeight = viewportHeight;

    if (outputTarget != nullptr)
    {
        GfxContext::Get().SetBoundOutputFramebuffer(outputTarget);
        outputTarget->BindDrawTarget(false);
        if (outputTarget->GetWidth() > 0 && outputTarget->GetHeight() > 0)
        {
            outputWidth = outputTarget->GetWidth();
            outputHeight = outputTarget->GetHeight();
        }
    }
    else
    {
        GfxContext::Get().BindSwapChainRenderTarget(false);
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(outputWidth);
    viewport.Height = static_cast<float>(outputHeight);
    viewport.MaxDepth = 1.0f;
    D3D12_RECT scissor{0, 0, outputWidth, outputHeight};
    commandList->RSSetViewports(1, &viewport);
    commandList->RSSetScissorRects(1, &scissor);
}

void ScreenSpaceEffects::Apply(
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    const DirectionalShadowSettings& shadowSettings) const
{
    FinalizePendingSsaoGpuReadback();

    if (!m_enabled || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    const Framebuffer* outputTarget = GfxContext::Get().GetBoundOutputFramebuffer();

    const bool runSsao = m_ssaoEnabled && !IsPbrMaterialDebugMode(m_debugMode);
    const bool pbrDebugActive = IsPbrMaterialDebugMode(m_debugMode);

    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 inverseProjectionMatrix = glm::inverse(projectionMatrix);
    const glm::vec2 texelSize(
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height));

    if (runSsao)
    {
        const float ssaoClear[] = {1.0f, 1.0f, 1.0f, 1.0f};

        glm::vec4 packedKernelSamples[KernelSampleCount];
        for (int sampleIndex = 0; sampleIndex < KernelSampleCount; ++sampleIndex)
        {
            packedKernelSamples[sampleIndex] =
                glm::vec4(m_kernelSamples[static_cast<std::size_t>(sampleIndex)], 0.0f);
        }

        m_ssaoShader->Use(false);
        m_ssaoShader->SetInt("uDepthMap", 0);
        m_ssaoShader->SetInt("uNormalMap", 1);
        m_ssaoShader->SetInt("uNoiseMap", 2);
        m_ssaoShader->SetInt(
            "uUseGeometryNormals",
            m_sceneFramebuffer->HasGeometryNormals() ? 1 : 0);
        m_ssaoShader->SetMat4("uProjection", projectionMatrix);
        m_ssaoShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssaoShader->SetMat4("uView", camera.GetViewMatrix());
        m_ssaoShader->SetFloat("uRadius", m_ssaoRadius);
        m_ssaoShader->SetFloat("uBias", m_ssaoBias);
        m_ssaoShader->SetFloat("uNearPlane", camera.GetNearPlane());
        m_ssaoShader->SetFloat("uFarPlane", camera.GetFarPlane());
        m_ssaoShader->SetInt("uKernelSize", KernelSampleCount);
        m_ssaoShader->SetInt("uDebugMode", m_ssaoShaderDebugMode);
        m_ssaoShader->SetVec4Array("uSamples", packedKernelSamples, KernelSampleCount);
        m_ssaoShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_ssaoShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        m_ssaoShader->BindTextureSlot(2, m_noiseTexture.srvCpuHandle);
        DrawFullscreenToTarget(*m_ssaoShader, const_cast<InternalTarget&>(m_ssaoTarget), m_width, m_height, ssaoClear);

        if (m_ssaoShaderDebugMode == 0)
        {
            m_blurShader->Use(false);
            m_blurShader->SetInt("uInput", 0);
            m_blurShader->SetInt("uDepthMap", 1);
            m_blurShader->SetMat4("uInvProjection", inverseProjectionMatrix);
            m_blurShader->SetVec2("uTexelSize", texelSize);
            m_blurShader->SetFloat("uDepthThreshold", m_ssaoBlurDepthThreshold);
            m_blurShader->SetFloat("uBlurSpread", 1.0f);

            m_blurShader->SetVec2("uBlurDirection", glm::vec2(1.0f, 0.0f));
            m_blurShader->BindTextureSlot(0, m_ssaoTarget.srvCpuHandle);
            m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoBlurTarget), m_width, m_height, ssaoClear);

            m_blurShader->SetVec2("uBlurDirection", glm::vec2(0.0f, 1.0f));
            m_blurShader->BindTextureSlot(0, m_ssaoBlurTarget.srvCpuHandle);
            m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoTarget), m_width, m_height, ssaoClear);

            m_blurShader->SetFloat("uBlurSpread", 2.5f);
            m_blurShader->SetVec2("uBlurDirection", glm::vec2(1.0f, 0.0f));
            m_blurShader->BindTextureSlot(0, m_ssaoTarget.srvCpuHandle);
            m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoBlurTarget), m_width, m_height, ssaoClear);

            m_blurShader->SetVec2("uBlurDirection", glm::vec2(0.0f, 1.0f));
            m_blurShader->BindTextureSlot(0, m_ssaoBlurTarget.srvCpuHandle);
            m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoTarget), m_width, m_height, ssaoClear);
        }
    }

    const bool useShadowFactorComposite = m_sceneFramebuffer->HasShadowFactor() && !pbrDebugActive;

    std::uintptr_t shadowFactorSrv = m_sceneFramebuffer->GetColorSrvCpuHandle(3);
    if (useShadowFactorComposite &&
        shadowSettings.GetShadowBlurEnabled() &&
        shadowSettings.GetShadowBlurRadius() > 0.0f)
    {
        const float shadowClear[] = {1.0f, 1.0f, 1.0f, 1.0f};

        m_shadowBlurShader->SetInt("uInput", 0);
        m_shadowBlurShader->SetInt("uDepthMap", 1);
        m_shadowBlurShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_shadowBlurShader->SetFloat("uDirectionX", texelSize.x);
        m_shadowBlurShader->SetFloat("uDirectionY", 0.0f);
        m_shadowBlurShader->SetFloat("uBlurRadius", shadowSettings.GetShadowBlurRadius());
        m_shadowBlurShader->SetFloat("uDepthThreshold", shadowSettings.GetShadowBlurDepthThreshold());
        m_shadowBlurShader->SetFloat("uShadowThreshold", shadowSettings.GetShadowBlurShadowThreshold());
        m_shadowBlurShader->BindTextureSlot(0, shadowFactorSrv);
        m_shadowBlurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_shadowBlurShader,
            const_cast<InternalTarget&>(m_shadowBlurTarget),
            m_width,
            m_height,
            shadowClear);

        m_shadowBlurShader->SetInt("uInput", 0);
        m_shadowBlurShader->SetInt("uDepthMap", 1);
        m_shadowBlurShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_shadowBlurShader->SetFloat("uDirectionX", 0.0f);
        m_shadowBlurShader->SetFloat("uDirectionY", texelSize.y);
        m_shadowBlurShader->SetFloat("uBlurRadius", shadowSettings.GetShadowBlurRadius());
        m_shadowBlurShader->SetFloat("uDepthThreshold", shadowSettings.GetShadowBlurDepthThreshold());
        m_shadowBlurShader->SetFloat("uShadowThreshold", shadowSettings.GetShadowBlurShadowThreshold());
        m_shadowBlurShader->BindTextureSlot(0, m_shadowBlurTarget.srvCpuHandle);
        m_shadowBlurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_shadowBlurShader,
            const_cast<InternalTarget&>(m_shadowBlur2Target),
            m_width,
            m_height,
            shadowClear);

        shadowFactorSrv = m_shadowBlur2Target.srvCpuHandle;
    }

    std::uintptr_t hdrColorSrv = m_sceneFramebuffer->GetColorSrvCpuHandle(0);
    const char* hdrColorSource = "scene_direct";
    bool compositeRan = false;
    const bool compositeUsesSsao = runSsao;
    const char* ssaoDebugViewSource = "none";

    if (m_sceneFramebuffer->HasSplitLighting() && !pbrDebugActive)
    {
        const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_compositeShader->SetInt("uDirectLighting", 0);
        m_compositeShader->SetInt("uIndirectLighting", 1);
        m_compositeShader->SetInt("uDepthMap", 2);
        m_compositeShader->SetInt("uSsaoMap", 3);
        m_compositeShader->SetInt("uUseSplitLighting", 1);
        m_compositeShader->SetInt("uUseSsao", runSsao ? 1 : 0);
        m_compositeShader->SetInt("uUseShadowFactor", useShadowFactorComposite ? 1 : 0);
        m_compositeShader->SetInt("uShadowFactorMap", 4);
        m_compositeShader->SetFloat("uSsaoPower", m_ssaoPower);
        m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
        m_compositeShader->SetInt(
            "uDebugOcclusionOnly",
            m_debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);
        m_compositeShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        m_compositeShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_compositeShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_compositeShader->BindTextureSlot(3, m_ssaoTarget.srvCpuHandle);
        m_compositeShader->BindTextureSlot(4, shadowFactorSrv);
        DrawFullscreenToTarget(
            *m_compositeShader,
            const_cast<InternalTarget&>(m_hdrCompositeTarget),
            m_width,
            m_height,
            compositeClear);

        hdrColorSrv = m_hdrCompositeTarget.srvCpuHandle;
        hdrColorSource = "hdr_composite_split";
        compositeRan = true;
    }
    else if (runSsao)
    {
        const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_compositeShader->SetInt("uDirectLighting", 0);
        m_compositeShader->SetInt("uIndirectLighting", 0);
        m_compositeShader->SetInt("uDepthMap", 2);
        m_compositeShader->SetInt("uSsaoMap", 3);
        m_compositeShader->SetInt("uUseSplitLighting", 0);
        m_compositeShader->SetInt("uUseSsao", 1);
        m_compositeShader->SetFloat("uSsaoPower", m_ssaoPower);
        m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
        m_compositeShader->SetInt(
            "uDebugOcclusionOnly",
            m_debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);
        m_compositeShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        m_compositeShader->BindTextureSlot(3, m_ssaoTarget.srvCpuHandle);
        m_compositeShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_compositeShader,
            const_cast<InternalTarget&>(m_hdrCompositeTarget),
            m_width,
            m_height,
            compositeClear);

        hdrColorSrv = m_hdrCompositeTarget.srvCpuHandle;
        hdrColorSource = "hdr_composite_ssao_only";
        compositeRan = true;
    }

    std::uintptr_t bloomSrv = 0;
    if (m_bloomEnabled && !IsPbrMaterialDebugMode(m_debugMode))
    {
        const int bloomWidth = std::max(1, m_width / 2);
        const int bloomHeight = std::max(1, m_height / 2);
        const glm::vec2 bloomTexelSize(
            1.0f / static_cast<float>(bloomWidth),
            1.0f / static_cast<float>(bloomHeight));
        const float bloomClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_bloomExtractShader->SetInt("uHdrColor", 0);
        m_bloomExtractShader->SetFloat("uThreshold", m_bloomThreshold);
        m_bloomExtractShader->SetFloat("uSoftKnee", m_bloomSoftKnee);
        m_bloomExtractShader->BindTextureSlot(0, hdrColorSrv);
        DrawFullscreenToTarget(
            *m_bloomExtractShader,
            const_cast<InternalTarget&>(m_bloomExtractTarget),
            bloomWidth,
            bloomHeight,
            bloomClear);

        m_bloomBlurShader->SetInt("uInput", 0);
        m_bloomBlurShader->SetFloat("uDirectionX", bloomTexelSize.x);
        m_bloomBlurShader->SetFloat("uDirectionY", 0.0f);
        m_bloomBlurShader->SetFloat("uBlurRadius", m_bloomBlurRadius);
        m_bloomBlurShader->BindTextureSlot(0, m_bloomExtractTarget.srvCpuHandle);
        DrawFullscreenToTarget(
            *m_bloomBlurShader,
            const_cast<InternalTarget&>(m_bloomBlurTarget),
            bloomWidth,
            bloomHeight,
            bloomClear);

        m_bloomBlurShader->SetInt("uInput", 0);
        m_bloomBlurShader->SetFloat("uDirectionX", 0.0f);
        m_bloomBlurShader->SetFloat("uDirectionY", bloomTexelSize.y);
        m_bloomBlurShader->SetFloat("uBlurRadius", m_bloomBlurRadius);
        m_bloomBlurShader->BindTextureSlot(0, m_bloomBlurTarget.srvCpuHandle);
        DrawFullscreenToTarget(
            *m_bloomBlurShader,
            const_cast<InternalTarget&>(m_bloomBlur2Target),
            bloomWidth,
            bloomHeight,
            bloomClear);

        bloomSrv = m_bloomBlur2Target.srvCpuHandle;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);

    if (IsPbrMaterialDebugMode(m_debugMode))
    {
        m_debugChannelShader->Use(false, true);
        m_debugChannelShader->SetInt("uOutputRgb", 1);
        m_debugChannelShader->SetInt("uInput", 0);
        m_debugChannelShader->BindTextureSlot(0, hdrColorSrv);
        m_debugChannelShader->FlushUniforms();
        DrawFullscreenQuad();
        CaptureSsaoDiagnosticsCpu(
            runSsao,
            compositeRan,
            compositeUsesSsao,
            pbrDebugActive,
            useShadowFactorComposite,
            hdrColorSource,
            ssaoDebugViewSource,
            hdrColorSrv,
            shadowFactorSrv);
        if (m_logSsaoApplySnapshot)
        {
            m_pendingSsaoGpuReadback = true;
        }
        return;
    }

    if (IsPostProcessDebugMode(m_debugMode))
    {
        std::uintptr_t debugSrv = 0;
        if (m_debugMode == RenderDebugMode::Ssao)
        {
            debugSrv = m_ssaoTarget.srvCpuHandle;
            ssaoDebugViewSource = runSsao
                ? ((m_ssaoShaderDebugMode != 0) ? "ssao_raw_debug" : "ssao_blur_live")
                : "ssao_blur_stale_pass_off";
        }
        else if (m_debugMode == RenderDebugMode::CompositeOcclusion && runSsao)
        {
            debugSrv = m_hdrCompositeTarget.srvCpuHandle;
            ssaoDebugViewSource = "composite_occlusion";
        }

        if (debugSrv != 0)
        {
            m_debugChannelShader->Use(false, true);
            m_debugChannelShader->SetInt("uOutputRgb", 0);
            m_debugChannelShader->SetInt("uInput", 0);
            m_debugChannelShader->BindTextureSlot(0, debugSrv);
            m_debugChannelShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runSsao,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                ssaoDebugViewSource,
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
    }

    const bool useFxaa = m_antiAliasingMode == AntiAliasingMode::FXAA;
    const bool useSmaa = m_antiAliasingMode == AntiAliasingMode::SMAA;
    const bool useTaa = m_antiAliasingMode == AntiAliasingMode::TAA;
    const bool useSsaa = m_antiAliasingMode == AntiAliasingMode::SSAA;
    const bool needsLdrIntermediate =
        useFxaa || useSmaa || useTaa || (useSsaa && m_viewportWidth > 0 && m_viewportHeight > 0
        && (m_width != m_viewportWidth || m_height != m_viewportHeight));
    const float ldrClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

    auto runTonemapPass = [&](const bool drawToLdrTarget) {
        m_tonemapShader->Use(false, true);
        m_tonemapShader->SetInt("uHdrColor", 0);
        m_tonemapShader->SetFloat("uExposure", m_exposure);
        m_tonemapShader->SetInt("uTonemapMode", static_cast<int>(m_tonemapMode));
        m_tonemapShader->SetInt("uUseBloom", m_bloomEnabled ? 1 : 0);
        m_tonemapShader->SetFloat("uBloomIntensity", m_bloomIntensity);
        m_tonemapShader->SetInt("uBloom", 1);
        m_tonemapShader->BindTextureSlot(0, hdrColorSrv);
        if (m_bloomEnabled)
        {
            m_tonemapShader->BindTextureSlot(1, bloomSrv);
        }
        if (drawToLdrTarget)
        {
            DrawFullscreenToTarget(
                *m_tonemapShader,
                const_cast<InternalTarget&>(m_ldrTonemapTarget),
                m_width,
                m_height,
                ldrClear,
                true);
        }
        else
        {
            DrawFullscreenPass(*m_tonemapShader, true);
        }
    };

    const bool ldrTargetReady =
        m_ldrTonemapTarget.srvCpuHandle != 0 && m_width > 0 && m_height > 0;

    auto blitLdrToViewport = [&](const std::uintptr_t sourceSrv) {
        BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        m_downsampleShader->Use(false, true);
        m_downsampleShader->BindTextureSlot(0, sourceSrv);
        DrawFullscreenPass(*m_downsampleShader, true);
    };

    if (needsLdrIntermediate && ldrTargetReady)
    {
        runTonemapPass(true);

        if (useFxaa)
        {
            BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
            m_fxaaShader->Use(false, true);
            m_fxaaShader->SetFloat("uTexelSizeX", texelSize.x);
            m_fxaaShader->SetFloat("uTexelSizeY", texelSize.y);
            m_fxaaShader->SetFloat("uSubpixQuality", m_fxaaSubpixQuality);
            m_fxaaShader->SetFloat("uEdgeThreshold", m_fxaaEdgeThreshold);
            m_fxaaShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            DrawFullscreenPass(*m_fxaaShader, true);
        }
        else if (useSmaa)
        {
            m_smaaEdgeShader->Use(false, true);
            m_smaaEdgeShader->SetFloat("uTexelSizeX", texelSize.x);
            m_smaaEdgeShader->SetFloat("uTexelSizeY", texelSize.y);
            m_smaaEdgeShader->SetFloat("uThreshold", m_smaaThreshold);
            m_smaaEdgeShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            DrawFullscreenToTarget(
                *m_smaaEdgeShader,
                const_cast<InternalTarget&>(m_smaaEdgeTarget),
                m_width,
                m_height,
                ldrClear,
                true);

            m_smaaNeighborShader->Use(false, true);
            m_smaaNeighborShader->SetFloat("uTexelSizeX", texelSize.x);
            m_smaaNeighborShader->SetFloat("uTexelSizeY", texelSize.y);
            m_smaaNeighborShader->SetFloat("uSearchSteps", static_cast<float>(m_smaaSearchSteps));
            m_smaaNeighborShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            m_smaaNeighborShader->BindTextureSlot(1, m_smaaEdgeTarget.srvCpuHandle);
            DrawFullscreenToTarget(
                *m_smaaNeighborShader,
                const_cast<InternalTarget&>(m_smaaOutputTarget),
                m_width,
                m_height,
                ldrClear,
                true);

            blitLdrToViewport(m_smaaOutputTarget.srvCpuHandle);
        }
        else if (useTaa)
        {
            const glm::mat4 viewProjection = projectionMatrix * camera.GetViewMatrix();
            const glm::mat4 invViewProjection = glm::inverse(viewProjection);

            m_taaShader->Use(false, true);
            m_taaShader->SetMat4("uInvViewProj", invViewProjection);
            m_taaShader->SetMat4("uPrevViewProj", m_prevViewProjection);
            m_taaShader->SetFloat("uBlendFactor", m_taaBlendFactor);
            m_taaShader->SetFloat("uHistoryValid", m_taaHistoryValid ? 1.0f : 0.0f);
            m_taaShader->SetFloat("uTexelSizeX", texelSize.x);
            m_taaShader->SetFloat("uTexelSizeY", texelSize.y);
            m_taaShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            m_taaShader->BindTextureSlot(1, m_taaHistoryTarget.srvCpuHandle);
            m_taaShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(
                *m_taaShader,
                const_cast<InternalTarget&>(m_taaResolveTarget),
                m_width,
                m_height,
                ldrClear,
                true);

            blitLdrToViewport(m_taaResolveTarget.srvCpuHandle);

            std::swap(
                const_cast<InternalTarget&>(m_taaHistoryTarget),
                const_cast<InternalTarget&>(m_taaResolveTarget));
            m_taaHistoryValid = true;
        }
        else if (useSsaa && (m_width != viewportWidth || m_height != viewportHeight))
        {
            blitLdrToViewport(m_ldrTonemapTarget.srvCpuHandle);
        }
        else
        {
            BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
            m_downsampleShader->Use(false, true);
            m_downsampleShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            DrawFullscreenPass(*m_downsampleShader, true);
        }
    }
    else
    {
        BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        runTonemapPass(false);
    }

    if (m_logHdrApplySnapshot)
    {
        m_logHdrApplySnapshot = false;
        std::uint32_t srvUsed = 0;
        std::uint32_t srvCapacity = 0;
        GfxContext::Get().GetSrvDescriptorUsage(srvUsed, srvCapacity);
        RenderPathDiagnostics::LogHdrApplySnapshot(
            m_width,
            m_height,
            viewportWidth,
            viewportHeight,
            m_sceneFramebuffer->IsValid(),
            m_sceneFramebuffer->HasSplitLighting(),
            runSsao,
            useShadowFactorComposite,
            outputTarget != nullptr,
            hdrColorSrv,
            m_sceneFramebuffer->GetColorSrvCpuHandle(0),
            m_sceneFramebuffer->GetColorSrvCpuHandle(1),
            m_sceneFramebuffer->GetDepthSrvCpuHandle(),
            srvUsed,
            srvCapacity);
    }

    CaptureSsaoDiagnosticsCpu(
        runSsao,
        compositeRan,
        compositeUsesSsao,
        pbrDebugActive,
        useShadowFactorComposite,
        hdrColorSource,
        ssaoDebugViewSource,
        hdrColorSrv,
        shadowFactorSrv);
    if (m_logSsaoApplySnapshot)
    {
        m_pendingSsaoGpuReadback = true;
    }
}

bool ScreenSpaceEffects::IsEnabled() const
{
    return m_enabled;
}

void ScreenSpaceEffects::SetEnabled(bool enabled)
{
    if (m_enabled == enabled)
    {
        return;
    }

    m_enabled = enabled;
    RenderPathDiagnostics::LogHdrToggled(enabled);
    m_logHdrApplySnapshot = true;
}

bool ScreenSpaceEffects::IsSsaoEnabled() const
{
    return m_ssaoEnabled;
}

void ScreenSpaceEffects::SetSsaoEnabled(bool enabled)
{
    if (m_ssaoEnabled == enabled)
    {
        return;
    }

    m_ssaoEnabled = enabled;
    RenderPathDiagnostics::LogSsaoToggled(enabled);
    m_logSsaoApplySnapshot = true;
}

const SsaoDiagnosticsSnapshot& ScreenSpaceEffects::GetSsaoDiagnostics() const
{
    return m_ssaoDiagnostics;
}

void ScreenSpaceEffects::FinalizePendingSsaoGpuReadback() const
{
    if (!m_pendingSsaoGpuReadback)
    {
        return;
    }

    m_pendingSsaoGpuReadback = false;

    if (m_width <= 0 || m_height <= 0)
    {
        if (m_logSsaoApplySnapshot)
        {
            m_logSsaoApplySnapshot = false;
            RenderPathDiagnostics::LogSsaoApplySnapshot(m_ssaoDiagnostics);
        }
        return;
    }

    const int centerX = m_width / 2;
    const int centerY = m_height / 2;
    float rgba[4] = {};

    if (ReadbackTextureCenterRgba16F(
            m_ssaoTarget.resource,
            m_width,
            m_height,
            centerX,
            centerY,
            rgba))
    {
        m_ssaoDiagnostics.centerSsaoRaw = rgba[0];
        m_ssaoDiagnostics.gpuReadbackValid = true;
    }

    if (m_sceneFramebuffer != nullptr)
    {
        float hardwareDepth = -1.0f;
        if (ReadbackDepthCenter(
                m_sceneFramebuffer->GetDepthResource(),
                m_width,
                m_height,
                centerX,
                centerY,
                hardwareDepth))
        {
            m_ssaoDiagnostics.centerHardwareDepth = hardwareDepth;
            m_ssaoDiagnostics.centerDepth = hardwareDepth;
            m_ssaoDiagnostics.gpuReadbackValid = true;
        }
    }

    if (ReadbackTextureCenterRgba16F(
            m_ssaoTarget.resource,
            m_width,
            m_height,
            centerX,
            centerY,
            rgba))
    {
        m_ssaoDiagnostics.centerSsaoBlur = rgba[0];
        m_ssaoDiagnostics.gpuReadbackValid = true;
    }

    if (m_sceneFramebuffer != nullptr && m_sceneFramebuffer->HasGeometryNormals())
    {
        if (ReadbackTextureCenterRgba16F(
                m_sceneFramebuffer->GetColorResource(2),
                m_width,
                m_height,
                centerX,
                centerY,
                rgba))
        {
            m_ssaoDiagnostics.centerNormalR = rgba[0];
            m_ssaoDiagnostics.centerNormalG = rgba[1];
            m_ssaoDiagnostics.centerNormalB = rgba[2];
            m_ssaoDiagnostics.gpuReadbackValid = true;
        }
    }

    if (m_logSsaoApplySnapshot)
    {
        m_logSsaoApplySnapshot = false;
        RenderPathDiagnostics::LogSsaoApplySnapshot(m_ssaoDiagnostics);
    }
}

void ScreenSpaceEffects::CaptureSsaoDiagnosticsCpu(
    const bool runSsao,
    const bool compositeRan,
    const bool compositeUsesSsao,
    const bool pbrDebugActive,
    const bool useShadowFactorComposite,
    const char* hdrColorSource,
    const char* ssaoDebugViewSource,
    const std::uintptr_t hdrColorSrv,
    const std::uintptr_t shadowFactorSrv) const
{
    ++m_ssaoDiagnosticsFrame;
    m_ssaoDiagnostics.captureFrame = m_ssaoDiagnosticsFrame;
    m_ssaoDiagnostics.enabled = m_ssaoEnabled;
    m_ssaoDiagnostics.postProcessEnabled = m_enabled;
    m_ssaoDiagnostics.passExecuted = runSsao;
    m_ssaoDiagnostics.compositeUsesSsao = compositeUsesSsao;
    m_ssaoDiagnostics.compositeRan = compositeRan;
    m_ssaoDiagnostics.shadowComposite = useShadowFactorComposite;
    m_ssaoDiagnostics.splitLighting = m_sceneFramebuffer->HasSplitLighting();
    m_ssaoDiagnostics.geometryNormals = m_sceneFramebuffer->HasGeometryNormals();
    m_ssaoDiagnostics.pbrDebugActive = pbrDebugActive;
    m_ssaoDiagnostics.debugMode = m_debugMode;
    m_ssaoDiagnostics.sceneWidth = m_width;
    m_ssaoDiagnostics.sceneHeight = m_height;
    m_ssaoDiagnostics.depthSrv = m_sceneFramebuffer->GetDepthSrvCpuHandle();
    m_ssaoDiagnostics.normalSrv = m_sceneFramebuffer->GetColorSrvCpuHandle(2);
    m_ssaoDiagnostics.noiseSrv = m_noiseTexture.srvCpuHandle;
    m_ssaoDiagnostics.ssaoRawSrv = m_ssaoTarget.srvCpuHandle;
    m_ssaoDiagnostics.ssaoBlurSrv = m_ssaoBlurTarget.srvCpuHandle;
    m_ssaoDiagnostics.hdrColorSrv = hdrColorSrv;
    m_ssaoDiagnostics.shadowFactorSrv = shadowFactorSrv;
    m_ssaoDiagnostics.hasUniformSamples = m_ssaoShader->HasUniform("uSamples");
    m_ssaoDiagnostics.hasUniformKernelSize = m_ssaoShader->HasUniform("uKernelSize");
    m_ssaoDiagnostics.kernelCount = KernelSampleCount;
    if (!m_kernelSamples.empty())
    {
        m_ssaoDiagnostics.kernelSample0X = m_kernelSamples[0].x;
        m_ssaoDiagnostics.kernelSample0Y = m_kernelSamples[0].y;
        m_ssaoDiagnostics.kernelSample0Z = m_kernelSamples[0].z;
    }
    m_ssaoDiagnostics.radius = m_ssaoRadius;
    m_ssaoDiagnostics.bias = m_ssaoBias;
    m_ssaoDiagnostics.aoStrength = m_aoStrength;
    m_ssaoDiagnostics.ssaoPower = m_ssaoPower;
    m_ssaoDiagnostics.hdrColorSource = hdrColorSource != nullptr ? hdrColorSource : "null";
    m_ssaoDiagnostics.ssaoDebugViewSource =
        ssaoDebugViewSource != nullptr ? ssaoDebugViewSource : "null";
}

float ScreenSpaceEffects::GetSsaoRadius() const
{
    return m_ssaoRadius;
}

void ScreenSpaceEffects::SetSsaoRadius(float radius)
{
    m_ssaoRadius = std::max(radius, 0.01f);
}

float ScreenSpaceEffects::GetSsaoBias() const
{
    return m_ssaoBias;
}

void ScreenSpaceEffects::SetSsaoBias(float bias)
{
    m_ssaoBias = std::max(bias, 0.0f);
}

float ScreenSpaceEffects::GetSsaoPower() const
{
    return m_ssaoPower;
}

void ScreenSpaceEffects::SetSsaoPower(float power)
{
    m_ssaoPower = std::max(power, 0.1f);
}

int ScreenSpaceEffects::GetSsaoShaderDebugMode() const
{
    return m_ssaoShaderDebugMode;
}

void ScreenSpaceEffects::SetSsaoShaderDebugMode(const int mode)
{
    m_ssaoShaderDebugMode = std::clamp(mode, 0, 6);
}

float ScreenSpaceEffects::GetAoStrength() const
{
    return m_aoStrength;
}

void ScreenSpaceEffects::SetAoStrength(float strength)
{
    m_aoStrength = std::clamp(strength, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetExposure() const
{
    return m_exposure;
}

void ScreenSpaceEffects::SetExposure(float exposure)
{
    m_exposure = std::clamp(exposure, -4.0f, 6.0f);
}

TonemapMode ScreenSpaceEffects::GetTonemapMode() const
{
    return m_tonemapMode;
}

void ScreenSpaceEffects::SetTonemapMode(TonemapMode mode)
{
    m_tonemapMode = mode;
}

bool ScreenSpaceEffects::IsBloomEnabled() const
{
    return m_bloomEnabled;
}

void ScreenSpaceEffects::SetBloomEnabled(bool enabled)
{
    m_bloomEnabled = enabled;
}

float ScreenSpaceEffects::GetBloomThreshold() const
{
    return m_bloomThreshold;
}

void ScreenSpaceEffects::SetBloomThreshold(float threshold)
{
    m_bloomThreshold = std::max(threshold, 0.0f);
}

float ScreenSpaceEffects::GetBloomSoftKnee() const
{
    return m_bloomSoftKnee;
}

void ScreenSpaceEffects::SetBloomSoftKnee(float softKnee)
{
    m_bloomSoftKnee = std::clamp(softKnee, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetBloomIntensity() const
{
    return m_bloomIntensity;
}

void ScreenSpaceEffects::SetBloomIntensity(float intensity)
{
    m_bloomIntensity = std::max(intensity, 0.0f);
}

float ScreenSpaceEffects::GetBloomBlurRadius() const
{
    return m_bloomBlurRadius;
}

void ScreenSpaceEffects::SetBloomBlurRadius(float blurRadius)
{
    m_bloomBlurRadius = std::clamp(blurRadius, 0.25f, 4.0f);
}

RenderDebugMode ScreenSpaceEffects::GetDebugMode() const
{
    return m_debugMode;
}

void ScreenSpaceEffects::SetDebugMode(const RenderDebugMode mode)
{
    m_debugMode = mode;
}

AntiAliasingMode ScreenSpaceEffects::GetAntiAliasingMode() const
{
    return m_antiAliasingMode;
}

void ScreenSpaceEffects::SetAntiAliasingMode(const AntiAliasingMode mode)
{
    if (mode == AntiAliasingMode::MSAA)
    {
        return;
    }

    if (m_antiAliasingMode != mode)
    {
        ResetTaaHistory();
        m_lastAntiAliasingMode = mode;
        m_width = 0;
        m_height = 0;
    }

    m_antiAliasingMode = mode;
}

float ScreenSpaceEffects::GetRenderScale() const
{
    return m_renderScale;
}

void ScreenSpaceEffects::SetRenderScale(const float scale)
{
    const float clampedScale = std::clamp(scale, 1.0f, 2.0f);
    if (m_renderScale != clampedScale)
    {
        m_renderScale = clampedScale;
        m_width = 0;
        m_height = 0;
    }
}

float ScreenSpaceEffects::GetTaaBlendFactor() const
{
    return m_taaBlendFactor;
}

void ScreenSpaceEffects::SetTaaBlendFactor(const float factor)
{
    m_taaBlendFactor = std::clamp(factor, 0.0f, 0.99f);
}

float ScreenSpaceEffects::GetSmaaThreshold() const
{
    return m_smaaThreshold;
}

void ScreenSpaceEffects::SetSmaaThreshold(const float threshold)
{
    m_smaaThreshold = std::clamp(threshold, 0.01f, 0.25f);
}

int ScreenSpaceEffects::GetSmaaSearchSteps() const
{
    return m_smaaSearchSteps;
}

void ScreenSpaceEffects::SetSmaaSearchSteps(const int steps)
{
    m_smaaSearchSteps = std::clamp(steps, 1, 8);
}

float ScreenSpaceEffects::GetFxaaSubpixQuality() const
{
    return m_fxaaSubpixQuality;
}

void ScreenSpaceEffects::SetFxaaSubpixQuality(const float quality)
{
    m_fxaaSubpixQuality = std::clamp(quality, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetFxaaEdgeThreshold() const
{
    return m_fxaaEdgeThreshold;
}

void ScreenSpaceEffects::SetFxaaEdgeThreshold(const float threshold)
{
    m_fxaaEdgeThreshold = std::clamp(threshold, 0.03125f, 0.5f);
}

float ScreenSpaceEffects::GetSsaoBlurDepthThreshold() const
{
    return m_ssaoBlurDepthThreshold;
}

void ScreenSpaceEffects::SetSsaoBlurDepthThreshold(const float threshold)
{
    m_ssaoBlurDepthThreshold = std::clamp(threshold, 0.001f, 0.25f);
}

void ScreenSpaceEffects::BlitDepthToFramebuffer(
    const std::uintptr_t drawFramebuffer,
    const int viewportWidth,
    const int viewportHeight) const
{
    (void)drawFramebuffer;
    (void)viewportWidth;
    (void)viewportHeight;
    // Depth texture copies are not reliable across D3D12 drivers; gizmos draw without
    // viewport depth for now.
}
