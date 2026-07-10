#include "engine/raytracing/DxrRestirDispatch.h"

#include "engine/raytracing/DxrTrace.h"

DxrRestirDispatch::~DxrRestirDispatch()
{
    Release();
}

void DxrRestirDispatch::Release()
{
    m_spatialShaderBindingTable.Release();
    m_spatialPipelineReady = false;
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
    if (m_pipelineReady && m_spatialPipelineReady)
    {
        return true;
    }

    // One RTPSO exports both raygens; temporal SBT uses the base table, spatial uses a second.
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

    return true;
}
