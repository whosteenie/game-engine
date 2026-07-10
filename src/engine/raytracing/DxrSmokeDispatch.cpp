#include "engine/raytracing/DxrSmokeDispatch.h"

#include "engine/platform/EngineLog.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrRootSignature.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rhi/GfxContext.h"

DxrSmokeDispatch::~DxrSmokeDispatch()
{
    Release();
}

void DxrSmokeDispatch::Release()
{
    ReleaseCore();
}

bool DxrSmokeDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipeline(error);
}

bool DxrSmokeDispatch::EnsurePipeline(std::string& outError)
{
    return EnsurePipelineWith(
        "smoke",
        [](DxrPipeline& pipeline, std::string& pipelineError) {
            return pipeline.CreateSmokePipeline(pipelineError);
        },
        [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
            return shaderBindingTable.BuildSmokeTable(pipeline.GetProperties(), tableError);
        },
        outError);
}

void DxrSmokeDispatch::DispatchIfEnabled(
    const DxrAccelerationStructures& accelerationStructures,
    const bool dxrEnabled,
    const bool smokeDebugMode,
    void* commandList,
    const int width,
    const int height)
{
    if (!smokeDebugMode)
    {
        return;
    }

    ID3D12GraphicsCommandList4* commandList4 = ResolveDispatchCommandList(
        accelerationStructures,
        dxrEnabled,
        width,
        height,
        commandList,
        DxrDispatchGeometryRequirement::TlasOnly,
        "smoke");
    if (commandList4 == nullptr)
    {
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
