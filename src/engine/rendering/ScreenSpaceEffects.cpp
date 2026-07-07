#include "engine/rendering/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/platform/EngineLog.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/platform/RenderPathDiagnostics.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/lighting/EnvironmentMap.h"
#include "engine/lighting/IBL.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Framebuffer.h"
#include "engine/rendering/RenderDebug.h"
#include "engine/rendering/Shader.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <glm/gtc/type_ptr.hpp>
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

    glm::vec3 SolidBackgroundLinear(const EnvironmentMap& environmentMap)
    {
        const glm::vec3 colorSrgb = environmentMap.GetSolidBackgroundColorSrgb();
        return glm::vec3(
            SrgbChannelToLinear(colorSrgb.x),
            SrgbChannelToLinear(colorSrgb.y),
            SrgbChannelToLinear(colorSrgb.z));
    }

    void SetCompositeBackgroundUniforms(
        Shader& shader,
        const Camera& camera,
        const EnvironmentMap& environmentMap)
    {
        glm::mat4 view = camera.GetViewMatrix();
        view[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const glm::mat4 invView = glm::inverse(view);
        const glm::mat4 invProjection = glm::inverse(camera.GetProjectionMatrix());

        shader.SetMat4("uInvView", invView);
        shader.SetMat4("uInvProjection", invProjection);
        shader.SetInt(
            "uBackgroundMode",
            environmentMap.UsesSkyboxBackground() ? 0 : 1);
        shader.SetFloat("uSkyboxExposure", environmentMap.GetExposure());
        shader.SetFloat("uEnvironmentRotationY", environmentMap.GetIBL().GetRotationYRadians());
        shader.SetVec3("uSolidBackgroundColor", SolidBackgroundLinear(environmentMap));

        if (environmentMap.UsesSkyboxBackground())
        {
            shader.BindTextureSlot(5, environmentMap.GetIBL().GetHdrEquirectSrvCpuHandle());
            shader.SetInt("uEquirectMap", 5);
        }
    }

    bool IsPostProcessDebugMode(RenderDebugMode mode)
    {
        return mode == RenderDebugMode::Ssao ||
               mode == RenderDebugMode::GtaoRaw ||
               mode == RenderDebugMode::GtaoFiltered ||
               mode == RenderDebugMode::CompositeOcclusion ||
               mode == RenderDebugMode::MotionVectors ||
               IsGBufferDebugMode(mode) ||
               IsRadianceDebugMode(mode) ||
               IsGiTemporalDebugMode(mode) ||
               IsSsgiDenoiseDebugMode(mode) ||
               IsSsrDebugMode(mode) ||
               IsDxrDebugMode(mode);
    }

    int SsrDebugModeIndex(RenderDebugMode mode)
    {
        return mode == RenderDebugMode::SsrSceneValidity ? 1 : 0;
    }

    int SsrTraceDebugModeIndex(RenderDebugMode mode)
    {
        return mode == RenderDebugMode::SsrTraceConfidence ? 1 : 0;
    }

    int SsrDenoiseDebugModeIndex(RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::SsrDenoiseTemporal:
            return 1;
        case RenderDebugMode::SsrDenoiseFinal:
        case RenderDebugMode::SsrUpscaled:
            return 2;
        case RenderDebugMode::SsrSvgfVariance:
            return 3;
        default:
            return 0;
        }
    }

    int GiTemporalDebugModeIndex(RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::GiDisocclusion:
            return 1;
        case RenderDebugMode::RadianceTemporalDelta:
            return 2;
        default:
            return 0;
        }
    }

    int RadianceDebugModeIndex(RenderDebugMode mode)
    {
        return mode == RenderDebugMode::RadianceValidity ? 1 : 0;
    }

    int GBufferDebugModeIndex(RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::GBufferAlbedo:
            return 0;
        case RenderDebugMode::GBufferRoughness:
            return 1;
        case RenderDebugMode::GBufferMetallic:
            return 2;
        case RenderDebugMode::GBufferEmissive:
            return 3;
        default:
            return 0;
        }
    }

    int SsgiDebugModeIndex(RenderDebugMode mode)
    {
        switch (mode)
        {
        case RenderDebugMode::SsgiTraceHitMask:
            return 1;
        case RenderDebugMode::SsgiTraceHitDistance:
            return 2;
        default:
            return 0;
        }
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

ScreenSpaceEffects::ScreenSpaceEffects()
    : m_sceneFramebuffer(std::make_unique<Framebuffer>()),
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
          EngineConstants::ScreenCompositeFragmentShader)),
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
          EngineConstants::TemporalReprojectFragmentShader)),
      m_giDepthHistoryShader(std::make_unique<Shader>(
          EngineConstants::FullscreenVertexShader,
          EngineConstants::GiDepthHistoryFragmentShader)),
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
          ShaderSamplerOverrides{(1u << 1), true}))
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
    DestroyInternalTarget(m_dlssOutputTarget);
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
        ThrowPostProcessTargetError("Failed to create post-process render target");
    }

    target.resource = resource;
    target.allocation = allocation;
    target.width = width;
    target.height = height;
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

    CreateTexture2DSrv(
        device,
        resource,
        {target.srvCpuHandle},
        static_cast<DXGI_FORMAT>(format),
        1);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{GfxContext::Get().GetOffscreenRtvCpuHandle(target.rtvIndex)};
    device->CreateRenderTargetView(resource, nullptr, rtvHandle);
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
    // NGX writes the DLSS output via a UAV. No RTV is created (nothing rasterizes into it).
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

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
    target.resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    target.rtvIndex = UINT32_MAX;

    target.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (target.srvIndex == UINT32_MAX)
    {
        DestroyInternalTarget(target);
        ThrowPostProcessTargetError("Failed to allocate DLSS output SRV descriptor");
    }
    target.srvCpuHandle = GfxContext::Get().GetSrvCpuHandle(target.srvIndex);

    CreateTexture2DSrv(
        device,
        resource,
        {target.srvCpuHandle},
        static_cast<DXGI_FORMAT>(format),
        1);
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
    target.resourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
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
    if (m_noiseTexture.srvIndex == UINT32_MAX)
    {
        textureAllocation->Release();
        textureResource->Release();
        throw std::runtime_error("Failed to allocate SSAO noise SRV descriptor");
    }
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

void ScreenSpaceEffects::InvalidateTemporalHistory() const
{
    m_motionVectorFrameState = {};
    m_radianceHistoryValid = false;
    m_giFrameIndex = 0;
    m_giPrevViewProjection = glm::mat4(1.0f);
    m_ssrHistoryValid = false;
    m_ssrFrameIndex = 0;
    m_prevFrameBloomSrv = 0;
    m_dlssHistoryValid = false;
}

void ScreenSpaceEffects::ResetTaaHistory() const
{
    m_taaHistoryValid = false;
    m_taaFrameIndex = 0;
    m_bloomHistoryValid = false;
    m_bloomTemporalWarmupFrames = 0;
    m_dlssHistoryValid = false;
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

    if (m_width == renderWidth && m_height == renderHeight && m_sceneFramebuffer->IsValid()
        && m_sceneFramebuffer->GetSampleCount() == GetEffectiveGeometryMsaaSampleCount())
    {
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
        // DLSS scaling output lives at DISPLAY res (upscale target), unlike the render-res targets
        // above. S2 LDR stopgap: 8-bit UNORM matching the tonemap LDR target we feed as input.
        SceneRenderTrace::Scope dlssScope("resize dlss output target");
        CreateUavTarget(
            m_dlssOutputTarget,
            viewportWidth,
            viewportHeight,
            static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM));
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

namespace
{
    // TAA and both DLSS modes are temporal and require sub-pixel jitter on the projection. (S3 will
    // harden the DLSS jitter convention; S2 needs it on so the upscaler has sub-pixel coverage.)
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
    m_motionVectorFrameState.prevView = camera.GetViewMatrix();
    m_motionVectorFrameState.prevProjection = camera.GetProjectionMatrix();
    m_motionVectorFrameState.prevUnjitteredProjection = camera.GetUnjitteredProjectionMatrix();
    m_motionVectorFrameState.prevViewProjection =
        m_motionVectorFrameState.prevUnjitteredProjection * m_motionVectorFrameState.prevView;
    m_giPrevViewProjection = m_motionVectorFrameState.prevViewProjection;
    m_motionVectorFrameState.historyValid = true;
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

void ScreenSpaceEffects::EndScenePass() const
{
    if (m_sceneFramebuffer->UsesMsaa())
    {
        m_sceneFramebuffer->ResolveMsaa();

        if (m_sceneFramebuffer->GetMsaaDepthSrvCpuHandle() != 0)
        {
            EnsureMsaaDepthResolveShader();
            m_sceneFramebuffer->BeginMsaaDepthResolvePass();
            m_msaaDepthResolveShader->Use(false, false);
            m_msaaDepthResolveShader->SetInt(
                "uSampleCount",
                m_sceneFramebuffer->GetSampleCount());
            m_msaaDepthResolveShader->BindTextureSlot(
                0,
                m_sceneFramebuffer->GetMsaaDepthSrvCpuHandle());
            m_msaaDepthResolveShader->SetInt("uMsaaDepth", 0);
            DrawFullscreenPass(*m_msaaDepthResolveShader, false);
            m_sceneFramebuffer->FinishMsaaDepthResolvePass();
        }
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
    if (m_debugMode != RenderDebugMode::RtDispatchSmoke || m_dxrSmokeDebugSrv == 0
        || outputTarget == nullptr)
    {
        return;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_debugChannelShader->Use(false, true);
    m_debugChannelShader->SetInt("uOutputRgb", 1);
    m_debugChannelShader->SetInt("uOutputAlpha", 0);
    m_debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
    m_debugChannelShader->SetInt("uInput", 0);
    m_debugChannelShader->BindTextureSlot(0, m_dxrSmokeDebugSrv);
    m_debugChannelShader->FlushUniforms();
    DrawFullscreenQuad();
}

void ScreenSpaceEffects::BlitRtPrimaryDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight,
    const float maxTraceDistance) const
{
    if (!IsRtPrimaryDebugMode(m_debugMode) || !IsRtPrimaryDebugBlitReady()
        || m_dxrPrimaryOutputSrv == 0 || m_dxrPrimaryMetadataSrv == 0
        || outputTarget == nullptr)
    {
        return;
    }

    int viewMode = 0;
    switch (m_debugMode)
    {
    case RenderDebugMode::RtPrimaryHit:
        viewMode = 0;
        break;
    case RenderDebugMode::RtPrimaryDepth:
        viewMode = 1;
        break;
    case RenderDebugMode::RtPrimaryNormal:
        viewMode = 2;
        break;
    default:
        return;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_dxrPrimaryDebugShader->Use(false, true);
    m_dxrPrimaryDebugShader->SetInt("uViewMode", viewMode);
    m_dxrPrimaryDebugShader->SetFloat("uMaxTraceDistance", maxTraceDistance);
    m_dxrPrimaryDebugShader->SetInt("uPrimaryOutput", 0);
    m_dxrPrimaryDebugShader->SetInt("uPrimaryMetadata", 1);
    m_dxrPrimaryDebugShader->BindTextureSlot(0, m_dxrPrimaryOutputSrv);
    m_dxrPrimaryDebugShader->BindTextureSlot(1, m_dxrPrimaryMetadataSrv);
    m_dxrPrimaryDebugShader->FlushUniforms();
    DrawFullscreenQuad();
}

void ScreenSpaceEffects::BlitRtReflectionDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    if (!IsRtReflectionDebugMode(m_debugMode) || m_dxrReflectionSrv == 0 || outputTarget == nullptr)
    {
        return;
    }

    // RtSpecReplacement is rendered by the composite debug branch inside Apply(), not here.
    if (m_debugMode == RenderDebugMode::RtSpecReplacement)
    {
        return;
    }

    const bool showHitDistance = m_debugMode == RenderDebugMode::RtReflectionConfidence;
    const bool showDenoised = m_debugMode == RenderDebugMode::RtReflectionDenoised;

    // Denoised view = the D6 resolve preview: denoised radiance on hits, fresh env from the
    // raw buffer on miss/sky pixels (NRD leaves stale reprojected history there, which smears
    // the skybox under camera motion). Falls back to the raw buffer if no denoised output.
    if (showDenoised && m_dxrReflectionDenoisedSrv != 0)
    {
        BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        m_rtReflectionResolveShader->Use(false, true);
        m_rtReflectionResolveShader->SetInt("uDenoised", 0);
        m_rtReflectionResolveShader->SetInt("uRaw", 1);
        m_rtReflectionResolveShader->SetVec2(
            "uUvScale",
            glm::vec2(m_dxrReflectionUvScaleX, m_dxrReflectionUvScaleY));
        m_rtReflectionResolveShader->SetFloat("uMaxTraceDistance", m_dxrReflectionMaxTraceDistance);
        m_rtReflectionResolveShader->BindTextureSlot(0, m_dxrReflectionDenoisedSrv);
        m_rtReflectionResolveShader->BindTextureSlot(1, m_dxrReflectionSrv);
        m_rtReflectionResolveShader->FlushUniforms();
        DrawFullscreenQuad();
        return;
    }

    const std::uintptr_t sourceSrv = m_dxrReflectionSrv;

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_debugChannelShader->Use(false, true);
    m_debugChannelShader->SetInt("uInput", 0);
    m_debugChannelShader->SetInt("uOutputRgb", showHitDistance ? 0 : 1);
    m_debugChannelShader->SetInt("uOutputAlpha", showHitDistance ? 1 : 0);
    // Alpha now carries raw hit distance (RELAX packing); normalize for display.
    m_debugChannelShader->SetFloat(
        "uAlphaScale",
        m_dxrReflectionMaxTraceDistance > 0.0f ? 1.0f / m_dxrReflectionMaxTraceDistance : 1.0f);
    // The reflection texture can be larger than the last dispatch (quality shrink keeps the
    // bigger allocation); sample only the freshly written region.
    m_debugChannelShader->SetVec2(
        "uUvScale",
        glm::vec2(m_dxrReflectionUvScaleX, m_dxrReflectionUvScaleY));
    m_debugChannelShader->BindTextureSlot(0, sourceSrv);
    m_debugChannelShader->FlushUniforms();
    DrawFullscreenQuad();
}

void ScreenSpaceEffects::BlitRtShadowDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    if (!IsRtShadowDebugMode(m_debugMode) || outputTarget == nullptr)
    {
        return;
    }

    // Denoised view uses the SIGMA output (square-unpacked); raw view uses the 1-spp penumbra
    // buffer mapped to a binary mask. Denoised falls back to raw when the denoiser is off.
    const bool wantDenoised = m_debugMode == RenderDebugMode::RtShadowDenoised;
    const bool useDenoised = wantDenoised && m_dxrShadowDenoisedSrv != 0;
    const std::uintptr_t sourceSrv = useDenoised ? m_dxrShadowDenoisedSrv : m_dxrShadowPenumbraSrv;
    if (sourceSrv == 0)
    {
        return;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_dxrShadowDebugShader->Use(false, true);
    m_dxrShadowDebugShader->SetInt("uInput", 0);
    m_dxrShadowDebugShader->SetInt("uRawPenumbra", useDenoised ? 0 : 1);
    m_dxrShadowDebugShader->SetVec2(
        "uUvScale", glm::vec2(m_dxrShadowUvScaleX, m_dxrShadowUvScaleY));
    m_dxrShadowDebugShader->BindTextureSlot(0, sourceSrv);
    m_dxrShadowDebugShader->FlushUniforms();
    DrawFullscreenQuad();
}

void ScreenSpaceEffects::BlitRtGiDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    if (!IsRtGiDebugMode(m_debugMode) || outputTarget == nullptr)
    {
        return;
    }

    // RtGiInject visualizes the injected delta (albedo * gi * strength): re-run the inject shader
    // in debug mode straight to the output. Requires the scene MRTs.
    if (m_debugMode == RenderDebugMode::RtGiInject)
    {
        const std::uintptr_t giInjectSrv =
            m_dxrGiDenoisedSrv != 0 ? m_dxrGiDenoisedSrv : m_dxrGiRawSrv;
        if (giInjectSrv == 0 || m_sceneFramebuffer == nullptr
            || !m_sceneFramebuffer->HasMaterialGbuffer())
        {
            return;
        }

        BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        m_dxrGiInjectShader->Use(false, true);
        m_dxrGiInjectShader->SetInt("uIndirectMap", 0);
        m_dxrGiInjectShader->SetInt("uGiDenoisedMap", 1);
        m_dxrGiInjectShader->SetInt("uDepthMap", 2);
        m_dxrGiInjectShader->SetInt("uMaterial0Map", 3);
        m_dxrGiInjectShader->SetInt("uMaterial1Map", 4);
        m_dxrGiInjectShader->SetVec2(
            "uGiUvScale", glm::vec2(m_dxrGiUvScaleX, m_dxrGiUvScaleY));
        m_dxrGiInjectShader->SetFloat("uStrength", m_dxrGiStrength);
        m_dxrGiInjectShader->SetInt("uDebugGiInject", 1);
        m_dxrGiInjectShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_dxrGiInjectShader->BindTextureSlot(1, giInjectSrv);
        m_dxrGiInjectShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_dxrGiInjectShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_dxrGiInjectShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        m_dxrGiInjectShader->FlushUniforms();
        DrawFullscreenQuad();
        return;
    }

    // RtGiRaw / RtGiDenoised: straight rgb visualization of the GI radiance buffer.
    const bool wantDenoised = m_debugMode == RenderDebugMode::RtGiDenoised;
    const std::uintptr_t sourceSrv =
        (wantDenoised && m_dxrGiDenoisedSrv != 0) ? m_dxrGiDenoisedSrv : m_dxrGiRawSrv;
    if (sourceSrv == 0)
    {
        return;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_debugChannelShader->Use(false, true);
    m_debugChannelShader->SetInt("uInput", 0);
    m_debugChannelShader->SetInt("uOutputRgb", 1);
    m_debugChannelShader->SetInt("uOutputAlpha", 0);
    m_debugChannelShader->SetFloat("uAlphaScale", 1.0f);
    m_debugChannelShader->SetVec2(
        "uUvScale", glm::vec2(m_dxrGiUvScaleX, m_dxrGiUvScaleY));
    m_debugChannelShader->BindTextureSlot(0, sourceSrv);
    m_debugChannelShader->FlushUniforms();
    DrawFullscreenQuad();
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

    const D3D12_RESOURCE_STATES renderTargetState = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const D3D12_RESOURCE_STATES shaderResourceState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES beforeState = static_cast<D3D12_RESOURCE_STATES>(target.resourceState);
    if (beforeState == D3D12_RESOURCE_STATE_COMMON)
    {
        beforeState = shaderResourceState;
    }
    TransitionResource(
        commandList,
        resource,
        beforeState,
        renderTargetState);
    target.resourceState = static_cast<std::uint32_t>(renderTargetState);

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

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
    TransitionResource(
        commandList,
        resource,
        renderTargetState,
        shaderResourceState);
    target.resourceState = static_cast<std::uint32_t>(shaderResourceState);
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
    const DirectionalShadowSettings& shadowSettings,
    const EnvironmentMap& environmentMap) const
{
    FinalizePendingSsaoGpuReadback();

    if (!m_enabled || !m_sceneFramebuffer->IsValid())
    {
        const_cast<ScreenSpaceEffects*>(this)->m_ssrSceneColorRanLastFrame = false;
        const_cast<ScreenSpaceEffects*>(this)->m_ssrTraceRanLastFrame = false;
        const_cast<ScreenSpaceEffects*>(this)->m_ssrDenoiseRanLastFrame = false;
        const_cast<ScreenSpaceEffects*>(this)->m_ssrTemporalRanLastFrame = false;
        return;
    }

    const Framebuffer* outputTarget = GfxContext::Get().GetBoundOutputFramebuffer();

    const bool pbrDebugActive = IsPbrMaterialDebugMode(m_debugMode);
    const bool runAo = m_aoMode != AmbientOcclusionMode::Off && !pbrDebugActive;
    const bool runSsao = runAo && m_aoMode == AmbientOcclusionMode::SSAO;
    const bool runGtao = runAo && m_aoMode == AmbientOcclusionMode::GTAO;
    std::uintptr_t aoCompositeSrv = m_ssaoTarget.srvCpuHandle;

    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 inverseProjectionMatrix = glm::inverse(projectionMatrix);
    const glm::vec2 texelSize(
        1.0f / static_cast<float>(m_width),
        1.0f / static_cast<float>(m_height));

    if (runSsao || runGtao)
    {
        const float ssaoClear[] = {1.0f, 1.0f, 1.0f, 1.0f};

        if (runSsao)
        {
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
        }
        else
        {
            m_gtaoShader->Use(false);
            m_gtaoShader->SetInt("uDepthMap", 0);
            m_gtaoShader->SetInt("uNormalMap", 1);
            m_gtaoShader->SetMat4("uProjection", projectionMatrix);
            m_gtaoShader->SetMat4("uInvProjection", inverseProjectionMatrix);
            m_gtaoShader->SetMat4("uView", camera.GetViewMatrix());
            m_gtaoShader->SetVec2(
                "uProjectionScale",
                glm::vec2(projectionMatrix[0][0], projectionMatrix[1][1]));
            m_gtaoShader->SetFloat("uRadius", m_gtaoRadius);
            m_gtaoShader->SetFloat("uThickness", m_gtaoThickness);
            m_gtaoShader->SetFloat("uFalloff", m_gtaoFalloff);
            m_gtaoShader->SetFloat("uNearPlane", camera.GetNearPlane());
            m_gtaoShader->SetFloat("uFarPlane", camera.GetFarPlane());
            m_gtaoShader->SetInt("uDirections", m_gtaoDirections);
            m_gtaoShader->SetInt("uSteps", m_gtaoSteps);
            m_gtaoShader->SetInt(
                "uUseGeometryNormals",
                m_sceneFramebuffer->HasGeometryNormals() ? 1 : 0);
            m_gtaoShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_gtaoShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
            DrawFullscreenToTarget(
                *m_gtaoShader,
                const_cast<InternalTarget&>(m_gtaoRawTarget),
                m_width,
                m_height,
                ssaoClear);
            aoCompositeSrv = m_gtaoRawTarget.srvCpuHandle;
        }

        if ((runSsao && m_ssaoShaderDebugMode == 0) || (runGtao && m_gtaoDenoiseEnabled))
        {
            m_blurShader->Use(false);
            m_blurShader->SetInt("uInput", 0);
            m_blurShader->SetInt("uDepthMap", 1);
            m_blurShader->SetInt("uNormalMap", 2);
            m_blurShader->SetMat4("uInvProjection", inverseProjectionMatrix);
            m_blurShader->SetVec2("uTexelSize", texelSize);
            m_blurShader->SetFloat("uDepthThreshold", m_ssaoBlurDepthThreshold);
            m_blurShader->SetFloat("uBlurSpread", runGtao ? 0.8f : 1.0f);
            m_blurShader->SetFloat("uNormalPower", runGtao ? 8.0f : 4.0f);
            m_blurShader->SetInt(
                "uUseNormalWeight",
                m_sceneFramebuffer->HasGeometryNormals() ? 1 : 0);

            m_blurShader->SetVec2("uBlurDirection", glm::vec2(1.0f, 0.0f));
            m_blurShader->BindTextureSlot(0, runGtao ? m_gtaoRawTarget.srvCpuHandle : m_ssaoTarget.srvCpuHandle);
            m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_blurShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
            DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoBlurTarget), m_width, m_height, ssaoClear);

            m_blurShader->SetVec2("uBlurDirection", glm::vec2(0.0f, 1.0f));
            m_blurShader->BindTextureSlot(0, m_ssaoBlurTarget.srvCpuHandle);
            m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_blurShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
            DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoTarget), m_width, m_height, ssaoClear);

            if (runSsao)
            {
                m_blurShader->SetFloat("uBlurSpread", 2.5f);
                m_blurShader->SetVec2("uBlurDirection", glm::vec2(1.0f, 0.0f));
                m_blurShader->BindTextureSlot(0, m_ssaoTarget.srvCpuHandle);
                m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
                m_blurShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
                DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoBlurTarget), m_width, m_height, ssaoClear);

                m_blurShader->SetVec2("uBlurDirection", glm::vec2(0.0f, 1.0f));
                m_blurShader->BindTextureSlot(0, m_ssaoBlurTarget.srvCpuHandle);
                m_blurShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
                m_blurShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
                DrawFullscreenToTarget(*m_blurShader, const_cast<InternalTarget&>(m_ssaoTarget), m_width, m_height, ssaoClear);
            }
            aoCompositeSrv = m_ssaoTarget.srvCpuHandle;
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

    const bool runRadianceAssembly =
        !pbrDebugActive &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_radianceTarget.resource != nullptr;

    const float radianceClear[] = {0.0f, 0.0f, 0.0f, 0.0f};

    if (runRadianceAssembly)
    {
        SceneRenderTrace::Scope radianceScope("radiance assembly");
        m_radianceAssemblyShader->Use(false);
        m_radianceAssemblyShader->SetInt("uDirectLighting", 0);
        m_radianceAssemblyShader->SetInt("uIndirectLighting", 1);
        m_radianceAssemblyShader->SetInt("uDepthMap", 2);
        m_radianceAssemblyShader->SetInt("uMaterial0Map", 3);
        m_radianceAssemblyShader->SetInt("uMaterial1Map", 4);
        m_radianceAssemblyShader->SetInt("uIncludeFillDirect", 1);
        m_radianceAssemblyShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        m_radianceAssemblyShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_radianceAssemblyShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_radianceAssemblyShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_radianceAssemblyShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        DrawFullscreenToTarget(
            *m_radianceAssemblyShader,
            const_cast<InternalTarget&>(m_radianceTarget),
            m_width,
            m_height,
            radianceClear);
        radianceScope.Success();
    }

    const bool wantsSsr = m_ssrEnabled || IsSsrDebugMode(m_debugMode);

    const bool runSsrSceneColorAssembly =
        wantsSsr &&
        !pbrDebugActive &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasShadowFactor() &&
        m_ssrSceneColorTarget.resource != nullptr;

    const_cast<ScreenSpaceEffects*>(this)->m_ssrSceneColorRanLastFrame = runSsrSceneColorAssembly;

    if (runSsrSceneColorAssembly)
    {
        SceneRenderTrace::Scope ssrSceneColorScope("ssr scene color");
        // Bloom halos in reflections: use LAST frame's bloom output (bloom runs after SSR).
        // m_prevFrameBloomSrv is reset whenever bloom targets are recreated, so a zero value
        // means "no valid bloom yet" and disables the term.
        const bool useBloomInReflections =
            m_bloomEnabled && m_prevFrameBloomSrv != 0;
        m_ssrSceneColorShader->Use(false);
        m_ssrSceneColorShader->SetInt("uDirectLighting", 0);
        m_ssrSceneColorShader->SetInt("uSunShadowMap", 1);
        m_ssrSceneColorShader->SetInt("uDepthMap", 2);
        m_ssrSceneColorShader->SetInt("uIndirectLighting", 3);
        m_ssrSceneColorShader->SetInt("uPrevBloom", 4);
        m_ssrSceneColorShader->SetInt(
            "uUseShadowFactor",
            useShadowFactorComposite ? 1 : 0);
        // SSR-11: reflect indirect/ambient too (RT1 is same-frame data at this point).
        m_ssrSceneColorShader->SetInt("uUseIndirect", 1);
        m_ssrSceneColorShader->SetFloat(
            "uBloomIntensity",
            useBloomInReflections ? m_bloomIntensity : 0.0f);
        m_ssrSceneColorShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        m_ssrSceneColorShader->BindTextureSlot(1, shadowFactorSrv);
        m_ssrSceneColorShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_ssrSceneColorShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_ssrSceneColorShader->BindTextureSlot(
            4,
            useBloomInReflections ? m_prevFrameBloomSrv
                                  : m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        DrawFullscreenToTarget(
            *m_ssrSceneColorShader,
            const_cast<InternalTarget&>(m_ssrSceneColorTarget),
            m_ssrSceneColorTarget.width,
            m_ssrSceneColorTarget.height,
            radianceClear);
        ssrSceneColorScope.Success();
    }

    const bool runSsrTrace =
        wantsSsr &&
        runSsrSceneColorAssembly &&
        !pbrDebugActive &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasGeometryNormals() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_ssrSceneColorTarget.resource != nullptr &&
        m_ssrTraceTarget.resource != nullptr &&
        (m_ssrEnabled || IsSsrTraceDebugMode(m_debugMode) || IsSsrDenoiseDebugMode(m_debugMode)
         || IsSsrCompositeDebugMode(m_debugMode));

    const_cast<ScreenSpaceEffects*>(this)->m_ssrTraceRanLastFrame = runSsrTrace;
    const_cast<ScreenSpaceEffects*>(this)->m_lastSsrSpatialSrv = 0;
    const_cast<ScreenSpaceEffects*>(this)->m_lastSsrVarianceSrv = 0;
    const_cast<ScreenSpaceEffects*>(this)->m_lastSsrDenoiseSrv = 0;
    const_cast<ScreenSpaceEffects*>(this)->m_lastSsrResolvedSrv = 0;

    if (runSsrTrace)
    {
        SceneRenderTrace::Scope ssrTraceScope("ssr trace");
        m_ssrTraceShader->Use(false);
        m_ssrTraceShader->SetInt("uDepthMap", 0);
        m_ssrTraceShader->SetInt("uNormalMap", 1);
        m_ssrTraceShader->SetInt("uMaterial0Map", 2);
        m_ssrTraceShader->SetInt("uSceneColorMap", 3);
        m_ssrTraceShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssrTraceShader->SetMat4("uProjection", projectionMatrix);
        m_ssrTraceShader->SetMat4("uView", camera.GetViewMatrix());
        m_ssrTraceShader->SetFloat("uMaxTraceDistance", m_ssrMaxTraceDistance);
        m_ssrTraceShader->SetInt("uStepCount", m_ssrStepCount);
        m_ssrTraceShader->SetFloat("uThickness", m_ssrThickness);
        m_ssrTraceShader->SetFloat("uRoughnessCutoff", m_ssrRoughnessCutoff);
        m_ssrTraceShader->SetFloat("uFrameIndex", static_cast<float>(m_ssrFrameIndex));
        m_ssrTraceShader->SetFloat("uStepExponent", m_ssrStepExponent);
        m_ssrTraceShader->SetInt("uSampleCount", m_ssrSampleCount);
        // SSR-08: the trace target and its scene-color input run at trace resolution
        // (m_ssrTraceResolutionScale), so the manual bilinear/edge-penalty footprints in
        // ssr_trace.ps.hlsl need the trace-res texel size, not the full render-res one.
        m_ssrTraceShader->SetVec2(
            "uTexelSize",
            glm::vec2(
                1.0f / static_cast<float>(std::max(m_ssrTraceTarget.width, 1)),
                1.0f / static_cast<float>(std::max(m_ssrTraceTarget.height, 1))));
        m_ssrTraceShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_ssrTraceShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        m_ssrTraceShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_ssrTraceShader->BindTextureSlot(3, m_ssrSceneColorTarget.srvCpuHandle);
        DrawFullscreenToTarget(
            *m_ssrTraceShader,
            const_cast<InternalTarget&>(m_ssrTraceTarget),
            m_ssrTraceTarget.width,
            m_ssrTraceTarget.height,
            radianceClear);
        ++m_ssrFrameIndex;
        ssrTraceScope.Success();
    }

    const bool runSsrDenoise =
        runSsrTrace &&
        (m_ssrDenoiseEnabled || IsSsrDenoiseDebugMode(m_debugMode)) &&
        m_ssrSpatialTarget.resource != nullptr &&
        m_ssrSpatialBlurTarget.resource != nullptr &&
        m_ssrVarianceHistoryTarget.resource != nullptr &&
        m_ssrVarianceTemporalTarget.resource != nullptr;

    const_cast<ScreenSpaceEffects*>(this)->m_ssrDenoiseRanLastFrame = runSsrDenoise;

    std::uintptr_t ssrDenoiseInputSrv = m_ssrTraceTarget.srvCpuHandle;
    std::uintptr_t ssrVarianceSrv = 0;
    if (runSsrDenoise)
    {
        SceneRenderTrace::Scope ssrSvgfScope("ssr svgf");
        const float temporalClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
        const glm::vec2 traceTexelSize(
            1.0f / static_cast<float>(m_ssrTraceTarget.width),
            1.0f / static_cast<float>(m_ssrTraceTarget.height));
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        m_sceneFramebuffer->RestoreDepthShaderResource();

        const bool useMotionVectors = m_sceneFramebuffer->HasVelocity();
        const bool runSsrTemporal =
            m_ssrHistoryTarget.resource != nullptr && m_ssrTemporalTarget.resource != nullptr;

        const_cast<ScreenSpaceEffects*>(this)->m_ssrTemporalRanLastFrame = runSsrTemporal;

        if (runSsrTemporal && useMotionVectors)
        {
            SceneRenderTrace::Scope ssrTemporalScope("ssr svgf temporal color");
            const glm::mat4 viewMatrix = camera.GetViewMatrix();
            const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
            const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * viewMatrix);
            const glm::mat4 prevViewProj = m_motionVectorFrameState.historyValid
                ? m_motionVectorFrameState.prevViewProjection
                : unjitteredProjection * viewMatrix;

            m_ssrSvgfTemporalShader->Use(false);
            m_ssrSvgfTemporalShader->SetInt("uCurrentTrace", 0);
            m_ssrSvgfTemporalShader->SetInt("uHistoryTrace", 1);
            m_ssrSvgfTemporalShader->SetInt("uVelocity", 2);
            m_ssrSvgfTemporalShader->SetInt("uDepth", 3);
            m_ssrSvgfTemporalShader->SetInt("uNormalMap", 4);
            m_ssrSvgfTemporalShader->SetInt("uHistoryDepth", 5);
            m_ssrSvgfTemporalShader->SetMat4("uInvProjection", inverseProjectionMatrix);
            m_ssrSvgfTemporalShader->SetMat4("uInvViewProj", invViewProjCurr);
            m_ssrSvgfTemporalShader->SetMat4("uPrevViewProj", prevViewProj);
            m_ssrSvgfTemporalShader->SetFloat("uBlendFactor", m_ssrTemporalBlendFactor);
            m_ssrSvgfTemporalShader->SetFloat("uSameUvBlendFactor", m_ssrSameUvBlendFactor);
            m_ssrSvgfTemporalShader->SetFloat("uHistoryValid", m_ssrHistoryValid ? 1.0f : 0.0f);
            m_ssrSvgfTemporalShader->SetFloat("uDepthThreshold", m_ssrDepthThreshold);
            m_ssrSvgfTemporalShader->SetFloat("uTexelSizeX", traceTexelSize.x);
            m_ssrSvgfTemporalShader->SetFloat("uTexelSizeY", traceTexelSize.y);
            m_ssrSvgfTemporalShader->BindTextureSlot(0, ssrDenoiseInputSrv);
            m_ssrSvgfTemporalShader->BindTextureSlot(1, m_ssrHistoryTarget.srvCpuHandle);
            m_ssrSvgfTemporalShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(4));
            m_ssrSvgfTemporalShader->BindTextureSlot(3, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_ssrSvgfTemporalShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
            m_ssrSvgfTemporalShader->BindTextureSlot(5, m_ssrHistoryDepthTarget.srvCpuHandle);
            DrawFullscreenToTarget(
                *m_ssrSvgfTemporalShader,
                const_cast<InternalTarget&>(m_ssrTemporalTarget),
                m_ssrTraceTarget.width,
                m_ssrTraceTarget.height,
                temporalClear);

            // SSR-06 follow-up: maintain the depth history in the motion-vector path too
            // (previously only the fallback path wrote it, so the MV path had nothing valid
            // to test disocclusion against). Runs AFTER the temporal draw so the pass above
            // reads last frame's depth before it is overwritten with this frame's.
            m_giDepthHistoryShader->Use(false);
            m_giDepthHistoryShader->SetInt("uDepth", 0);
            m_giDepthHistoryShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(
                *m_giDepthHistoryShader,
                const_cast<InternalTarget&>(m_ssrHistoryDepthTarget),
                m_ssrTraceTarget.width,
                m_ssrTraceTarget.height,
                temporalClear);
            ssrTemporalScope.Success();
        }
        else if (runSsrTemporal)
        {
            SceneRenderTrace::Scope ssrTemporalScope("ssr svgf temporal color");
            const glm::mat4 viewMatrix = camera.GetViewMatrix();
            const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
            const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * viewMatrix);
            const glm::mat4 prevViewProj = m_motionVectorFrameState.historyValid
                ? m_motionVectorFrameState.prevViewProjection
                : unjitteredProjection * viewMatrix;

            m_temporalReprojectShader->Use(false);
            m_temporalReprojectShader->SetInt("uCurrentRadiance", 0);
            m_temporalReprojectShader->SetInt("uHistoryRadiance", 1);
            m_temporalReprojectShader->SetInt("uDepth", 2);
            m_temporalReprojectShader->SetInt("uHistoryDepth", 3);
            m_temporalReprojectShader->SetMat4("uInvViewProj", invViewProjCurr);
            m_temporalReprojectShader->SetMat4("uPrevViewProj", prevViewProj);
            m_temporalReprojectShader->SetFloat("uBlendFactor", m_ssrTemporalBlendFactor);
            m_temporalReprojectShader->SetFloat(
                "uHistoryValid",
                m_ssrHistoryValid && m_motionVectorFrameState.historyValid ? 1.0f : 0.0f);
            m_temporalReprojectShader->SetFloat("uTexelSizeX", traceTexelSize.x);
            m_temporalReprojectShader->SetFloat("uTexelSizeY", traceTexelSize.y);
            m_temporalReprojectShader->SetFloat("uDepthRejectThreshold", m_ssrDepthThreshold);
            m_temporalReprojectShader->BindTextureSlot(0, ssrDenoiseInputSrv);
            m_temporalReprojectShader->BindTextureSlot(1, m_ssrHistoryTarget.srvCpuHandle);
            m_temporalReprojectShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_temporalReprojectShader->BindTextureSlot(3, m_ssrHistoryDepthTarget.srvCpuHandle);
            DrawFullscreenToTarget(
                *m_temporalReprojectShader,
                const_cast<InternalTarget&>(m_ssrTemporalTarget),
                m_ssrTraceTarget.width,
                m_ssrTraceTarget.height,
                temporalClear);

            m_giDepthHistoryShader->Use(false);
            m_giDepthHistoryShader->SetInt("uDepth", 0);
            m_giDepthHistoryShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(
                *m_giDepthHistoryShader,
                const_cast<InternalTarget&>(m_ssrHistoryDepthTarget),
                m_ssrTraceTarget.width,
                m_ssrTraceTarget.height,
                temporalClear);
            ssrTemporalScope.Success();
        }

        if (runSsrTemporal)
        {
            SceneRenderTrace::Scope ssrVarianceScope("ssr svgf temporal variance");
            const glm::mat4 viewMatrix = camera.GetViewMatrix();
            const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
            const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * viewMatrix);
            const glm::mat4 prevViewProj = m_motionVectorFrameState.historyValid
                ? m_motionVectorFrameState.prevViewProjection
                : unjitteredProjection * viewMatrix;

            m_ssrSvgfVarianceTemporalShader->Use(false);
            m_ssrSvgfVarianceTemporalShader->SetInt("uCurrentTrace", 0);
            m_ssrSvgfVarianceTemporalShader->SetInt("uFilteredColor", 1);
            m_ssrSvgfVarianceTemporalShader->SetInt("uHistoryVariance", 2);
            m_ssrSvgfVarianceTemporalShader->SetInt("uVelocity", 3);
            m_ssrSvgfVarianceTemporalShader->SetInt("uDepth", 4);
            m_ssrSvgfVarianceTemporalShader->SetMat4("uInvProjection", inverseProjectionMatrix);
            m_ssrSvgfVarianceTemporalShader->SetMat4("uInvViewProj", invViewProjCurr);
            m_ssrSvgfVarianceTemporalShader->SetMat4("uPrevViewProj", prevViewProj);
            m_ssrSvgfVarianceTemporalShader->SetInt("uUseMotionVectors", useMotionVectors ? 1 : 0);
            m_ssrSvgfVarianceTemporalShader->SetFloat("uBlendFactor", m_ssrTemporalBlendFactor);
            m_ssrSvgfVarianceTemporalShader->SetFloat("uHistoryValid", m_ssrHistoryValid ? 1.0f : 0.0f);
            m_ssrSvgfVarianceTemporalShader->SetFloat("uDepthThreshold", m_ssrDepthThreshold);
            m_ssrSvgfVarianceTemporalShader->SetFloat("uTexelSizeX", traceTexelSize.x);
            m_ssrSvgfVarianceTemporalShader->SetFloat("uTexelSizeY", traceTexelSize.y);
            m_ssrSvgfVarianceTemporalShader->BindTextureSlot(0, ssrDenoiseInputSrv);
            m_ssrSvgfVarianceTemporalShader->BindTextureSlot(1, m_ssrTemporalTarget.srvCpuHandle);
            m_ssrSvgfVarianceTemporalShader->BindTextureSlot(2, m_ssrVarianceHistoryTarget.srvCpuHandle);
            if (useMotionVectors)
            {
                m_ssrSvgfVarianceTemporalShader->BindTextureSlot(
                    3,
                    m_sceneFramebuffer->GetColorSrvCpuHandle(4));
            }
            else
            {
                m_ssrSvgfVarianceTemporalShader->BindTextureSlot(
                    3,
                    m_sceneFramebuffer->GetDepthSrvCpuHandle());
            }
            m_ssrSvgfVarianceTemporalShader->BindTextureSlot(4, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(
                *m_ssrSvgfVarianceTemporalShader,
                const_cast<InternalTarget&>(m_ssrVarianceTemporalTarget),
                m_ssrTraceTarget.width,
                m_ssrTraceTarget.height,
                temporalClear);
            ssrVarianceScope.Success();

            std::swap(
                const_cast<InternalTarget&>(m_ssrHistoryTarget),
                const_cast<InternalTarget&>(m_ssrTemporalTarget));
            std::swap(
                const_cast<InternalTarget&>(m_ssrVarianceHistoryTarget),
                const_cast<InternalTarget&>(m_ssrVarianceTemporalTarget));
            m_ssrHistoryValid = true;
            ssrDenoiseInputSrv = m_ssrHistoryTarget.srvCpuHandle;
            ssrVarianceSrv = m_ssrVarianceHistoryTarget.srvCpuHandle;
            const_cast<ScreenSpaceEffects*>(this)->m_lastSsrTemporalSrv = ssrDenoiseInputSrv;
            const_cast<ScreenSpaceEffects*>(this)->m_lastSsrVarianceSrv = ssrVarianceSrv;
        }

        SceneRenderTrace::Scope ssrAtrousScope("ssr svgf atrous");
        m_ssrSvgfAtrousShader->Use(false);
        m_ssrSvgfAtrousShader->SetInt("uColor", 0);
        m_ssrSvgfAtrousShader->SetInt("uVariance", 1);
        m_ssrSvgfAtrousShader->SetInt("uDepthMap", 2);
        m_ssrSvgfAtrousShader->SetInt("uNormalMap", 3);
        m_ssrSvgfAtrousShader->SetInt("uMaterial0Map", 4);
        m_ssrSvgfAtrousShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssrSvgfAtrousShader->SetVec2("uTexelSize", traceTexelSize);
        m_ssrSvgfAtrousShader->SetFloat("uDepthThreshold", m_ssrSpatialDepthThreshold);
        m_ssrSvgfAtrousShader->SetFloat("uBlurSpread", m_ssrSpatialBlurSpread);
        m_ssrSvgfAtrousShader->SetFloat("uRoughnessSpreadMin", m_ssrRoughnessSpreadMin);
        m_ssrSvgfAtrousShader->SetFloat("uRoughnessSpreadMax", m_ssrRoughnessSpreadMax);
        m_ssrSvgfAtrousShader->SetFloat("uNormalPower", 16.0f);
        m_ssrSvgfAtrousShader->SetFloat("uPhiEpsilon", m_ssrSvgfPhiEpsilon);
        m_ssrSvgfAtrousShader->SetFloat("uFilterStrength", m_ssrSvgfFilterStrength);

        static constexpr float kSsrSvgfAtrousStepScales[] = {1.0f, 2.0f, 4.0f, 8.0f};
        std::uintptr_t atrousInputSrv = ssrDenoiseInputSrv;
        bool writeToBlurTarget = true;
        bool recordedSpatialDebug = false;
        for (const float stepScale : kSsrSvgfAtrousStepScales)
        {
            InternalTarget& outputTarget = writeToBlurTarget
                ? const_cast<InternalTarget&>(m_ssrSpatialBlurTarget)
                : const_cast<InternalTarget&>(m_ssrSpatialTarget);
            m_ssrSvgfAtrousShader->SetFloat("uStepScale", stepScale);
            m_ssrSvgfAtrousShader->BindTextureSlot(0, atrousInputSrv);
            m_ssrSvgfAtrousShader->BindTextureSlot(1, ssrVarianceSrv);
            m_ssrSvgfAtrousShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_ssrSvgfAtrousShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
            m_ssrSvgfAtrousShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
            DrawFullscreenToTarget(
                *m_ssrSvgfAtrousShader,
                outputTarget,
                m_ssrTraceTarget.width,
                m_ssrTraceTarget.height,
                radianceClear);
            atrousInputSrv = outputTarget.srvCpuHandle;
            if (!recordedSpatialDebug)
            {
                const_cast<ScreenSpaceEffects*>(this)->m_lastSsrSpatialSrv = atrousInputSrv;
                recordedSpatialDebug = true;
            }
            writeToBlurTarget = !writeToBlurTarget;
        }

        ssrDenoiseInputSrv = atrousInputSrv;
        ssrAtrousScope.Success();
        ssrSvgfScope.Success();
    }
    else
    {
        const_cast<ScreenSpaceEffects*>(this)->m_ssrTemporalRanLastFrame = false;
    }

    const_cast<ScreenSpaceEffects*>(this)->m_lastSsrDenoiseSrv = ssrDenoiseInputSrv;

    const bool runSsrUpscale =
        runSsrDenoise &&
        m_ssrTraceResolutionScale < 1.0f &&
        m_ssrResolvedTarget.resource != nullptr;

    if (runSsrUpscale)
    {
        SceneRenderTrace::Scope ssrUpscaleScope("ssr upscale");
        m_ssrUpscaleShader->Use(false);
        m_ssrUpscaleShader->SetInt("uTraceMap", 0);
        m_ssrUpscaleShader->SetInt("uDepthMap", 1);
        m_ssrUpscaleShader->SetInt("uNormalMap", 2);
        m_ssrUpscaleShader->SetInt("uMaterial0Map", 3);
        m_ssrUpscaleShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssrUpscaleShader->SetVec2("uTexelSize", texelSize);
        m_ssrUpscaleShader->SetFloat("uDepthThreshold", m_ssrSpatialDepthThreshold);
        m_ssrUpscaleShader->SetFloat("uNormalPower", 8.0f);
        m_ssrUpscaleShader->SetFloat("uRoughnessSpreadMin", 0.75f);
        m_ssrUpscaleShader->SetFloat("uRoughnessSpreadMax", 1.75f);
        m_ssrUpscaleShader->BindTextureSlot(0, ssrDenoiseInputSrv);
        m_ssrUpscaleShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_ssrUpscaleShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        m_ssrUpscaleShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        DrawFullscreenToTarget(
            *m_ssrUpscaleShader,
            const_cast<InternalTarget&>(m_ssrResolvedTarget),
            m_width,
            m_height,
            radianceClear);
        const_cast<ScreenSpaceEffects*>(this)->m_lastSsrResolvedSrv = m_ssrResolvedTarget.srvCpuHandle;
        ssrUpscaleScope.Success();
    }
    else if (runSsrDenoise && ssrDenoiseInputSrv != 0)
    {
        const_cast<ScreenSpaceEffects*>(this)->m_lastSsrResolvedSrv = ssrDenoiseInputSrv;
    }

    else if (runSsrTrace && m_ssrTraceTarget.srvCpuHandle != 0)
    {
        const_cast<ScreenSpaceEffects*>(this)->m_lastSsrResolvedSrv = m_ssrTraceTarget.srvCpuHandle;
    }

    // D6: RT specular composite and SSR spec composite are mutually exclusive — only one
    // may adjust spec IBL in RT1 per frame (dxr-groundwork.md pipeline rule). The composite now
    // runs whenever its feature is ENABLED (not only when a fresh trace exists): the PBR raster
    // omits spec IBL from RT1 in that case (uOmitSpecularIbl), so a composite MUST add it back —
    // falling back to pure IBL (uHasRtTrace/uHasSsrTrace = 0) when there is no fresh trace.
    const bool rtHasFreshTrace = m_dxrReflectionSrv != 0;
    const bool rtCompositeWanted = m_dxrReflectionCompositeEnabled;
    const bool rtCompositeDebugOnly =
        !rtCompositeWanted && m_debugMode == RenderDebugMode::RtSpecReplacement
        && m_dxrReflectionSrv != 0;

    const bool ssrHasFreshTrace = m_lastSsrResolvedSrv != 0;
    const bool runSsrIndirect =
        !pbrDebugActive &&
        !rtCompositeWanted &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_ssrIndirectTarget.resource != nullptr &&
        environmentMap.GetIBL().IsReady() &&
        (m_ssrEnabled || IsSsrCompositeDebugMode(m_debugMode));

    std::uintptr_t indirectCompositeSrv = m_sceneFramebuffer->GetColorSrvCpuHandle(1);
    if (runSsrIndirect)
    {
        SceneRenderTrace::Scope ssrIndirectScope("ssr indirect composite");
        const float indirectClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 invView = glm::inverse(viewMatrix);
        const IBL& ibl = environmentMap.GetIBL();

        m_ssrIndirectShader->Use(false);
        m_ssrIndirectShader->SetInt("uIndirectMap", 0);
        m_ssrIndirectShader->SetInt("uSsrMap", 1);
        m_ssrIndirectShader->SetInt("uDepthMap", 2);
        m_ssrIndirectShader->SetInt("uNormalMap", 3);
        m_ssrIndirectShader->SetInt("uMaterial0Map", 4);
        m_ssrIndirectShader->SetInt("uMaterial1Map", 5);
        m_ssrIndirectShader->SetInt("uPrefilterMap", 6);
        m_ssrIndirectShader->SetInt("uBrdfLut", 7);
        m_ssrIndirectShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssrIndirectShader->SetMat4("uInvView", invView);
        m_ssrIndirectShader->SetFloat("uEnvironmentIntensity", ibl.GetEnvironmentIntensity());
        m_ssrIndirectShader->SetFloat("uMaxReflectionLod", ibl.GetMaxReflectionLod());
        m_ssrIndirectShader->SetFloat("uSsrStrength", m_ssrStrength);
        m_ssrIndirectShader->SetFloat("uReceiverFadeDistance", m_ssrMaxTraceDistance);
        m_ssrIndirectShader->SetInt(
            "uDebugSpecReplacement",
            (!m_ssrEnabled && m_debugMode == RenderDebugMode::SsrSpecReplacement) ? 1 : 0);
        m_ssrIndirectShader->SetInt("uHasSsrTrace", ssrHasFreshTrace ? 1 : 0);
        m_ssrIndirectShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        // Fall back to a valid texture (RT1) when there is no fresh SSR resolve; uHasSsrTrace=0
        // forces the weight to 0 so the sample is ignored and pure IBL is added.
        m_ssrIndirectShader->BindTextureSlot(
            1, ssrHasFreshTrace ? m_lastSsrResolvedSrv : m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_ssrIndirectShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_ssrIndirectShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        m_ssrIndirectShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_ssrIndirectShader->BindTextureSlot(5, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        m_ssrIndirectShader->BindTextureSlot(6, ibl.GetPrefilterMapSrvCpuHandle());
        m_ssrIndirectShader->BindTextureSlot(7, ibl.GetBrdfLutSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_ssrIndirectShader,
            const_cast<InternalTarget&>(m_ssrIndirectTarget),
            m_width,
            m_height,
            indirectClear);
        if (m_ssrEnabled)
        {
            indirectCompositeSrv = m_ssrIndirectTarget.srvCpuHandle;
        }
        ssrIndirectScope.Success();
    }

    // D6 — RT specular composite: replace the recomputed spec IBL in RT1 with the denoised
    // RT reflection wherever the raw trace has a valid (non-miss) hit distance. Standalone
    // RT pass: no SSR buffer reads (dxr-groundwork.md Phase D6).
    const bool runRtIndirect =
        (rtCompositeWanted || rtCompositeDebugOnly) &&
        !pbrDebugActive &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_rtIndirectTarget.resource != nullptr &&
        environmentMap.GetIBL().IsReady();

    if (runRtIndirect)
    {
        SceneRenderTrace::Scope rtIndirectScope("dxr indirect composite");
        const float indirectClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 invView = glm::inverse(viewMatrix);
        const IBL& ibl = environmentMap.GetIBL();
        // Fall back to a valid texture (RT1) when there is no fresh trace; uHasRtTrace=0 forces
        // the weight to 0 so the RT sample is ignored and pure IBL is added.
        const std::uintptr_t rt1Srv = m_sceneFramebuffer->GetColorSrvCpuHandle(1);
        const std::uintptr_t denoisedSrv = rtHasFreshTrace
            ? (m_dxrReflectionDenoisedSrv != 0 ? m_dxrReflectionDenoisedSrv : m_dxrReflectionSrv)
            : rt1Srv;
        const std::uintptr_t rawSrv = rtHasFreshTrace ? m_dxrReflectionSrv : rt1Srv;

        m_dxrIndirectShader->Use(false);
        m_dxrIndirectShader->SetInt("uIndirectMap", 0);
        m_dxrIndirectShader->SetInt("uRtDenoisedMap", 1);
        m_dxrIndirectShader->SetInt("uRtRawMap", 2);
        m_dxrIndirectShader->SetInt("uDepthMap", 3);
        m_dxrIndirectShader->SetInt("uNormalMap", 4);
        m_dxrIndirectShader->SetInt("uMaterial0Map", 5);
        m_dxrIndirectShader->SetInt("uMaterial1Map", 6);
        m_dxrIndirectShader->SetInt("uPrefilterMap", 7);
        m_dxrIndirectShader->SetInt("uBrdfLut", 8);
        m_dxrIndirectShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_dxrIndirectShader->SetMat4("uInvView", invView);
        m_dxrIndirectShader->SetFloat("uEnvironmentIntensity", ibl.GetEnvironmentIntensity());
        m_dxrIndirectShader->SetFloat("uMaxReflectionLod", ibl.GetMaxReflectionLod());
        m_dxrIndirectShader->SetFloat("uStrength", 1.0f);
        m_dxrIndirectShader->SetFloat(
            "uMaxTraceDistance",
            m_dxrReflectionMaxTraceDistance > 0.0f ? m_dxrReflectionMaxTraceDistance : 100.0f);
        m_dxrIndirectShader->SetVec2(
            "uRtUvScale",
            glm::vec2(m_dxrReflectionUvScaleX, m_dxrReflectionUvScaleY));
        m_dxrIndirectShader->SetInt(
            "uDebugSpecReplacement",
            rtCompositeDebugOnly ? 1 : 0);
        m_dxrIndirectShader->SetInt("uHasRtTrace", rtHasFreshTrace ? 1 : 0);
        m_dxrIndirectShader->SetFloat("uRoughnessCutoff", m_dxrReflectionRoughnessCutoff);
        m_dxrIndirectShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_dxrIndirectShader->BindTextureSlot(1, denoisedSrv);
        m_dxrIndirectShader->BindTextureSlot(2, rawSrv);
        m_dxrIndirectShader->BindTextureSlot(3, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_dxrIndirectShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        m_dxrIndirectShader->BindTextureSlot(5, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_dxrIndirectShader->BindTextureSlot(6, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        m_dxrIndirectShader->BindTextureSlot(7, environmentMap.GetIBL().GetPrefilterMapSrvCpuHandle());
        m_dxrIndirectShader->BindTextureSlot(8, environmentMap.GetIBL().GetBrdfLutSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_dxrIndirectShader,
            const_cast<InternalTarget&>(m_rtIndirectTarget),
            m_width,
            m_height,
            indirectClear);
        if (rtCompositeWanted)
        {
            indirectCompositeSrv = m_rtIndirectTarget.srvCpuHandle;
        }
        rtIndirectScope.Success();
    }

    // D9 — RT diffuse GI inject: add the denoised one-bounce diffuse radiance into the current
    // indirect chain (post spec-replacement, so spec + GI stack). Reads whatever indirectCompositeSrv
    // currently is (NOT raw RT1). Mutually exclusive with SSGI inject (gated in the composite below).
    const std::uintptr_t giInjectSrv =
        m_dxrGiDenoisedSrv != 0 ? m_dxrGiDenoisedSrv : m_dxrGiRawSrv;
    // Run whenever GI is ENABLED (not only on a fresh trace): the raster omits the SH diffuse
    // ambient in that case (uOmitDiffuseIbl), so the inject MUST run to replace it — falling back
    // to a recomputed SH ambient (uHasGiTrace=0) when there is no fresh trace.
    const bool giHasFreshTrace = giInjectSrv != 0;
    const bool runRtGiInject =
        m_dxrGiCompositeEnabled &&
        !pbrDebugActive &&
        m_sceneFramebuffer->HasSplitLighting() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_rtGiInjectTarget.resource != nullptr &&
        environmentMap.GetIBL().IsReady();

    if (runRtGiInject)
    {
        SceneRenderTrace::Scope rtGiInjectScope("dxr gi inject");
        const float injectClear[] = {0.0f, 0.0f, 0.0f, 1.0f};
        const IBL& giIbl = environmentMap.GetIBL();

        m_dxrGiInjectShader->Use(false);
        m_dxrGiInjectShader->SetInt("uIndirectMap", 0);
        m_dxrGiInjectShader->SetInt("uGiDenoisedMap", 1);
        m_dxrGiInjectShader->SetInt("uDepthMap", 2);
        m_dxrGiInjectShader->SetInt("uMaterial0Map", 3);
        m_dxrGiInjectShader->SetInt("uMaterial1Map", 4);
        m_dxrGiInjectShader->SetInt("uNormalMap", 5);
        m_dxrGiInjectShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_dxrGiInjectShader->SetMat4("uInvView", glm::inverse(camera.GetViewMatrix()));
        m_dxrGiInjectShader->SetVec2(
            "uGiUvScale", glm::vec2(m_dxrGiUvScaleX, m_dxrGiUvScaleY));
        m_dxrGiInjectShader->SetFloat("uStrength", m_dxrGiStrength);
        m_dxrGiInjectShader->SetInt("uDebugGiInject", 0);
        m_dxrGiInjectShader->SetInt("uHasGiTrace", giHasFreshTrace ? 1 : 0);
        m_dxrGiInjectShader->SetFloat("uEnvironmentIntensity", giIbl.GetEnvironmentIntensity());
        m_dxrGiInjectShader->SetVec4Array(
            "uIrradianceSh",
            giIbl.GetIrradianceSh9().coefficients.data(),
            static_cast<int>(giIbl.GetIrradianceSh9().coefficients.size()));
        m_dxrGiInjectShader->BindTextureSlot(0, indirectCompositeSrv);
        // Fall back to a valid texture (RT1) when there is no fresh GI trace; uHasGiTrace=0 makes
        // the shader recompute the SH ambient instead of sampling this.
        m_dxrGiInjectShader->BindTextureSlot(
            1, giHasFreshTrace ? giInjectSrv : m_sceneFramebuffer->GetColorSrvCpuHandle(1));
        m_dxrGiInjectShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_dxrGiInjectShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_dxrGiInjectShader->BindTextureSlot(4, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        m_dxrGiInjectShader->BindTextureSlot(5, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        DrawFullscreenToTarget(
            *m_dxrGiInjectShader,
            const_cast<InternalTarget&>(m_rtGiInjectTarget),
            m_width,
            m_height,
            injectClear);
        indirectCompositeSrv = m_rtGiInjectTarget.srvCpuHandle;
        rtGiInjectScope.Success();
    }

    std::uintptr_t temporalInputSrv = m_radianceTarget.srvCpuHandle;

    const bool runSsgiTrace =
        runRadianceAssembly &&
        m_ssgiEnabled &&
        m_sceneFramebuffer->HasGeometryNormals() &&
        m_radianceTraceInputTarget.resource != nullptr;

    if (runSsgiTrace)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope traceScope("ssgi trace");
        m_ssgiTraceShader->Use(false);
        m_ssgiTraceShader->SetInt("uDepthMap", 0);
        m_ssgiTraceShader->SetInt("uNormalMap", 1);
        m_ssgiTraceShader->SetInt("uMaterial0Map", 2);
        m_ssgiTraceShader->SetInt("uMaterial1Map", 3);
        m_ssgiTraceShader->SetInt("uRadianceMap", 4);
        m_ssgiTraceShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssgiTraceShader->SetMat4("uProjection", projectionMatrix);
        m_ssgiTraceShader->SetMat4("uView", camera.GetViewMatrix());
        m_ssgiTraceShader->SetFloat("uMaxTraceDistance", m_ssgiMaxTraceDistance);
        m_ssgiTraceShader->SetInt("uStepCount", m_ssgiStepCount);
        m_ssgiTraceShader->SetFloat("uThickness", m_ssgiThickness);
        m_ssgiTraceShader->SetFloat("uFrameIndex", static_cast<float>(m_giFrameIndex));
        m_ssgiTraceShader->SetFloat("uEdgeFadeScale", 20.0f);
        m_ssgiTraceShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_ssgiTraceShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
        m_ssgiTraceShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
        m_ssgiTraceShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        m_ssgiTraceShader->BindTextureSlot(4, m_radianceTarget.srvCpuHandle);
        DrawFullscreenToTarget(
            *m_ssgiTraceShader,
            const_cast<InternalTarget&>(m_radianceTraceInputTarget),
            m_width,
            m_height,
            radianceClear);
        temporalInputSrv = m_radianceTraceInputTarget.srvCpuHandle;
        traceScope.Success();
        ssgiSection.Success();
    }

    const bool runSsgiDenoise =
        runRadianceAssembly &&
        m_ssgiDenoiseEnabled &&
        m_sceneFramebuffer->HasGeometryNormals() &&
        m_sceneFramebuffer->HasMaterialGbuffer() &&
        m_radianceTraceInputTarget.resource != nullptr &&
        m_radianceSpatialTarget.resource != nullptr &&
        (runSsgiTrace || m_ssgiNoiseInjectionEnabled);

    if (runRadianceAssembly && !runSsgiTrace && m_ssgiNoiseInjectionEnabled)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope noiseScope("ssgi noise inject");
        const float noiseStrength =
            m_ssgiNoiseInjectionEnabled ? m_ssgiNoiseStrength : 0.0f;
        m_ssgiNoiseInjectShader->Use(false);
        m_ssgiNoiseInjectShader->SetInt("uRadianceMap", 0);
        m_ssgiNoiseInjectShader->SetInt("uDepthMap", 1);
        m_ssgiNoiseInjectShader->SetFloat("uNoiseStrength", noiseStrength);
        m_ssgiNoiseInjectShader->SetFloat(
            "uFrameIndex",
            static_cast<float>(m_giFrameIndex));
        m_ssgiNoiseInjectShader->BindTextureSlot(0, m_radianceTarget.srvCpuHandle);
        m_ssgiNoiseInjectShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_ssgiNoiseInjectShader,
            const_cast<InternalTarget&>(m_radianceTraceInputTarget),
            m_width,
            m_height,
            radianceClear);
        temporalInputSrv = m_radianceTraceInputTarget.srvCpuHandle;
        noiseScope.Success();
        ssgiSection.Success();
    }

    if (runSsgiDenoise)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope denoiseScope("ssgi denoise spatial");
        m_ssgiDenoiseSpatialShader->Use(false);
        m_ssgiDenoiseSpatialShader->SetInt("uInput", 0);
        m_ssgiDenoiseSpatialShader->SetInt("uDepthMap", 1);
        m_ssgiDenoiseSpatialShader->SetInt("uNormalMap", 2);
        m_ssgiDenoiseSpatialShader->SetInt("uMaterial0Map", 3);
        m_ssgiDenoiseSpatialShader->SetMat4("uInvProjection", inverseProjectionMatrix);
        m_ssgiDenoiseSpatialShader->SetVec2("uTexelSize", texelSize);
        m_ssgiDenoiseSpatialShader->SetFloat("uDepthThreshold", m_ssgiSpatialDepthThreshold);
        m_ssgiDenoiseSpatialShader->SetFloat("uBlurSpread", m_ssgiSpatialBlurSpread);
        m_ssgiDenoiseSpatialShader->SetFloat("uRoughnessSpreadMin", m_ssgiRoughnessSpreadMin);
        m_ssgiDenoiseSpatialShader->SetFloat("uRoughnessSpreadMax", m_ssgiRoughnessSpreadMax);
        m_ssgiDenoiseSpatialShader->SetFloat("uNormalPower", 4.0f);
        static constexpr float kSsgiAtrousStepScales[] = {1.0f, 2.0f, 4.0f, 2.0f};
        std::uintptr_t atrousInputSrv = temporalInputSrv;
        bool writeToBlurTarget = true;
        for (const float stepScale : kSsgiAtrousStepScales)
        {
            InternalTarget& outputTarget = writeToBlurTarget
                ? const_cast<InternalTarget&>(m_radianceSpatialBlurTarget)
                : const_cast<InternalTarget&>(m_radianceSpatialTarget);
            m_ssgiDenoiseSpatialShader->SetFloat("uStepScale", stepScale);
            m_ssgiDenoiseSpatialShader->BindTextureSlot(0, atrousInputSrv);
            m_ssgiDenoiseSpatialShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_ssgiDenoiseSpatialShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(2));
            m_ssgiDenoiseSpatialShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
            DrawFullscreenToTarget(
                *m_ssgiDenoiseSpatialShader,
                outputTarget,
                m_width,
                m_height,
                radianceClear);
            atrousInputSrv = outputTarget.srvCpuHandle;
            writeToBlurTarget = !writeToBlurTarget;
        }

        temporalInputSrv = atrousInputSrv;
        denoiseScope.Success();
        ssgiSection.Success();
    }

    const bool runGiTemporal =
        runRadianceAssembly &&
        m_radianceHistoryTarget.resource != nullptr &&
        m_radianceTemporalTarget.resource != nullptr &&
        m_radianceHistoryDepthTarget.resource != nullptr &&
        (runSsgiDenoise ||
         (!m_ssgiEnabled && !m_ssgiNoiseInjectionEnabled && !m_ssgiDenoiseEnabled));

    if (runGiTemporal)
    {
        SceneRenderTrace::Section ssgiSection("ssgi");
        SceneRenderTrace::Scope temporalScope("gi temporal reproject");
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
        const glm::mat4 invViewProjCurr = glm::inverse(unjitteredProjection * viewMatrix);
        const glm::mat4 prevViewProj = m_motionVectorFrameState.historyValid
            ? m_motionVectorFrameState.prevViewProjection
            : unjitteredProjection * viewMatrix;
        const float temporalClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
        commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        m_sceneFramebuffer->RestoreDepthShaderResource();

        m_temporalReprojectShader->Use(false);
        m_temporalReprojectShader->SetInt("uCurrentRadiance", 0);
        m_temporalReprojectShader->SetInt("uHistoryRadiance", 1);
        m_temporalReprojectShader->SetInt("uDepth", 2);
        m_temporalReprojectShader->SetInt("uHistoryDepth", 3);
        m_temporalReprojectShader->SetMat4("uInvViewProj", invViewProjCurr);
        m_temporalReprojectShader->SetMat4("uPrevViewProj", prevViewProj);
        m_temporalReprojectShader->SetFloat("uBlendFactor", m_giTemporalBlendFactor);
        m_temporalReprojectShader->SetFloat(
            "uHistoryValid",
            m_radianceHistoryValid && m_motionVectorFrameState.historyValid ? 1.0f : 0.0f);
        m_temporalReprojectShader->SetFloat("uTexelSizeX", texelSize.x);
        m_temporalReprojectShader->SetFloat("uTexelSizeY", texelSize.y);
        m_temporalReprojectShader->SetFloat("uDepthRejectThreshold", m_giDepthThreshold);
        m_temporalReprojectShader->BindTextureSlot(0, temporalInputSrv);
        m_temporalReprojectShader->BindTextureSlot(1, m_radianceHistoryTarget.srvCpuHandle);
        m_temporalReprojectShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_temporalReprojectShader->BindTextureSlot(3, m_radianceHistoryDepthTarget.srvCpuHandle);
        DrawFullscreenToTarget(
            *m_temporalReprojectShader,
            const_cast<InternalTarget&>(m_radianceTemporalTarget),
            m_width,
            m_height,
            temporalClear);

        // Stored for Phase 5+ proper depth disocclusion (point-sampled prev-view Z compare).
        m_giDepthHistoryShader->Use(false);
        m_giDepthHistoryShader->SetInt("uDepth", 0);
        m_giDepthHistoryShader->BindTextureSlot(0, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        DrawFullscreenToTarget(
            *m_giDepthHistoryShader,
            const_cast<InternalTarget&>(m_radianceHistoryDepthTarget),
            m_width,
            m_height,
            temporalClear);

        std::swap(
            const_cast<InternalTarget&>(m_radianceHistoryTarget),
            const_cast<InternalTarget&>(m_radianceTemporalTarget));
        m_radianceHistoryValid = true;
        ++m_giFrameIndex;
        temporalScope.Success();
        ssgiSection.Success();
    }

    m_lastSsgiInjectSrv = 0;
    if (runSsgiTrace)
    {
        if (runGiTemporal && m_radianceHistoryTarget.srvCpuHandle != 0)
        {
            m_lastSsgiInjectSrv = m_radianceHistoryTarget.srvCpuHandle;
        }
        else if (runSsgiDenoise && m_radianceSpatialTarget.srvCpuHandle != 0)
        {
            m_lastSsgiInjectSrv = m_radianceSpatialTarget.srvCpuHandle;
        }
        else if (m_radianceTraceInputTarget.srvCpuHandle != 0)
        {
            m_lastSsgiInjectSrv = m_radianceTraceInputTarget.srvCpuHandle;
        }
    }

    std::uintptr_t hdrColorSrv = m_sceneFramebuffer->GetColorSrvCpuHandle(0);
    const char* hdrColorSource = "scene_direct";
    bool compositeRan = false;
    const bool compositeUsesSsao = runAo;
    const char* ssaoDebugViewSource = "none";

    if (m_sceneFramebuffer->HasSplitLighting() && !pbrDebugActive)
    {
        const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_compositeShader->SetInt("uDirectLighting", 0);
        m_compositeShader->SetInt("uIndirectLighting", 1);
        m_compositeShader->SetInt("uDepthMap", 2);
        m_compositeShader->SetInt("uSsaoMap", 3);
        m_compositeShader->SetInt("uUseSplitLighting", 1);
        m_compositeShader->SetInt("uUseSsao", runAo ? 1 : 0);
        m_compositeShader->SetInt("uUseShadowFactor", useShadowFactorComposite ? 1 : 0);
        m_compositeShader->SetInt("uShadowFactorMap", 4);
        m_compositeShader->SetFloat("uSsaoPower", runGtao ? m_gtaoPower : m_ssaoPower);
        m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
        m_compositeShader->SetInt(
            "uDebugOcclusionOnly",
            m_debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);
        // D9: RT GI and SSGI inject are mutually exclusive — RT GI (already added into the
        // indirect chain above) takes priority, so drop SSGI's composite contribution.
        const bool useSsgiInject =
            runSsgiTrace && m_lastSsgiInjectSrv != 0 && !runRtGiInject;
        m_compositeShader->SetInt("uUseSsgi", useSsgiInject ? 1 : 0);
        m_compositeShader->SetFloat("uSsgiStrength", m_ssgiStrength);
        m_compositeShader->SetInt("uSsgiMap", 6);
        // D8: replace the CSM sun shadow factor with the SIGMA-denoised RT mask when enabled and
        // fresh this frame. t7 must always bind a valid texture (fall back to shadowFactorSrv).
        const bool useRtShadow = m_dxrShadowCompositeEnabled && m_dxrShadowDenoisedSrv != 0;
        m_compositeShader->SetInt("uUseRtShadow", useRtShadow ? 1 : 0);
        m_compositeShader->SetInt("uRtShadowMap", 7);
        m_compositeShader->SetVec2(
            "uRtShadowUvScale", glm::vec2(m_dxrShadowUvScaleX, m_dxrShadowUvScaleY));
        SetCompositeBackgroundUniforms(*m_compositeShader, camera, environmentMap);
        m_compositeShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        m_compositeShader->BindTextureSlot(1, indirectCompositeSrv);
        m_compositeShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_compositeShader->BindTextureSlot(3, aoCompositeSrv);
        m_compositeShader->BindTextureSlot(4, shadowFactorSrv);
        m_compositeShader->BindTextureSlot(
            6,
            useSsgiInject ? m_lastSsgiInjectSrv : m_radianceTarget.srvCpuHandle);
        m_compositeShader->BindTextureSlot(7, useRtShadow ? m_dxrShadowDenoisedSrv : shadowFactorSrv);
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
    else if (runAo)
    {
        const float compositeClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_compositeShader->SetInt("uDirectLighting", 0);
        m_compositeShader->SetInt("uIndirectLighting", 0);
        m_compositeShader->SetInt("uDepthMap", 2);
        m_compositeShader->SetInt("uSsaoMap", 3);
        m_compositeShader->SetInt("uUseSplitLighting", 0);
        m_compositeShader->SetInt("uUseSsao", 1);
        m_compositeShader->SetFloat("uSsaoPower", runGtao ? m_gtaoPower : m_ssaoPower);
        m_compositeShader->SetFloat("uAoStrength", m_aoStrength);
        m_compositeShader->SetInt(
            "uDebugOcclusionOnly",
            m_debugMode == RenderDebugMode::CompositeOcclusion ? 1 : 0);
        SetCompositeBackgroundUniforms(*m_compositeShader, camera, environmentMap);
        m_compositeShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(0));
        m_compositeShader->BindTextureSlot(3, aoCompositeSrv);
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

    const bool useTaa = m_antiAliasingMode == AntiAliasingMode::TAA;
    if (useTaa && m_taaResolveTarget.srvCpuHandle != 0 && m_sceneFramebuffer->HasVelocity())
    {
        SceneRenderTrace::Section taaSection("aa-taa");
        SceneRenderTrace::Scope taaScope("taa resolve");
        const glm::mat4 viewMatrix = camera.GetViewMatrix();
        const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
        const glm::mat4 invViewProjection = glm::inverse(unjitteredProjection * viewMatrix);
        const glm::mat4 prevViewProjection = m_motionVectorFrameState.historyValid
            ? m_motionVectorFrameState.prevViewProjection
            : unjitteredProjection * viewMatrix;
        const float hdrClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_taaShader->Use(false, false);
        m_taaShader->SetMat4("uInvViewProj", invViewProjection);
        m_taaShader->SetMat4("uPrevViewProj", prevViewProjection);
        m_taaShader->SetFloat("uBlendFactor", m_taaBlendFactor);
        m_taaShader->SetFloat("uHistoryValid", m_taaHistoryValid ? 1.0f : 0.0f);
        m_taaShader->SetFloat("uTexelSizeX", texelSize.x);
        m_taaShader->SetFloat("uTexelSizeY", texelSize.y);
        m_taaShader->BindTextureSlot(0, hdrColorSrv);
        m_taaShader->BindTextureSlot(1, m_taaHistoryTarget.srvCpuHandle);
        m_taaShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
        m_taaShader->BindTextureSlot(3, m_sceneFramebuffer->GetColorSrvCpuHandle(4));
        DrawFullscreenToTarget(
            *m_taaShader,
            const_cast<InternalTarget&>(m_taaResolveTarget),
            m_width,
            m_height,
            hdrClear,
            false);

        hdrColorSrv = m_taaResolveTarget.srvCpuHandle;
        hdrColorSource = "hdr_taa";

        std::swap(
            const_cast<InternalTarget&>(m_taaHistoryTarget),
            const_cast<InternalTarget&>(m_taaResolveTarget));
        m_taaHistoryValid = true;
        taaScope.Success();
        taaSection.Success();
    }

    std::uintptr_t bloomSrv = 0;
    if (m_bloomEnabled && !IsPbrMaterialDebugMode(m_debugMode))
    {
        SceneRenderTrace::Section bloomSection("bloom");
        SceneRenderTrace::Scope bloomExtractScope("bloom extract");
        const int bloomWidth = std::max(1, m_width / 2);
        const int bloomHeight = std::max(1, m_height / 2);
        const glm::vec2 bloomTexelSize(
            1.0f / static_cast<float>(bloomWidth),
            1.0f / static_cast<float>(bloomHeight));
        const float bloomClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

        m_bloomExtractShader->SetInt("uHdrColor", 0);
        m_bloomExtractShader->SetFloat("uThreshold", m_bloomThreshold);
        m_bloomExtractShader->SetFloat("uSoftKnee", m_bloomSoftKnee);
        m_bloomExtractShader->SetFloat("uExposure", m_exposure);
        m_bloomExtractShader->SetFloat("uFullTexelSizeX", texelSize.x);
        m_bloomExtractShader->SetFloat("uFullTexelSizeY", texelSize.y);
        const bool useMaterialGbuffer = m_sceneFramebuffer->HasMaterialGbuffer();
        m_bloomExtractShader->SetFloat("uUseMaterialGbuffer", useMaterialGbuffer ? 1.0f : 0.0f);
        m_bloomExtractShader->BindTextureSlot(0, hdrColorSrv);
        if (useMaterialGbuffer)
        {
            m_bloomExtractShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
            m_bloomExtractShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
        }
        DrawFullscreenToTarget(
            *m_bloomExtractShader,
            const_cast<InternalTarget&>(m_bloomExtractTarget),
            bloomWidth,
            bloomHeight,
            bloomClear);
        bloomExtractScope.Success();

        SceneRenderTrace::Scope bloomBlurScope("bloom blur");
        const auto drawBloomBlurPass =
            [&](InternalTarget& target, const std::uintptr_t inputSrv, const float dirX, const float dirY)
        {
            m_bloomBlurShader->Use(false, false);
            m_bloomBlurShader->SetFloat("uDirectionX", dirX);
            m_bloomBlurShader->SetFloat("uDirectionY", dirY);
            m_bloomBlurShader->SetFloat("uBlurRadius", m_bloomBlurRadius);
            m_bloomBlurShader->BindTextureSlot(0, inputSrv);
            DrawFullscreenToTarget(
                *m_bloomBlurShader,
                const_cast<InternalTarget&>(target),
                bloomWidth,
                bloomHeight,
                bloomClear);
        };

        drawBloomBlurPass(
            const_cast<InternalTarget&>(m_bloomBlurTarget),
            m_bloomExtractTarget.srvCpuHandle,
            bloomTexelSize.x,
            0.0f);
        drawBloomBlurPass(
            const_cast<InternalTarget&>(m_bloomBlur2Target),
            m_bloomBlurTarget.srvCpuHandle,
            0.0f,
            bloomTexelSize.y);
        drawBloomBlurPass(
            const_cast<InternalTarget&>(m_bloomBlurTarget),
            m_bloomBlur2Target.srvCpuHandle,
            bloomTexelSize.x,
            0.0f);
        drawBloomBlurPass(
            const_cast<InternalTarget&>(m_bloomBlur2Target),
            m_bloomBlurTarget.srvCpuHandle,
            0.0f,
            bloomTexelSize.y);
        bloomBlurScope.Success();

        if (m_sceneFramebuffer->HasVelocity() && m_bloomTemporalTarget.srvCpuHandle != 0)
        {
            SceneRenderTrace::Scope bloomTemporalScope("bloom temporal");
            const float bloomWarmupFactor = m_bloomHistoryValid
                ? std::min(1.0f, static_cast<float>(m_bloomTemporalWarmupFrames) / 4.0f)
                : 0.0f;
            m_bloomTemporalShader->Use(false, false);
            m_bloomTemporalShader->SetFloat("uBlendFactor", m_bloomTemporalBlendFactor);
            m_bloomTemporalShader->SetFloat("uSameUvBlendFactor", m_bloomSameUvBlendFactor);
            m_bloomTemporalShader->SetFloat("uHistoryValid", m_bloomHistoryValid ? 1.0f : 0.0f);
            m_bloomTemporalShader->SetFloat("uDepthThreshold", m_bloomDepthThreshold);
            m_bloomTemporalShader->SetFloat("uTexelSizeX", bloomTexelSize.x);
            m_bloomTemporalShader->SetFloat("uTexelSizeY", bloomTexelSize.y);
            m_bloomTemporalShader->SetFloat("uWarmupFactor", bloomWarmupFactor);
            m_bloomTemporalShader->BindTextureSlot(0, m_bloomBlur2Target.srvCpuHandle);
            m_bloomTemporalShader->BindTextureSlot(1, m_bloomHistoryTarget.srvCpuHandle);
            m_bloomTemporalShader->BindTextureSlot(2, m_sceneFramebuffer->GetColorSrvCpuHandle(4));
            m_bloomTemporalShader->BindTextureSlot(3, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            DrawFullscreenToTarget(
                *m_bloomTemporalShader,
                const_cast<InternalTarget&>(m_bloomTemporalTarget),
                bloomWidth,
                bloomHeight,
                bloomClear);

            bloomSrv = m_bloomTemporalTarget.srvCpuHandle;

            std::swap(
                const_cast<InternalTarget&>(m_bloomHistoryTarget),
                const_cast<InternalTarget&>(m_bloomTemporalTarget));
            m_bloomHistoryValid = true;
            ++m_bloomTemporalWarmupFrames;
            bloomTemporalScope.Success();
        }
        else
        {
            bloomSrv = m_bloomBlur2Target.srvCpuHandle;
        }

        bloomSection.Success();
        // Remember this frame's final bloom for next frame's ssr_scene_color (halo term).
        const_cast<ScreenSpaceEffects*>(this)->m_prevFrameBloomSrv = bloomSrv;
    }
    else
    {
        const_cast<ScreenSpaceEffects*>(this)->m_prevFrameBloomSrv = 0;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);

    if (IsPbrMaterialDebugMode(m_debugMode))
    {
        m_debugChannelShader->Use(false, true);
        m_debugChannelShader->SetInt("uOutputRgb", 1);
        m_debugChannelShader->SetInt("uOutputAlpha", 0);
        m_debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
        m_debugChannelShader->SetInt("uInput", 0);
        m_debugChannelShader->BindTextureSlot(0, hdrColorSrv);
        m_debugChannelShader->FlushUniforms();
        DrawFullscreenQuad();
        CaptureSsaoDiagnosticsCpu(
            runAo,
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
            ssaoDebugViewSource = runAo
                ? ((m_ssaoShaderDebugMode != 0) ? "ssao_raw_debug" : "ssao_blur_live")
                : "ssao_blur_stale_pass_off";
        }
        else if (m_debugMode == RenderDebugMode::GtaoRaw)
        {
            debugSrv = m_gtaoRawTarget.srvCpuHandle;
            ssaoDebugViewSource = runGtao ? "gtao_raw" : "gtao_raw_stale_or_inactive";
        }
        else if (m_debugMode == RenderDebugMode::GtaoFiltered)
        {
            debugSrv = aoCompositeSrv;
            ssaoDebugViewSource = runGtao ? "gtao_filtered" : "gtao_filtered_stale_or_inactive";
        }
        else if (m_debugMode == RenderDebugMode::CompositeOcclusion && runAo)
        {
            debugSrv = m_hdrCompositeTarget.srvCpuHandle;
            ssaoDebugViewSource = "composite_occlusion";
        }
        else if (m_debugMode == RenderDebugMode::MotionVectors && m_sceneFramebuffer->HasVelocity())
        {
            m_velocityDebugShader->Use(false, true);
            m_velocityDebugShader->SetInt("uVelocityMap", 0);
            m_velocityDebugShader->SetInt("uDepthMap", 1);
            m_velocityDebugShader->SetFloat("uVelocityScale", 80.0f);
            m_velocityDebugShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(4));
            m_velocityDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_velocityDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                "motion_vectors",
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
        else if (IsGBufferDebugMode(m_debugMode) && m_sceneFramebuffer->HasMaterialGbuffer())
        {
            m_gbufferDebugShader->Use(false, true);
            m_gbufferDebugShader->SetInt("uMaterial0Map", 0);
            m_gbufferDebugShader->SetInt("uMaterial1Map", 1);
            m_gbufferDebugShader->SetInt("uDepthMap", 2);
            m_gbufferDebugShader->SetInt("uGBufferDebugMode", GBufferDebugModeIndex(m_debugMode));
            m_gbufferDebugShader->BindTextureSlot(0, m_sceneFramebuffer->GetColorSrvCpuHandle(5));
            m_gbufferDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetColorSrvCpuHandle(6));
            m_gbufferDebugShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_gbufferDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                "gbuffer_material",
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
        else if (IsRadianceDebugMode(m_debugMode) && m_radianceTarget.srvCpuHandle != 0)
        {
            m_radianceDebugShader->Use(false, true);
            m_radianceDebugShader->SetInt("uRadianceMap", 0);
            m_radianceDebugShader->SetInt("uDepthMap", 1);
            m_radianceDebugShader->SetInt("uRadianceDebugMode", RadianceDebugModeIndex(m_debugMode));
            m_radianceDebugShader->BindTextureSlot(0, m_radianceTarget.srvCpuHandle);
            m_radianceDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_radianceDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                "radiance_buffer",
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
        else if (IsSsrSceneDebugMode(m_debugMode) && m_ssrSceneColorTarget.srvCpuHandle != 0)
        {
            m_ssrDebugShader->Use(false, true);
            m_ssrDebugShader->SetInt("uSceneColorMap", 0);
            m_ssrDebugShader->SetInt("uDepthMap", 1);
            m_ssrDebugShader->SetInt("uSsrDebugMode", SsrDebugModeIndex(m_debugMode));
            m_ssrDebugShader->BindTextureSlot(0, m_ssrSceneColorTarget.srvCpuHandle);
            m_ssrDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_ssrDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                "ssr_scene_color",
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
        else if (IsSsrTraceDebugMode(m_debugMode) && m_ssrTraceTarget.srvCpuHandle != 0)
        {
            m_ssrTraceDebugShader->Use(false, true);
            m_ssrTraceDebugShader->SetInt("uTraceMap", 0);
            m_ssrTraceDebugShader->SetInt("uDepthMap", 1);
            m_ssrTraceDebugShader->SetInt(
                "uSsrTraceDebugMode",
                SsrTraceDebugModeIndex(m_debugMode));
            m_ssrTraceDebugShader->BindTextureSlot(0, m_ssrTraceTarget.srvCpuHandle);
            m_ssrTraceDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_ssrTraceDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                "ssr_trace",
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
        else if (IsSsrDenoiseDebugMode(m_debugMode))
        {
            std::uintptr_t debugSrv = 0;
            const char* debugSource = "ssr_denoise";
            if (m_debugMode == RenderDebugMode::SsrDenoiseSpatial)
            {
                debugSrv = m_lastSsrSpatialSrv != 0 ? m_lastSsrSpatialSrv : m_ssrTraceTarget.srvCpuHandle;
                debugSource = "ssr_denoise_spatial";
            }
            else if (m_debugMode == RenderDebugMode::SsrDenoiseTemporal)
            {
                debugSrv = m_lastSsrTemporalSrv != 0 ? m_lastSsrTemporalSrv : m_ssrTraceTarget.srvCpuHandle;
                debugSource = "ssr_svgf_temporal";
            }
            else if (m_debugMode == RenderDebugMode::SsrSvgfVariance)
            {
                debugSrv = m_lastSsrVarianceSrv != 0 ? m_lastSsrVarianceSrv : m_ssrTraceTarget.srvCpuHandle;
                debugSource = "ssr_svgf_variance";
            }
            else if (m_debugMode == RenderDebugMode::SsrDenoiseFinal)
            {
                debugSrv = m_lastSsrDenoiseSrv != 0 ? m_lastSsrDenoiseSrv : m_ssrTraceTarget.srvCpuHandle;
                debugSource = "ssr_svgf_final";
            }
            else if (m_debugMode == RenderDebugMode::SsrUpscaled)
            {
                debugSrv = m_lastSsrResolvedSrv != 0 ? m_lastSsrResolvedSrv : m_ssrTraceTarget.srvCpuHandle;
                debugSource = "ssr_upscaled";
            }

            if (debugSrv != 0)
            {
                m_ssrDenoiseDebugShader->Use(false, true);
                m_ssrDenoiseDebugShader->SetInt("uTraceMap", 0);
                m_ssrDenoiseDebugShader->SetInt("uDepthMap", 1);
                m_ssrDenoiseDebugShader->SetInt(
                    "uDebugMode",
                    SsrDenoiseDebugModeIndex(m_debugMode));
                m_ssrDenoiseDebugShader->SetFloat(
                    "uDebugScale",
                    m_debugMode == RenderDebugMode::SsrSvgfVariance ? 8.0f : 1.0f);
                m_ssrDenoiseDebugShader->BindTextureSlot(0, debugSrv);
                m_ssrDenoiseDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
                m_ssrDenoiseDebugShader->FlushUniforms();
                DrawFullscreenQuad();
                CaptureSsaoDiagnosticsCpu(
                    runAo,
                    compositeRan,
                    compositeUsesSsao,
                    pbrDebugActive,
                    useShadowFactorComposite,
                    hdrColorSource,
                    debugSource,
                    hdrColorSrv,
                    shadowFactorSrv);
                if (m_logSsaoApplySnapshot)
                {
                    m_pendingSsaoGpuReadback = true;
                }
                return;
            }
        }
        else if (m_debugMode == RenderDebugMode::RtSpecReplacement && runRtIndirect
                 && m_rtIndirectTarget.srvCpuHandle != 0)
        {
            // The composite pass wrote the replacement weight (debug) or the composited
            // indirect (composite active) into the RT indirect target — show it directly.
            m_debugChannelShader->Use(false, true);
            m_debugChannelShader->SetInt("uOutputRgb", 1);
            m_debugChannelShader->SetInt("uOutputAlpha", 0);
            m_debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
            m_debugChannelShader->SetInt("uInput", 0);
            m_debugChannelShader->BindTextureSlot(0, m_rtIndirectTarget.srvCpuHandle);
            m_debugChannelShader->FlushUniforms();
            DrawFullscreenQuad();
            return;
        }
        else if (IsSsrCompositeDebugMode(m_debugMode) && runSsrIndirect && m_ssrIndirectTarget.srvCpuHandle != 0)
        {
            m_ssrDenoiseDebugShader->Use(false, true);
            m_ssrDenoiseDebugShader->SetInt("uTraceMap", 0);
            m_ssrDenoiseDebugShader->SetInt("uDepthMap", 1);
            m_ssrDenoiseDebugShader->SetInt("uDebugMode", 0);
            m_ssrDenoiseDebugShader->SetFloat("uDebugScale", 1.0f);
            m_ssrDenoiseDebugShader->BindTextureSlot(0, m_ssrIndirectTarget.srvCpuHandle);
            m_ssrDenoiseDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_ssrDenoiseDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                "ssr_spec_replacement",
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }
        else if (IsSsgiDenoiseDebugMode(m_debugMode))
        {
            std::uintptr_t debugSrv = 0;
            const char* debugSource = "ssgi_denoise";
            if (m_debugMode == RenderDebugMode::SsgiTraceRaw)
            {
                debugSrv = m_radianceTarget.srvCpuHandle;
                if (m_ssgiEnabled && m_radianceTraceInputTarget.srvCpuHandle != 0)
                {
                    debugSrv = m_radianceTraceInputTarget.srvCpuHandle;
                }
                else if (
                    (m_ssgiDenoiseEnabled || m_ssgiNoiseInjectionEnabled) &&
                    m_radianceTraceInputTarget.srvCpuHandle != 0)
                {
                    debugSrv = m_radianceTraceInputTarget.srvCpuHandle;
                }
                debugSource = "ssgi_trace_raw";
            }
            else if (
                m_debugMode == RenderDebugMode::SsgiTraceHitMask ||
                m_debugMode == RenderDebugMode::SsgiTraceHitDistance)
            {
                debugSrv = m_radianceTraceInputTarget.srvCpuHandle;
                debugSource = m_debugMode == RenderDebugMode::SsgiTraceHitMask
                    ? "ssgi_trace_hit_mask"
                    : "ssgi_trace_hit_distance";
            }
            else if (m_debugMode == RenderDebugMode::SsgiDenoiseSpatial)
            {
                debugSrv = m_radianceSpatialTarget.srvCpuHandle;
                debugSource = "ssgi_denoise_spatial";
            }
            else if (
                m_debugMode == RenderDebugMode::SsgiDenoiseTemporal ||
                m_debugMode == RenderDebugMode::SsgiDenoiseFinal)
            {
                debugSrv = m_radianceHistoryTarget.srvCpuHandle;
                debugSource = m_debugMode == RenderDebugMode::SsgiDenoiseTemporal
                    ? "ssgi_denoise_temporal"
                    : "ssgi_denoise_final";
            }
            else if (m_debugMode == RenderDebugMode::SsgiInject)
            {
                debugSrv = m_lastSsgiInjectSrv != 0 ? m_lastSsgiInjectSrv : m_radianceTarget.srvCpuHandle;
                debugSource = "ssgi_inject";
            }
            else if (m_debugMode == RenderDebugMode::SsgiFinalContribution)
            {
                debugSrv = m_lastSsgiInjectSrv != 0 ? m_lastSsgiInjectSrv : m_radianceTarget.srvCpuHandle;
                debugSource = "ssgi_final_contribution";
            }

            if (debugSrv != 0)
            {
                m_ssgiDenoiseDebugShader->Use(false, true);
                m_ssgiDenoiseDebugShader->SetInt("uRadianceMap", 0);
                m_ssgiDenoiseDebugShader->SetInt("uDepthMap", 1);
                m_ssgiDenoiseDebugShader->SetInt("uDebugMode", SsgiDebugModeIndex(m_debugMode));
                m_ssgiDenoiseDebugShader->SetFloat(
                    "uDebugScale",
                    m_debugMode == RenderDebugMode::SsgiFinalContribution ? m_ssgiStrength : 1.0f);
                m_ssgiDenoiseDebugShader->BindTextureSlot(0, debugSrv);
                m_ssgiDenoiseDebugShader->BindTextureSlot(1, m_sceneFramebuffer->GetDepthSrvCpuHandle());
                m_ssgiDenoiseDebugShader->FlushUniforms();
                DrawFullscreenQuad();
                CaptureSsaoDiagnosticsCpu(
                    runAo,
                    compositeRan,
                    compositeUsesSsao,
                    pbrDebugActive,
                    useShadowFactorComposite,
                    hdrColorSource,
                    debugSource,
                    hdrColorSrv,
                    shadowFactorSrv);
                if (m_logSsaoApplySnapshot)
                {
                    m_pendingSsaoGpuReadback = true;
                }
                return;
            }
        }
        else if (
            IsGiTemporalDebugMode(m_debugMode) &&
            m_radianceHistoryTarget.srvCpuHandle != 0)
        {
            m_giTemporalDebugShader->Use(false, true);
            m_giTemporalDebugShader->SetInt("uTemporalRadiance", 0);
            m_giTemporalDebugShader->SetInt("uCurrentRadiance", 1);
            m_giTemporalDebugShader->SetInt("uDepthMap", 2);
            m_giTemporalDebugShader->SetInt(
                "uGiTemporalDebugMode",
                GiTemporalDebugModeIndex(m_debugMode));
            m_giTemporalDebugShader->SetFloat("uDifferenceGain", 25.0f);
            m_giTemporalDebugShader->BindTextureSlot(0, m_radianceHistoryTarget.srvCpuHandle);
            m_giTemporalDebugShader->BindTextureSlot(1, m_radianceTarget.srvCpuHandle);
            m_giTemporalDebugShader->BindTextureSlot(2, m_sceneFramebuffer->GetDepthSrvCpuHandle());
            m_giTemporalDebugShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
                compositeRan,
                compositeUsesSsao,
                pbrDebugActive,
                useShadowFactorComposite,
                hdrColorSource,
                m_debugMode == RenderDebugMode::GiDisocclusion
                    ? "gi_disocclusion"
                    : (m_debugMode == RenderDebugMode::RadianceTemporalDelta
                          ? "radiance_temporal_delta"
                          : "radiance_temporal"),
                hdrColorSrv,
                shadowFactorSrv);
            if (m_logSsaoApplySnapshot)
            {
                m_pendingSsaoGpuReadback = true;
            }
            return;
        }

        if (debugSrv != 0)
        {
            m_debugChannelShader->Use(false, true);
            m_debugChannelShader->SetInt("uOutputRgb", 0);
            m_debugChannelShader->SetInt("uOutputAlpha", 0);
            m_debugChannelShader->SetVec2("uUvScale", glm::vec2(1.0f, 1.0f));
            m_debugChannelShader->SetInt("uInput", 0);
            m_debugChannelShader->BindTextureSlot(0, debugSrv);
            m_debugChannelShader->FlushUniforms();
            DrawFullscreenQuad();
            CaptureSsaoDiagnosticsCpu(
                runAo,
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
    const bool useSsaa = m_antiAliasingMode == AntiAliasingMode::SSAA;
    const bool needsLdrIntermediate =
        useFxaa || useSmaa || (useSsaa && m_viewportWidth > 0 && m_viewportHeight > 0
        && (m_width != m_viewportWidth || m_height != m_viewportHeight));
    const float ldrClear[] = {0.0f, 0.0f, 0.0f, 1.0f};

    auto runTonemapPass = [&](const bool drawToLdrTarget) {
        m_tonemapShader->Use(false, true);
        m_tonemapShader->SetInt("uHdrColor", 0);
        m_tonemapShader->SetFloat("uExposure", m_exposure);
        m_tonemapShader->SetInt("uTonemapMode", static_cast<int>(m_tonemapMode));
        m_tonemapShader->SetInt("uUseBloom", m_bloomEnabled ? 1 : 0);
        m_tonemapShader->SetFloat("uBloomIntensity", m_bloomIntensity);
        m_tonemapShader->SetFloat("uBloomTexelSizeX", texelSize.x * 2.0f);
        m_tonemapShader->SetFloat("uBloomTexelSizeY", texelSize.y * 2.0f);
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

    const bool wantDlss = m_antiAliasingMode == AntiAliasingMode::DLAA
        || m_antiAliasingMode == AntiAliasingMode::DLSS;

    if (wantDlss && ldrTargetReady)
    {
        SceneRenderTrace::Section dlssSection("dlss");
        // S2 LDR stopgap: tonemap the HDR composite into the render-res LDR target, feed THAT to DLSS
        // as the scaling input, and blit the display-res result to the viewport. S4 will move the DLSS
        // input to pre-tonemap HDR and run bloom/tonemap at display res.
        {
            SceneRenderTrace::Scope tonemapScope("tonemap to ldr (dlss)");
            runTonemapPass(true);
            tonemapScope.Success();
        }

        bool dlssRan = false;
        DlssContext& dlss = DlssContext::Get();
        const bool dlssUsable = dlss.IsReady() && dlss.IsRuntimeInitialized()
            && dlss.IsDlssSupported() && m_dlssOutputTarget.resource != nullptr
            && m_sceneFramebuffer->HasVelocity() && m_sceneFramebuffer->GetDepthResource() != nullptr;

        if (dlssUsable)
        {
            SceneRenderTrace::Scope evalScope("dlss evaluate");
            auto* commandList =
                static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());

            // Depth + all scene color (incl. RT4 velocity) -> PIXEL_SHADER_RESOURCE. The LDR input is
            // already there post-tonemap. SL adds its own barriers around evaluate and restores these.
            m_sceneFramebuffer->EnsureShaderResourceState();
            constexpr std::uint32_t kPixelSrv =
                static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

            auto* dlssOut = static_cast<ID3D12Resource*>(m_dlssOutputTarget.resource);
            TransitionResource(
                commandList,
                dlssOut,
                static_cast<D3D12_RESOURCE_STATES>(m_dlssOutputTarget.resourceState),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            m_dlssOutputTarget.resourceState =
                static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

            DlssFrameInputs in{};
            in.commandList = commandList;
            in.colorInput = m_ldrTonemapTarget.resource;
            in.colorInputState = m_ldrTonemapTarget.resourceState;
            in.colorOutput = dlssOut;
            in.colorOutputState = static_cast<unsigned int>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            in.depth = m_sceneFramebuffer->GetDepthResource();
            in.depthState = kPixelSrv;
            in.motionVectors = m_sceneFramebuffer->GetColorResource(4);
            in.motionVectorsState = kPixelSrv;
            in.renderWidth = static_cast<unsigned int>(m_width);
            in.renderHeight = static_cast<unsigned int>(m_height);
            in.displayWidth = static_cast<unsigned int>(m_dlssOutputTarget.width);
            in.displayHeight = static_cast<unsigned int>(m_dlssOutputTarget.height);
            in.quality = m_antiAliasingMode == AntiAliasingMode::DLAA
                ? DlssQuality::DLAA
                : ToDlssQuality(m_dlssPreset);
            in.colorIsHdr = false; // LDR stopgap
            in.depthInverted = false; // standard 0..1 depth cleared to 1, LESS test
            in.reset = !m_dlssHistoryValid;

            // NDC motion (curr-prev) -> backward UV motion in [-1,1]: scale 0.5, negate x, flip y.
            // Flagged for validation in S3 (fast-pan ghosting test).
            in.mvecScaleX = -0.5f;
            in.mvecScaleY = 0.5f;

            const glm::vec2 jitterNdc = camera.GetProjectionJitter();
            in.jitterX = jitterNdc.x * 0.5f * static_cast<float>(m_width);
            in.jitterY = -jitterNdc.y * 0.5f * static_cast<float>(m_height);

            // glm column-major storage maps byte-for-byte onto Streamline's row-major float4x4 and
            // transposes the operator into SL's row-vector convention — a straight copy is correct
            // (see DlssContext.cpp CopyMatrix). Matrices must be jitter-free (use unjittered proj).
            const glm::mat4 view = camera.GetViewMatrix();
            const glm::mat4 unjitteredProj = camera.GetUnjitteredProjectionMatrix();
            const glm::mat4 currViewProj = unjitteredProj * view;
            const glm::mat4 clipToPrevClip = m_motionVectorFrameState.historyValid
                ? m_motionVectorFrameState.prevViewProjection * glm::inverse(currViewProj)
                : glm::mat4(1.0f);
            std::memcpy(in.cameraViewToClip, glm::value_ptr(unjitteredProj), sizeof(float) * 16);
            const glm::mat4 clipToView = glm::inverse(unjitteredProj);
            std::memcpy(in.clipToCameraView, glm::value_ptr(clipToView), sizeof(float) * 16);
            std::memcpy(in.clipToPrevClip, glm::value_ptr(clipToPrevClip), sizeof(float) * 16);
            const glm::mat4 prevClipToClip = glm::inverse(clipToPrevClip);
            std::memcpy(in.prevClipToClip, glm::value_ptr(prevClipToClip), sizeof(float) * 16);

            in.cameraNear = camera.GetNearPlane();
            in.cameraFar = camera.GetFarPlane();
            in.cameraFovVertical = glm::radians(camera.GetFov());
            in.cameraAspect = camera.GetAspect();
            const glm::vec3 camPos = camera.GetPosition();
            const glm::vec3 camFwd = camera.GetFront();
            const glm::vec3 camRight(view[0][0], view[1][0], view[2][0]);
            const glm::vec3 camUp(view[0][1], view[1][1], view[2][1]);
            in.cameraPos[0] = camPos.x; in.cameraPos[1] = camPos.y; in.cameraPos[2] = camPos.z;
            in.cameraForward[0] = camFwd.x; in.cameraForward[1] = camFwd.y; in.cameraForward[2] = camFwd.z;
            in.cameraRight[0] = camRight.x; in.cameraRight[1] = camRight.y; in.cameraRight[2] = camRight.z;
            in.cameraUp[0] = camUp.x; in.cameraUp[1] = camUp.y; in.cameraUp[2] = camUp.z;

            dlssRan = dlss.Evaluate(in);

            // SL runs with eDisableCLStateTracking; restore our SRV heap before the blit below.
            GfxContext::Get().RebindFrameDescriptorHeaps();

            if (dlssRan)
            {
                TransitionResource(
                    commandList,
                    dlssOut,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_dlssOutputTarget.resourceState = kPixelSrv;
                m_dlssHistoryValid = true;
                blitLdrToViewport(m_dlssOutputTarget.srvCpuHandle);
            }
            else
            {
                // Restore output to a shader-readable state even on failure so tracking stays sane.
                TransitionResource(
                    commandList,
                    dlssOut,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_dlssOutputTarget.resourceState = kPixelSrv;
                m_dlssHistoryValid = false;
            }
            evalScope.Success();
        }

        if (!dlssRan)
        {
            // Fallback (DLSS unsupported / not ready / evaluate failed): bilinear-upscale the LDR
            // tonemap target to the viewport, matching the S1 behavior.
            SceneRenderTrace::Scope fallbackScope("dlss fallback bilinear");
            blitLdrToViewport(m_ldrTonemapTarget.srvCpuHandle);
            fallbackScope.Success();
        }

        dlssSection.Success();
    }
    else if (needsLdrIntermediate && ldrTargetReady)
    {
        SceneRenderTrace::Section tonemapSection("tonemap");
        {
            SceneRenderTrace::Scope tonemapScope("tonemap to ldr");
            runTonemapPass(true);
            tonemapScope.Success();
        }

        if (useFxaa)
        {
            SceneRenderTrace::Section aaOutputSection("aa-output");
            SceneRenderTrace::Scope fxaaScope("fxaa");
            BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
            m_fxaaShader->Use(false, true);
            m_fxaaShader->SetFloat("uTexelSizeX", texelSize.x);
            m_fxaaShader->SetFloat("uTexelSizeY", texelSize.y);
            m_fxaaShader->SetFloat("uSubpixQuality", m_fxaaSubpixQuality);
            m_fxaaShader->SetFloat("uEdgeThreshold", m_fxaaEdgeThreshold);
            m_fxaaShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            DrawFullscreenPass(*m_fxaaShader, true);
            fxaaScope.Success();
            aaOutputSection.Success();
        }
        else if (useSmaa)
        {
            SceneRenderTrace::Section aaOutputSection("aa-output");
            SceneRenderTrace::Scope smaaScope("smaa");
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
            smaaScope.Success();
            aaOutputSection.Success();
        }
        else if (useSsaa && (m_width != viewportWidth || m_height != viewportHeight))
        {
            SceneRenderTrace::Section aaOutputSection("aa-output");
            SceneRenderTrace::Scope ssaaScope("ssaa blit");
            blitLdrToViewport(m_ldrTonemapTarget.srvCpuHandle);
            ssaaScope.Success();
            aaOutputSection.Success();
        }
        else
        {
            SceneRenderTrace::Scope blitScope("ldr blit to viewport");
            BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
            m_downsampleShader->Use(false, true);
            m_downsampleShader->BindTextureSlot(0, m_ldrTonemapTarget.srvCpuHandle);
            DrawFullscreenPass(*m_downsampleShader, true);
            blitScope.Success();
        }

        tonemapSection.Success();
    }
    else
    {
        SceneRenderTrace::Section tonemapSection("tonemap");
        SceneRenderTrace::Scope tonemapScope("tonemap direct");
        BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
        runTonemapPass(false);
        tonemapScope.Success();
        tonemapSection.Success();
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
            runAo,
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
        runAo,
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
    m_ssaoDiagnostics.normalSrv = m_sceneFramebuffer->GetColorSrvCpuHandle(2);
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
    if (IsRtPrimaryDebugMode(mode) && !IsRtPrimaryDebugMode(m_debugMode))
    {
        m_rtPrimaryDebugSettleFrames = 0;
    }

    m_debugMode = mode;
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

std::uintptr_t ScreenSpaceEffects::GetSceneColorSrvCpuHandle(const int attachmentIndex) const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return 0;
    }

    return m_sceneFramebuffer->GetColorSrvCpuHandle(attachmentIndex);
}

void ScreenSpaceEffects::PrepareSceneColorForDxrRead() const
{
    if (m_sceneFramebuffer == nullptr || !m_sceneFramebuffer->IsValid())
    {
        return;
    }

    constexpr std::uint32_t kAllShaderRead = static_cast<std::uint32_t>(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    // RT0 direct, RT1 indirect, RT2 shading normal, RT3 sun shadow, RT5 material0
    // (see devdoc/dxr-reflections.md binding table).
    m_sceneFramebuffer->TransitionColorAttachment(0, kAllShaderRead);
    m_sceneFramebuffer->TransitionColorAttachment(1, kAllShaderRead);
    m_sceneFramebuffer->TransitionColorAttachment(2, kAllShaderRead);
    m_sceneFramebuffer->TransitionColorAttachment(3, kAllShaderRead);
    m_sceneFramebuffer->TransitionColorAttachment(5, kAllShaderRead);
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

    // TAA, DLAA and DLSS own the resolve stage and are incompatible with geometry MSAA.
    const bool ownsResolve = mode == AntiAliasingMode::TAA || mode == AntiAliasingMode::DLAA
        || mode == AntiAliasingMode::DLSS;
    if (ownsResolve && m_msaaSampleCount > 1)
    {
        m_msaaSampleCount = 1;
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

    if (clampedCount > 1 && m_antiAliasingMode == AntiAliasingMode::TAA)
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
    m_debugMode = source.m_debugMode;
    m_antiAliasingMode = source.m_antiAliasingMode;
    m_dlssPreset = source.m_dlssPreset;
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
