#include "engine/raytracing/DxrSmokeDispatch.h"

#include "engine/platform/EngineLog.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

DxrSmokeDispatch::~DxrSmokeDispatch()
{
    Release();
}

void DxrSmokeDispatch::Release()
{
    m_shaderBindingTable.Release();
    m_pipeline.Release();
    m_dispatchContext.Release();
    m_pipelineReady = false;
}

bool DxrSmokeDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrSmokeDispatch::EnsurePipeline(std::string& outError)
{
    outError.clear();
    if (m_pipelineReady)
    {
        return true;
    }

    DxrBreadcrumb("smoke EnsurePipeline begin");
    if (!m_pipeline.CreateSmokePipeline(outError))
    {
        DxrBreadcrumb("smoke EnsurePipeline failed: CreateSmokePipeline");
        return false;
    }

    if (!m_shaderBindingTable.BuildSmokeTable(m_pipeline.GetProperties(), outError))
    {
        DxrBreadcrumb("smoke EnsurePipeline failed: BuildSmokeTable");
        m_pipeline.Release();
        return false;
    }

    m_pipelineReady = true;
    DxrBreadcrumb("smoke EnsurePipeline ok");
    return true;
}

void DxrSmokeDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const bool dxrEnabled,
    const bool smokeDebugMode,
    void* commandList,
    const int width,
    const int height)
{
    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled || !smokeDebugMode || width <= 0 || height <= 0)
    {
        return;
    }

    if (!accelerationStructures.IsTlasBuilt())
    {
        DxrBreadcrumb("smoke skipped: TLAS not built");
        return;
    }

    ID3D12GraphicsCommandList4* commandList4 = DxrContext::Get().QueryCommandList4(commandList);
    if (commandList4 == nullptr)
    {
        DxrBreadcrumb("smoke skipped: CommandList4 unavailable");
        return;
    }

    if (!m_pipelineReady)
    {
        DxrBreadcrumb("smoke skipped: pipeline not ready");
        return;
    }

    DxrBreadcrumb("smoke dispatch begin");
    SceneRenderTrace::Scope dispatchScope("dxr-dispatch-smoke");

    std::string error;

    DxrRootSignature::DispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(width);
    constants.outputHeight = static_cast<std::uint32_t>(height);
    constants.clearColor[0] = 1.0f;
    constants.clearColor[1] = 0.0f;
    constants.clearColor[2] = 1.0f;
    constants.clearColor[3] = 1.0f;

    if (!m_dispatchContext.DispatchSmoke(
            commandList4,
            m_pipeline.GetStateObject(),
            m_pipeline.GetGlobalRootSignature(),
            m_shaderBindingTable,
            accelerationStructures.GetTlasResource(),
            accelerationStructures.GetTlasGpuVirtualAddress(),
            width,
            height,
            constants,
            error))
    {
        const std::string failureMessage =
            std::string("smoke dispatch failed: DispatchSmoke (") + error + ")";
        DxrLogErrorOnce("dispatch-smoke-failure", failureMessage);
        DxrBreadcrumbOnce("dispatch-smoke-failure", failureMessage);
        dispatchScope.Success();
        DxrEnableTrustMode();
        return;
    }

    DxrBreadcrumb("smoke dispatch ok");
    dispatchScope.Success();
    DxrEnableTrustMode();
}

std::uintptr_t DxrSmokeDispatch::GetOutputSrvCpuHandle() const
{
    return m_dispatchContext.GetOutputSrvCpuHandle();
}

bool DxrSmokeDispatch::HasValidOutput() const
{
    return m_dispatchContext.GetOutputSrvCpuHandle() != 0;
}
