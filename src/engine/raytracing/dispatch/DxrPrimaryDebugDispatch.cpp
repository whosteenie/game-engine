#include "engine/raytracing/dispatch/DxrPrimaryDebugDispatch.h"

#include "engine/camera/Camera.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/raytracing/acceleration/DxrAccelerationStructures.h"
#include "engine/raytracing/pipeline/DxrRootSignature.h"
#include "engine/raytracing/core/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstring>

DxrPrimaryDebugDispatch::~DxrPrimaryDebugDispatch()
{
    Release();
}

void DxrPrimaryDebugDispatch::Release()
{
    ReleaseCore();
    m_dispatchedThisFrame = false;
}

void DxrPrimaryDebugDispatch::ResetProjectResources()
{
    ResetDispatchResources();
    m_dispatchedThisFrame = false;
}

bool DxrPrimaryDebugDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrPrimaryDebugDispatch::EnsurePipeline(std::string& outError)
{
    return EnsurePipelineWith(
        "primary-debug",
        [](DxrPipeline& pipeline, std::string& pipelineError) {
            return pipeline.CreatePrimaryDebugPipeline(pipelineError);
        },
        [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
            return shaderBindingTable.BuildPrimaryDebugTable(pipeline.GetProperties(), tableError);
        },
        outError);
}

bool DxrPrimaryDebugDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const Camera& camera,
    const bool dxrEnabled,
    const bool debugTraceEnabled,
    const bool primaryDebugViewActive,
    void* commandList,
    const std::uintptr_t depthSrvCpuHandle,
    const int width,
    const int height,
    const float maxTraceDistance)
{
    m_dispatchedThisFrame = false;

    if (!debugTraceEnabled && !primaryDebugViewActive)
    {
        return false;
    }

    ID3D12GraphicsCommandList4* commandList4 = ResolveDispatchCommandList(
        accelerationStructures,
        dxrEnabled,
        width,
        height,
        commandList,
        DxrDispatchGeometryRequirement::TlasAndGeometryLookup,
        "primary-debug");
    if (commandList4 == nullptr)
    {
        return false;
    }

    std::string error;
    if (!m_pipelineReady && !EnsurePipeline(error))
    {
        DxrBreadcrumb("primary-debug skipped: pipeline not ready");
        return false;
    }

    DxrBreadcrumb("primary-debug dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-primary-debug");

    const glm::mat4 viewMatrix = camera.GetViewMatrix();
    const glm::mat4 projectionMatrix = camera.GetProjectionMatrix();
    const glm::mat4 viewProj = projectionMatrix * viewMatrix;
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::vec3 cameraPos = camera.GetPosition();

    DxrRootSignature::PrimaryDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(width);
    constants.outputHeight = static_cast<std::uint32_t>(height);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.viewProj, glm::value_ptr(viewProj), sizeof(constants.viewProj));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.nearPlane = camera.GetNearPlane();
    constants.farPlane = camera.GetFarPlane();
    constants.maxTraceDistance = maxTraceDistance;

    if (!m_dispatchContext.DispatchPrimaryDebug(
            commandList4,
            m_pipeline.GetStateObject(),
            m_pipeline.GetGlobalRootSignature(),
            m_shaderBindingTable,
            accelerationStructures.GetTlasResource(),
            accelerationStructures.GetTlasGpuVirtualAddress(),
            depthSrvCpuHandle,
            accelerationStructures.GetGeometryLookupSrvIndex(),
            accelerationStructures.GetSceneVertexFloatsSrvIndex(),
            accelerationStructures.GetSceneIndicesSrvIndex(),
            width,
            height,
            constants,
            error))
    {
        const std::string failureMessage =
            std::string("primary debug dispatch failed: DispatchPrimaryDebug (") + error + ")";
        DxrLogErrorOnce("dispatch-primary-debug-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-primary-debug-failure", failureMessage);
        dispatchScope.Success();
        DxrEnableTrustMode();
        return false;
    }

    DxrBreadcrumb("primary-debug dispatch ok");
    dispatchScope.Success();
    DxrEnableTrustMode();
    m_dispatchedThisFrame = true;
    return true;
}

std::uintptr_t DxrPrimaryDebugDispatch::GetPrimaryOutputSrvCpuHandle() const
{
    return m_dispatchContext.GetPrimaryOutputSrvCpuHandle();
}

std::uintptr_t DxrPrimaryDebugDispatch::GetPrimaryMetadataSrvCpuHandle() const
{
    return m_dispatchContext.GetPrimaryMetadataSrvCpuHandle();
}

bool DxrPrimaryDebugDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetPrimaryOutputSrvCpuHandle() != 0
        && m_dispatchContext.GetPrimaryMetadataSrvCpuHandle() != 0;
}
