#include "engine/raytracing/dispatch/DxrShadowsDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/raytracing/acceleration/DxrAccelerationStructures.h"
#include "engine/raytracing/core/DxrContext.h"
#include "engine/raytracing/core/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

DxrShadowsDispatch::~DxrShadowsDispatch()
{
    Release();
}

void DxrShadowsDispatch::Release()
{
    m_denoiser.Release();
    ReleaseCore();
    m_nrdHistoryValid = false;
}

void DxrShadowsDispatch::ResetProjectResources()
{
    ResetDispatchResources();
    m_denoiser.Release();
    m_prevViewToClip = glm::mat4(1.0f);
    m_prevWorldToView = glm::mat4(1.0f);
    m_prevJitterUv = glm::vec2(0.0f);
    m_frameIndex = 0;
    m_nrdHistoryValid = false;
    m_dispatchedThisFrame = false;
    m_denoisedThisFrame = false;
}

bool DxrShadowsDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrShadowsDispatch::EnsurePipeline(std::string& outError)
{
    return EnsurePipelineWith(
        "shadows",
        [](DxrPipeline& pipeline, std::string& pipelineError) {
            return pipeline.CreateShadowsPipeline(pipelineError);
        },
        [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
            return shaderBindingTable.BuildShadowTable(pipeline.GetProperties(), tableError);
        },
        outError);
}

bool DxrShadowsDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    const bool dxrEnabled,
    const bool shadowsEnabled,
    const bool shadowDebugViewActive,
    void* commandList,
    const FrameInputs& frameInputs,
    const int width,
    const int height,
    const int gbufferWidth,
    const int gbufferHeight,
    const float maxTraceDistance,
    const bool denoiseEnabled)
{
    m_dispatchedThisFrame = false;
    m_denoisedThisFrame = false;

    if (!shadowsEnabled && !shadowDebugViewActive)
    {
        return false;
    }

    if (gbufferWidth <= 0 || gbufferHeight <= 0)
    {
        return false;
    }

    if (frameInputs.depthSrvCpuHandle == 0 || frameInputs.normalSrvCpuHandle == 0
        || frameInputs.material0SrvCpuHandle == 0 || frameInputs.velocitySrvCpuHandle == 0)
    {
        DxrBreadcrumb("shadows skipped: frame inputs unavailable");
        return false;
    }

    ID3D12GraphicsCommandList4* commandList4 = ResolveDispatchCommandList(
        accelerationStructures,
        dxrEnabled,
        width,
        height,
        commandList,
        DxrDispatchGeometryRequirement::TlasOnly,
        "shadows");
    if (commandList4 == nullptr)
    {
        return false;
    }

    std::string error;

    if (!m_pipelineReady && !EnsurePipeline(error))
    {
        DxrBreadcrumb("shadows skipped: pipeline not ready");
        return false;
    }

    DxrBreadcrumb("shadows dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-shadows");

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    // Jittered projection: G-buffer position reconstruction must match the depth buffer exactly.
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 viewProj = projectionMatrix * viewMatrix;
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 cameraPos = camera.GetPosition();
    const glm::vec3 sunDirection = glm::normalize(frameInputs.sunDirection);
    const float angularRadiusRadians =
        glm::radians(std::clamp(frameInputs.sunAngularRadiusDegrees, 0.01f, 5.0f));

    DxrRootSignature::ShadowDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(width);
    constants.outputHeight = static_cast<std::uint32_t>(height);
    constants.gbufferWidth = static_cast<std::uint32_t>(gbufferWidth);
    constants.gbufferHeight = static_cast<std::uint32_t>(gbufferHeight);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.worldToView, glm::value_ptr(viewMatrix), sizeof(constants.worldToView));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.sunAngularTanRadius = std::tan(angularRadiusRadians);
    constants.sunDirection[0] = sunDirection.x;
    constants.sunDirection[1] = sunDirection.y;
    constants.sunDirection[2] = sunDirection.z;
    constants.maxTraceDistance = maxTraceDistance;
    constants.frameIndex = m_frameIndex;

    DxrDispatchContext::ShadowDispatchInputs dispatchInputs{};
    dispatchInputs.tlasResource = accelerationStructures.GetTlasResource();
    dispatchInputs.tlasGpuVirtualAddress = accelerationStructures.GetTlasGpuVirtualAddress();
    dispatchInputs.depthSrvCpuHandle = frameInputs.depthSrvCpuHandle;
    dispatchInputs.normalSrvCpuHandle = frameInputs.normalSrvCpuHandle;
    dispatchInputs.material0SrvCpuHandle = frameInputs.material0SrvCpuHandle;
    dispatchInputs.velocitySrvCpuHandle = frameInputs.velocitySrvCpuHandle;

    if (!m_dispatchContext.DispatchShadows(
            commandList4,
            m_pipeline.GetStateObject(),
            m_pipeline.GetGlobalRootSignature(),
            m_shaderBindingTable,
            dispatchInputs,
            width,
            height,
            constants,
            error))
    {
        const std::string failureMessage =
            std::string("shadow dispatch failed: DispatchShadows (") + error + ")";
        DxrLogErrorOnce("dispatch-shadows-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-shadows-failure", failureMessage);
        dispatchScope.Success();
        return false;
    }

    DxrBreadcrumb("shadows dispatch ok");
    dispatchScope.Success();
    m_dispatchedThisFrame = true;

    // NRD SIGMA_SHADOW over the freshly traced penumbra buffer.
    if (denoiseEnabled)
    {
        const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
        const glm::vec2 jitterNdc = camera.GetProjectionJitter();
        const glm::vec2 jitterPixels(
            jitterNdc.x * 0.5f * static_cast<float>(width),
            -jitterNdc.y * 0.5f * static_cast<float>(height));

        NrdShadowDenoiser::FrameParameters frameParameters{};
        frameParameters.viewToClip = unjitteredProjection;
        frameParameters.worldToView = viewMatrix;
        frameParameters.viewToClipPrev = m_nrdHistoryValid ? m_prevViewToClip : unjitteredProjection;
        frameParameters.worldToViewPrev = m_nrdHistoryValid ? m_prevWorldToView : viewMatrix;
        frameParameters.cameraJitterUv = jitterPixels;
        frameParameters.cameraJitterUvPrev = m_nrdHistoryValid ? m_prevJitterUv : jitterPixels;
        frameParameters.lightDirection = sunDirection;
        frameParameters.frameIndex = m_frameIndex;
        frameParameters.denoisingRange = std::max(maxTraceDistance * 2.0f, 100.0f);
        frameParameters.resetHistory = !m_nrdHistoryValid;

        std::string denoiseError;
        DxrDispatchContext::ShadowNrdResources nrdResources = m_dispatchContext.GetShadowNrdResources();
        if (m_denoiser.Denoise(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                nrdResources,
                frameParameters,
                denoiseError))
        {
            m_denoisedThisFrame = true;
            m_nrdHistoryValid = true;
        }
        else
        {
            DxrLogErrorOnce(
                "nrd-shadow-denoise-failure", std::string("NRD shadow denoise failed: ") + denoiseError);
            m_nrdHistoryValid = false;
        }

        m_prevViewToClip = unjitteredProjection;
        m_prevWorldToView = viewMatrix;
        m_prevJitterUv = jitterPixels;
    }
    else
    {
        m_nrdHistoryValid = false;
    }

    ++m_frameIndex;
    return true;
}

std::uintptr_t DxrShadowsDispatch::GetPenumbraSrvCpuHandle() const
{
    return m_dispatchContext.GetShadowPenumbraSrvCpuHandle();
}

std::uintptr_t DxrShadowsDispatch::GetDenoisedSrvCpuHandle() const
{
    return m_denoisedThisFrame ? m_dispatchContext.GetShadowDenoisedSrvCpuHandle() : 0;
}

bool DxrShadowsDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetShadowPenumbraSrvCpuHandle() != 0;
}

float DxrShadowsDispatch::GetOutputUvScaleX() const
{
    const int textureWidth = m_dispatchContext.GetShadowOutputWidth();
    const int dispatchWidth = m_dispatchContext.GetShadowDispatchWidth();
    if (textureWidth <= 0 || dispatchWidth <= 0)
    {
        return 1.0f;
    }

    return static_cast<float>(dispatchWidth) / static_cast<float>(textureWidth);
}

float DxrShadowsDispatch::GetOutputUvScaleY() const
{
    const int textureHeight = m_dispatchContext.GetShadowOutputHeight();
    const int dispatchHeight = m_dispatchContext.GetShadowDispatchHeight();
    if (textureHeight <= 0 || dispatchHeight <= 0)
    {
        return 1.0f;
    }

    return static_cast<float>(dispatchHeight) / static_cast<float>(textureHeight);
}
