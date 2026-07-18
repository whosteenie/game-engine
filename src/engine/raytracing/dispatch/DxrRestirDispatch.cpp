#include "engine/raytracing/dispatch/DxrRestirDispatch.h"

#include "engine/raytracing/core/DxrTrace.h"

DxrRestirDispatch::~DxrRestirDispatch()
{
    Release();
}

void DxrRestirDispatch::Release()
{
    m_spatialShaderBindingTable.Release();
    m_giBoilingFilterShaderBindingTable.Release();
    m_spatialPipelineReady = false;
    m_giBoilingFilterPipelineReady = false;
    ReleaseCore();
}

bool DxrRestirDispatch::WarmUpPipelineIfNeeded()
{
    std::string error;
    return EnsurePipelines(error);
}

bool DxrRestirDispatch::EnsurePipelines(std::string& outError)
{
    outError.clear();
    if (m_pipelineReady && m_spatialPipelineReady && m_giBoilingFilterPipelineReady)
    {
        return true;
    }

    // One RTPSO exports temporal, GI boiling-filter, and spatial raygens, each with its own SBT.
    if (!EnsurePipelineWith(
            "restir-temporal",
            [](DxrPipeline& pipeline, std::string& pipelineError) {
                return pipeline.CreateRestirPipeline(pipelineError);
            },
            [](ShaderBindingTable& shaderBindingTable, const DxrPipeline& pipeline, std::string& tableError) {
                return shaderBindingTable.BuildRestirTemporalTable(pipeline.GetProperties(), tableError);
            },
            outError))
    {
        m_spatialPipelineReady = false;
        return false;
    }

    if (!m_spatialPipelineReady)
    {
        DxrBreadcrumb("restir-spatial EnsurePipeline begin");
        if (!m_spatialShaderBindingTable.BuildRestirSpatialTable(m_pipeline.GetProperties(), outError))
        {
            DxrBreadcrumb("restir-spatial EnsurePipeline failed: build SBT");
            m_spatialShaderBindingTable.Release();
            m_shaderBindingTable.Release();
            m_pipeline.Release();
            m_pipelineReady = false;
            return false;
        }
        m_spatialPipelineReady = true;
        DxrBreadcrumb("restir-spatial EnsurePipeline ok");
    }

    if (!m_giBoilingFilterPipelineReady)
    {
        DxrBreadcrumb("restir-gi-boiling-filter EnsurePipeline begin");
        if (!m_giBoilingFilterShaderBindingTable.BuildRestirGiBoilingFilterTable(
                m_pipeline.GetProperties(), outError))
        {
            DxrBreadcrumb("restir-gi-boiling-filter EnsurePipeline failed: build SBT");
            m_giBoilingFilterShaderBindingTable.Release();
            m_spatialShaderBindingTable.Release();
            m_shaderBindingTable.Release();
            m_pipeline.Release();
            m_pipelineReady = false;
            m_spatialPipelineReady = false;
            return false;
        }
        m_giBoilingFilterPipelineReady = true;
        DxrBreadcrumb("restir-gi-boiling-filter EnsurePipeline ok");
    }

    return true;
}

void DxrRestirDispatch::ResetProjectResources()
{
    ResetDispatchResources();
}
