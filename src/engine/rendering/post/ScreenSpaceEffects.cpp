#include "engine/rendering/post/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"
#include "engine/platform/system/ExceptionMessage.h"
#include "engine/platform/diagnostics/RenderPathDiagnostics.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/core/RenderDebug.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/rendering/post/AmbientOcclusionPass.h"
#include "engine/rendering/post/BloomTonemapPass.h"
#include "engine/rendering/post/DxrDebugBlitPass.h"
#include "engine/rendering/post/ScreenSpaceReflectionPass.h"
#include "engine/rendering/post/ScreenSpaceGiPass.h"
#include "engine/rendering/post/AntiAliasingPass.h"
#include "engine/rendering/post/DlssResolvePass.h"
#include "engine/rendering/post/PathTracerDisplayPass.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/post/effects/Apply.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace
{
    bool ArePathTracerGpuEventsEnabled()
    {
        static const bool enabled = [] {
            const char* const value = std::getenv("GAME_ENGINE_PT_GPU_EVENTS");
            return value == nullptr || std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    void BeginPathTracerGpuEvent(
        ID3D12GraphicsCommandList* const commandList,
        const wchar_t* const name,
        const UINT nameSize)
    {
        if (ArePathTracerGpuEventsEnabled())
        {
            commandList->BeginEvent(0, name, nameSize);
        }
    }

    void EndPathTracerGpuEvent(ID3D12GraphicsCommandList* const commandList)
    {
        if (ArePathTracerGpuEventsEnabled())
        {
            commandList->EndEvent();
        }
    }

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

    glm::vec3 SolidBackgroundLinear(const EnvironmentMap& environmentMap)
    {
        const glm::vec3 colorSrgb = environmentMap.GetSolidBackgroundColorSrgb();
        return glm::vec3(
            SrgbChannelToLinear(colorSrgb.x),
            SrgbChannelToLinear(colorSrgb.y),
            SrgbChannelToLinear(colorSrgb.z));
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

    bool MatricesNearEqual(const glm::mat4& left, const glm::mat4& right)
    {
        const float* lhs = glm::value_ptr(left);
        const float* rhs = glm::value_ptr(right);
        for (int index = 0; index < 16; ++index)
        {
            if (std::abs(lhs[index] - rhs[index]) > 0.00001f)
            {
                return false;
            }
        }
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
        ID3D12Resource* resource,
        const std::uint32_t descriptorIndex,
        DXGI_FORMAT format,
        std::uint32_t mipLevels)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = mipLevels;
        GfxContext::Get().CreateShaderResourceView(resource, &srvDesc, descriptorIndex);
    }

    [[noreturn]] void ThrowPostProcessTargetError(const char* phase)
    {
        const std::string gpuError = GfxContext::GetLastGpuAllocationError();
        std::string message = phase;
        if (!gpuError.empty())
        {
            message += ": ";
            message += gpuError;
        }

        throw std::runtime_error(message);
    }
}

ScreenSpaceEffects::ScreenSpaceEffects(const std::uint32_t dlssViewportId)
    : m_dlssViewportId(dlssViewportId),
      m_sceneFramebuffer(std::make_unique<Framebuffer>()),
      // SSR-07 rollout: depth/normal/velocity registers point-sampled in every screen-space
      // pass (bilinear depth at silhouettes produced streaks/halos). Address modes preserved
      // where a pass relies on them (SSAO noise tiling keeps its defaults).
      m_ssaoShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoFragmentShader,
          // s0 = depth, s1 = normals; s2 (tiled noise) keeps default addressing
          ShaderSamplerOverrides{(1u << 0) | (1u << 1), false})),
      m_gtaoShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::GtaoFragmentShader,
          // s0 = depth, s1 = normals
          ShaderSamplerOverrides{(1u << 0) | (1u << 1), true})),
      m_blurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsaoBlurFragmentShader,
          // s1 = depth, s2 = normals; s0 (AO input) stays linear
          ShaderSamplerOverrides{(1u << 1) | (1u << 2), true})),
      m_compositeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::ScreenCompositeFragmentShader,
          ShaderSamplerOverrides{0, false, 1u << 5})),
      m_bloomExtractShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomExtractFragmentShader)),
      m_bloomBlurShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomBlurFragmentShader)),
      m_bloomTemporalShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::BloomTemporalFragmentShader)),
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
      m_ptOpticalLayersShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtOpticalLayersFragmentShader)),
      m_taaShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::TaaFragmentShader,
          // s2 = depth, s3 = velocity; s0/s1 (color/history) stay linear
          ShaderSamplerOverrides{(1u << 2) | (1u << 3), true})),
      m_smaaEdgeShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SmaaEdgeFragmentShader)),
      m_smaaNeighborShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SmaaNeighborFragmentShader)),
      m_debugChannelShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DebugChannelFragmentShader)),
      m_rtReflectionResolveShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RtReflectionResolveFragmentShader)),
      m_dxrPrimaryDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DxrPrimaryDebugFragmentShader)),
      m_ptAccumulateShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtAccumulateFragmentShader)),
      m_ptMeanShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtMeanFragmentShader)),
      m_ptTemporalStatsShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtTemporalStatsFragmentShader,
          ShaderSamplerOverrides{(1u << 0) | (1u << 1) | (1u << 2) | (1u << 4), true})),
      m_ptTemporalQualityShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtTemporalQualityFragmentShader,
          ShaderSamplerOverrides{(1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4), true})),
      m_ptTemporalStatsDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtTemporalStatsDebugFragmentShader,
          ShaderSamplerOverrides{(1u << 0), true})),
      m_ptMotionReprojectionDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtMotionReprojectionDebugFragmentShader,
          ShaderSamplerOverrides{
              (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4), true})),
      m_ptMotionDepthCopyShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtMotionDepthCopyFragmentShader,
          ShaderSamplerOverrides{(1u << 0), true})),
      m_ptBoilMetricShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::PtBoilMetricFragmentShader,
          ShaderSamplerOverrides{(1u << 0) | (1u << 1), true})),
      m_dxrShadowDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DxrShadowDebugFragmentShader)),
      m_velocityDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::VelocityDebugFragmentShader)),
      m_gbufferDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::GBufferDebugFragmentShader)),
      m_radianceAssemblyShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RadianceAssemblyFragmentShader)),
      m_radianceDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RadianceDebugFragmentShader)),
      m_temporalReprojectShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::TemporalReprojectFragmentShader,
          ShaderSamplerOverrides{1u << 2, false, 0})),
      m_giDepthHistoryShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::GiDepthHistoryFragmentShader)),
      m_dlssMotionDilateShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DlssMotionDilateFragmentShader,
          ShaderSamplerOverrides{(1u << 0) | (1u << 1), true})),
      m_dlssMotionCopyShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DlssMotionCopyFragmentShader,
          ShaderSamplerOverrides{(1u << 0), true})),
      m_dlssZeroMotionShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DlssZeroMotionFragmentShader)),
      m_rrMotionValidityShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RrMotionValidityFragmentShader,
          ShaderSamplerOverrides{(1u << 0) | (1u << 1), true})),
      m_giTemporalDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::GiTemporalDebugFragmentShader)),
      m_ssgiNoiseInjectShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsgiNoiseInjectFragmentShader,
          // s1 = depth; s0 (radiance) stays linear
          ShaderSamplerOverrides{(1u << 1), true})),
      m_ssgiDenoiseSpatialShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsgiDenoiseSpatialFragmentShader,
          // s1 = depth, s2 = normals, s3 = material0; s0 (input) stays linear
          ShaderSamplerOverrides{(1u << 1) | (1u << 2) | (1u << 3), true})),
      m_ssgiDenoiseDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsgiDenoiseDebugFragmentShader)),
      m_ssgiTraceShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsgiTraceFragmentShader,
          // s0 = depth, s1 = normals, s2/s3 = materials; s4 (radiance) stays linear but clamps
          ShaderSamplerOverrides{(1u << 0) | (1u << 1) | (1u << 2) | (1u << 3), true})),
      // SSR-07: SSR passes opt in to explicit samplers. pointSampleRegisterMask bits follow
      // each shader's register(sN) declarations; clampAllRegisters stops the default WRAP on
      // s4-s7 from wrapping G-buffer/LUT reads across screen edges.
      m_ssrSceneColorShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrSceneColorFragmentShader,
          // s2 = depth
          ShaderSamplerOverrides{(1u << 2), true})),
      m_ssrDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrDebugFragmentShader)),
      m_ssrTraceShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrTraceFragmentShader,
          // s0 = depth, s1 = normals, s2 = material0; s3 (scene color) stays linear
          ShaderSamplerOverrides{(1u << 0) | (1u << 1) | (1u << 2), true})),
      m_ssrTraceDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrTraceDebugFragmentShader)),
      m_ssrDenoiseDebugShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrDenoiseDebugFragmentShader)),
      m_ssrSvgfTemporalShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrSvgfTemporalFragmentShader,
          // s2 = velocity, s3 = depth, s4 = normals, s5 = history depth;
          // s0/s1 (color/history) stay linear
          ShaderSamplerOverrides{(1u << 2) | (1u << 3) | (1u << 4) | (1u << 5), true})),
      m_ssrSvgfVarianceTemporalShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrSvgfVarianceTemporalFragmentShader,
          // s3 = velocity, s4 = depth
          ShaderSamplerOverrides{(1u << 3) | (1u << 4), true})),
      m_ssrSvgfAtrousShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrSvgfAtrousFragmentShader,
          // s1 = variance, s2 = depth, s3 = normals, s4 = material0
          ShaderSamplerOverrides{(1u << 1) | (1u << 2) | (1u << 3) | (1u << 4), true})),
      m_ssrUpscaleShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrUpscaleFragmentShader,
          // s1 = depth, s2 = normals, s3 = material0; s0 (trace color) stays linear
          ShaderSamplerOverrides{(1u << 1) | (1u << 2) | (1u << 3), true})),
      m_ssrIndirectShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::SsrIndirectFragmentShader,
          // s2 = depth, s3 = normals, s4/s5 = materials; s6 (prefilter cube) and
          // s7 (BRDF LUT) stay linear but now CLAMP instead of wrapping at grazing angles
          ShaderSamplerOverrides{(1u << 2) | (1u << 3) | (1u << 4) | (1u << 5), true})),
      m_dxrIndirectShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DxrIndirectFragmentShader,
          // s0 = linear clamp (colors/env/LUT), s1 = point clamp (G-buffer + raw hitDist)
          ShaderSamplerOverrides{(1u << 1), true})),
      m_dxrGiInjectShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DxrGiInjectFragmentShader,
          // s0 = linear clamp (colors/GI radiance), s1 = point clamp (G-buffer)
          ShaderSamplerOverrides{(1u << 1), true})),
      m_rrGuidesShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RrGuidesFragmentShader,
          // s0 = point clamp (G-buffer reads)
          ShaderSamplerOverrides{(1u << 0), true})),
      m_rrTemporalValidityShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RrTemporalValidityFragmentShader,
          ShaderSamplerOverrides{0x7fu, true})),
      m_rrHistoryCopyShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::RrHistoryCopyFragmentShader,
          ShaderSamplerOverrides{1u, true})),
      m_draw(m_quadVb)
{
    SceneRenderTrace::Section initSection("sse-init");
    {
        SceneRenderTrace::Scope quadScope("CreateFullscreenQuad");
        CreateFullscreenQuad();
        quadScope.Success();
    }
    {
        SceneRenderTrace::Scope kernelScope("CreateKernel");
        CreateKernel();
        kernelScope.Success();
    }
    {
        SceneRenderTrace::Scope noiseScope("CreateNoiseTexture");
        CreateNoiseTexture();
        noiseScope.Success();
    }
    initSection.Success();

    // Grid alpha-blends into the resolved scene HDR buffer before bloom; selection overlay after tonemap.
    // Stage 3+: Bloom toggle, depth blit for gizmo occlusion, play-mode parity.
}

void ScreenSpaceEffects::PrewarmShaderStages()
{
    static const std::array<HlslStageCompileRequest, 55> kStages = {
        HlslStageCompileRequest{EngineConstants::FullscreenVertexShader, "main", "vs_6_0"},
        HlslStageCompileRequest{EngineConstants::SsaoFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::GtaoFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsaoBlurFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::ScreenCompositeFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::BloomExtractFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::BloomBlurFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::BloomTemporalFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::ShadowBlurFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::TonemapFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::FxaaFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DownsampleFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtOpticalLayersFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::TaaFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SmaaEdgeFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SmaaNeighborFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DebugChannelFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::RtReflectionResolveFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DxrPrimaryDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtAccumulateFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtMeanFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtTemporalStatsFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtTemporalStatsDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtMotionReprojectionDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtMotionDepthCopyFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::PtBoilMetricFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DxrShadowDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::VelocityDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::GBufferDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::RadianceAssemblyFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::RadianceDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::TemporalReprojectFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::GiDepthHistoryFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DlssMotionDilateFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DlssMotionCopyFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DlssZeroMotionFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::RrMotionValidityFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::GiTemporalDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsgiNoiseInjectFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsgiDenoiseSpatialFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsgiDenoiseDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsgiTraceFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrSceneColorFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrTraceFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrTraceDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrDenoiseDebugFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrSvgfTemporalFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrSvgfVarianceTemporalFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrSvgfAtrousFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrUpscaleFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::SsrIndirectFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DxrIndirectFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::DxrGiInjectFragmentShader, "main", "ps_6_0"},
        HlslStageCompileRequest{EngineConstants::RrGuidesFragmentShader, "main", "ps_6_0"},
    };
    PrewarmHlslStages(std::vector<HlslStageCompileRequest>(kStages.begin(), kStages.end()));
}

ScreenSpaceEffects::~ScreenSpaceEffects()
{
    DestroyInternalTarget(m_ssaoTarget);
    DestroyInternalTarget(m_ssaoBlurTarget);
    DestroyInternalTarget(m_gtaoRawTarget);
    DestroyInternalTarget(m_shadowBlurTarget);
    DestroyInternalTarget(m_shadowBlur2Target);
    DestroyInternalTarget(m_hdrCompositeTarget);
    DestroyInternalTarget(m_radianceTarget);
    DestroyInternalTarget(m_radianceTraceInputTarget);
    DestroyInternalTarget(m_radianceSpatialBlurTarget);
    DestroyInternalTarget(m_radianceSpatialTarget);
    DestroyInternalTarget(m_radianceHistoryTarget);
    DestroyInternalTarget(m_radianceTemporalTarget);
    DestroyInternalTarget(m_radianceHistoryDepthTarget);
    DestroyInternalTarget(m_ssrSceneColorTarget);
    DestroyInternalTarget(m_ssrTraceTarget);
    DestroyInternalTarget(m_ssrSpatialBlurTarget);
    DestroyInternalTarget(m_ssrSpatialTarget);
    DestroyInternalTarget(m_ssrHistoryTarget);
    DestroyInternalTarget(m_ssrTemporalTarget);
    DestroyInternalTarget(m_ssrVarianceHistoryTarget);
    DestroyInternalTarget(m_ssrVarianceTemporalTarget);
    DestroyInternalTarget(m_ssrHistoryDepthTarget);
    DestroyInternalTarget(m_ssrResolvedTarget);
    DestroyInternalTarget(m_ssrIndirectTarget);
    DestroyInternalTarget(m_rtIndirectTarget);
    DestroyInternalTarget(m_rtGiInjectTarget);
    DestroyInternalTarget(m_rrDiffuseAlbedoTarget);
    DestroyInternalTarget(m_rrSpecularAlbedoTarget);
    DestroyInternalTarget(m_rrNormalRoughnessTarget);
    DestroyInternalTarget(m_rrSpecularHitDistanceTarget);
    DestroyInternalTarget(m_rrOpticalTransmissionDiffuseAlbedoTarget);
    DestroyInternalTarget(m_rrOpticalTransmissionSpecularAlbedoTarget);
    DestroyInternalTarget(m_rrOpticalTransmissionNormalRoughnessTarget);
    DestroyInternalTarget(m_rrTemporalPrimaryMaskTarget);
    DestroyInternalTarget(m_rrTemporalTransmissionMaskTarget);
    DestroyInternalTarget(m_rrTemporalPrimaryMotionTarget);
    DestroyInternalTarget(m_rrTemporalTransmissionMotionTarget);
    DestroyInternalTarget(m_rrTemporalPrimaryValidityDiagnosticsTarget);
    DestroyInternalTarget(m_rrTemporalTransmissionValidityDiagnosticsTarget);
    DestroyInternalTarget(m_rrTemporalPrimaryPrevDepthTarget);
    DestroyInternalTarget(m_rrTemporalPrimaryPrevNormalTarget);
    DestroyInternalTarget(m_rrTemporalPrimaryPrevOwnerTarget);
    DestroyInternalTarget(m_rrTemporalTransmissionPrevDepthTarget);
    DestroyInternalTarget(m_rrTemporalTransmissionPrevNormalTarget);
    DestroyInternalTarget(m_rrTemporalTransmissionPrevOwnerTarget);
    DestroyInternalTarget(m_bloomExtractTarget);
    DestroyInternalTarget(m_bloomBlurTarget);
    DestroyInternalTarget(m_bloomBlur2Target);
    DestroyInternalTarget(m_bloomHistoryTarget);
    DestroyInternalTarget(m_bloomTemporalTarget);
    DestroyInternalTarget(m_ldrTonemapTarget);
    DestroyInternalTarget(m_smaaEdgeTarget);
    DestroyInternalTarget(m_smaaOutputTarget);
    DestroyInternalTarget(m_taaHistoryTarget);
    DestroyInternalTarget(m_taaResolveTarget);
    DestroyInternalTarget(m_ptAccumSumTarget);
    DestroyInternalTarget(m_ptAccumScratchTarget);
    DestroyInternalTarget(m_ptTemporalStatsTarget);
    DestroyInternalTarget(m_ptTemporalStatsScratchTarget);
    DestroyInternalTarget(m_ptTemporalQualityTarget);
    DestroyInternalTarget(m_ptTemporalPrevRadianceTarget);
    DestroyInternalTarget(m_ptTemporalPrevDepthTarget);
    DestroyInternalTarget(m_ptBoilMetricTarget);
    DestroyInternalTarget(m_dlssOutputTarget);
    DestroyInternalTarget(m_dlssOpticalTransmissionOutputTarget);
    DestroyInternalTarget(m_dlssOpticalCompositeTarget);
    DestroyInternalTarget(m_ptOpticalReflectionInputTarget);
    DestroyInternalDepthTarget(m_dlssDisplayDepthTarget);
    DestroyInternalDepthTarget(m_ptDlssDepthTarget);
    DestroyInternalDepthTarget(m_ptOpticalTransmissionDlssDepthTarget);
    DestroyInternalTarget(m_ptDlssMotionTarget);
    DestroyInternalTarget(m_dlssDilatedMotionTarget);
    DestroyInternalTarget(m_dlssOpticalTransmissionMotionTarget);
    for (PtBoilMetricReadbackSlot& slot : m_ptBoilMetricReadbackSlots)
    {
        if (slot.resource != nullptr || slot.allocation != nullptr)
        {
            GfxContext::Get().DeferredReleaseResource(slot.allocation, slot.resource);
            slot = {};
        }
    }
    DestroyInternalTarget(m_dlssBloomExtractTarget);
    DestroyInternalTarget(m_dlssBloomBlurTarget);
    DestroyInternalTarget(m_dlssBloomBlur2Target);
    DestroyInternalTarget(m_dlssBloomHistoryTarget);
    DestroyInternalTarget(m_dlssBloomTemporalTarget);
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

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        ThrowPostProcessTargetError("Failed to create post-process render target");
    }

    target.resource = resource;
    target.allocation = allocation;
    target.width = width;
    target.height = height;
    target.format = format;
    target.resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    target.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (target.srvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        ThrowPostProcessTargetError("Failed to allocate post-process SRV descriptor");
    }

    target.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(target.srvIndex);
    target.rtvIndex = GfxContext::Get().AllocateOffscreenRtvBlock(1);
    if (target.rtvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        ThrowPostProcessTargetError("Failed to allocate post-process RTV descriptor");
    }

    CreateTexture2DSrv(resource, target.srvIndex, static_cast<DXGI_FORMAT>(format), 1);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex)};
    device->CreateRenderTargetView(resource, nullptr, rtvHandle);
}

void ScreenSpaceEffects::CreateInternalDepthTarget(
    InternalDepthTarget& target,
    const int width,
    const int height)
{
    DestroyInternalDepthTarget(target);

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = static_cast<UINT64>(width);
    resourceDesc.Height = static_cast<UINT>(height);
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        ThrowPostProcessTargetError("Failed to create display depth target");
    }

    target.resource = resource;
    target.allocation = allocation;
    target.width = width;
    target.height = height;
    target.resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE);

    target.dsvIndex = GfxContext::Get().AllocateOffscreenDsv();
    if (target.dsvIndex == UINT32_MAX)
    {
        DestroyInternalDepthTarget(target);
        ThrowPostProcessTargetError("Failed to allocate display depth DSV descriptor");
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{
        GfxContext::Get().GetOffscreenDsvCpuHandle(target.dsvIndex)};
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(resource, &dsvDesc, dsvHandle);

    target.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (target.srvIndex == UINT32_MAX)
    {
        DestroyInternalDepthTarget(target);
        ThrowPostProcessTargetError("Failed to allocate display depth SRV descriptor");
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    GfxContext::Get().CreateShaderResourceView(resource, &srvDesc, target.srvIndex);
    target.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(target.srvIndex);
}

void ScreenSpaceEffects::DestroyInternalDepthTarget(InternalDepthTarget& target) const
{
    if (!GfxContext::Get().IsInitialized())
    {
        target.dsvIndex = UINT32_MAX;
        target.srvIndex = UINT32_MAX;
        target.srvCpuHandle = 0;
        target.allocation = nullptr;
        target.resource = nullptr;
        target.width = 0;
        target.height = 0;
        target.resourceState =
            static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        return;
    }

    if (target.dsvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenDsv(target.dsvIndex);
        target.dsvIndex = UINT32_MAX;
    }
    if (target.srvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenSrv(target.srvIndex);
        target.srvIndex = UINT32_MAX;
    }
    target.srvCpuHandle = 0;

    if (target.allocation != nullptr || target.resource != nullptr)
    {
        GfxContext::Get().DeferredReleaseResource(target.allocation, target.resource);
    }

    target.allocation = nullptr;
    target.resource = nullptr;
    target.width = 0;
    target.height = 0;
    target.resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_DEPTH_WRITE);
}

void ScreenSpaceEffects::CreateUavTarget(
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
    // NGX writes via UAV; PT editor grid overlays rasterize into the same texture after evaluate.
    resourceDesc.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    ID3D12Resource* resource = nullptr;
    D3D12MA::Allocation* allocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource))))
    {
        ThrowPostProcessTargetError("Failed to create DLSS output UAV target");
    }

    target.resource = resource;
    target.allocation = allocation;
    target.width = width;
    target.height = height;
    target.format = format;
    target.resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    target.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (target.srvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        ThrowPostProcessTargetError("Failed to allocate DLSS output SRV descriptor");
    }
    target.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(target.srvIndex);

    CreateTexture2DSrv(resource, target.srvIndex, static_cast<DXGI_FORMAT>(format), 1);

    target.rtvIndex = GfxContext::Get().AllocateOffscreenRtvBlock(1);
    if (target.rtvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        ThrowPostProcessTargetError("Failed to allocate DLSS output RTV descriptor");
    }

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
        target.format = 0;
        target.resourceState =
            static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return;
    }

    // CRASH-01: targets can be destroyed mid-frame (e.g. Resize between Scene View and Game
    // View renders) while the current command list already references them. Defer the release
    // and the descriptor-slot recycling until the covering fence has completed.
    if (target.srvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenSrv(target.srvIndex);
        target.srvIndex = UINT32_MAX;
    }

    if (target.rtvIndex != UINT32_MAX)
    {
        GfxContext::Get().DeferredFreeOffscreenRtvBlock(target.rtvIndex, 1);
        target.rtvIndex = UINT32_MAX;
    }

    if (target.allocation != nullptr || target.resource != nullptr)
    {
        GfxContext::Get().DeferredReleaseResource(target.allocation, target.resource);
        target.allocation = nullptr;
    }

    target.resource = nullptr;
    target.srvCpuHandle = 0;
    target.width = 0;
    target.height = 0;
    target.format = 0;
    target.resourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void ScreenSpaceEffects::ResizeInternalTarget(
    InternalTarget& target,
    const int width,
    const int height,
    const int format)
{
    // Idempotent: only (re)allocate when the target does not already match. `CreateInternalTarget`
    // unconditionally destroys + reallocates a fresh (undefined-contents) texture, so recreating an
    // already-correct target every frame both wastes an allocation and — critically — WIPES any
    // accumulated contents. The path-tracer reference accumulation ping-pongs across frames into
    // m_ptAccumSumTarget/m_ptAccumScratchTarget; recreating them each frame made the running sum
    // never persist (dark, permanently-noisy image divided by an ever-growing sample count, with
    // ghost content from recycled heap memory). A resize must preserve contents when nothing changed.
    if (target.resource != nullptr
        && target.width == width
        && target.height == height
        && target.format == format)
    {
        return;
    }
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
    if (m_noiseTexture.srvIndex == UINT32_MAX)
    {
        textureAllocation->Release();
        textureResource->Release();
        throw std::runtime_error("Failed to allocate SSAO noise SRV descriptor");
    }
    m_noiseTexture.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(m_noiseTexture.srvIndex);
    CreateTexture2DSrv(textureResource, m_noiseTexture.srvIndex, format, 1);
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
    ResizeInternalTarget(m_gtaoRawTarget, width, height, format);
    ResizeInternalTarget(m_shadowBlurTarget, width, height, format);
    ResizeInternalTarget(m_shadowBlur2Target, width, height, format);
}

void ScreenSpaceEffects::ResizeHdrColorTarget(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResizeInternalTarget(m_hdrCompositeTarget, width, height, format);
    ResizeInternalTarget(m_radianceTarget, width, height, format);
    ResizeInternalTarget(m_radianceTraceInputTarget, width, height, format);
    ResizeInternalTarget(m_radianceSpatialBlurTarget, width, height, format);
    ResizeInternalTarget(m_radianceSpatialTarget, width, height, format);
    ResizeInternalTarget(m_radianceHistoryTarget, width, height, format);
    ResizeInternalTarget(m_radianceTemporalTarget, width, height, format);
    ResizeInternalTarget(m_radianceHistoryDepthTarget, width, height, format);
    ResizeInternalTarget(m_ssrIndirectTarget, width, height, format);
    ResizeInternalTarget(m_rtIndirectTarget, width, height, format);
    ResizeInternalTarget(m_rtGiInjectTarget, width, height, format);
    // RR1 material guides at render res. Albedos are [0,1] (RGBA8 ok); normal-roughness needs a
    // signed format for raw world normals, so fp16.
    const int albedoFormat = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    ResizeInternalTarget(m_rrDiffuseAlbedoTarget, width, height, albedoFormat);
    ResizeInternalTarget(m_rrSpecularAlbedoTarget, width, height, albedoFormat);
    ResizeInternalTarget(m_rrNormalRoughnessTarget, width, height, format);
    ResizeInternalTarget(m_rrOpticalTransmissionDiffuseAlbedoTarget, width, height, albedoFormat);
    ResizeInternalTarget(m_rrOpticalTransmissionSpecularAlbedoTarget, width, height, albedoFormat);
    ResizeInternalTarget(m_rrOpticalTransmissionNormalRoughnessTarget, width, height, format);
    const int scalarFormat = static_cast<int>(DXGI_FORMAT_R16_FLOAT);
    const int depthHistoryFormat = static_cast<int>(DXGI_FORMAT_R16_FLOAT);
    const int ownerFormat = static_cast<int>(DXGI_FORMAT_R32_UINT);
    ResizeInternalTarget(m_rrTemporalPrimaryMaskTarget, width, height, scalarFormat);
    ResizeInternalTarget(m_rrTemporalTransmissionMaskTarget, width, height, scalarFormat);
    ResizeInternalTarget(m_rrTemporalPrimaryValidityDiagnosticsTarget, width, height, format);
    ResizeInternalTarget(m_rrTemporalTransmissionValidityDiagnosticsTarget, width, height, format);
    ResizeInternalTarget(m_rrTemporalPrimaryPrevDepthTarget, width, height, depthHistoryFormat);
    ResizeInternalTarget(m_rrTemporalPrimaryPrevNormalTarget, width, height, format);
    ResizeInternalTarget(m_rrTemporalPrimaryPrevOwnerTarget, width, height, ownerFormat);
    ResizeInternalTarget(m_rrTemporalTransmissionPrevDepthTarget, width, height, depthHistoryFormat);
    ResizeInternalTarget(m_rrTemporalTransmissionPrevNormalTarget, width, height, format);
    ResizeInternalTarget(m_rrTemporalTransmissionPrevOwnerTarget, width, height, ownerFormat);
    m_rrTemporalPrimaryHistoryValid = false;
    m_rrTemporalTransmissionHistoryValid = false;
    ResizeInternalTarget(m_ptOpticalReflectionInputTarget, width, height, format);
    // RR4 spec hit-distance guide: single-channel raw ray length in world units (unambiguous channel).
    const int hitDistFormat = static_cast<int>(DXGI_FORMAT_R16_FLOAT);
    ResizeInternalTarget(m_rrSpecularHitDistanceTarget, width, height, hitDistFormat);
    // P4: render-res D24 depth target for the path tracer's DLSS depth input (resolved from the PT
    // R32 depth each frame). D24 (not the R32 UAV) is what Streamline expects, avoiding shimmer.
    CreateInternalDepthTarget(m_ptDlssDepthTarget, width, height);
    CreateInternalDepthTarget(m_ptOpticalTransmissionDlssDepthTarget, width, height);
    const int supportedMotionFormat = static_cast<int>(DXGI_FORMAT_R16G16_FLOAT);
    ResizeInternalTarget(m_rrTemporalPrimaryMotionTarget, width, height, supportedMotionFormat);
    ResizeInternalTarget(m_rrTemporalTransmissionMotionTarget, width, height, supportedMotionFormat);
    ResizeInternalTarget(m_ptDlssMotionTarget, width, height, supportedMotionFormat);
    ResizeInternalTarget(m_dlssDilatedMotionTarget, width, height, supportedMotionFormat);
    ResizeInternalTarget(m_dlssOpticalTransmissionMotionTarget, width, height, supportedMotionFormat);
}

void ScreenSpaceEffects::ResizeSsrTargets(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    const int traceWidth =
        std::max(1, static_cast<int>(std::lround(width * m_ssrTraceResolutionScale)));
    const int traceHeight =
        std::max(1, static_cast<int>(std::lround(height * m_ssrTraceResolutionScale)));
    ResizeInternalTarget(m_ssrSceneColorTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrTraceTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrSpatialBlurTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrSpatialTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrHistoryTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrTemporalTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrVarianceHistoryTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrVarianceTemporalTarget, traceWidth, traceHeight, format);
    ResizeInternalTarget(m_ssrHistoryDepthTarget, traceWidth, traceHeight, format);
    if (m_ssrTraceResolutionScale < 1.0f)
    {
        ResizeInternalTarget(m_ssrResolvedTarget, width, height, format);
    }
    else
    {
        DestroyInternalTarget(m_ssrResolvedTarget);
    }
}

void ScreenSpaceEffects::ResizeBloomTargets(const int width, const int height)
{
    const int bloomWidth = std::max(1, width / 2);
    const int bloomHeight = std::max(1, height / 2);
    const int format = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);

    ResizeInternalTarget(m_bloomExtractTarget, bloomWidth, bloomHeight, format);
    ResizeInternalTarget(m_bloomBlurTarget, bloomWidth, bloomHeight, format);
    ResizeInternalTarget(m_bloomBlur2Target, bloomWidth, bloomHeight, format);
    ResizeInternalTarget(m_bloomHistoryTarget, bloomWidth, bloomHeight, format);
    ResizeInternalTarget(m_bloomTemporalTarget, bloomWidth, bloomHeight, format);
    // Targets were recreated: last frame's bloom SRV handle is stale.
    m_prevFrameBloomSrv = 0;
}

void ScreenSpaceEffects::ResizeLdrTonemapTarget(const int width, const int height)
{
    const int format = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    ResizeInternalTarget(m_ldrTonemapTarget, width, height, format);
}

void ScreenSpaceEffects::ResizeAntiAliasingTargets(const int width, const int height)
{
    const int ldrFormat = static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM);
    const int hdrFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    ResizeInternalTarget(m_smaaEdgeTarget, width, height, ldrFormat);
    ResizeInternalTarget(m_smaaOutputTarget, width, height, ldrFormat);
    ResizeInternalTarget(m_taaHistoryTarget, width, height, hdrFormat);
    ResizeInternalTarget(m_taaResolveTarget, width, height, hdrFormat);
}

void ScreenSpaceEffects::ResizeDlssDisplayDepthTarget(const int viewportWidth, const int viewportHeight)
{
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        DestroyInternalDepthTarget(m_dlssDisplayDepthTarget);
        return;
    }

    if (m_dlssDisplayDepthTarget.resource != nullptr
        && m_dlssDisplayDepthTarget.width == viewportWidth
        && m_dlssDisplayDepthTarget.height == viewportHeight)
    {
        return;
    }

    CreateInternalDepthTarget(m_dlssDisplayDepthTarget, viewportWidth, viewportHeight);
}

void ScreenSpaceEffects::ResizeDlssDisplayTargets(const int viewportWidth, const int viewportHeight)
{
    const int hdrFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    CreateUavTarget(m_dlssOutputTarget, viewportWidth, viewportHeight, hdrFormat);
    CreateUavTarget(m_dlssOpticalTransmissionOutputTarget, viewportWidth, viewportHeight, hdrFormat);
    ResizeInternalTarget(m_dlssOpticalCompositeTarget, viewportWidth, viewportHeight, hdrFormat);
    ResizeDlssDisplayDepthTarget(viewportWidth, viewportHeight);

    const int bloomWidth = std::max(1, viewportWidth / 2);
    const int bloomHeight = std::max(1, viewportHeight / 2);
    ResizeInternalTarget(m_dlssBloomExtractTarget, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomBlurTarget, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomBlur2Target, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomHistoryTarget, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomTemporalTarget, bloomWidth, bloomHeight, hdrFormat);
}

bool ScreenSpaceEffects::HasSplitLighting() const
{
    return m_sceneFramebuffer != nullptr && m_sceneFramebuffer->HasSplitLighting();
}

void ScreenSpaceEffects::BeginScenePass(const EnvironmentMap& environmentMap) const
{
    m_sceneFramebuffer->BindDrawTarget(false);

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

    const float directClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const glm::vec3 solidBackground = SolidBackgroundLinear(environmentMap);
    const float solidClear[] = {solidBackground.x, solidBackground.y, solidBackground.z, 1.0f};
    const float blackClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float* indirectClear =
        environmentMap.UsesSolidColorBackground() ? solidClear : blackClear;

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
        if (m_sceneFramebuffer->HasVelocity())
        {
            const float velocityClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
            D3D12_CPU_DESCRIPTOR_HANDLE velocityRtv{m_sceneFramebuffer->GetColorRtvCpuHandle(4)};
            commandList->ClearRenderTargetView(velocityRtv, velocityClear, 0, nullptr);
        }
        if (m_sceneFramebuffer->HasMaterialGbuffer())
        {
            const float material0Clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
            const float material1Clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
            D3D12_CPU_DESCRIPTOR_HANDLE material0Rtv{m_sceneFramebuffer->GetColorRtvCpuHandle(5)};
            D3D12_CPU_DESCRIPTOR_HANDLE material1Rtv{m_sceneFramebuffer->GetColorRtvCpuHandle(6)};
            commandList->ClearRenderTargetView(material0Rtv, material0Clear, 0, nullptr);
            commandList->ClearRenderTargetView(material1Rtv, material1Clear, 0, nullptr);
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

int ScreenSpaceEffects::GetEffectiveGeometryMsaaSampleCount() const
{
    if (m_msaaSampleCount <= 1 || GfxContext::Get().GetActiveMsaaSampleCount() <= 1)
    {
        return 1;
    }

    return GfxContext::Get().GetActiveMsaaSampleCount();
}

void ScreenSpaceEffects::EnsureMsaaDepthResolveShader() const
{
    if (m_msaaDepthResolveShader != nullptr)
    {
        return;
    }

    SceneRenderTrace::Scope shaderScope("EnsureMsaaDepthResolveShader");
    const_cast<ScreenSpaceEffects*>(this)->m_msaaDepthResolveShader = std::make_unique<Shader>(
        EngineConstants::FullscreenVertexShader,
        EngineConstants::MsaaDepthResolveFragmentShader);
    shaderScope.Success();
}

void ScreenSpaceEffects::EnsureDepthBlitShader() const
{
    if (m_depthBlitShader != nullptr)
    {
        return;
    }

    const_cast<ScreenSpaceEffects*>(this)->m_depthBlitShader = std::make_unique<Shader>(
        EngineConstants::FullscreenVertexShader,
        EngineConstants::DepthBlitFragmentShader,
        ShaderSamplerOverrides{(1u << 0), true});
}

bool ScreenSpaceEffects::ResolvePathTracerDlssDepth() const
{
    EnsureDepthBlitShader();
    PathTracerDlssDepthResolveInputs inputs{};
    inputs.pathTracerDepthSrv = m_pathTracerDepthSrv;
    inputs.ptDlssDepthTarget = const_cast<InternalDepthTarget*>(&m_ptDlssDepthTarget);
    inputs.depthBlitShader = m_depthBlitShader.get();
    inputs.renderWidth = m_width;
    inputs.renderHeight = m_height;
    return PathTracerDisplayPass::ResolveDlssDepth(BuildPostProcessContext(), inputs);
}

void ScreenSpaceEffects::EnsurePtSkyMotionPatchShader() const
{
    if (m_ptSkyMotionPatchShader != nullptr)
    {
        return;
    }

    const_cast<ScreenSpaceEffects*>(this)->m_ptSkyMotionPatchShader = std::make_unique<Shader>(
        EngineConstants::FullscreenVertexShader,
        EngineConstants::PtSkyMotionPatchFragmentShader,
        ShaderSamplerOverrides{(1u << 0) | (1u << 1) | (1u << 2), true});
}

bool ScreenSpaceEffects::PatchPathTracerSkyMotion() const
{
    EnsurePtSkyMotionPatchShader();
    PathTracerSkyMotionPatchInputs inputs{};
    inputs.pathTracerMetadataSrv = m_dxrPathTracerMetadataSrv;
    inputs.pathTracerMotionSrv = m_pathTracerMotionSrv;
    inputs.sceneFramebuffer = m_sceneFramebuffer.get();
    inputs.ptDlssMotionTarget = const_cast<InternalTarget*>(&m_ptDlssMotionTarget);
    inputs.skyMotionPatchShader = m_ptSkyMotionPatchShader.get();
    return PathTracerDisplayPass::PatchSkyMotion(BuildPostProcessContext(), inputs);
}

std::uint32_t ScreenSpaceEffects::PreparePathTracerRrBundle() const
{
    // Return bits: 1 = primary PT material guides, 2 = primary PT D24 depth, 4 = complete
    // independent smooth-dielectric transmission bundle.
    constexpr std::uint32_t kGuidesReady = 1u;
    constexpr std::uint32_t kDepthReady = 2u;
    constexpr std::uint32_t kOpticalTransmissionReady = 4u;

    m_ptFullGuidesThisFrame = false;
    const int mode = m_ptRrBundleMode;
    if (mode == 1 || !m_pathTracerActive
        || m_pathTracerConvergenceMode != PtConvergenceMode::RealTime
        || m_width <= 0 || m_height <= 0)
    {
        return 0;
    }

    // This function owns only the PT inputs prepared for the following RR evaluation. Keep all
    // recorded depth and material-guide work in one independently attributable capture scope.
    auto* const commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    static constexpr wchar_t kPathTracerRrPreparationMarker[] = L"PT.RR.Preparation";
    BeginPathTracerGpuEvent(
        commandList,
        kPathTracerRrPreparationMarker,
        static_cast<UINT>(sizeof(kPathTracerRrPreparationMarker)));

    const bool fullPtBundle = mode == 0;
    const bool wantGuides = fullPtBundle || mode == 2;
    const bool wantDepth = fullPtBundle || mode == 3 || mode == 4;
    std::uint32_t ready = 0;

    if (wantDepth)
    {
        if (ResolvePathTracerDlssDepth())
        {
            ready |= kDepthReady;
        }
        else if (fullPtBundle)
        {
            EndPathTracerGpuEvent(commandList);
            return 0; // full bundle is all-or-nothing (never a partial swap)
        }
    }

    bool transmissionDepthReady = false;
    if (fullPtBundle && m_pathTracerOpticalTransmissionDepthSrv != 0)
    {
        EnsureDepthBlitShader();
        PathTracerDlssDepthResolveInputs txInputs{};
        txInputs.pathTracerDepthSrv = m_pathTracerOpticalTransmissionDepthSrv;
        txInputs.ptDlssDepthTarget = const_cast<InternalDepthTarget*>(
            &m_ptOpticalTransmissionDlssDepthTarget);
        txInputs.depthBlitShader = m_depthBlitShader.get();
        txInputs.renderWidth = m_width;
        txInputs.renderHeight = m_height;
        transmissionDepthReady = PathTracerDisplayPass::ResolveDlssDepth(
            BuildPostProcessContext(), txInputs);
        if (!transmissionDepthReady)
        {
            EndPathTracerGpuEvent(commandList);
            return 0;
        }
    }

    if (wantGuides)
    {
        const bool guidesAvailable = m_pathTracerDiffuseAlbedoSrv != 0
            && m_pathTracerSpecularAlbedoSrv != 0 && m_pathTracerNormalRoughnessSrv != 0
            && (!fullPtBundle || (m_pathTracerOpticalTransmissionDiffuseAlbedoSrv != 0
                && m_pathTracerOpticalTransmissionSpecularAlbedoSrv != 0
                && m_pathTracerOpticalTransmissionNormalRoughnessSrv != 0))
            && m_downsampleShader != nullptr
            && m_rrDiffuseAlbedoTarget.resource != nullptr
            && m_rrSpecularAlbedoTarget.resource != nullptr
            && m_rrNormalRoughnessTarget.resource != nullptr;
        if (!guidesAvailable)
        {
            EndPathTracerGpuEvent(commandList);
            return fullPtBundle ? 0u : ready; // full bundle: all-or-nothing
        }

        // Copy the PT bounce-0 guides into the RR internal targets (format-identical blits) so
        // the DLSS evaluate and the RR guide debug views keep reading the same targets as the
        // raster path. GenerateRrGuides skips its raster modes via m_ptFullGuidesThisFrame.
        SceneRenderTrace::Scope guideCopyScope("pt rr guides copy");
        const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const std::pair<std::uintptr_t, InternalTarget*> copies[] = {
            {m_pathTracerDiffuseAlbedoSrv, const_cast<InternalTarget*>(&m_rrDiffuseAlbedoTarget)},
            {m_pathTracerSpecularAlbedoSrv, const_cast<InternalTarget*>(&m_rrSpecularAlbedoTarget)},
            {m_pathTracerNormalRoughnessSrv, const_cast<InternalTarget*>(&m_rrNormalRoughnessTarget)},
            {m_pathTracerOpticalTransmissionDiffuseAlbedoSrv, const_cast<InternalTarget*>(&m_rrOpticalTransmissionDiffuseAlbedoTarget)},
            {m_pathTracerOpticalTransmissionSpecularAlbedoSrv, const_cast<InternalTarget*>(&m_rrOpticalTransmissionSpecularAlbedoTarget)},
            {m_pathTracerOpticalTransmissionNormalRoughnessSrv, const_cast<InternalTarget*>(&m_rrOpticalTransmissionNormalRoughnessTarget)},
        };
        for (const auto& copy : copies)
        {
            m_downsampleShader->Use(false, false);
            m_downsampleShader->BindTextureSlot(0, copy.first);
            DrawFullscreenToTarget(*m_downsampleShader, *copy.second, m_width, m_height, clear);
        }
        guideCopyScope.Success();

        m_ptFullGuidesThisFrame = true;
        ready |= kGuidesReady;
        if (transmissionDepthReady
            && m_pathTracerOpticalTransmissionOutputResource != nullptr
            && m_pathTracerOpticalTransmissionMotionResource != nullptr)
        {
            ready |= kOpticalTransmissionReady;
        }

    }

    EndPathTracerGpuEvent(commandList);
    return ready;
}

RrTemporalValidityResult ScreenSpaceEffects::PrepareRrTemporalValidity(
    const RrTemporalValidityInputs& inputs) const
{
    RrTemporalValidityResult result{};
    if (m_width <= 0 || m_height <= 0 || inputs.depthSrv == 0
        || inputs.normalRoughnessSrv == 0 || inputs.ownerSrv == 0
        || inputs.ownerResource == nullptr || inputs.motionSrv == 0
        || m_rrTemporalValidityShader == nullptr || m_rrHistoryCopyShader == nullptr
        || inputs.ownerResourceState == UINT32_MAX)
    {
        return result;
    }

    InternalTarget& mask = inputs.transmission
        ? const_cast<InternalTarget&>(m_rrTemporalTransmissionMaskTarget)
        : const_cast<InternalTarget&>(m_rrTemporalPrimaryMaskTarget);
    InternalTarget& previousDepth = inputs.transmission
        ? const_cast<InternalTarget&>(m_rrTemporalTransmissionPrevDepthTarget)
        : const_cast<InternalTarget&>(m_rrTemporalPrimaryPrevDepthTarget);
    InternalTarget& previousNormal = inputs.transmission
        ? const_cast<InternalTarget&>(m_rrTemporalTransmissionPrevNormalTarget)
        : const_cast<InternalTarget&>(m_rrTemporalPrimaryPrevNormalTarget);
    InternalTarget& previousOwner = inputs.transmission
        ? const_cast<InternalTarget&>(m_rrTemporalTransmissionPrevOwnerTarget)
        : const_cast<InternalTarget&>(m_rrTemporalPrimaryPrevOwnerTarget);
    bool& localHistoryValid = inputs.transmission
        ? m_rrTemporalTransmissionHistoryValid
        : m_rrTemporalPrimaryHistoryValid;
    if (mask.resource == nullptr || previousDepth.resource == nullptr
        || previousNormal.resource == nullptr || previousOwner.resource == nullptr)
    {
        localHistoryValid = false;
        return result;
    }

    const float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    const auto configureValidity = [&](const bool diagnostics)
    {
        m_rrTemporalValidityShader->Use(false, false);
        m_rrTemporalValidityShader->SetFloat(
            "uHistoryValid", inputs.historyValid && localHistoryValid ? 1.0f : 0.0f);
        m_rrTemporalValidityShader->SetFloat("uDepthRelativeThreshold", 0.02f);
        m_rrTemporalValidityShader->SetFloat("uNormalDotThreshold", 0.85f);
        m_rrTemporalValidityShader->SetFloat("uDiagnosticOutput", diagnostics ? 1.0f : 0.0f);
        m_rrTemporalValidityShader->SetFloat("uMvecScaleX", inputs.mvecScaleX);
        m_rrTemporalValidityShader->SetFloat("uMvecScaleY", inputs.mvecScaleY);
        m_rrTemporalValidityShader->SetVec2(
            "uCurrentJitterNdc",
            glm::vec2(inputs.currentJitterNdcX, inputs.currentJitterNdcY));
        m_rrTemporalValidityShader->SetVec2(
            "uPreviousJitterNdc",
            glm::vec2(inputs.previousJitterNdcX, inputs.previousJitterNdcY));
        m_rrTemporalValidityShader->SetMat4(
            "uClipToPrevClip", glm::make_mat4(inputs.clipToPrevClip));
        m_rrTemporalValidityShader->BindTextureSlot(0, inputs.depthSrv);
        m_rrTemporalValidityShader->BindTextureSlot(1, inputs.normalRoughnessSrv);
        m_rrTemporalValidityShader->BindTextureSlot(2, inputs.ownerSrv);
        m_rrTemporalValidityShader->BindTextureSlot(3, inputs.motionSrv);
        m_rrTemporalValidityShader->BindTextureSlot(4, previousDepth.srvCpuHandle);
        m_rrTemporalValidityShader->BindTextureSlot(5, previousNormal.srvCpuHandle);
        m_rrTemporalValidityShader->BindTextureSlot(6, previousOwner.srvCpuHandle);
    };

    configureValidity(false);
    DrawFullscreenToTarget(
        *m_rrTemporalValidityShader, mask, m_width, m_height, clear);

    // Diagnostics have the same decision shader but independent targets, matching the two RR
    // viewport histories. Neither evaluation may overwrite the other's evidence.
    configureValidity(true);
    InternalTarget& diagnostics = inputs.transmission
        ? const_cast<InternalTarget&>(m_rrTemporalTransmissionValidityDiagnosticsTarget)
        : const_cast<InternalTarget&>(m_rrTemporalPrimaryValidityDiagnosticsTarget);
    DrawFullscreenToTarget(
        *m_rrTemporalValidityShader,
        diagnostics,
        m_width,
        m_height,
        clear);

    // Commit only after the exact current bundle has produced its rejection decision.
    m_rrHistoryCopyShader->Use(false, false);
    m_rrHistoryCopyShader->SetFloat("uDepthOnly", 1.0f);
    m_rrHistoryCopyShader->BindTextureSlot(0, inputs.depthSrv);
    DrawFullscreenToTarget(
        *m_rrHistoryCopyShader, previousDepth, m_width, m_height, clear);

    m_rrHistoryCopyShader->Use(false, false);
    m_rrHistoryCopyShader->SetFloat("uDepthOnly", 0.0f);
    m_rrHistoryCopyShader->BindTextureSlot(0, inputs.normalRoughnessSrv);
    DrawFullscreenToTarget(
        *m_rrHistoryCopyShader, previousNormal, m_width, m_height, clear);

    // R32_UINT cannot use the shared fp16 fullscreen PSO. Copy the exact owner bits instead of
    // converting them through a floating render target (which would destroy hash identity).
    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* ownerResource = static_cast<ID3D12Resource*>(inputs.ownerResource);
    auto* previousOwnerResource = static_cast<ID3D12Resource*>(previousOwner.resource);
    const auto ownerState = static_cast<D3D12_RESOURCE_STATES>(inputs.ownerResourceState);
    const auto previousOwnerState =
        static_cast<D3D12_RESOURCE_STATES>(previousOwner.resourceState);
    TransitionResource(commandList, ownerResource, ownerState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(
        commandList, previousOwnerResource, previousOwnerState, D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->CopyResource(previousOwnerResource, ownerResource);
    TransitionResource(
        commandList, ownerResource, D3D12_RESOURCE_STATE_COPY_SOURCE, ownerState);
    TransitionResource(
        commandList,
        previousOwnerResource,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    previousOwner.resourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    localHistoryValid = true;
    result.maskSrv = mask.srvCpuHandle;
    result.ready = true;
    return result;
}

void ScreenSpaceEffects::SetPtRrBundleMode(const int mode)
{
    if (m_ptRrBundleMode == mode)
    {
        return;
    }
    m_ptRrBundleMode = mode;
    ResetPathTracerTemporalDiagnostics();
}

void ScreenSpaceEffects::EndScenePass() const
{
    if (m_sceneFramebuffer->UsesMsaa() && m_sceneFramebuffer->GetMsaaDepthSrvCpuHandle() != 0)
    {
        EnsureMsaaDepthResolveShader();
        MsaaDepthResolveInputs msaaInputs{};
        msaaInputs.sceneFramebuffer = m_sceneFramebuffer.get();
        msaaInputs.msaaDepthResolveShader = m_msaaDepthResolveShader.get();
        AntiAliasingPass::ExecuteMsaaDepthResolve(BuildPostProcessContext(), msaaInputs);
    }
    else if (m_sceneFramebuffer->UsesMsaa())
    {
        m_sceneFramebuffer->ResolveMsaa();
    }

    m_sceneFramebuffer->Unbind();
}

void ScreenSpaceEffects::BeginSceneGridPass() const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    m_sceneFramebuffer->BindSplitLightingOverlayDrawTarget();
}

void ScreenSpaceEffects::EndSceneGridPass() const
{
    if (m_sceneFramebuffer == nullptr)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    m_sceneFramebuffer->EnsureShaderResourceState();
}

void ScreenSpaceEffects::BlitRtDispatchSmokeDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    DxrDebugBlitInputs inputs{};
    inputs.debugMode = m_debugMode;
    inputs.debugChannelShader = m_debugChannelShader.get();
    inputs.dxrSmokeDebugSrv = m_dxrSmokeDebugSrv;
    DxrDebugBlitPass::BlitDispatchSmoke(
        BuildPostProcessContext(), inputs, outputTarget, viewportWidth, viewportHeight);
}

void ScreenSpaceEffects::BlitRtPrimaryDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight,
    const float maxTraceDistance) const
{
    DxrDebugBlitInputs inputs{};
    inputs.debugMode = m_debugMode;
    inputs.dxrPrimaryDebugShader = m_dxrPrimaryDebugShader.get();
    inputs.dxrPrimaryOutputSrv = m_dxrPrimaryOutputSrv;
    inputs.dxrPrimaryMetadataSrv = m_dxrPrimaryMetadataSrv;
    inputs.primaryDebugBlitReady = IsRtPrimaryDebugBlitReady();
    inputs.maxTraceDistance = maxTraceDistance;
    DxrDebugBlitPass::BlitPrimary(
        BuildPostProcessContext(), inputs, outputTarget, viewportWidth, viewportHeight);
}

void ScreenSpaceEffects::DrawFullscreenQuad() const
{
    m_draw.DrawFullscreenQuad();
}

void ScreenSpaceEffects::DrawFullscreenPass(Shader& shader, const bool viewportLdr) const
{
    m_draw.DrawFullscreenPass(shader, viewportLdr);
}

void ScreenSpaceEffects::DrawFullscreenToTarget(
    Shader& shader,
    InternalTarget& target,
    const int width,
    const int height,
    const float clearColor[4],
    const bool viewportLdr) const
{
    m_draw.DrawFullscreenToTarget(shader, target, width, height, clearColor, viewportLdr);
}

void ScreenSpaceEffects::BindOutputTarget(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    m_draw.BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
}

PostProcessContext ScreenSpaceEffects::BuildPostProcessContext() const
{
    return PostProcessContext{m_draw, m_width, m_height};
}

PathTracerHdrCopyInputs ScreenSpaceEffects::BuildPathTracerHdrCopyInputs() const
{
    PathTracerHdrCopyInputs inputs{};
    inputs.convergenceMode = m_pathTracerConvergenceMode;
    inputs.accumSampleCount = m_ptAccumSampleCount;
    inputs.accumSumDisplaySrv = m_ptAccumSumDisplaySrv;
    inputs.pathTracerOutputSrv = m_dxrPathTracerOutputSrv;
    inputs.meanShader = m_ptMeanShader.get();
    inputs.downsampleShader = m_downsampleShader.get();
    inputs.hdrCompositeTarget = const_cast<InternalTarget*>(&m_hdrCompositeTarget);
    return inputs;
}

void ScreenSpaceEffects::Apply(
    const Camera& camera,
    const int viewportWidth,
    const int viewportHeight,
    const DirectionalShadowSettings& shadowSettings,
    const EnvironmentMap& environmentMap) const
{
    FinalizePendingSsaoGpuReadback();
    FinalizePendingPtBoilMetricReadback();

    if (!m_enabled || !m_sceneFramebuffer->IsValid())
    {
        m_historyCompatibilityState.CancelPending();
        const_cast<ScreenSpaceEffects*>(this)->m_ssrSceneColorRanLastFrame = false;
        const_cast<ScreenSpaceEffects*>(this)->m_ssrTraceRanLastFrame = false;
        const_cast<ScreenSpaceEffects*>(this)->m_ssrDenoiseRanLastFrame = false;
        const_cast<ScreenSpaceEffects*>(this)->m_ssrTemporalRanLastFrame = false;
        return;
    }

    ApplyFrameState state;
    InitApplyFrame(state, camera, viewportWidth, viewportHeight, shadowSettings, environmentMap);
    {
        const GfxContext::GpuTimerScope gpuScopeLighting("Post-process/Lighting stage");
        RunApplyLightingStage(state);
    }
    bool debugEarlyOut = false;
    {
        const GfxContext::GpuTimerScope gpuScopeDebug("Post-process/Debug stage");
        debugEarlyOut = RunApplyDebugStage(state);
    }
    if (debugEarlyOut)
    {
        CommitRenderedHistoryCompatibility();
        return;
    }
    {
        const GfxContext::GpuTimerScope gpuScopePresentation("Post-process/Presentation stage");
        RunApplyPresentationStage(state);
    }
    // Optical-layer diagnostics consume scratch targets produced by the current frame's RR
    // evaluation, so they must run after presentation rather than in the earlier generic debug
    // stage. Previously these modes fell through and left the final composite on screen.
    if (IsPtOpticalLayerDebugMode(state.debugMode))
    {
        BlitPtOpticalLayerDebug(state.outputTarget, state.viewportWidth, state.viewportHeight);
        const_cast<ScreenSpaceEffects*>(this)->m_postProcessDebugRenderedThisFrame = true;
    }
    FinalizeApplyFrame(state);
    CommitRenderedHistoryCompatibility();
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
    return m_aoMode != AmbientOcclusionMode::Off;
}

void ScreenSpaceEffects::SetSsaoEnabled(bool enabled)
{
    if (IsSsaoEnabled() == enabled)
    {
        return;
    }

    m_ssaoEnabled = enabled;
    m_aoMode = enabled ? AmbientOcclusionMode::SSAO : AmbientOcclusionMode::Off;
    RenderPathDiagnostics::LogSsaoToggled(enabled);
    m_logSsaoApplySnapshot = true;
}

AmbientOcclusionMode ScreenSpaceEffects::GetAmbientOcclusionMode() const
{
    return m_aoMode;
}

void ScreenSpaceEffects::SetAmbientOcclusionMode(const AmbientOcclusionMode mode)
{
    if (m_aoMode == mode)
    {
        return;
    }

    m_aoMode = mode;
    m_ssaoEnabled = mode != AmbientOcclusionMode::Off;
    RenderPathDiagnostics::LogSsaoToggled(mode != AmbientOcclusionMode::Off);
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

    InternalTarget& rawReadbackTarget =
        m_aoMode == AmbientOcclusionMode::GTAO
            ? const_cast<InternalTarget&>(m_gtaoRawTarget)
            : const_cast<InternalTarget&>(m_ssaoTarget);
    InternalTarget& filteredReadbackTarget = const_cast<InternalTarget&>(m_ssaoTarget);

    if (ReadbackTextureCenterRgba16F(
            rawReadbackTarget.resource,
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
            filteredReadbackTarget.resource,
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
                m_sceneFramebuffer->GetGBufferColorResource(GBufferSlot::ShadingNormal),
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
    m_ssaoDiagnostics.enabled = m_aoMode != AmbientOcclusionMode::Off;
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
    m_ssaoDiagnostics.normalSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal);
    m_ssaoDiagnostics.noiseSrv = m_noiseTexture.srvCpuHandle;
    m_ssaoDiagnostics.ssaoRawSrv =
        m_aoMode == AmbientOcclusionMode::GTAO ? m_gtaoRawTarget.srvCpuHandle : m_ssaoTarget.srvCpuHandle;
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
    m_ssaoDiagnostics.radius = m_aoMode == AmbientOcclusionMode::GTAO ? m_gtaoRadius : m_ssaoRadius;
    m_ssaoDiagnostics.bias = m_aoMode == AmbientOcclusionMode::GTAO ? m_gtaoThickness : m_ssaoBias;
    m_ssaoDiagnostics.aoStrength = m_aoStrength;
    m_ssaoDiagnostics.ssaoPower = m_aoMode == AmbientOcclusionMode::GTAO ? m_gtaoPower : m_ssaoPower;
    m_ssaoDiagnostics.hdrColorSource = hdrColorSource != nullptr ? hdrColorSource : "null";
    m_ssaoDiagnostics.ssaoDebugViewSource =
        ssaoDebugViewSource != nullptr ? ssaoDebugViewSource : "null";
}

