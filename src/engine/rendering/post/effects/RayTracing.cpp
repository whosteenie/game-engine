#include "engine/rendering/post/ScreenSpaceEffects.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/rendering/resources/Framebuffer.h"
#include "engine/rendering/shaders/Shader.h"
#include "engine/rendering/post/DxrDebugBlitPass.h"
#include "engine/rendering/post/PathTracerDisplayPass.h"
#include "engine/rendering/post/PostProcessContext.h"
#include "engine/rhi/DlssContext.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include <glm/gtc/type_ptr.hpp>

namespace
{
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
    const std::uintptr_t normalRoughnessSrv,
    const std::uintptr_t rrPrimaryOwnerSrv,
    void* const rrPrimaryOwnerResource,
    const std::uint32_t rrPrimaryOwnerResourceState,
    const std::uintptr_t rrTransmissionOwnerSrv,
    void* const rrTransmissionOwnerResource,
    const std::uint32_t rrTransmissionOwnerResourceState,
    const bool mirrorChainPsr,
    const std::uintptr_t psrThroughputSrv,
    void* const specularMotionResource,
    const std::uint32_t specularMotionResourceState,
    const std::uintptr_t specularMotionSrv,
    void* const opticalTransmissionOutputResource,
    const std::uint32_t opticalTransmissionOutputResourceState,
    const std::uintptr_t opticalTransmissionOutputSrv,
    void* const opticalTransmissionDepthResource,
    const std::uint32_t opticalTransmissionDepthResourceState,
    const std::uintptr_t opticalTransmissionDepthSrv,
    void* const opticalTransmissionMotionResource,
    const std::uint32_t opticalTransmissionMotionResourceState,
    const std::uintptr_t opticalTransmissionMotionSrv,
    const std::uintptr_t opticalTransmissionDiffuseAlbedoSrv,
    const std::uintptr_t opticalTransmissionSpecularAlbedoSrv,
    const std::uintptr_t opticalTransmissionNormalRoughnessSrv)
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
    m_pathTracerRrPrimaryOwnerSrv = rrPrimaryOwnerSrv;
    m_pathTracerRrPrimaryOwnerResource = rrPrimaryOwnerResource;
    m_pathTracerRrPrimaryOwnerResourceState = rrPrimaryOwnerResourceState;
    m_pathTracerRrTransmissionOwnerSrv = rrTransmissionOwnerSrv;
    m_pathTracerRrTransmissionOwnerResource = rrTransmissionOwnerResource;
    m_pathTracerRrTransmissionOwnerResourceState = rrTransmissionOwnerResourceState;
    m_ptMirrorChainPsr = mirrorChainPsr;
    m_pathTracerPsrThroughputSrv = psrThroughputSrv;
    m_pathTracerSpecularMotionResource = specularMotionResource;
    m_pathTracerSpecularMotionResourceState = specularMotionResourceState;
    m_pathTracerSpecularMotionSrv = specularMotionSrv;
    m_pathTracerOpticalTransmissionOutputResource = opticalTransmissionOutputResource;
    m_pathTracerOpticalTransmissionOutputResourceState = opticalTransmissionOutputResourceState;
    m_pathTracerOpticalTransmissionOutputSrv = opticalTransmissionOutputSrv;
    m_pathTracerOpticalTransmissionDepthResource = opticalTransmissionDepthResource;
    m_pathTracerOpticalTransmissionDepthResourceState = opticalTransmissionDepthResourceState;
    m_pathTracerOpticalTransmissionDepthSrv = opticalTransmissionDepthSrv;
    m_pathTracerOpticalTransmissionMotionResource = opticalTransmissionMotionResource;
    m_pathTracerOpticalTransmissionMotionResourceState = opticalTransmissionMotionResourceState;
    m_pathTracerOpticalTransmissionMotionSrv = opticalTransmissionMotionSrv;
    m_pathTracerOpticalTransmissionDiffuseAlbedoSrv = opticalTransmissionDiffuseAlbedoSrv;
    m_pathTracerOpticalTransmissionSpecularAlbedoSrv = opticalTransmissionSpecularAlbedoSrv;
    m_pathTracerOpticalTransmissionNormalRoughnessSrv = opticalTransmissionNormalRoughnessSrv;
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

bool ScreenSpaceEffects::GenerateSupportedDlssMotion(const std::uintptr_t motionSrv) const
{
    if (motionSrv == 0 || m_width <= 0 || m_height <= 0
        || m_dlssDilatedMotionTarget.resource == nullptr || m_dlssMotionCopyShader == nullptr)
    {
        return false;
    }

    SceneRenderTrace::Scope copyScope("dlss supported motion conversion");
    const float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_dlssMotionCopyShader->Use(false);
    m_dlssMotionCopyShader->SetInt("uMotion", 0);
    m_dlssMotionCopyShader->BindTextureSlot(0, motionSrv);
    DrawFullscreenToTarget(
        *m_dlssMotionCopyShader,
        const_cast<InternalTarget&>(m_dlssDilatedMotionTarget),
        m_width,
        m_height,
        clear);
    copyScope.Success();
    return true;
}

bool ScreenSpaceEffects::GenerateSupportedOpticalTransmissionDlssMotion(
    const std::uintptr_t motionSrv) const
{
    if (motionSrv == 0 || m_width <= 0 || m_height <= 0
        || m_dlssOpticalTransmissionMotionTarget.resource == nullptr
        || m_dlssMotionCopyShader == nullptr)
    {
        return false;
    }

    SceneRenderTrace::Scope copyScope("dlss optical transmission motion conversion");
    const float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_dlssMotionCopyShader->Use(false);
    m_dlssMotionCopyShader->SetInt("uMotion", 0);
    m_dlssMotionCopyShader->BindTextureSlot(0, motionSrv);
    DrawFullscreenToTarget(
        *m_dlssMotionCopyShader,
        const_cast<InternalTarget&>(m_dlssOpticalTransmissionMotionTarget),
        m_width,
        m_height,
        clear);
    copyScope.Success();
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
    m_dlssZeroMotionShader->SetVec2("uMotionValue", glm::vec2(0.0f));
    DrawFullscreenToTarget(
        *m_dlssZeroMotionShader,
        const_cast<InternalTarget&>(m_dlssDilatedMotionTarget),
        m_width,
        m_height,
        clear);
    reconstructionScope.Success();
    return true;
}

bool ScreenSpaceEffects::GenerateValidityFilteredRrMotion(
    const bool transmission,
    const std::uintptr_t motionSrv,
    const std::uintptr_t maskSrv) const
{
    InternalTarget& target = transmission
        ? const_cast<InternalTarget&>(m_rrTemporalTransmissionMotionTarget)
        : const_cast<InternalTarget&>(m_rrTemporalPrimaryMotionTarget);
    if (motionSrv == 0 || maskSrv == 0 || m_width <= 0 || m_height <= 0
        || target.resource == nullptr || m_rrMotionValidityShader == nullptr)
    {
        return false;
    }

    SceneRenderTrace::Scope validityScope(
        transmission ? "rr transmission motion validity" : "rr primary motion validity");
    const float clear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_rrMotionValidityShader->Use(false);
    m_rrMotionValidityShader->SetInt("uMotion", 0);
    m_rrMotionValidityShader->SetInt("uValidityMask", 1);
    m_rrMotionValidityShader->BindTextureSlot(0, motionSrv);
    m_rrMotionValidityShader->BindTextureSlot(1, maskSrv);
    DrawFullscreenToTarget(
        *m_rrMotionValidityShader,
        target,
        m_width,
        m_height,
        clear);
    validityScope.Success();
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
    case RenderDebugMode::RrTransmissionDiffuseAlbedo:
        srv = m_rrOpticalTransmissionDiffuseAlbedoTarget.srvCpuHandle;
        break;
    case RenderDebugMode::RrTransmissionSpecularAlbedo:
        srv = m_rrOpticalTransmissionSpecularAlbedoTarget.srvCpuHandle;
        break;
    case RenderDebugMode::RrTransmissionNormalRoughness:
        srv = m_rrOpticalTransmissionNormalRoughnessTarget.srvCpuHandle;
        break;
    case RenderDebugMode::RrTemporalValidity:
        srv = m_rrTemporalPrimaryValidityDiagnosticsTarget.srvCpuHandle;
        break;
    case RenderDebugMode::RrTransmissionTemporalValidity:
        srv = m_rrTemporalTransmissionValidityDiagnosticsTarget.srvCpuHandle;
        break;
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

void ScreenSpaceEffects::BlitPtOpticalLayerDebug(
    const Framebuffer* outputTarget,
    const int viewportWidth,
    const int viewportHeight) const
{
    const bool transmissionMode =
        m_debugMode == RenderDebugMode::PtOpticalRawTransmission
        || m_debugMode == RenderDebugMode::PtOpticalReconstructedTransmission
        || m_debugMode == RenderDebugMode::PtOpticalTransmissionReconstructionDelta;
    const bool reconstructedMode =
        m_debugMode == RenderDebugMode::PtOpticalReconstructedReflection
        || m_debugMode == RenderDebugMode::PtOpticalReconstructedTransmission
        || m_debugMode == RenderDebugMode::PtOpticalReflectionReconstructionDelta
        || m_debugMode == RenderDebugMode::PtOpticalTransmissionReconstructionDelta;
    if (!IsPtOpticalLayerDebugMode(m_debugMode) || outputTarget == nullptr
        || m_ptOpticalLayersShader == nullptr
        || (reconstructedMode && !m_pathTracerDlssResolvedThisFrame)
        || (transmissionMode && reconstructedMode && !m_dlssOpticalTransmissionHistoryValid))
    {
        return;
    }

    std::uintptr_t firstSrv = 0;
    std::uintptr_t secondSrv = 0;
    int shaderMode = 2; // Reinhard display of firstSrv.
    switch (m_debugMode)
    {
    case RenderDebugMode::PtOpticalRawReflection:
        firstSrv = m_ptOpticalReflectionInputTarget.srvCpuHandle;
        break;
    case RenderDebugMode::PtOpticalRawTransmission:
        firstSrv = m_pathTracerOpticalTransmissionOutputSrv;
        break;
    case RenderDebugMode::PtOpticalReconstructedReflection:
        firstSrv = m_dlssOutputTarget.srvCpuHandle;
        break;
    case RenderDebugMode::PtOpticalReconstructedTransmission:
        firstSrv = m_dlssOpticalTransmissionOutputTarget.srvCpuHandle;
        break;
    case RenderDebugMode::PtOpticalReflectionReconstructionDelta:
        firstSrv = m_ptOpticalReflectionInputTarget.srvCpuHandle;
        secondSrv = m_dlssOutputTarget.srvCpuHandle;
        shaderMode = 3;
        break;
    case RenderDebugMode::PtOpticalTransmissionReconstructionDelta:
        firstSrv = m_pathTracerOpticalTransmissionOutputSrv;
        secondSrv = m_dlssOpticalTransmissionOutputTarget.srvCpuHandle;
        shaderMode = 3;
        break;
    default:
        return;
    }
    if (firstSrv == 0)
    {
        return;
    }
    if (secondSrv == 0)
    {
        secondSrv = firstSrv;
    }

    BindOutputTarget(outputTarget, viewportWidth, viewportHeight);
    m_ptOpticalLayersShader->Use(false, true);
    m_ptOpticalLayersShader->SetInt("uComposite", shaderMode);
    m_ptOpticalLayersShader->BindTextureSlot(0, firstSrv);
    m_ptOpticalLayersShader->BindTextureSlot(1, secondSrv);
    m_ptOpticalLayersShader->FlushUniforms();
    DrawFullscreenQuad();
}

