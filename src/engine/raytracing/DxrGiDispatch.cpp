#include "engine/raytracing/DxrGiDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>

DxrGiDispatch::~DxrGiDispatch()
{
    Release();
}

void DxrGiDispatch::Release()
{
    m_denoiser.Release();
    ReleaseCore();
    m_nrdHistoryValid = false;
}

void DxrGiDispatch::ResetProjectResources()
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

bool DxrGiDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrGiDispatch::EnsurePipeline(std::string& outError)
{
    m_denoiser.SetSignal(NrdDenoiser::Signal::Diffuse);

    return EnsurePipelineWith(
        "gi",
        [](DxrPipeline& pipeline, std::string& pipelineError) {
            return pipeline.CreateGiPipeline(pipelineError);
        },
        [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
            return shaderBindingTable.BuildGiTable(pipeline.GetProperties(), tableError);
        },
        outError);
}

bool DxrGiDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    const bool dxrEnabled,
    const bool giEnabled,
    const bool giDebugViewActive,
    void* commandList,
    const FrameInputs& frameInputs,
    const int outputWidth,
    const int outputHeight,
    const int gbufferWidth,
    const int gbufferHeight,
    const float maxTraceDistance,
    const int samplesPerPixel,
    const bool denoiseEnabled,
    const float temporalBlend,
    const int atrousIterations,
    const bool antiFirefly)
{
    m_dispatchedThisFrame = false;
    m_denoisedThisFrame = false;

    if (!giEnabled && !giDebugViewActive)
    {
        return false;
    }

    if (gbufferWidth <= 0 || gbufferHeight <= 0)
    {
        return false;
    }

    if (frameInputs.depthSrvCpuHandle == 0 || frameInputs.normalSrvCpuHandle == 0
        || frameInputs.material0SrvCpuHandle == 0 || frameInputs.directSrvCpuHandle == 0
        || frameInputs.sunShadowSrvCpuHandle == 0 || frameInputs.indirectSrvCpuHandle == 0
        || frameInputs.prefilterSrvCpuHandle == 0 || frameInputs.velocitySrvCpuHandle == 0)
    {
        DxrBreadcrumb("gi skipped: frame inputs unavailable");
        return false;
    }

    ID3D12GraphicsCommandList4* commandList4 = ResolveDispatchCommandList(
        accelerationStructures,
        dxrEnabled,
        outputWidth,
        outputHeight,
        commandList,
        DxrDispatchGeometryRequirement::TlasAndGeometryLookup,
        "gi");
    if (commandList4 == nullptr)
    {
        return false;
    }

    std::string error;

    if (!m_pipelineReady && !EnsurePipeline(error))
    {
        DxrBreadcrumb("gi skipped: pipeline not ready");
        return false;
    }

    DxrBreadcrumb("gi dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-gi");

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    // Jittered projection: G-buffer position reconstruction must match the depth buffer exactly.
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 viewProj = projectionMatrix * viewMatrix;
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 cameraPos = camera.GetPosition();

    DxrRootSignature::ReflectionDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(outputWidth);
    constants.outputHeight = static_cast<std::uint32_t>(outputHeight);
    constants.gbufferWidth = static_cast<std::uint32_t>(gbufferWidth);
    constants.gbufferHeight = static_cast<std::uint32_t>(gbufferHeight);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.viewProj, glm::value_ptr(viewProj), sizeof(constants.viewProj));
    std::memcpy(constants.worldToView, glm::value_ptr(viewMatrix), sizeof(constants.worldToView));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.maxTraceDistance = maxTraceDistance;
    constants.environmentIntensity = frameInputs.environmentIntensity;
    constants.maxReflectionLod = frameInputs.maxReflectionLod;
    constants.frameIndex = m_frameIndex;
    constants.samplesPerPixel =
        static_cast<std::uint32_t>(samplesPerPixel < 1 ? 1 : (samplesPerPixel > 16 ? 16 : samplesPerPixel));

    // In-hit analytic shading inputs (shared hit_shading.hlsli).
    constants.sunDirection[0] = frameInputs.sunDirection.x;
    constants.sunDirection[1] = frameInputs.sunDirection.y;
    constants.sunDirection[2] = frameInputs.sunDirection.z;
    constants.sunIntensity = frameInputs.sunIntensity;
    constants.sunColor[0] = frameInputs.sunColor.x;
    constants.sunColor[1] = frameInputs.sunColor.y;
    constants.sunColor[2] = frameInputs.sunColor.z;
    for (std::size_t i = 0; i < frameInputs.irradianceSh9.size(); ++i)
    {
        constants.irradianceSh9[i][0] = frameInputs.irradianceSh9[i].x;
        constants.irradianceSh9[i][1] = frameInputs.irradianceSh9[i].y;
        constants.irradianceSh9[i][2] = frameInputs.irradianceSh9[i].z;
        constants.irradianceSh9[i][3] = frameInputs.irradianceSh9[i].w;
    }

    DxrDispatchContext::ReflectionDispatchInputs dispatchInputs{};
    dispatchInputs.tlasResource = accelerationStructures.GetTlasResource();
    dispatchInputs.tlasGpuVirtualAddress = accelerationStructures.GetTlasGpuVirtualAddress();
    dispatchInputs.depthSrvCpuHandle = frameInputs.depthSrvCpuHandle;
    dispatchInputs.normalSrvCpuHandle = frameInputs.normalSrvCpuHandle;
    dispatchInputs.material0SrvCpuHandle = frameInputs.material0SrvCpuHandle;
    dispatchInputs.geometryLookupSrvIndex = accelerationStructures.GetGeometryLookupSrvIndex();
    dispatchInputs.sceneVertexFloatsSrvIndex = accelerationStructures.GetSceneVertexFloatsSrvIndex();
    dispatchInputs.sceneIndicesSrvIndex = accelerationStructures.GetSceneIndicesSrvIndex();
    dispatchInputs.materialSrvIndex = accelerationStructures.GetMaterialSrvIndex();
    dispatchInputs.directSrvCpuHandle = frameInputs.directSrvCpuHandle;
    dispatchInputs.sunShadowSrvCpuHandle = frameInputs.sunShadowSrvCpuHandle;
    dispatchInputs.indirectSrvCpuHandle = frameInputs.indirectSrvCpuHandle;
    dispatchInputs.prefilterSrvCpuHandle = frameInputs.prefilterSrvCpuHandle;
    dispatchInputs.velocitySrvCpuHandle = frameInputs.velocitySrvCpuHandle;

    if (!m_dispatchContext.DispatchGi(
            commandList4,
            m_pipeline.GetStateObject(),
            m_pipeline.GetGlobalRootSignature(),
            m_shaderBindingTable,
            dispatchInputs,
            outputWidth,
            outputHeight,
            constants,
            error))
    {
        const std::string failureMessage =
            std::string("gi dispatch failed: DispatchGi (") + error + ")";
        DxrLogErrorOnce("dispatch-gi-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-gi-failure", failureMessage);
        dispatchScope.Success();
        return false;
    }

    DxrBreadcrumb("gi dispatch ok");
    dispatchScope.Success();
    m_dispatchedThisFrame = true;

    // NRD RELAX_DIFFUSE over the freshly traced buffer (dedicated instance).
    if (denoiseEnabled)
    {
        const glm::mat4 unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
        const glm::vec2 jitterNdc = camera.GetProjectionJitter();
        // NRD cameraJitter is in pixels, [-0.5; 0.5]: sampleUv = pixelUv + jitter/rectSize.
        const glm::vec2 jitterPixels(
            jitterNdc.x * 0.5f * static_cast<float>(outputWidth),
            -jitterNdc.y * 0.5f * static_cast<float>(outputHeight));

        NrdDenoiser::FrameParameters frameParameters{};
        frameParameters.viewToClip = unjitteredProjection;
        frameParameters.worldToView = viewMatrix;
        frameParameters.viewToClipPrev = m_nrdHistoryValid ? m_prevViewToClip : unjitteredProjection;
        frameParameters.worldToViewPrev = m_nrdHistoryValid ? m_prevWorldToView : viewMatrix;
        frameParameters.cameraJitterUv = jitterPixels;
        frameParameters.cameraJitterUvPrev = m_nrdHistoryValid ? m_prevJitterUv : jitterPixels;
        frameParameters.frameIndex = m_frameIndex;
        frameParameters.denoisingRange = std::max(maxTraceDistance * 2.0f, 100.0f);
        frameParameters.temporalBlend = temporalBlend;
        frameParameters.atrousIterations = atrousIterations;
        frameParameters.antiFirefly = antiFirefly;
        frameParameters.resetHistory = !m_nrdHistoryValid;

        std::string denoiseError;
        DxrDispatchContext::ReflectionNrdResources nrdResources = m_dispatchContext.GetGiNrdResources();
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
            DxrLogErrorOnce("nrd-gi-denoise-failure", std::string("NRD GI denoise failed: ") + denoiseError);
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

std::uintptr_t DxrGiDispatch::GetDenoisedSrvCpuHandle() const
{
    return m_denoisedThisFrame ? m_dispatchContext.GetGiDenoisedSrvCpuHandle() : 0;
}

std::uintptr_t DxrGiDispatch::GetGiOutputSrvCpuHandle() const
{
    return m_dispatchContext.GetGiOutputSrvCpuHandle();
}

bool DxrGiDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetGiOutputSrvCpuHandle() != 0;
}

float DxrGiDispatch::GetOutputUvScaleX() const
{
    const int textureWidth = m_dispatchContext.GetGiOutputWidth();
    const int dispatchWidth = m_dispatchContext.GetGiDispatchWidth();
    if (textureWidth <= 0 || dispatchWidth <= 0)
    {
        return 1.0f;
    }

    return static_cast<float>(dispatchWidth) / static_cast<float>(textureWidth);
}

float DxrGiDispatch::GetOutputUvScaleY() const
{
    const int textureHeight = m_dispatchContext.GetGiOutputHeight();
    const int dispatchHeight = m_dispatchContext.GetGiDispatchHeight();
    if (textureHeight <= 0 || dispatchHeight <= 0)
    {
        return 1.0f;
    }

    return static_cast<float>(dispatchHeight) / static_cast<float>(textureHeight);
}
