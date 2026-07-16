#include "engine/rendering/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/FrameDiagnostics.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/RenderPathDiagnostics.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/Shader.h"
#include "engine/rendering/post/AmbientOcclusionPass.h"
#include "engine/rendering/post/BloomTonemapPass.h"
#include "engine/rendering/post/DxrDebugBlitPass.h"
#include "engine/rendering/post/ScreenSpaceReflectionPass.h"
#include "engine/rendering/post/ScreenSpaceGiPass.h"
#include "engine/rendering/post/AntiAliasingPass.h"
#include "engine/rendering/post/DlssResolvePass.h"
#include "engine/rendering/post/PathTracerDisplayPass.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rendering/ScreenSpaceEffectsApply.h"
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
      m_dlssZeroMotionShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::DlssZeroMotionFragmentShader)),
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
    static const std::array<HlslStageCompileRequest, 52> kStages = {
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
        HlslStageCompileRequest{EngineConstants::DlssZeroMotionFragmentShader, "main", "ps_6_0"},
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
    DestroyInternalDepthTarget(m_dlssDisplayDepthTarget);
    DestroyInternalDepthTarget(m_ptDlssDepthTarget);
    DestroyInternalTarget(m_ptDlssMotionTarget);
    DestroyInternalTarget(m_dlssDilatedMotionTarget);
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
    resourceDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
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
    device->CreateDepthStencilView(resource, nullptr, dsvHandle);
}

void ScreenSpaceEffects::DestroyInternalDepthTarget(InternalDepthTarget& target) const
{
    if (!GfxContext::Get().IsInitialized())
    {
        target.dsvIndex = UINT32_MAX;
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
    // RR4 spec hit-distance guide: single-channel raw ray length in world units (unambiguous channel).
    const int hitDistFormat = static_cast<int>(DXGI_FORMAT_R16_FLOAT);
    ResizeInternalTarget(m_rrSpecularHitDistanceTarget, width, height, hitDistFormat);
    // P4: render-res D24 depth target for the path tracer's DLSS depth input (resolved from the PT
    // R32 depth each frame). D24 (not the R32 UAV) is what Streamline expects, avoiding shimmer.
    CreateInternalDepthTarget(m_ptDlssDepthTarget, width, height);
    ResizeInternalTarget(m_ptDlssMotionTarget, width, height, format);
    ResizeInternalTarget(m_dlssDilatedMotionTarget, width, height, format);
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
    ResizeDlssDisplayDepthTarget(viewportWidth, viewportHeight);

    const int bloomWidth = std::max(1, viewportWidth / 2);
    const int bloomHeight = std::max(1, viewportHeight / 2);
    ResizeInternalTarget(m_dlssBloomExtractTarget, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomBlurTarget, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomBlur2Target, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomHistoryTarget, bloomWidth, bloomHeight, hdrFormat);
    ResizeInternalTarget(m_dlssBloomTemporalTarget, bloomWidth, bloomHeight, hdrFormat);
    m_dlssBloomHistoryValid = false;
    m_dlssBloomTemporalWarmupFrames = 0;
}

namespace
{
    DlssQuality ToDlssQuality(const DlssPreset preset)
    {
        switch (preset)
        {
        case DlssPreset::Quality: return DlssQuality::Quality;
        case DlssPreset::Balanced: return DlssQuality::Balanced;
        case DlssPreset::Performance: return DlssQuality::Performance;
        case DlssPreset::UltraPerformance: return DlssQuality::UltraPerformance;
        default: return DlssQuality::Quality;
        }
    }
}

float DlssPresetRenderScale(const DlssPreset preset)
{
    switch (preset)
    {
    case DlssPreset::Quality:
        return 0.667f;
    case DlssPreset::Balanced:
        return 0.58f;
    case DlssPreset::Performance:
        return 0.5f;
    case DlssPreset::UltraPerformance:
        return 0.333f;
    default:
        return 0.667f;
    }
}

float ScreenSpaceEffects::GetActiveRenderScale() const
{
    switch (m_antiAliasingMode)
    {
    case AntiAliasingMode::SSAA:
        return std::clamp(m_renderScale, 1.0f, 2.0f);
    case AntiAliasingMode::DLAA:
        // DLSS at native resolution: internal == display.
        return 1.0f;
    case AntiAliasingMode::DLSS:
        // Super resolution: render below display res; the composite/DLSS pass upscales to viewport.
        return DlssPresetRenderScale(m_dlssPreset);
    default:
        return 1.0f;
    }
}

int ScreenSpaceEffects::GetRenderWidth() const
{
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(m_viewportWidth) * GetActiveRenderScale())));
}

int ScreenSpaceEffects::GetRenderHeight() const
{
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(m_viewportHeight) * GetActiveRenderScale())));
}

float ScreenSpaceEffects::GetAutoMaterialMipBias() const
{
    if (m_antiAliasingMode != AntiAliasingMode::DLAA && m_antiAliasingMode != AntiAliasingMode::DLSS)
    {
        return 0.0f;
    }
    if (m_viewportWidth <= 0)
    {
        return 0.0f;
    }

    const float renderScale = static_cast<float>(GetRenderWidth())
        / static_cast<float>(m_viewportWidth);
    return std::log2(renderScale);
}

void ScreenSpaceEffects::InvalidateTemporalHistory() const
{
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "reconstruction", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x10u);
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "dlss-display-bloom", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x10u);
    m_motionVectorFrameState = {};
    m_radianceHistoryValid = false;
    m_giFrameIndex = 0;
    m_giPrevViewProjection = glm::mat4(1.0f);
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
    m_prevFrameBloomSrv = 0;
    m_dlssHistoryValid = false;
    m_dlssBloomHistoryValid = false;
    m_dlssBloomTemporalWarmupFrames = 0;
    const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerTemporalDiagnostics();
}

void ScreenSpaceEffects::ResetTaaHistory() const
{
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "render-bloom", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        "none", "existing-quality", m_width, m_height, m_viewportWidth, m_viewportHeight,
        false, false, 0x20u);
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "reconstruction", "request",
        m_pathTracerActive ? "path-tracer" : "raster", "existing-guides",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x20u);
    m_taaHistoryValid = false;
    m_taaFrameIndex = 0;
    m_bloomHistoryValid = false;
    m_bloomTemporalWarmupFrames = 0;
    m_dlssHistoryValid = false;
    m_dlssBloomHistoryValid = false;
    m_dlssBloomTemporalWarmupFrames = 0;
}

void ScreenSpaceEffects::Resize(const int viewportWidth, const int viewportHeight)
{
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        return;
    }

    const int prevViewportWidth = m_viewportWidth;
    const int prevViewportHeight = m_viewportHeight;
    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    const int renderWidth = GetRenderWidth();
    const int renderHeight = GetRenderHeight();

    if (m_width == renderWidth && m_height == renderHeight && m_sceneFramebuffer->IsValid()
        && m_sceneFramebuffer->GetSampleCount() == GetEffectiveGeometryMsaaSampleCount())
    {
        const bool wantsDlssDisplay = m_antiAliasingMode == AntiAliasingMode::DLAA
            || m_antiAliasingMode == AntiAliasingMode::DLSS;
        if (wantsDlssDisplay
            && (prevViewportWidth != viewportWidth || prevViewportHeight != viewportHeight))
        {
            ResizeDlssDisplayTargets(viewportWidth, viewportHeight);
            m_dlssHistoryValid = false;
            m_dlssBloomHistoryValid = false;
            m_dlssBloomTemporalWarmupFrames = 0;
        }
        return;
    }

    {
        SceneRenderTrace::Scope sceneFbScope("resize scene framebuffer");
        if (!m_sceneFramebuffer->Resize(
                renderWidth,
                renderHeight,
                FramebufferColorMode::SplitDirectIndirect,
                GetEffectiveGeometryMsaaSampleCount()))
        {
            throw std::runtime_error("Scene framebuffer size is invalid.");
        }

        sceneFbScope.Success();
    }
    {
        SceneRenderTrace::Scope singleChannelScope("resize single-channel targets");
        ResizeSingleChannelTargets(renderWidth, renderHeight);
        singleChannelScope.Success();
    }
    {
        SceneRenderTrace::Scope hdrScope("resize hdr targets");
        ResizeHdrColorTarget(renderWidth, renderHeight);
        hdrScope.Success();
    }
    {
        SceneRenderTrace::Scope ssrScope("resize ssr targets");
        ResizeSsrTargets(renderWidth, renderHeight);
        ssrScope.Success();
    }
    {
        SceneRenderTrace::Scope bloomScope("resize bloom targets");
        ResizeBloomTargets(renderWidth, renderHeight);
        bloomScope.Success();
    }
    {
        SceneRenderTrace::Scope ldrScope("resize ldr tonemap target");
        ResizeLdrTonemapTarget(renderWidth, renderHeight);
        ldrScope.Success();
    }
    {
        SceneRenderTrace::Scope aaTargetsScope("resize aa targets");
        ResizeAntiAliasingTargets(renderWidth, renderHeight);
        aaTargetsScope.Success();
    }
    {
        // DLSS display-res targets: HDR upscale output + post-DLSS bloom chain (S4).
        SceneRenderTrace::Scope dlssScope("resize dlss display targets");
        ResizeDlssDisplayTargets(viewportWidth, viewportHeight);
        dlssScope.Success();
    }
    m_width = renderWidth;
    m_height = renderHeight;
    ResetTaaHistory();
    InvalidateTemporalHistory();
}

void ScreenSpaceEffects::ReloadGeometryMsaaTargets(const int viewportWidth, const int viewportHeight)
{
    SceneRenderTrace::Scope reloadScope("ReloadGeometryMsaaTargets");
    if (viewportWidth <= 0 || viewportHeight <= 0)
    {
        throw std::runtime_error("Viewport size is invalid for geometry MSAA reload.");
    }

    m_viewportWidth = viewportWidth;
    m_viewportHeight = viewportHeight;
    m_width = 0;
    m_height = 0;
    Resize(viewportWidth, viewportHeight);

    if (!m_sceneFramebuffer->IsValid())
    {
        throw std::runtime_error("Scene framebuffer is invalid after geometry MSAA reload.");
    }

    if (m_sceneFramebuffer->GetSampleCount() != GetEffectiveGeometryMsaaSampleCount())
    {
        throw std::runtime_error("Scene framebuffer MSAA sample count does not match the active count.");
    }

    reloadScope.Success();
}

namespace
{
    float RadicalInverse(const std::uint32_t index, const std::uint32_t base)
    {
        float result = 0.0f;
        float fraction = 1.0f;
        std::uint32_t i = index;
        while (i > 0)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(i % base);
            i /= base;
        }
        return result;
    }

    // Halton(2,3) sub-pixel jitter for TAA/DLSS. DLSS-RR Integration Guide §3.6 recommends ≥32
    // distinct phases; 64-cycle procedural sequence (no table, no short repeat).
    glm::vec2 HaltonJitter(const int frameIndex, const int width, const int height)
    {
        static constexpr int kPhaseCount = 64;
        const std::uint32_t sampleIndex =
            static_cast<std::uint32_t>(frameIndex % kPhaseCount);
        const float haltonX = RadicalInverse(sampleIndex, 2);
        const float haltonY = RadicalInverse(sampleIndex, 3);
        const float jitterX = ((haltonX - 0.5f) * 2.0f) / static_cast<float>(width);
        const float jitterY = ((haltonY - 0.5f) * 2.0f) / static_cast<float>(height);
        return glm::vec2(jitterX, jitterY);
    }
}

namespace
{
    // TAA and both DLSS modes are temporal and require sub-pixel jitter on the projection.
    bool ModeUsesTemporalJitter(const AntiAliasingMode mode)
    {
        return mode == AntiAliasingMode::TAA || mode == AntiAliasingMode::DLAA
            || mode == AntiAliasingMode::DLSS;
    }
}

void ScreenSpaceEffects::PrepareAntiAliasingFrame(Camera& camera, const bool freezeJitter) const
{
    camera.ClearProjectionJitter();
    if (ModeUsesTemporalJitter(m_antiAliasingMode) && m_width > 0 && m_height > 0 && !freezeJitter)
    {
        camera.SetProjectionJitter(HaltonJitter(m_taaFrameIndex, m_width, m_height));
    }
}

void ScreenSpaceEffects::FinalizeAntiAliasingFrame(const Camera& /*camera*/, const bool freezeJitter) const
{
    if (!ModeUsesTemporalJitter(m_antiAliasingMode) || freezeJitter)
    {
        return;
    }

    ++m_taaFrameIndex;
}

const MotionVectorFrameState& ScreenSpaceEffects::GetMotionVectorFrameState() const
{
    return m_motionVectorFrameState;
}

void ScreenSpaceEffects::AdvanceTemporalFrame(const Camera& camera) const
{
    const glm::mat4 view = camera.GetViewMatrix();
    const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
    const glm::mat4 unjitteredViewProjection = unjitteredProjection * view;
    m_motionVectorFrameState.previousCamera = TemporalCamera::MakeState(
        view,
        unjitteredProjection,
        glm::inverse(unjitteredViewProjection),
        camera.GetPosition(),
        camera.GetProjectionJitter());
    m_motionVectorFrameState.prevView = view;
    m_motionVectorFrameState.prevProjection = TemporalCamera::ApplyJitter(
        unjitteredProjection,
        camera.GetProjectionJitter());
    m_motionVectorFrameState.prevUnjitteredProjection = unjitteredProjection;
    m_motionVectorFrameState.prevViewProjection =
        unjitteredViewProjection;
    m_giPrevViewProjection = m_motionVectorFrameState.prevViewProjection;
    m_motionVectorFrameState.historyValid =
        TemporalCamera::IsComplete(m_motionVectorFrameState.previousCamera);
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
    // Return bits: 1 = PT material guides copied into the rr* targets, 2 = PT D24 depth resolved.
    constexpr std::uint32_t kGuidesReady = 1u;
    constexpr std::uint32_t kDepthReady = 2u;

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

    if (wantGuides)
    {
        const bool guidesAvailable = m_pathTracerDiffuseAlbedoSrv != 0
            && m_pathTracerSpecularAlbedoSrv != 0 && m_pathTracerNormalRoughnessSrv != 0
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

    }

    EndPathTracerGpuEvent(commandList);
    return ready;
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

void ScreenSpaceEffects::SetDxrPathTracerDisplay(
    const bool active,
    const std::uintptr_t outputSrv,
    const std::uintptr_t metadataSrv,
    const PtConvergenceMode convergenceMode,
    void* const outputResource,
    const std::uint32_t outputResourceState,
    void* const depthResource,
    const std::uint32_t depthResourceState,
    void* const motionResource,
    const std::uint32_t motionResourceState,
    const std::uintptr_t depthSrv,
    const std::uintptr_t motionSrv,
    const std::uintptr_t diffuseAlbedoSrv,
    const std::uintptr_t specularAlbedoSrv,
    const std::uintptr_t normalRoughnessSrv)
{
    if (!active)
    {
        ResetPathTracerAccumulation();
        ResetPathTracerTemporalDiagnostics();
        m_ptGiStaticMetricValid = false;
        m_ptGiMotionMetricValid = false;
        m_ptGiStaticSampleCount = 0;
        m_ptGiMotionSampleCount = 0;
    }

    m_pathTracerActive = active;
    m_pathTracerConvergenceMode = convergenceMode;
    m_dxrPathTracerOutputSrv = outputSrv;
    m_dxrPathTracerMetadataSrv = metadataSrv;
    m_pathTracerOutputResource = outputResource;
    m_pathTracerOutputResourceState = outputResourceState;
    m_pathTracerDepthResource = depthResource;
    m_pathTracerDepthResourceState = depthResourceState;
    m_pathTracerDepthSrv = depthSrv;
    m_pathTracerMotionResource = motionResource;
    m_pathTracerMotionResourceState = motionResourceState;
    m_pathTracerMotionSrv = motionSrv;
    m_pathTracerDiffuseAlbedoSrv = diffuseAlbedoSrv;
    m_pathTracerSpecularAlbedoSrv = specularAlbedoSrv;
    m_pathTracerNormalRoughnessSrv = normalRoughnessSrv;
    m_ptFullGuidesThisFrame = false;
    m_pathTracerDlssResolvedThisFrame = false;
}

void ScreenSpaceEffects::SetPathTracerGridOverlayCallback(PathTracerGridOverlayFn fn)
{
    m_pathTracerGridOverlayDraw = std::move(fn);
}

void ScreenSpaceEffects::PreparePathTracerDlssHdrInput() const
{
    if (!m_pathTracerActive || m_dxrPathTracerOutputSrv == 0 || m_width <= 0 || m_height <= 0)
    {
        return;
    }

    const int hdrFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    const_cast<ScreenSpaceEffects*>(this)->ResizeInternalTarget(
        const_cast<ScreenSpaceEffects*>(this)->m_hdrCompositeTarget,
        m_width,
        m_height,
        hdrFormat);

    PathTracerDisplayPass::PrepareDlssHdrInput(
        BuildPostProcessContext(), BuildPathTracerHdrCopyInputs());
}

void ScreenSpaceEffects::CopyPathTracerHdrToCompositeTarget(const float clearColor[4]) const
{
    PathTracerDisplayPass::CopyHdrToCompositeTarget(
        BuildPostProcessContext(), BuildPathTracerHdrCopyInputs(), clearColor);
}

void ScreenSpaceEffects::DrawPathTracerGridOverlayOntoHdrTarget(
    const Camera& camera,
    InternalTarget& target,
    const int width,
    const int height) const
{
    PathTracerGridOverlayInputs inputs{};
    inputs.camera = &camera;
    inputs.target = &target;
    inputs.width = width;
    inputs.height = height;
    inputs.renderWidth = m_width;
    inputs.renderHeight = m_height;
    inputs.sceneFramebuffer = m_sceneFramebuffer.get();
    inputs.dlssDisplayDepthTarget = const_cast<InternalDepthTarget*>(&m_dlssDisplayDepthTarget);
    EnsureDepthBlitShader();
    inputs.depthBlitShader = m_depthBlitShader.get();
    inputs.gridOverlayDraw = m_pathTracerGridOverlayDraw;
    PathTracerDisplayPass::DrawGridOverlay(BuildPostProcessContext(), inputs);
}

void ScreenSpaceEffects::ResetPathTracerAccumulation()
{
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "pt-reference-accumulation", "request",
        "path-tracer", "reference-history-key-v1", "none", "reference",
        m_width, m_height, m_viewportWidth, m_viewportHeight, false, false, 0x40u);
    m_ptAccumSampleCount = 0;
    m_ptAccumHistoryKey = {};
    m_ptAccumPingPongReadFromScratch = false;
    m_ptAccumSumDisplaySrv = 0;
}

void ScreenSpaceEffects::ResetPathTracerTemporalDiagnostics()
{
    m_ptTemporalDiagnosticsPaused = false;
    m_ptTemporalStatsSampleCount = 0;
    m_ptTemporalPrevRadianceValid = false;
    m_pendingPtBoilMetricReadback = false;
    m_ptBoilMetricValid = false;
    m_ptBoilMetric = 0.0f;
    m_ptMeanLuminance = 0.0f;
    if (m_debugMode == RenderDebugMode::PtRestirGiSpatialStaticVariance)
    {
        m_ptGiStaticMetricValid = false;
        m_ptGiStaticSampleCount = 0;
        m_ptGiStaticReadbackCount = 0;
        m_ptGiStaticDeltaSum = 0.0;
        m_ptGiStaticRelativeDeltaSum = 0.0;
        m_ptGiStaticMeanLuminanceSum = 0.0;
        m_ptGiStaticQualityMetrics = {};
        m_ptGiStaticQualityMetricSums.fill(0.0);
    }
    else if (m_debugMode == RenderDebugMode::PtRestirGiSpatialMotionDelta)
    {
        m_ptGiMotionMetricValid = false;
        m_ptGiMotionSampleCount = 0;
        m_ptGiMotionReadbackCount = 0;
        m_ptGiMotionDeltaSum = 0.0;
        m_ptGiMotionRelativeDeltaSum = 0.0;
        m_ptGiMotionValidFractionSum = 0.0;
        m_ptGiMotionP95RelativeDelta = 0.0f;
        m_ptGiMotionP99RelativeDelta = 0.0f;
        m_ptGiMotionPeakRelativeDelta = 0.0f;
        m_ptGiMotionHotFraction = 0.0f;
        m_ptGiMotionNeighborCorrelation = 0.0f;
        m_ptGiMotionLowFrequencyRatio = 0.0f;
        m_ptGiMotionBlurredHotFraction = 0.0f;
        m_ptGiMotionUpperP99RelativeDelta = 0.0f;
        m_ptGiMotionLowerP99RelativeDelta = 0.0f;
        m_ptGiMotionUpperHotFraction = 0.0f;
        m_ptGiMotionLowerHotFraction = 0.0f;
        m_ptGiMotionP95RelativeDeltaSum = 0.0;
        m_ptGiMotionP99RelativeDeltaSum = 0.0;
        m_ptGiMotionHotFractionSum = 0.0;
        m_ptGiMotionNeighborCorrelationSum = 0.0;
        m_ptGiMotionLowFrequencyRatioSum = 0.0;
        m_ptGiMotionBlurredHotFractionSum = 0.0;
        m_ptGiMotionUpperP99RelativeDeltaSum = 0.0;
        m_ptGiMotionLowerP99RelativeDeltaSum = 0.0;
        m_ptGiMotionUpperHotFractionSum = 0.0;
        m_ptGiMotionLowerHotFractionSum = 0.0;
        m_ptGiMotionQualityMetrics = {};
        m_ptGiMotionQualityMetricSums.fill(0.0);
    }
    m_ptTemporalStatsPrevViewProjection = glm::mat4(1.0f);
    for (PtBoilMetricReadbackSlot& slot : m_ptBoilMetricReadbackSlots)
    {
        slot.pending = false;
        slot.fenceValue = 0;
    }
}

void ScreenSpaceEffects::SetPathTracerGiDiagnosticRoi(const glm::vec4& roi)
{
    glm::vec4 clamped = glm::clamp(roi, glm::vec4(0.0f), glm::vec4(1.0f));
    clamped.z = std::max(clamped.z, clamped.x + 0.01f);
    clamped.w = std::max(clamped.w, clamped.y + 0.01f);
    clamped.z = std::min(clamped.z, 1.0f);
    clamped.w = std::min(clamped.w, 1.0f);
    if (glm::all(glm::equal(clamped, m_ptGiDiagnosticRoi)))
    {
        return;
    }
    m_ptGiDiagnosticRoi = clamped;
    m_ptGiStaticMetricValid = false;
    m_ptGiMotionMetricValid = false;
    m_ptGiStaticSampleCount = 0;
    m_ptGiMotionSampleCount = 0;
    m_ptGiStaticReadbackCount = 0;
    m_ptGiMotionReadbackCount = 0;
    m_ptGiStaticDeltaSum = 0.0;
    m_ptGiStaticRelativeDeltaSum = 0.0;
    m_ptGiStaticMeanLuminanceSum = 0.0;
    m_ptGiMotionDeltaSum = 0.0;
    m_ptGiMotionRelativeDeltaSum = 0.0;
    m_ptGiMotionValidFractionSum = 0.0;
    m_ptGiMotionP95RelativeDelta = 0.0f;
    m_ptGiMotionP99RelativeDelta = 0.0f;
    m_ptGiMotionPeakRelativeDelta = 0.0f;
    m_ptGiMotionHotFraction = 0.0f;
    m_ptGiMotionNeighborCorrelation = 0.0f;
    m_ptGiMotionLowFrequencyRatio = 0.0f;
    m_ptGiMotionBlurredHotFraction = 0.0f;
    m_ptGiMotionUpperP99RelativeDelta = 0.0f;
    m_ptGiMotionLowerP99RelativeDelta = 0.0f;
    m_ptGiMotionUpperHotFraction = 0.0f;
    m_ptGiMotionLowerHotFraction = 0.0f;
    m_ptGiMotionP95RelativeDeltaSum = 0.0;
    m_ptGiMotionP99RelativeDeltaSum = 0.0;
    m_ptGiMotionHotFractionSum = 0.0;
    m_ptGiMotionNeighborCorrelationSum = 0.0;
    m_ptGiMotionLowFrequencyRatioSum = 0.0;
    m_ptGiMotionBlurredHotFractionSum = 0.0;
    m_ptGiMotionUpperP99RelativeDeltaSum = 0.0;
    m_ptGiMotionLowerP99RelativeDeltaSum = 0.0;
    m_ptGiMotionUpperHotFractionSum = 0.0;
    m_ptGiMotionLowerHotFractionSum = 0.0;
    m_ptGiStaticQualityMetrics = {};
    m_ptGiMotionQualityMetrics = {};
    m_ptGiStaticQualityMetricSums.fill(0.0);
    m_ptGiMotionQualityMetricSums.fill(0.0);
    ResetPathTracerTemporalDiagnostics();
}

void ScreenSpaceEffects::EnsurePtBoilMetricReadbackSlots() const
{
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        return;
    }

    constexpr UINT64 kReadbackPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    for (PtBoilMetricReadbackSlot& slot : m_ptBoilMetricReadbackSlots)
    {
        if (slot.resource != nullptr)
        {
            continue;
        }

        D3D12_RESOURCE_DESC readbackDesc{};
        readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readbackDesc.Width = kReadbackPitch;
        readbackDesc.Height = 1;
        readbackDesc.DepthOrArraySize = 1;
        readbackDesc.MipLevels = 1;
        readbackDesc.Format = DXGI_FORMAT_UNKNOWN;
        readbackDesc.SampleDesc.Count = 1;
        readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC allocationDesc{};
        allocationDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        if (FAILED(allocator->CreateResource(
                &allocationDesc,
                &readbackDesc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                &allocation,
                IID_PPV_ARGS(&resource))))
        {
            if (allocation != nullptr)
            {
                allocation->Release();
            }
            if (resource != nullptr)
            {
                resource->Release();
            }
            continue;
        }

        slot.resource = resource;
        slot.allocation = allocation;
    }
}

void ScreenSpaceEffects::FinalizePendingPtBoilMetricReadback() const
{
    const std::uint64_t completedFence = GfxContext::Get().GetCompletedFenceValue();
    bool anyPending = false;
    bool readAny = false;
    for (PtBoilMetricReadbackSlot& slot : m_ptBoilMetricReadbackSlots)
    {
        if (!slot.pending)
        {
            continue;
        }

        if (slot.fenceValue == 0)
        {
            slot.fenceValue = GfxContext::Get().GetLastSubmittedFenceValue();
            anyPending = true;
            continue;
        }

        if (slot.fenceValue > completedFence)
        {
            anyPending = true;
            continue;
        }

        auto* readbackResource = static_cast<ID3D12Resource*>(slot.resource);
        if (readbackResource == nullptr)
        {
            slot.pending = false;
            continue;
        }

        D3D12_RANGE readRange{0, sizeof(std::uint16_t) * 32};
        void* mapped = nullptr;
        if (SUCCEEDED(readbackResource->Map(0, &readRange, &mapped)) && mapped != nullptr)
        {
            const auto* halfChannels = static_cast<const std::uint16_t*>(mapped);
            const float delta = std::max(HalfToFloat(halfChannels[0]), 0.0f);
            const float meanLuminance = std::max(HalfToFloat(halfChannels[1]), 0.0f);
            const float relativeDelta = std::max(HalfToFloat(halfChannels[2]), 0.0f);
            const float auxiliaryMetric = std::max(HalfToFloat(halfChannels[3]), 0.0f);
            const float p95RelativeDelta = std::max(HalfToFloat(halfChannels[4]), 0.0f);
            const float p99RelativeDelta = std::max(HalfToFloat(halfChannels[5]), 0.0f);
            const float peakRelativeDelta = std::max(HalfToFloat(halfChannels[6]), 0.0f);
            const float hotFraction = std::clamp(HalfToFloat(halfChannels[7]), 0.0f, 1.0f);
            const float neighborCorrelation =
                std::clamp(HalfToFloat(halfChannels[8]), -1.0f, 1.0f);
            const float lowFrequencyRatio = std::max(HalfToFloat(halfChannels[9]), 0.0f);
            const float blurredHotFraction =
                std::clamp(HalfToFloat(halfChannels[10]), 0.0f, 1.0f);
            const float upperP99RelativeDelta = std::max(HalfToFloat(halfChannels[12]), 0.0f);
            const float lowerP99RelativeDelta = std::max(HalfToFloat(halfChannels[13]), 0.0f);
            const float upperHotFraction =
                std::clamp(HalfToFloat(halfChannels[14]), 0.0f, 1.0f);
            const float lowerHotFraction =
                std::clamp(HalfToFloat(halfChannels[15]), 0.0f, 1.0f);
            const std::array<float, 8> qualityValues = {
                std::max(HalfToFloat(halfChannels[16]), 0.0f),
                std::max(HalfToFloat(halfChannels[17]), 0.0f),
                std::clamp(HalfToFloat(halfChannels[18]), 0.0f, 1.0f),
                std::clamp(HalfToFloat(halfChannels[19]), 0.0f, 1.0f),
                std::max(HalfToFloat(halfChannels[20]), 0.0f),
                std::max(HalfToFloat(halfChannels[21]), 0.0f),
                std::max(HalfToFloat(halfChannels[22]), 0.0f),
                std::max(HalfToFloat(halfChannels[23]), 0.0f),
            };
            const auto updateQualityMetrics = [](PathTracerGiQualityMetrics& metrics,
                                                  std::array<double, 8>& sums,
                                                  const std::array<float, 8>& values,
                                                  const double divisor)
            {
                for (std::size_t index = 0; index < sums.size(); ++index)
                {
                    sums[index] += values[index];
                }
                metrics.meanChromaDelta = static_cast<float>(sums[0] / divisor);
                metrics.p95ChromaDelta = static_cast<float>(sums[1] / divisor);
                metrics.chromaHotFraction = static_cast<float>(sums[2] / divisor);
                metrics.temporalValidFraction = static_cast<float>(sums[3] / divisor);
                metrics.meanLocalLumaResidual = static_cast<float>(sums[4] / divisor);
                metrics.p95LocalLumaResidual = static_cast<float>(sums[5] / divisor);
                metrics.meanLocalChromaResidual = static_cast<float>(sums[6] / divisor);
                metrics.p95LocalChromaResidual = static_cast<float>(sums[7] / divisor);
            };
            if (slot.kind == PtTemporalMetricKind::GiStatic)
            {
                ++m_ptGiStaticReadbackCount;
                m_ptGiStaticDeltaSum += delta;
                m_ptGiStaticRelativeDeltaSum += relativeDelta;
                m_ptGiStaticMeanLuminanceSum += meanLuminance;
                const double sampleDivisor = static_cast<double>(m_ptGiStaticReadbackCount);
                m_ptGiStaticDelta = static_cast<float>(m_ptGiStaticDeltaSum / sampleDivisor);
                m_ptGiStaticRelativeDelta =
                    static_cast<float>(m_ptGiStaticRelativeDeltaSum / sampleDivisor);
                m_ptGiStaticRelativeSigma = auxiliaryMetric;
                m_ptGiStaticMeanLuminance =
                    static_cast<float>(m_ptGiStaticMeanLuminanceSum / sampleDivisor);
                updateQualityMetrics(
                    m_ptGiStaticQualityMetrics,
                    m_ptGiStaticQualityMetricSums,
                    qualityValues,
                    sampleDivisor);
                m_ptGiStaticSampleCount = slot.sampleCount;
                m_ptGiStaticMetricValid = true;
            }
            else if (slot.kind == PtTemporalMetricKind::GiMotion)
            {
                ++m_ptGiMotionReadbackCount;
                m_ptGiMotionDeltaSum += delta;
                m_ptGiMotionRelativeDeltaSum += relativeDelta;
                m_ptGiMotionValidFractionSum += std::min(auxiliaryMetric, 1.0f);
                m_ptGiMotionP95RelativeDeltaSum += p95RelativeDelta;
                m_ptGiMotionP99RelativeDeltaSum += p99RelativeDelta;
                m_ptGiMotionPeakRelativeDelta =
                    std::max(m_ptGiMotionPeakRelativeDelta, peakRelativeDelta);
                m_ptGiMotionHotFractionSum += hotFraction;
                m_ptGiMotionNeighborCorrelationSum += neighborCorrelation;
                m_ptGiMotionLowFrequencyRatioSum += lowFrequencyRatio;
                m_ptGiMotionBlurredHotFractionSum += blurredHotFraction;
                m_ptGiMotionUpperP99RelativeDeltaSum += upperP99RelativeDelta;
                m_ptGiMotionLowerP99RelativeDeltaSum += lowerP99RelativeDelta;
                m_ptGiMotionUpperHotFractionSum += upperHotFraction;
                m_ptGiMotionLowerHotFractionSum += lowerHotFraction;
                const double sampleDivisor = static_cast<double>(m_ptGiMotionReadbackCount);
                m_ptGiMotionDelta = static_cast<float>(m_ptGiMotionDeltaSum / sampleDivisor);
                m_ptGiMotionRelativeDelta =
                    static_cast<float>(m_ptGiMotionRelativeDeltaSum / sampleDivisor);
                m_ptGiMotionValidFraction =
                    static_cast<float>(m_ptGiMotionValidFractionSum / sampleDivisor);
                m_ptGiMotionP95RelativeDelta =
                    static_cast<float>(m_ptGiMotionP95RelativeDeltaSum / sampleDivisor);
                m_ptGiMotionP99RelativeDelta =
                    static_cast<float>(m_ptGiMotionP99RelativeDeltaSum / sampleDivisor);
                m_ptGiMotionHotFraction =
                    static_cast<float>(m_ptGiMotionHotFractionSum / sampleDivisor);
                m_ptGiMotionNeighborCorrelation =
                    static_cast<float>(m_ptGiMotionNeighborCorrelationSum / sampleDivisor);
                m_ptGiMotionLowFrequencyRatio =
                    static_cast<float>(m_ptGiMotionLowFrequencyRatioSum / sampleDivisor);
                m_ptGiMotionBlurredHotFraction =
                    static_cast<float>(m_ptGiMotionBlurredHotFractionSum / sampleDivisor);
                m_ptGiMotionUpperP99RelativeDelta =
                    static_cast<float>(m_ptGiMotionUpperP99RelativeDeltaSum / sampleDivisor);
                m_ptGiMotionLowerP99RelativeDelta =
                    static_cast<float>(m_ptGiMotionLowerP99RelativeDeltaSum / sampleDivisor);
                m_ptGiMotionUpperHotFraction =
                    static_cast<float>(m_ptGiMotionUpperHotFractionSum / sampleDivisor);
                m_ptGiMotionLowerHotFraction =
                    static_cast<float>(m_ptGiMotionLowerHotFractionSum / sampleDivisor);
                updateQualityMetrics(
                    m_ptGiMotionQualityMetrics,
                    m_ptGiMotionQualityMetricSums,
                    qualityValues,
                    sampleDivisor);
                m_ptGiMotionSampleCount = slot.sampleCount;
                m_ptGiMotionMetricValid = true;
            }
            else
            {
                m_ptBoilMetric = delta;
                m_ptMeanLuminance = meanLuminance;
                m_ptBoilMetricValid = true;
            }
            readAny = true;
            readbackResource->Unmap(0, nullptr);
        }
        slot.pending = false;
    }

    m_pendingPtBoilMetricReadback = anyPending;
    if (!readAny && !anyPending)
    {
        m_pendingPtBoilMetricReadback = false;
    }
}

void ScreenSpaceEffects::RecordPtBoilMetricReadback() const
{
    if (!GfxContext::Get().IsFrameRecording() || m_ptBoilMetricTarget.resource == nullptr)
    {
        return;
    }

    EnsurePtBoilMetricReadbackSlots();
    PtBoilMetricReadbackSlot& slot =
        m_ptBoilMetricReadbackSlots[m_ptBoilMetricReadbackWriteIndex % m_ptBoilMetricReadbackSlots.size()];
    if (slot.resource == nullptr || slot.pending)
    {
        return;
    }

    auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    auto* sourceResource = static_cast<ID3D12Resource*>(m_ptBoilMetricTarget.resource);
    auto* readbackResource = static_cast<ID3D12Resource*>(slot.resource);
    if (commandList == nullptr || sourceResource == nullptr || readbackResource == nullptr)
    {
        return;
    }

    constexpr UINT kReadbackPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const D3D12_RESOURCE_STATES beforeState =
        static_cast<D3D12_RESOURCE_STATES>(m_ptBoilMetricTarget.resourceState);
    TransitionResource(
        commandList,
        sourceResource,
        beforeState,
        D3D12_RESOURCE_STATE_COPY_SOURCE);
    m_ptBoilMetricTarget.resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = sourceResource;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readbackResource;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint.Offset = 0;
    destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    destination.PlacedFootprint.Footprint.Width = 8;
    destination.PlacedFootprint.Footprint.Height = 1;
    destination.PlacedFootprint.Footprint.Depth = 1;
    destination.PlacedFootprint.Footprint.RowPitch = kReadbackPitch;

    commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

    TransitionResource(
        commandList,
        sourceResource,
        D3D12_RESOURCE_STATE_COPY_SOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_ptBoilMetricTarget.resourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    slot.fenceValue = 0;
    slot.pending = true;
    slot.kind = m_debugMode == RenderDebugMode::PtRestirGiSpatialStaticVariance
        ? PtTemporalMetricKind::GiStatic
        : (m_debugMode == RenderDebugMode::PtRestirGiSpatialMotionDelta
            ? PtTemporalMetricKind::GiMotion
            : PtTemporalMetricKind::FullPathTracer);
    slot.sampleCount = m_ptTemporalStatsSampleCount;
    m_pendingPtBoilMetricReadback = true;
    m_ptBoilMetricReadbackWriteIndex =
        (m_ptBoilMetricReadbackWriteIndex + 1u)
        % static_cast<std::uint32_t>(m_ptBoilMetricReadbackSlots.size());
}

void ScreenSpaceEffects::UpdatePathTracerTemporalDiagnostics(const Camera& camera) const
{
    if (!m_pathTracerActive || m_dxrPathTracerOutputSrv == 0 || m_width <= 0 || m_height <= 0)
    {
        const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerTemporalDiagnostics();
        return;
    }

    if (m_ptTemporalDiagnosticsPaused)
    {
        return;
    }

    constexpr int statsFormat = static_cast<int>(DXGI_FORMAT_R32G32B32A32_FLOAT);
    constexpr int metricFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    auto* mutableThis = const_cast<ScreenSpaceEffects*>(this);
    mutableThis->ResizeInternalTarget(mutableThis->m_ptTemporalStatsTarget, m_width, m_height, statsFormat);
    mutableThis->ResizeInternalTarget(mutableThis->m_ptTemporalStatsScratchTarget, m_width, m_height, statsFormat);
    mutableThis->ResizeInternalTarget(mutableThis->m_ptTemporalPrevRadianceTarget, m_width, m_height, statsFormat);
    mutableThis->ResizeInternalTarget(mutableThis->m_ptTemporalQualityTarget, m_width, m_height, statsFormat);
    mutableThis->ResizeInternalTarget(mutableThis->m_ptBoilMetricTarget, 8, 1, metricFormat);

    const bool giSignal = IsPtRestirGiSpatialStatsDebugMode(m_debugMode);
    const bool motionReproject = m_debugMode == RenderDebugMode::PtRestirGiSpatialMotionDelta;
    if (motionReproject && m_pathTracerMotionSrv == 0)
    {
        const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerTemporalDiagnostics();
        return;
    }
    if (giSignal && (m_dxrPathTracerMetadataSrv == 0 || m_pathTracerNormalRoughnessSrv == 0))
    {
        const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerTemporalDiagnostics();
        return;
    }

    const std::uint32_t selectedInstanceIdPlusOne =
        giSignal && m_ptGiDiagnosticSelectedInstanceId != UINT32_MAX
        ? m_ptGiDiagnosticSelectedInstanceId + 1u
        : 0u;

    const glm::mat4 viewProjection = camera.GetUnjitteredProjectionMatrix() * camera.GetViewMatrix();
    const bool cameraChanged =
        m_ptTemporalStatsSampleCount > 0
        && !MatricesNearEqual(viewProjection, m_ptTemporalStatsPrevViewProjection);
    if (!motionReproject && cameraChanged)
    {
        // A static-camera measurement has a precise contract: any camera movement starts a new
        // sample session. Clear both GPU history and CPU readback averages so navigation cannot
        // silently contaminate the number shown after the camera settles.
        mutableThis->ResetPathTracerTemporalDiagnostics();
    }
    const bool resetStats = m_ptTemporalStatsSampleCount == 0 || (!motionReproject && cameraChanged);

    const float clearStats[] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_ptTemporalStatsShader->Use(false, false);
    m_ptTemporalStatsShader->SetInt("uCurrentRadiance", 0);
    m_ptTemporalStatsShader->SetInt("uPrevRadiance", 1);
    m_ptTemporalStatsShader->SetInt("uPrevStats", 2);
    m_ptTemporalStatsShader->SetInt("uMotion", 3);
    m_ptTemporalStatsShader->SetInt("uResetStats", resetStats ? 1 : 0);
    m_ptTemporalStatsShader->SetInt("uPrevFrameValid", m_ptTemporalPrevRadianceValid ? 1 : 0);
    m_ptTemporalStatsShader->SetInt("uMotionReproject", motionReproject ? 1 : 0);
    m_ptTemporalStatsShader->SetInt("uGiSignal", giSignal ? 1 : 0);
    m_ptTemporalStatsShader->SetInt(
        "uSelectedInstanceIdPlusOne",
        static_cast<int>(selectedInstanceIdPlusOne));
    m_ptTemporalStatsShader->SetVec2("uMotionScale", glm::vec2(-0.5f, 0.5f));
    m_ptTemporalStatsShader->BindTextureSlot(0, m_dxrPathTracerOutputSrv);
    m_ptTemporalStatsShader->BindTextureSlot(
        1,
        m_ptTemporalPrevRadianceValid
            ? m_ptTemporalPrevRadianceTarget.srvCpuHandle
            : m_dxrPathTracerOutputSrv);
    m_ptTemporalStatsShader->BindTextureSlot(2, m_ptTemporalStatsTarget.srvCpuHandle);
    m_ptTemporalStatsShader->BindTextureSlot(
        3,
        m_pathTracerMotionSrv != 0 ? m_pathTracerMotionSrv : m_dxrPathTracerOutputSrv);
    m_ptTemporalStatsShader->BindTextureSlot(
        4,
        m_dxrPathTracerMetadataSrv != 0 ? m_dxrPathTracerMetadataSrv : m_dxrPathTracerOutputSrv);
    DrawFullscreenToTarget(
        *m_ptTemporalStatsShader,
        const_cast<InternalTarget&>(m_ptTemporalStatsScratchTarget),
        m_width,
        m_height,
        clearStats);

    std::swap(mutableThis->m_ptTemporalStatsTarget, mutableThis->m_ptTemporalStatsScratchTarget);

    const float clearQuality[] = {-1.0f, -1.0f, -1.0f, 0.0f};
    m_ptTemporalQualityShader->Use(false, false);
    m_ptTemporalQualityShader->SetInt("uCurrentRadiance", 0);
    m_ptTemporalQualityShader->SetInt("uPrevRadiance", 1);
    m_ptTemporalQualityShader->SetInt("uMotion", 2);
    m_ptTemporalQualityShader->SetInt("uMetadata", 3);
    m_ptTemporalQualityShader->SetInt("uNormalRoughness", 4);
    m_ptTemporalQualityShader->SetInt("uPrevFrameValid", m_ptTemporalPrevRadianceValid ? 1 : 0);
    m_ptTemporalQualityShader->SetInt("uMotionReproject", motionReproject ? 1 : 0);
    m_ptTemporalQualityShader->SetInt(
        "uSelectedInstanceIdPlusOne",
        static_cast<int>(selectedInstanceIdPlusOne));
    m_ptTemporalQualityShader->SetVec2("uMotionScale", glm::vec2(-0.5f, 0.5f));
    m_ptTemporalQualityShader->BindTextureSlot(0, m_dxrPathTracerOutputSrv);
    m_ptTemporalQualityShader->BindTextureSlot(
        1,
        m_ptTemporalPrevRadianceValid
            ? m_ptTemporalPrevRadianceTarget.srvCpuHandle
            : m_dxrPathTracerOutputSrv);
    m_ptTemporalQualityShader->BindTextureSlot(
        2,
        m_pathTracerMotionSrv != 0 ? m_pathTracerMotionSrv : m_dxrPathTracerOutputSrv);
    m_ptTemporalQualityShader->BindTextureSlot(
        3,
        m_dxrPathTracerMetadataSrv != 0 ? m_dxrPathTracerMetadataSrv : m_dxrPathTracerOutputSrv);
    m_ptTemporalQualityShader->BindTextureSlot(
        4,
        m_pathTracerNormalRoughnessSrv != 0
            ? m_pathTracerNormalRoughnessSrv
            : m_dxrPathTracerOutputSrv);
    DrawFullscreenToTarget(
        *m_ptTemporalQualityShader,
        const_cast<InternalTarget&>(m_ptTemporalQualityTarget),
        m_width,
        m_height,
        clearQuality);

    const float clearRadiance[] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_downsampleShader->Use(false, false);
    m_downsampleShader->BindTextureSlot(0, m_dxrPathTracerOutputSrv);
    DrawFullscreenToTarget(
        *m_downsampleShader,
        const_cast<InternalTarget&>(m_ptTemporalPrevRadianceTarget),
        m_width,
        m_height,
        clearRadiance);

    const float clearMetric[] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_ptBoilMetricShader->Use(false, false);
    m_ptBoilMetricShader->SetInt("uStatsMap", 0);
    m_ptBoilMetricShader->SetInt("uQualityMap", 1);
    m_ptBoilMetricShader->SetInt(
        "uStaticVarianceMetric",
        m_debugMode == RenderDebugMode::PtRestirGiSpatialStaticVariance ? 1 : 0);
    const glm::vec4 metricRoi = giSignal
        ? m_ptGiDiagnosticRoi
        : glm::vec4(0.0f, 0.0f, 1.0f, 1.0f);
    m_ptBoilMetricShader->SetVec2("uRoiMin", glm::vec2(metricRoi));
    m_ptBoilMetricShader->SetVec2("uRoiMax", glm::vec2(metricRoi.z, metricRoi.w));
    m_ptBoilMetricShader->BindTextureSlot(0, m_ptTemporalStatsTarget.srvCpuHandle);
    m_ptBoilMetricShader->BindTextureSlot(1, m_ptTemporalQualityTarget.srvCpuHandle);
    DrawFullscreenToTarget(
        *m_ptBoilMetricShader,
        const_cast<InternalTarget&>(m_ptBoilMetricTarget),
        8,
        1,
        clearMetric);

    m_ptTemporalStatsSampleCount = resetStats ? 1u : m_ptTemporalStatsSampleCount + 1u;
    m_ptTemporalPrevRadianceValid = true;
    m_ptTemporalStatsPrevViewProjection = viewProjection;
    RecordPtBoilMetricReadback();
}

void ScreenSpaceEffects::InvalidateMotionHistory() const
{
    m_motionVectorFrameState = {};
    m_radianceHistoryValid = false;
    m_giFrameIndex = 0;
    m_giPrevViewProjection = glm::mat4(1.0f);
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
}

void ScreenSpaceEffects::InvalidateAllTemporalState() const
{
    ResetTaaHistory();
    InvalidateTemporalHistory();
    const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerAccumulation();
}

void ScreenSpaceEffects::AccumulatePathTracerReference(
    const PathTracerHistoryKey& historyKey,
    const std::uintptr_t currentFrameSrv,
    const int width,
    const int height)
{
    // fp32 sum: reference accumulation runs to thousands of spp. fp16 overflowed bright HDR pixels
    // past 65504 (stuck-white firefly speckles that never averaged out) and quantized the growing
    // sum into visible gradient banding (worst on the sky). The pt_accumulate/pt_mean shaders always
    // documented RGBA32F intent. The Shader class builds an RGBA32F fullscreen PSO variant selected
    // by this target format (PostProcessDraw::ResolveFullscreenPipelineFlags).
    const int accumFormat = static_cast<int>(DXGI_FORMAT_R32G32B32A32_FLOAT);
    ResizeInternalTarget(m_ptAccumSumTarget, width, height, accumFormat);
    ResizeInternalTarget(m_ptAccumScratchTarget, width, height, accumFormat);

    PathTracerAccumulateInputs inputs{};
    inputs.historyKey = historyKey;
    inputs.currentHistoryKey = m_ptAccumHistoryKey;
    inputs.sampleCount = m_ptAccumSampleCount;
    inputs.pingPongReadFromScratch = m_ptAccumPingPongReadFromScratch;
    inputs.currentFrameSrv = currentFrameSrv;
    inputs.width = width;
    inputs.height = height;
    inputs.accumulateShader = m_ptAccumulateShader.get();
    inputs.sumTarget = &m_ptAccumSumTarget;
    inputs.scratchTarget = &m_ptAccumScratchTarget;

    PathTracerAccumulateOutputs outputs{};
    PathTracerDisplayPass::AccumulateReference(
        BuildPostProcessContext(), inputs, outputs);
    m_ptAccumHistoryKey = outputs.historyKey;
    m_ptAccumSampleCount = outputs.sampleCount;
    m_ptAccumPingPongReadFromScratch = outputs.pingPongReadFromScratch;
    m_ptAccumSumDisplaySrv = outputs.sumDisplaySrv;
}

void ScreenSpaceEffects::BlitPathTracer(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight,
    const float maxTraceDistance) const
{
    PathTracerBlitInputs inputs{};
    inputs.pathTracerActive = m_pathTracerActive;
    inputs.pathTracerBlitReady = IsPathTracerBlitReady();
    inputs.pathTracerPostIntegrated = m_pathTracerPostIntegrated;
    inputs.pathTracerDlssResolvedThisFrame = m_pathTracerDlssResolvedThisFrame;
    inputs.convergenceMode = m_pathTracerConvergenceMode;
    inputs.accumSampleCount = m_ptAccumSampleCount;
    inputs.accumSumDisplaySrv = m_ptAccumSumDisplaySrv;
    inputs.pathTracerOutputSrv = m_dxrPathTracerOutputSrv;
    inputs.pathTracerMetadataSrv = m_dxrPathTracerMetadataSrv;
    inputs.maxTraceDistance = maxTraceDistance;
    inputs.outputTarget = outputTarget;
    inputs.viewportWidth = viewportWidth;
    inputs.viewportHeight = viewportHeight;
    inputs.primaryDebugShader = m_dxrPrimaryDebugShader.get();
    PathTracerDisplayPass::Blit(BuildPostProcessContext(), inputs);
}

void ScreenSpaceEffects::BlitRtReflectionDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    DxrDebugBlitInputs inputs{};
    inputs.debugMode = m_debugMode;
    inputs.debugChannelShader = m_debugChannelShader.get();
    inputs.rtReflectionResolveShader = m_rtReflectionResolveShader.get();
    inputs.dxrReflectionSrv = m_dxrReflectionSrv;
    inputs.dxrReflectionDenoisedSrv = m_dxrReflectionDenoisedSrv;
    inputs.dxrReflectionUvScaleX = m_dxrReflectionUvScaleX;
    inputs.dxrReflectionUvScaleY = m_dxrReflectionUvScaleY;
    inputs.dxrReflectionMaxTraceDistance = m_dxrReflectionMaxTraceDistance;
    DxrDebugBlitPass::BlitReflection(
        BuildPostProcessContext(), inputs, outputTarget, viewportWidth, viewportHeight);
}

void ScreenSpaceEffects::BlitRtShadowDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    DxrDebugBlitInputs inputs{};
    inputs.debugMode = m_debugMode;
    inputs.dxrShadowDebugShader = m_dxrShadowDebugShader.get();
    inputs.dxrShadowPenumbraSrv = m_dxrShadowPenumbraSrv;
    inputs.dxrShadowDenoisedSrv = m_dxrShadowDenoisedSrv;
    inputs.dxrShadowUvScaleX = m_dxrShadowUvScaleX;
    inputs.dxrShadowUvScaleY = m_dxrShadowUvScaleY;
    DxrDebugBlitPass::BlitShadow(
        BuildPostProcessContext(), inputs, outputTarget, viewportWidth, viewportHeight);
}

void ScreenSpaceEffects::BlitRtGiDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    DxrDebugBlitInputs inputs{};
    inputs.debugMode = m_debugMode;
    inputs.debugChannelShader = m_debugChannelShader.get();
    inputs.dxrGiInjectShader = m_dxrGiInjectShader.get();
    inputs.dxrGiRawSrv = m_dxrGiRawSrv;
    inputs.dxrGiDenoisedSrv = m_dxrGiDenoisedSrv;
    inputs.dxrGiUvScaleX = m_dxrGiUvScaleX;
    inputs.dxrGiUvScaleY = m_dxrGiUvScaleY;
    inputs.dxrGiStrength = m_dxrGiStrength;
    if (m_sceneFramebuffer != nullptr)
    {
        inputs.sceneHasMaterialGbuffer = m_sceneFramebuffer->HasMaterialGbuffer();
        inputs.sceneIndirectSrv =
            m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::IndirectLighting);
        inputs.sceneDepthSrv = m_sceneFramebuffer->GetDepthSrvCpuHandle();
        inputs.sceneMaterial0Srv =
            m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough);
        inputs.sceneMaterial1Srv =
            m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic);
    }
    DxrDebugBlitPass::BlitGi(
        BuildPostProcessContext(), inputs, outputTarget, viewportWidth, viewportHeight);
}

bool ScreenSpaceEffects::PreparePathTracerMotionReprojectionAudit() const
{
    if (!m_pathTracerActive || m_dxrPathTracerOutputSrv == 0 || m_width <= 0 || m_height <= 0)
    {
        const_cast<ScreenSpaceEffects*>(this)->ResetPathTracerTemporalDiagnostics();
        return false;
    }

    auto* mutableThis = const_cast<ScreenSpaceEffects*>(this);
    const bool resized = m_ptTemporalPrevRadianceTarget.width != m_width
        || m_ptTemporalPrevRadianceTarget.height != m_height
        || m_ptTemporalPrevRadianceTarget.resource == nullptr
        || m_ptTemporalPrevDepthTarget.width != m_width
        || m_ptTemporalPrevDepthTarget.height != m_height
        || m_ptTemporalPrevDepthTarget.resource == nullptr;
    constexpr int radianceFormat = static_cast<int>(DXGI_FORMAT_R32G32B32A32_FLOAT);
    mutableThis->ResizeInternalTarget(
        mutableThis->m_ptTemporalPrevRadianceTarget, m_width, m_height, radianceFormat);
    mutableThis->ResizeInternalTarget(
        mutableThis->m_ptTemporalPrevDepthTarget, m_width, m_height, radianceFormat);
    if (resized)
    {
        mutableThis->m_ptTemporalPrevRadianceValid = false;
    }
    return m_ptTemporalPrevRadianceTarget.srvCpuHandle != 0;
}

void ScreenSpaceEffects::CommitPathTracerMotionReprojectionAudit(const std::uintptr_t depthSrv) const
{
    if (!PreparePathTracerMotionReprojectionAudit() || m_downsampleShader == nullptr
        || m_ptMotionDepthCopyShader == nullptr || depthSrv == 0)
    {
        return;
    }

    const float clearRadiance[] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_downsampleShader->Use(false, false);
    m_downsampleShader->BindTextureSlot(0, m_dxrPathTracerOutputSrv);
    DrawFullscreenToTarget(
        *m_downsampleShader,
        const_cast<InternalTarget&>(m_ptTemporalPrevRadianceTarget),
        m_width,
        m_height,
        clearRadiance);

    m_ptMotionDepthCopyShader->Use(false, false);
    m_ptMotionDepthCopyShader->SetInt("uDepth", 0);
    m_ptMotionDepthCopyShader->BindTextureSlot(0, depthSrv);
    DrawFullscreenToTarget(
        *m_ptMotionDepthCopyShader,
        const_cast<InternalTarget&>(m_ptTemporalPrevDepthTarget),
        m_width,
        m_height,
        clearRadiance);
    const_cast<ScreenSpaceEffects*>(this)->m_ptTemporalPrevRadianceValid = true;
}

bool ScreenSpaceEffects::GenerateDilatedDlssMotion(
    const std::uintptr_t depthSrv,
    const std::uintptr_t motionSrv) const
{
    if (depthSrv == 0 || motionSrv == 0 || m_width <= 0 || m_height <= 0
        || m_dlssDilatedMotionTarget.resource == nullptr || m_dlssMotionDilateShader == nullptr)
    {
        return false;
    }

    SceneRenderTrace::Scope dilateScope("dlss motion dilation");
    const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_dlssMotionDilateShader->Use(false);
    m_dlssMotionDilateShader->SetInt("uDepth", 0);
    m_dlssMotionDilateShader->SetInt("uMotion", 1);
    m_dlssMotionDilateShader->SetFloat("uTexelSizeX", 1.0f / static_cast<float>(m_width));
    m_dlssMotionDilateShader->SetFloat("uTexelSizeY", 1.0f / static_cast<float>(m_height));
    m_dlssMotionDilateShader->BindTextureSlot(0, depthSrv);
    m_dlssMotionDilateShader->BindTextureSlot(1, motionSrv);
    DrawFullscreenToTarget(
        *m_dlssMotionDilateShader,
        const_cast<InternalTarget&>(m_dlssDilatedMotionTarget),
        m_width,
        m_height,
        clear);
    dilateScope.Success();
    return true;
}

bool ScreenSpaceEffects::GenerateZeroDlssMotion() const
{
    if (m_width <= 0 || m_height <= 0 || m_dlssDilatedMotionTarget.resource == nullptr
        || m_dlssZeroMotionShader == nullptr)
    {
        return false;
    }

    SceneRenderTrace::Scope reconstructionScope("dlss camera motion reconstruction input");
    const float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_dlssZeroMotionShader->Use(false);
    DrawFullscreenToTarget(
        *m_dlssZeroMotionShader,
        const_cast<InternalTarget&>(m_dlssDilatedMotionTarget),
        m_width,
        m_height,
        clear);
    reconstructionScope.Success();
    return true;
}

void ScreenSpaceEffects::GenerateRrGuides() const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid()
        || !m_sceneFramebuffer->HasMaterialGbuffer()
        || m_rrNormalRoughnessTarget.resource == nullptr)
    {
        return;
    }

    SceneRenderTrace::Scope guideScope("rr guides");
    const float clear[] = {0.0f, 0.0f, 0.0f, 1.0f};
    const std::uintptr_t normalSrv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::ShadingNormal);   // RT2
    const std::uintptr_t material0Srv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialAlbedoRough); // RT5 albedo+rough
    const std::uintptr_t material1Srv = m_sceneFramebuffer->GetGBufferSrvCpuHandle(GBufferSlot::MaterialMetallic); // RT6 metallic

    // Real-time PT skips the raster skybox, so sky pixels carry G-buffer clear values — a BLACK
    // diffuse albedo that breaks RR demodulation and smears the sky under rotation. Patch sky pixels
    // (PT primary-miss metadata, same signal as the sky motion patch) to the emissive convention.
    const bool ptSkyGuides = m_pathTracerActive
        && m_pathTracerConvergenceMode == PtConvergenceMode::RealTime
        && m_dxrPathTracerMetadataSrv != 0;

    const std::pair<InternalTarget*, int> passes[] = {
        {const_cast<InternalTarget*>(&m_rrDiffuseAlbedoTarget), 0},
        {const_cast<InternalTarget*>(&m_rrSpecularAlbedoTarget), 1},
        {const_cast<InternalTarget*>(&m_rrNormalRoughnessTarget), 2},
    };
    for (const auto& pass : passes)
    {
        // P4b: the rr* material targets were already filled from the PT bounce-0 guides this
        // frame — regenerating from the raster G-buffer would reintroduce the mixed bundle.
        if (m_ptFullGuidesThisFrame)
        {
            break;
        }
        m_rrGuidesShader->Use(false);
        m_rrGuidesShader->SetInt("uNormalMap", 0);
        m_rrGuidesShader->SetInt("uMaterial0Map", 1);
        m_rrGuidesShader->SetInt("uMaterial1Map", 2);
        m_rrGuidesShader->SetInt("uGuideMode", pass.second);
        m_rrGuidesShader->SetInt("uUsePathTracerHitDistance", 0);
        m_rrGuidesShader->SetInt("uPatchPtSkyGuides", ptSkyGuides ? 1 : 0);
        m_rrGuidesShader->SetFloat("uReflectionUvScaleX", m_dxrReflectionUvScaleX);
        m_rrGuidesShader->SetFloat("uReflectionUvScaleY", m_dxrReflectionUvScaleY);
        m_rrGuidesShader->BindTextureSlot(0, normalSrv);
        m_rrGuidesShader->BindTextureSlot(1, material0Srv);
        m_rrGuidesShader->BindTextureSlot(2, material1Srv);
        // t3 (reflection) is unused in modes 0-2 but must be a valid descriptor; bind a placeholder.
        m_rrGuidesShader->BindTextureSlot(3, m_dxrReflectionSrv != 0 ? m_dxrReflectionSrv : normalSrv);
        // t4 (PT metadata) likewise needs a valid descriptor when the patch is inactive.
        m_rrGuidesShader->BindTextureSlot(
            4, ptSkyGuides ? m_dxrPathTracerMetadataSrv : normalSrv);
        DrawFullscreenToTarget(*m_rrGuidesShader, *pass.first, m_width, m_height, clear);
    }

    // RR4 spec hit-distance guide (mode 3). Hybrid: sampled from the reflection trace (quality-scaled
    // UV). Path-traced real-time: sampled from the STABLE deterministic primary spec hit distance in
    // the PT output .a at full render res (devdoc/dxr/pt/rr4-spec-hitdist.md) — only reflective pixels
    // carry a finite distance; rough/diffuse report max trace distance (no specular reprojection).
    const bool ptSpecGuide = m_pathTracerActive
        && m_pathTracerConvergenceMode == PtConvergenceMode::RealTime
        && m_dxrPathTracerOutputSrv != 0;
    if ((m_dxrReflectionSrv != 0 || ptSpecGuide) && m_rrSpecularHitDistanceTarget.resource != nullptr)
    {
        const std::uintptr_t hitDistSrv = ptSpecGuide ? m_dxrPathTracerOutputSrv : m_dxrReflectionSrv;
        m_rrGuidesShader->Use(false);
        m_rrGuidesShader->SetInt("uGuideMode", 3);
        m_rrGuidesShader->SetInt("uUsePathTracerHitDistance", ptSpecGuide ? 1 : 0);
        // Sky patch does not apply to mode 3: PT output .a already reports max trace distance
        // ("no specular reprojection") for primary misses.
        m_rrGuidesShader->SetInt("uPatchPtSkyGuides", 0);
        // PT output is full render res (uv scale 1); the hybrid reflection buffer may be quality-scaled.
        m_rrGuidesShader->SetFloat("uReflectionUvScaleX", ptSpecGuide ? 1.0f : m_dxrReflectionUvScaleX);
        m_rrGuidesShader->SetFloat("uReflectionUvScaleY", ptSpecGuide ? 1.0f : m_dxrReflectionUvScaleY);
        m_rrGuidesShader->BindTextureSlot(0, normalSrv);      // unused in mode 3, keep slots bound
        m_rrGuidesShader->BindTextureSlot(1, material0Srv);
        m_rrGuidesShader->BindTextureSlot(2, material1Srv);
        m_rrGuidesShader->BindTextureSlot(3, hitDistSrv);
        m_rrGuidesShader->BindTextureSlot(4, normalSrv);      // t4 placeholder (patch inactive)
        DrawFullscreenToTarget(
            *m_rrGuidesShader, const_cast<InternalTarget&>(m_rrSpecularHitDistanceTarget),
            m_width, m_height, clear);
    }
    guideScope.Success();
}

void ScreenSpaceEffects::BlitRrGuideDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    if (!IsRrGuideDebugMode(m_debugMode) || outputTarget == nullptr
        || m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->HasMaterialGbuffer())
    {
        return;
    }

    GenerateRrGuides();

    std::uintptr_t srv = 0;
    switch (m_debugMode)
    {
    case RenderDebugMode::RrDiffuseAlbedo: srv = m_rrDiffuseAlbedoTarget.srvCpuHandle; break;
    case RenderDebugMode::RrSpecularAlbedo: srv = m_rrSpecularAlbedoTarget.srvCpuHandle; break;
    case RenderDebugMode::RrNormalRoughness: srv = m_rrNormalRoughnessTarget.srvCpuHandle; break;
    default: return;
    }
    if (srv == 0)
    {
        return;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_debugChannelShader->Use(false, true);
    m_debugChannelShader->SetInt("uInput", 0);
    m_debugChannelShader->SetInt("uOutputRgb", 1);
    m_debugChannelShader->SetInt("uOutputAlpha", 0);
    m_debugChannelShader->SetFloat("uAlphaScale", 1.0f);
    m_debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
    m_debugChannelShader->BindTextureSlot(0, srv);
    m_debugChannelShader->FlushUniforms();
    DrawFullscreenQuad();
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
        return;
    }
    {
        const GfxContext::GpuTimerScope gpuScopePresentation("Post-process/Presentation stage");
        RunApplyPresentationStage(state);
    }
    FinalizeApplyFrame(state);
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

float ScreenSpaceEffects::GetGtaoRadius() const
{
    return m_gtaoRadius;
}

void ScreenSpaceEffects::SetGtaoRadius(const float radius)
{
    m_gtaoRadius = std::clamp(radius, 0.05f, 5.0f);
}

float ScreenSpaceEffects::GetGtaoThickness() const
{
    return m_gtaoThickness;
}

void ScreenSpaceEffects::SetGtaoThickness(const float thickness)
{
    m_gtaoThickness = std::clamp(thickness, 0.02f, 2.0f);
}

float ScreenSpaceEffects::GetGtaoFalloff() const
{
    return m_gtaoFalloff;
}

void ScreenSpaceEffects::SetGtaoFalloff(const float falloff)
{
    m_gtaoFalloff = std::clamp(falloff, 0.25f, 6.0f);
}

float ScreenSpaceEffects::GetGtaoPower() const
{
    return m_gtaoPower;
}

void ScreenSpaceEffects::SetGtaoPower(const float power)
{
    m_gtaoPower = std::clamp(power, 0.25f, 4.0f);
}

int ScreenSpaceEffects::GetGtaoDirections() const
{
    return m_gtaoDirections;
}

void ScreenSpaceEffects::SetGtaoDirections(const int directions)
{
    m_gtaoDirections = std::clamp(directions, 2, 8);
}

int ScreenSpaceEffects::GetGtaoSteps() const
{
    return m_gtaoSteps;
}

void ScreenSpaceEffects::SetGtaoSteps(const int steps)
{
    m_gtaoSteps = std::clamp(steps, 2, 12);
}

bool ScreenSpaceEffects::IsGtaoDenoiseEnabled() const
{
    return m_gtaoDenoiseEnabled;
}

void ScreenSpaceEffects::SetGtaoDenoiseEnabled(const bool enabled)
{
    m_gtaoDenoiseEnabled = enabled;
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
    FrameDiagnostics::LogHistoryEvent(
        m_dlssViewportId, "pt-temporal-diagnostics", "diagnostic-input",
        m_pathTracerActive ? "path-tracer" : "raster", "debug-mode-selection",
        m_rayReconstruction ? "rr" : "dlss", "existing-quality",
        m_width, m_height, m_viewportWidth, m_viewportHeight,
        false, mode != RenderDebugMode::None, static_cast<std::uint32_t>(mode));
    if (IsRtPrimaryDebugMode(mode) && !IsRtPrimaryDebugMode(m_debugMode))
    {
        m_rtPrimaryDebugSettleFrames = 0;
    }

    const bool isolateChanged =
        PtDebugIsolateModeFromRenderDebug(m_debugMode) != PtDebugIsolateModeFromRenderDebug(mode);
    const bool temporalDiagnosticsChanged =
        (IsPtTemporalStatsDebugMode(m_debugMode) || IsPtMotionReprojectionDebugMode(m_debugMode)
            || IsPtDepthReprojectionDebugMode(m_debugMode)
            || IsPtMatrixDepthReprojectionDebugMode(m_debugMode))
        != (IsPtTemporalStatsDebugMode(mode) || IsPtMotionReprojectionDebugMode(mode)
            || IsPtDepthReprojectionDebugMode(mode)
            || IsPtMatrixDepthReprojectionDebugMode(mode));
    const bool giTemporalDiagnosticKindChanged =
        IsPtRestirGiSpatialStatsDebugMode(m_debugMode)
        && IsPtRestirGiSpatialStatsDebugMode(mode)
        && m_debugMode != mode;
    m_debugMode = mode;
    if (isolateChanged || temporalDiagnosticsChanged || giTemporalDiagnosticKindChanged)
    {
        ResetPathTracerTemporalDiagnostics();
    }
}

void ScreenSpaceEffects::ResetRtPrimaryDebugBlitSettle()
{
    m_rtPrimaryDebugSettleFrames = 0;
}

void ScreenSpaceEffects::NotifyRtPrimaryDebugDispatched()
{
    if (!IsRtPrimaryDebugMode(m_debugMode))
    {
        return;
    }

    if (m_rtPrimaryDebugSettleFrames < 3)
    {
        ++m_rtPrimaryDebugSettleFrames;
    }
}

bool ScreenSpaceEffects::IsRtPrimaryDebugBlitReady() const
{
    if (!IsRtPrimaryDebugMode(m_debugMode))
    {
        return false;
    }

    return m_rtPrimaryDebugSettleFrames >= 2;
}

bool ScreenSpaceEffects::IsPathTracerBlitReady() const
{
    return m_dxrPrimaryDebugShader != nullptr;
}

void ScreenSpaceEffects::SetDxrSmokeDebugSrv(const std::uintptr_t srvCpuHandle)
{
    m_dxrSmokeDebugSrv = srvCpuHandle;
}

void ScreenSpaceEffects::SetDxrPrimaryDebugSrvs(
    const std::uintptr_t primaryOutputSrvCpuHandle,
    const std::uintptr_t primaryMetadataSrvCpuHandle)
{
    m_dxrPrimaryOutputSrv = primaryOutputSrvCpuHandle;
    m_dxrPrimaryMetadataSrv = primaryMetadataSrvCpuHandle;
}

void ScreenSpaceEffects::SetDxrReflectionSrv(
    const std::uintptr_t reflectionSrvCpuHandle,
    const float uvScaleX,
    const float uvScaleY,
    const std::uintptr_t denoisedSrvCpuHandle,
    const float maxTraceDistance)
{
    m_dxrReflectionSrv = reflectionSrvCpuHandle;
    m_dxrReflectionDenoisedSrv = denoisedSrvCpuHandle;
    m_dxrReflectionUvScaleX = uvScaleX;
    m_dxrReflectionUvScaleY = uvScaleY;
    m_dxrReflectionMaxTraceDistance = maxTraceDistance;
}

bool ScreenSpaceEffects::ReflectionCompositeReplacesSpecIbl(
    const bool dxrReflectionsEnabled, const bool iblReady, const RenderDebugMode debugMode) const
{
    // Must match the runRtIndirect / runSsrIndirect gating in Apply (minus trace freshness, which
    // is handled by the pure-IBL fallback). PBR debug modes render specific channels, not the
    // composite, so leave spec IBL baked there.
    if (IsPbrMaterialDebugMode(debugMode) || !iblReady)
    {
        return false;
    }
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->HasSplitLighting()
        || !m_sceneFramebuffer->HasMaterialGbuffer())
    {
        return false;
    }
    return dxrReflectionsEnabled || m_ssrEnabled;
}

bool ScreenSpaceEffects::GiInjectReplacesDiffuseIbl(
    const bool giActive, const bool iblReady, const RenderDebugMode debugMode) const
{
    // Must match the runRtGiInject gating in Apply (minus trace freshness, handled by the SH
    // fallback). PBR debug modes render specific channels, not the inject — leave ambient baked.
    if (IsPbrMaterialDebugMode(debugMode) || !iblReady)
    {
        return false;
    }
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->HasSplitLighting()
        || !m_sceneFramebuffer->HasMaterialGbuffer())
    {
        return false;
    }
    return giActive;
}

void ScreenSpaceEffects::SetDxrShadowSrv(
    const std::uintptr_t penumbraSrvCpuHandle,
    const std::uintptr_t denoisedSrvCpuHandle,
    const float uvScaleX,
    const float uvScaleY)
{
    m_dxrShadowPenumbraSrv = penumbraSrvCpuHandle;
    m_dxrShadowDenoisedSrv = denoisedSrvCpuHandle;
    m_dxrShadowUvScaleX = uvScaleX;
    m_dxrShadowUvScaleY = uvScaleY;
}

void ScreenSpaceEffects::SetDxrGiSrv(
    const std::uintptr_t giRawSrvCpuHandle,
    const std::uintptr_t giDenoisedSrvCpuHandle,
    const float uvScaleX,
    const float uvScaleY)
{
    m_dxrGiRawSrv = giRawSrvCpuHandle;
    m_dxrGiDenoisedSrv = giDenoisedSrvCpuHandle;
    m_dxrGiUvScaleX = uvScaleX;
    m_dxrGiUvScaleY = uvScaleY;
}

std::uintptr_t ScreenSpaceEffects::GetSceneColorSrvCpuHandle(const GBufferSlot slot) const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return 0;
    }

    return m_sceneFramebuffer->GetGBufferSrvCpuHandle(slot);
}

void ScreenSpaceEffects::PrepareSceneColorForDxrRead() const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    constexpr std::uint32_t kAllShaderRead = static_cast<std::uint32_t>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_sceneFramebuffer->TransitionDepthForDxrRead();
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::DirectLighting, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::IndirectLighting, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::ShadingNormal, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::SunShadowFactor, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::MotionVelocity, kAllShaderRead);
    m_sceneFramebuffer->TransitionGBufferSlot(GBufferSlot::MaterialAlbedoRough, kAllShaderRead);
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

    AntiAliasingMode effectiveMode = mode;
    if ((mode == AntiAliasingMode::DLAA || mode == AntiAliasingMode::DLSS)
        && DlssContext::Get().IsReady() && !DlssContext::Get().IsDlssSupported())
    {
        EngineLog::Warn(
            "dlss",
            "DLSS is not supported on this GPU; falling back to TAA.");
        effectiveMode = AntiAliasingMode::TAA;
    }

    // TAA, DLAA and DLSS own the resolve stage and are incompatible with geometry MSAA.
    const bool ownsResolve = effectiveMode == AntiAliasingMode::TAA
        || effectiveMode == AntiAliasingMode::DLAA || effectiveMode == AntiAliasingMode::DLSS;
    if (ownsResolve && m_msaaSampleCount > 1)
    {
        m_msaaSampleCount = 1;
    }

    if (m_antiAliasingMode != effectiveMode)
    {
        ResetTaaHistory();
        m_lastAntiAliasingMode = effectiveMode;
        m_width = 0;
        m_height = 0;
    }

    m_antiAliasingMode = effectiveMode;
}

DlssPreset ScreenSpaceEffects::GetDlssPreset() const
{
    return m_dlssPreset;
}

void ScreenSpaceEffects::SetDlssPreset(const DlssPreset preset)
{
    if (m_dlssPreset == preset)
    {
        return;
    }
    m_dlssPreset = preset;
    // The internal render resolution changes with the preset — force a target reallocation and drop
    // temporal history so the upscaler restarts cleanly.
    if (m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        ResetTaaHistory();
        m_width = 0;
        m_height = 0;
    }
}

bool ScreenSpaceEffects::GetRayReconstruction() const
{
    return m_rayReconstruction;
}

void ScreenSpaceEffects::SetRayReconstruction(const bool enabled)
{
    if (m_rayReconstruction == enabled)
    {
        return;
    }
    m_rayReconstruction = enabled;
    // Switching RR on/off changes the resolve owner (RR replaces the SR model) and the RT denoise
    // path — drop temporal history so the reconstruction restarts cleanly.
    ResetTaaHistory();
}

bool ScreenSpaceEffects::IsRayReconstructionActive() const
{
    if (!m_rayReconstruction)
    {
        return false;
    }
    if (m_antiAliasingMode != AntiAliasingMode::DLAA && m_antiAliasingMode != AntiAliasingMode::DLSS)
    {
        return false;
    }
    const DlssContext& dlss = DlssContext::Get();
    return dlss.IsReady() && dlss.IsRrSupported();
}

float ScreenSpaceEffects::GetDlssSharpness() const
{
    return m_dlssSharpness;
}

void ScreenSpaceEffects::SetDlssSharpness(const float sharpness)
{
    const float clamped = std::clamp(sharpness, 0.0f, 1.0f);
    if (m_dlssSharpness == clamped)
    {
        return;
    }
    m_dlssSharpness = clamped;
    if (m_antiAliasingMode == AntiAliasingMode::DLAA || m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        ResetTaaHistory();
    }
}

DlssRrPreset ScreenSpaceEffects::GetRrPreset() const
{
    return m_rrPreset;
}

void ScreenSpaceEffects::SetRrPreset(const DlssRrPreset preset)
{
    if (m_rrPreset == preset)
    {
        return;
    }
    m_rrPreset = preset;
    // The RR model swaps on the next evaluate; break temporal history so the new network starts
    // from a clean accumulation instead of inheriting the previous model's history.
    if (m_antiAliasingMode == AntiAliasingMode::DLAA || m_antiAliasingMode == AntiAliasingMode::DLSS)
    {
        ResetTaaHistory();
    }
}

int ScreenSpaceEffects::GetMsaaSampleCount() const
{
    return m_msaaSampleCount;
}

void ScreenSpaceEffects::SetMsaaSampleCount(const int sampleCount)
{
    int clampedCount = sampleCount;
    if (clampedCount <= 1)
    {
        clampedCount = 1;
    }
    else if (clampedCount != 2 && clampedCount != 4 && clampedCount != 8)
    {
        clampedCount = 4;
    }

    if (clampedCount > 1 && !GfxContext::Get().IsMsaaSampleCountSupported(clampedCount))
    {
        return;
    }

    if (clampedCount > 1
        && (m_antiAliasingMode == AntiAliasingMode::TAA
            || m_antiAliasingMode == AntiAliasingMode::DLAA
            || m_antiAliasingMode == AntiAliasingMode::DLSS))
    {
        SetAntiAliasingMode(AntiAliasingMode::None);
    }

    if (m_msaaSampleCount == clampedCount)
    {
        return;
    }

    m_msaaSampleCount = clampedCount;
    m_width = 0;
    m_height = 0;
}

bool ScreenSpaceEffects::IsMsaaPendingReload() const
{
    return m_msaaSampleCount != GfxContext::Get().GetActiveMsaaSampleCount();
}

void ScreenSpaceEffects::CopySettingsFrom(const ScreenSpaceEffects& source)
{
    m_enabled = source.m_enabled;
    m_ssaoEnabled = source.m_ssaoEnabled;
    m_aoMode = source.m_aoMode;
    m_ssaoRadius = source.m_ssaoRadius;
    m_ssaoBias = source.m_ssaoBias;
    m_ssaoPower = source.m_ssaoPower;
    m_gtaoRadius = source.m_gtaoRadius;
    m_gtaoThickness = source.m_gtaoThickness;
    m_gtaoFalloff = source.m_gtaoFalloff;
    m_gtaoPower = source.m_gtaoPower;
    m_gtaoDirections = source.m_gtaoDirections;
    m_gtaoSteps = source.m_gtaoSteps;
    m_gtaoDenoiseEnabled = source.m_gtaoDenoiseEnabled;
    m_ssaoShaderDebugMode = source.m_ssaoShaderDebugMode;
    m_aoStrength = source.m_aoStrength;
    m_exposure = source.m_exposure;
    m_tonemapMode = source.m_tonemapMode;
    m_bloomEnabled = source.m_bloomEnabled;
    m_bloomThreshold = source.m_bloomThreshold;
    m_bloomSoftKnee = source.m_bloomSoftKnee;
    m_bloomIntensity = source.m_bloomIntensity;
    m_bloomBlurRadius = source.m_bloomBlurRadius;
    m_bloomTemporalBlendFactor = source.m_bloomTemporalBlendFactor;
    m_bloomSameUvBlendFactor = source.m_bloomSameUvBlendFactor;
    m_bloomDepthThreshold = source.m_bloomDepthThreshold;
    m_antiAliasingMode = source.m_antiAliasingMode;
    m_dlssPreset = source.m_dlssPreset;
    m_rayReconstruction = source.m_rayReconstruction;
    m_dlssSharpness = source.m_dlssSharpness;
    m_rrPreset = source.m_rrPreset;
    m_forceDlssResetEveryFrame = source.m_forceDlssResetEveryFrame;
    m_useDilatedDlssMotionVectors = source.m_useDilatedDlssMotionVectors;
    m_reconstructDlssCameraMotion = source.m_reconstructDlssCameraMotion;
    m_freezeTemporalJitterDiagnostic = source.m_freezeTemporalJitterDiagnostic;
    m_useDlssSubmissionFrameIndexDiagnostic = source.m_useDlssSubmissionFrameIndexDiagnostic;
    m_msaaSampleCount = source.m_msaaSampleCount;
    m_fxaaSubpixQuality = source.m_fxaaSubpixQuality;
    m_fxaaEdgeThreshold = source.m_fxaaEdgeThreshold;
    m_renderScale = source.m_renderScale;
    m_taaBlendFactor = source.m_taaBlendFactor;
    m_giTemporalBlendFactor = source.m_giTemporalBlendFactor;
    m_giDepthThreshold = source.m_giDepthThreshold;
    m_ssgiDenoiseEnabled = source.m_ssgiDenoiseEnabled;
    m_ssgiNoiseInjectionEnabled = source.m_ssgiNoiseInjectionEnabled;
    m_ssgiNoiseStrength = source.m_ssgiNoiseStrength;
    m_ssgiSpatialDepthThreshold = source.m_ssgiSpatialDepthThreshold;
    m_ssgiSpatialBlurSpread = source.m_ssgiSpatialBlurSpread;
    m_ssgiRoughnessSpreadMin = source.m_ssgiRoughnessSpreadMin;
    m_ssgiRoughnessSpreadMax = source.m_ssgiRoughnessSpreadMax;
    m_ssgiEnabled = source.m_ssgiEnabled;
    m_ssgiStrength = source.m_ssgiStrength;
    m_ssgiMaxTraceDistance = source.m_ssgiMaxTraceDistance;
    m_ssgiStepCount = source.m_ssgiStepCount;
    m_ssgiThickness = source.m_ssgiThickness;
    m_ssrEnabled = source.m_ssrEnabled;
    m_ssrMaxTraceDistance = source.m_ssrMaxTraceDistance;
    m_ssrStepCount = source.m_ssrStepCount;
    m_ssrSampleCount = source.m_ssrSampleCount;
    m_ssrThickness = source.m_ssrThickness;
    m_ssrRoughnessCutoff = source.m_ssrRoughnessCutoff;
    m_ssrStepExponent = source.m_ssrStepExponent;
    m_ssrDenoiseEnabled = source.m_ssrDenoiseEnabled;
    m_ssrTemporalBlendFactor = source.m_ssrTemporalBlendFactor;
    m_ssrSameUvBlendFactor = source.m_ssrSameUvBlendFactor;
    m_ssrStrength = source.m_ssrStrength;
    m_ssrSpatialDepthThreshold = source.m_ssrSpatialDepthThreshold;
    m_ssrSpatialBlurSpread = source.m_ssrSpatialBlurSpread;
    m_ssrRoughnessSpreadMin = source.m_ssrRoughnessSpreadMin;
    m_ssrRoughnessSpreadMax = source.m_ssrRoughnessSpreadMax;
    m_ssrDepthThreshold = source.m_ssrDepthThreshold;
    m_smaaThreshold = source.m_smaaThreshold;
    m_smaaSearchSteps = source.m_smaaSearchSteps;
    m_ssaoBlurDepthThreshold = source.m_ssaoBlurDepthThreshold;
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

float ScreenSpaceEffects::GetGiTemporalBlendFactor() const
{
    return m_giTemporalBlendFactor;
}

void ScreenSpaceEffects::SetGiTemporalBlendFactor(const float factor)
{
    m_giTemporalBlendFactor = std::clamp(factor, 0.0f, 0.99f);
}

float ScreenSpaceEffects::GetGiDepthThreshold() const
{
    return m_giDepthThreshold;
}

void ScreenSpaceEffects::SetGiDepthThreshold(const float threshold)
{
    m_giDepthThreshold = std::clamp(threshold, 0.0005f, 0.05f);
}

bool ScreenSpaceEffects::IsSsgiDenoiseEnabled() const
{
    return m_ssgiDenoiseEnabled;
}

void ScreenSpaceEffects::SetSsgiDenoiseEnabled(const bool enabled)
{
    m_ssgiDenoiseEnabled = enabled;
}

bool ScreenSpaceEffects::IsSsgiNoiseInjectionEnabled() const
{
    return m_ssgiNoiseInjectionEnabled;
}

void ScreenSpaceEffects::SetSsgiNoiseInjectionEnabled(const bool enabled)
{
    m_ssgiNoiseInjectionEnabled = enabled;
}

float ScreenSpaceEffects::GetSsgiNoiseStrength() const
{
    return m_ssgiNoiseStrength;
}

void ScreenSpaceEffects::SetSsgiNoiseStrength(const float strength)
{
    m_ssgiNoiseStrength = std::clamp(strength, 0.0f, 1.0f);
}

float ScreenSpaceEffects::GetSsgiSpatialDepthThreshold() const
{
    return m_ssgiSpatialDepthThreshold;
}

void ScreenSpaceEffects::SetSsgiSpatialDepthThreshold(const float threshold)
{
    m_ssgiSpatialDepthThreshold = std::max(0.001f, threshold);
}

float ScreenSpaceEffects::GetSsgiSpatialBlurSpread() const
{
    return m_ssgiSpatialBlurSpread;
}

void ScreenSpaceEffects::SetSsgiSpatialBlurSpread(const float spread)
{
    m_ssgiSpatialBlurSpread = std::clamp(spread, 0.25f, 4.0f);
}

float ScreenSpaceEffects::GetSsgiRoughnessSpreadMin() const
{
    return m_ssgiRoughnessSpreadMin;
}

void ScreenSpaceEffects::SetSsgiRoughnessSpreadMin(const float spread)
{
    m_ssgiRoughnessSpreadMin = std::clamp(spread, 0.1f, 2.0f);
}

float ScreenSpaceEffects::GetSsgiRoughnessSpreadMax() const
{
    return m_ssgiRoughnessSpreadMax;
}

void ScreenSpaceEffects::SetSsgiRoughnessSpreadMax(const float spread)
{
    m_ssgiRoughnessSpreadMax = std::clamp(spread, 0.5f, 4.0f);
}

bool ScreenSpaceEffects::IsSsgiEnabled() const
{
    return m_ssgiEnabled;
}

void ScreenSpaceEffects::SetSsgiEnabled(const bool enabled)
{
    m_ssgiEnabled = enabled;
}

float ScreenSpaceEffects::GetSsgiStrength() const
{
    return m_ssgiStrength;
}

void ScreenSpaceEffects::SetSsgiStrength(const float strength)
{
    m_ssgiStrength = std::clamp(strength, 0.0f, 2.0f);
}

float ScreenSpaceEffects::GetSsgiMaxTraceDistance() const
{
    return m_ssgiMaxTraceDistance;
}

void ScreenSpaceEffects::SetSsgiMaxTraceDistance(const float distance)
{
    m_ssgiMaxTraceDistance = std::clamp(distance, 0.25f, 20.0f);
}

int ScreenSpaceEffects::GetSsgiStepCount() const
{
    return m_ssgiStepCount;
}

void ScreenSpaceEffects::SetSsgiStepCount(const int steps)
{
    m_ssgiStepCount = std::clamp(steps, 4, 32);
}

float ScreenSpaceEffects::GetSsgiThickness() const
{
    return m_ssgiThickness;
}

void ScreenSpaceEffects::SetSsgiThickness(const float thickness)
{
    m_ssgiThickness = std::clamp(thickness, 0.05f, 2.0f);
}

bool ScreenSpaceEffects::IsSsrEnabled() const
{
    return m_ssrEnabled;
}

void ScreenSpaceEffects::SetSsrEnabled(const bool enabled)
{
    if (m_ssrEnabled != enabled)
    {
        InvalidateSsrHistory();
    }
    m_ssrEnabled = enabled;
}

void ScreenSpaceEffects::InvalidateSsrHistory()
{
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
}

float ScreenSpaceEffects::GetSsrMaxTraceDistance() const
{
    return m_ssrMaxTraceDistance;
}

void ScreenSpaceEffects::SetSsrMaxTraceDistance(const float distance)
{
    m_ssrMaxTraceDistance = std::clamp(distance, 1.0f, 50.0f);
}

int ScreenSpaceEffects::GetSsrStepCount() const
{
    return m_ssrStepCount;
}

void ScreenSpaceEffects::SetSsrStepCount(const int steps)
{
    m_ssrStepCount = std::clamp(steps, 4, 64);
}

int ScreenSpaceEffects::GetSsrSampleCount() const
{
    return m_ssrSampleCount;
}

void ScreenSpaceEffects::SetSsrSampleCount(const int samples)
{
    m_ssrSampleCount = std::clamp(samples, 1, 8);
}

float ScreenSpaceEffects::GetSsrThickness() const
{
    return m_ssrThickness;
}

void ScreenSpaceEffects::SetSsrThickness(const float thickness)
{
    m_ssrThickness = std::clamp(thickness, 0.05f, 2.0f);
}

float ScreenSpaceEffects::GetSsrRoughnessCutoff() const
{
    return m_ssrRoughnessCutoff;
}

void ScreenSpaceEffects::SetSsrRoughnessCutoff(const float cutoff)
{
    m_ssrRoughnessCutoff = std::clamp(cutoff, 0.1f, 1.0f);
}

bool ScreenSpaceEffects::IsSsrDenoiseEnabled() const
{
    return m_ssrDenoiseEnabled;
}

void ScreenSpaceEffects::SetSsrDenoiseEnabled(const bool enabled)
{
    m_ssrDenoiseEnabled = enabled;
}

float ScreenSpaceEffects::GetSsrTemporalBlendFactor() const
{
    return m_ssrTemporalBlendFactor;
}

void ScreenSpaceEffects::SetSsrTemporalBlendFactor(const float factor)
{
    m_ssrTemporalBlendFactor = std::clamp(factor, 0.0f, 0.99f);
}

float ScreenSpaceEffects::GetSsrStrength() const
{
    return m_ssrStrength;
}

void ScreenSpaceEffects::SetSsrStrength(const float strength)
{
    m_ssrStrength = std::clamp(strength, 0.0f, 2.0f);
}

bool ScreenSpaceEffects::GetSsrSceneColorRanLastFrame() const
{
    return m_ssrSceneColorRanLastFrame;
}

bool ScreenSpaceEffects::GetSsrTraceRanLastFrame() const
{
    return m_ssrTraceRanLastFrame;
}

bool ScreenSpaceEffects::GetSsrDenoiseRanLastFrame() const
{
    return m_ssrDenoiseRanLastFrame;
}

bool ScreenSpaceEffects::GetSsrTemporalRanLastFrame() const
{
    return m_ssrTemporalRanLastFrame;
}

int ScreenSpaceEffects::GetSsrTraceTargetWidth() const
{
    return m_ssrTraceTarget.width;
}

int ScreenSpaceEffects::GetSsrTraceTargetHeight() const
{
    return m_ssrTraceTarget.height;
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

std::uintptr_t ScreenSpaceEffects::GetSceneDepthSrvCpuHandle() const
{
    if (m_sceneFramebuffer == nullptr)
    {
        return 0;
    }

    return m_sceneFramebuffer->GetDepthSrvCpuHandle();
}

bool ScreenSpaceEffects::BlitDepthToFramebuffer(const Framebuffer* viewportTarget) const
{
    if (viewportTarget == nullptr || !m_enabled || m_sceneFramebuffer == nullptr
        || !m_sceneFramebuffer->IsValid() || m_sceneFramebuffer->GetDepthResource() == nullptr
        || viewportTarget->GetDepthResource() == nullptr)
    {
        return false;
    }

    return viewportTarget->CopyDepthFrom(*m_sceneFramebuffer);
}
