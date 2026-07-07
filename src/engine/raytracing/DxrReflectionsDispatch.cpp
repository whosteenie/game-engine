#include "engine/raytracing/DxrReflectionsDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>

DxrReflectionsDispatch::~DxrReflectionsDispatch()
{
    Release();
}

void DxrReflectionsDispatch::Release()
{
    m_denoiser.Release();
    m_shaderBindingTable.Release();
    m_pipeline.Release();
    m_dispatchContext.Release();
    m_nrdHistoryValid = false;
    m_pipelineReady = false;
}

bool DxrReflectionsDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrReflectionsDispatch::EnsurePipeline(std::string& outError)
{
    outError.clear();
    if (m_pipelineReady)
    {
        return true;
    }

    DxrBreadcrumb("reflections EnsurePipeline begin");
    if (!m_pipeline.CreateReflectionsPipeline(outError))
    {
        DxrBreadcrumb("reflections EnsurePipeline failed: CreateReflectionsPipeline");
        return false;
    }

    if (!m_shaderBindingTable.BuildReflectionTable(m_pipeline.GetProperties(), outError))
    {
        DxrBreadcrumb("reflections EnsurePipeline failed: BuildReflectionTable");
        m_pipeline.Release();
        return false;
    }

    m_pipelineReady = true;
    DxrBreadcrumb("reflections EnsurePipeline ok");
    return true;
}

bool DxrReflectionsDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    const bool dxrEnabled,
    const bool reflectionsEnabled,
    const bool reflectionDebugViewActive,
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
    const bool antiFirefly,
    const int aoRayCount,
    const float roughnessCutoff)
{
    m_dispatchedThisFrame = false;
    m_denoisedThisFrame = false;

    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled
        || outputWidth <= 0 || outputHeight <= 0 || gbufferWidth <= 0 || gbufferHeight <= 0)
    {
        return false;
    }

    if (!reflectionsEnabled && !reflectionDebugViewActive)
    {
        return false;
    }

    if (!accelerationStructures.IsTlasBuilt() || !accelerationStructures.HasGeometryLookup())
    {
        DxrBreadcrumb("reflections skipped: TLAS or geometry lookup unavailable");
        return false;
    }

    if (frameInputs.depthSrvCpuHandle == 0 || frameInputs.normalSrvCpuHandle == 0
        || frameInputs.material0SrvCpuHandle == 0 || frameInputs.directSrvCpuHandle == 0
        || frameInputs.sunShadowSrvCpuHandle == 0 || frameInputs.indirectSrvCpuHandle == 0
        || frameInputs.prefilterSrvCpuHandle == 0 || frameInputs.velocitySrvCpuHandle == 0)
    {
        DxrBreadcrumb("reflections skipped: frame inputs unavailable");
        return false;
    }

    ID3D12GraphicsCommandList4* commandList4 = DxrContext::Get().QueryCommandList4(commandList);
    if (commandList4 == nullptr)
    {
        DxrBreadcrumb("reflections skipped: CommandList4 unavailable");
        return false;
    }

    std::string error;

    if (!m_pipelineReady)
    {
        if (!EnsurePipeline(error))
        {
            DxrBreadcrumb("reflections skipped: pipeline not ready");
            return false;
        }
    }

    DxrBreadcrumb("reflections dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-reflections");

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    // Jittered projection: G-buffer position reconstruction and hit->screen reprojection
    // must match the depth buffer exactly (same convention as the primary debug trace).
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
    constants.aoRayCount =
        static_cast<std::uint32_t>(aoRayCount < 0 ? 0 : (aoRayCount > 16 ? 16 : aoRayCount));
    constants.roughnessCutoff = roughnessCutoff < 0.0f ? 0.0f : (roughnessCutoff > 1.0f ? 1.0f : roughnessCutoff);
    constants.sunAngularTanRadius =
        std::tan(glm::radians(frameInputs.sunAngularRadiusDegrees < 0.0f ? 0.0f
            : (frameInputs.sunAngularRadiusDegrees > 5.0f ? 5.0f
                                                            : frameInputs.sunAngularRadiusDegrees)));
    constants.giStrength = frameInputs.giStrength;
    constants.hasGiTrace = frameInputs.hasGiTrace ? 1u : 0u;

    // In-hit analytic shading inputs.
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

    if (!m_dispatchContext.DispatchReflections(
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
            std::string("reflection dispatch failed: DispatchReflections (") + error + ")";
        DxrLogErrorOnce("dispatch-reflections-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-reflections-failure", failureMessage);
        dispatchScope.Success();
        return false;
    }

    DxrBreadcrumb("reflections dispatch ok");
    dispatchScope.Success();
    m_dispatchedThisFrame = true;

    // Phase D5 — NRD RELAX_SPECULAR over the freshly traced buffer.
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
        DxrDispatchContext::ReflectionNrdResources nrdResources =
            m_dispatchContext.GetReflectionNrdResources();
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
            DxrLogErrorOnce("nrd-denoise-failure", std::string("NRD denoise failed: ") + denoiseError);
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

std::uintptr_t DxrReflectionsDispatch::GetDenoisedSrvCpuHandle() const
{
    return m_denoisedThisFrame ? m_dispatchContext.GetReflectionDenoisedSrvCpuHandle() : 0;
}

std::uintptr_t DxrReflectionsDispatch::GetReflectionOutputSrvCpuHandle() const
{
    return m_dispatchContext.GetReflectionOutputSrvCpuHandle();
}

bool DxrReflectionsDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetReflectionOutputSrvCpuHandle() != 0;
}

float DxrReflectionsDispatch::GetOutputUvScaleX() const
{
    const int textureWidth = m_dispatchContext.GetReflectionOutputWidth();
    const int dispatchWidth = m_dispatchContext.GetReflectionDispatchWidth();
    if (textureWidth <= 0 || dispatchWidth <= 0)
    {
        return 1.0f;
    }

    return static_cast<float>(dispatchWidth) / static_cast<float>(textureWidth);
}

float DxrReflectionsDispatch::GetOutputUvScaleY() const
{
    const int textureHeight = m_dispatchContext.GetReflectionOutputHeight();
    const int dispatchHeight = m_dispatchContext.GetReflectionDispatchHeight();
    if (textureHeight <= 0 || dispatchHeight <= 0)
    {
        return 1.0f;
    }

    return static_cast<float>(dispatchHeight) / static_cast<float>(textureHeight);
}
