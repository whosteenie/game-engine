#include "engine/raytracing/dispatch/context/Detail.h"

void DxrDispatchContext::ReleaseRetiredPrimaryOutputs()
{
    for (RetiredOutput& retired : m_retiredPrimaryOutputs)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }

    m_retiredPrimaryOutputs.clear();
}

void DxrDispatchContext::CreatePrimaryOutputDescriptors()
{
    if (m_primaryOutputResource == nullptr || m_primaryMetadataResource == nullptr)
    {
        return;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE outputSrvHandle{};
    outputSrvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_primaryOutputSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC outputSrvDesc{};
    outputSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    outputSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outputSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    outputSrvDesc.Texture2D.MipLevels = 1;
    GfxContext::Get().CreateShaderResourceView(m_primaryOutputResource, &outputSrvDesc, m_primaryOutputSrvIndex);
    m_primaryOutputSrvCpuHandle = outputSrvHandle.ptr;

    D3D12_UNORDERED_ACCESS_VIEW_DESC outputUavDesc{};
    outputUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    outputUavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    GfxContext::Get().CreateUnorderedAccessView(
        m_primaryOutputResource, nullptr, &outputUavDesc, m_primaryOutputUavIndex);

    D3D12_CPU_DESCRIPTOR_HANDLE metadataSrvHandle{};
    metadataSrvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_primaryMetadataSrvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC metadataSrvDesc{};
    metadataSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    metadataSrvDesc.Format = DXGI_FORMAT_R32G32_UINT;
    metadataSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    metadataSrvDesc.Texture2D.MipLevels = 1;
    GfxContext::Get().CreateShaderResourceView(
        m_primaryMetadataResource, &metadataSrvDesc, m_primaryMetadataSrvIndex);
    m_primaryMetadataSrvCpuHandle = metadataSrvHandle.ptr;

    D3D12_UNORDERED_ACCESS_VIEW_DESC metadataUavDesc{};
    metadataUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    metadataUavDesc.Format = DXGI_FORMAT_R32G32_UINT;
    GfxContext::Get().CreateUnorderedAccessView(
        m_primaryMetadataResource, nullptr, &metadataUavDesc, m_primaryMetadataUavIndex);
}

bool DxrDispatchContext::EnsurePrimaryOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid primary debug output dimensions";
        return false;
    }

    ReleaseRetiredPrimaryOutputs();
    // PT guides retire into m_retiredReflectionOutputs (shared helper). Drain them here —
    // path-tracing mode never calls EnsureReflectionOutput, so without this every resize leaked
    // descriptors/GPU memory (G4 doubled the leak rate) until the heap exhausted and PT/DLSS
    // permanently failed.
    ReleaseRetiredReflectionOutputs();
    ReleaseRetiredRestirBuffers();
    for (RetiredOutput& retired : m_retiredPrimaryMetadata)
    {
        DestroyOutputResource(
            retired.resource,
            retired.allocation,
            retired.srvIndex,
            retired.uavIndex);
    }
    m_retiredPrimaryMetadata.clear();

    if (m_primaryOutputResource != nullptr && m_primaryOutputWidth == width
        && m_primaryOutputHeight == height)
    {
        // Exact size match required: PT depth/motion/guides are tagged into DLSS-RR with
        // renderWidth/Height. A larger kept allocation (grow-only) makes NGX return
        // 0xbad00005 InvalidParameter ("subrect/size exceed dimensions").
        return EnsurePathTracerGuides(width, height, outError);
    }

    if (m_primaryOutputResource != nullptr)
    {
        RetiredOutput retiredOutput{};
        retiredOutput.resource = m_primaryOutputResource;
        retiredOutput.allocation = m_primaryOutputAllocation;
        retiredOutput.srvIndex = m_primaryOutputSrvIndex;
        retiredOutput.uavIndex = m_primaryOutputUavIndex;
        m_retiredPrimaryOutputs.push_back(retiredOutput);

        RetiredOutput retiredMetadata{};
        retiredMetadata.resource = m_primaryMetadataResource;
        retiredMetadata.allocation = m_primaryMetadataAllocation;
        retiredMetadata.srvIndex = m_primaryMetadataSrvIndex;
        retiredMetadata.uavIndex = m_primaryMetadataUavIndex;
        m_retiredPrimaryMetadata.push_back(retiredMetadata);

        m_primaryOutputResource = nullptr;
        m_primaryOutputAllocation = nullptr;
        m_primaryMetadataResource = nullptr;
        m_primaryMetadataAllocation = nullptr;
        m_primaryOutputSrvIndex = UINT32_MAX;
        m_primaryOutputUavIndex = UINT32_MAX;
        m_primaryMetadataSrvIndex = UINT32_MAX;
        m_primaryMetadataUavIndex = UINT32_MAX;
        m_primaryOutputSrvCpuHandle = 0;
        m_primaryMetadataSrvCpuHandle = 0;
        m_primaryOutputWidth = 0;
        m_primaryOutputHeight = 0;
        m_primaryOutputResourceState = 0;
        m_primaryMetadataResourceState = 0;
        RetireOrDestroyReflectionTexture(m_ptDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptMotionTexture);
        RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
        RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
        RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
        RetireOrDestroyReflectionTexture(m_ptPsrThroughputTexture);
        RetireOrDestroyReflectionTexture(m_ptPsrMetadataTexture);
        RetireOrDestroyReflectionTexture(m_ptSpecularMotionTexture);
        RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionOutputTexture);
        RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionMotionTexture);
        RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDiffuseAlbedoTexture);
        RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionSpecularAlbedoTexture);
        RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionNormalRoughnessTexture);
        RetireOrDestroyReflectionTexture(m_ptDirectTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
        RetireOrDestroyReflectionTexture(m_ptRestirSurfacePositionDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptRestirSurfaceMaterialTexture);
        RetireOrDestroyReflectionTexture(m_ptRestirSurfaceAlbedoMetallicTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfacePositionDepthTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceMaterialTexture);
        RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceAlbedoMetallicTexture);
        m_ptPrevSurfaceHistoryValid = false;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (allocator == nullptr || device == nullptr)
    {
        outError = "GfxContext unavailable for DXR primary debug output textures";
        return false;
    }

    D3D12_RESOURCE_DESC outputDesc{};
    outputDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    outputDesc.Width = static_cast<UINT64>(width);
    outputDesc.Height = static_cast<UINT>(height);
    outputDesc.DepthOrArraySize = 1;
    outputDesc.MipLevels = 1;
    outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    outputDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &outputDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &m_primaryOutputAllocation,
            IID_PPV_ARGS(&m_primaryOutputResource))))
    {
        outError = "failed to allocate DXR primary debug output texture";
        return false;
    }

    D3D12_RESOURCE_DESC metadataDesc = outputDesc;
    metadataDesc.Format = DXGI_FORMAT_R32G32_UINT;
    if (FAILED(allocator->CreateResource(
            &allocationDesc,
            &metadataDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            &m_primaryMetadataAllocation,
            IID_PPV_ARGS(&m_primaryMetadataResource))))
    {
        outError = "failed to allocate DXR primary debug metadata texture";
        DestroyOutputResource(
            m_primaryOutputResource,
            m_primaryOutputAllocation,
            m_primaryOutputSrvIndex,
            m_primaryOutputUavIndex);
        return false;
    }

    m_primaryOutputWidth = width;
    m_primaryOutputHeight = height;
    m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);
    m_primaryMetadataResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);

    m_primaryOutputSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_primaryOutputUavIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_primaryMetadataSrvIndex = GfxContext::Get().AllocateOffscreenSrv();
    m_primaryMetadataUavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (m_primaryOutputSrvIndex == UINT32_MAX || m_primaryOutputUavIndex == UINT32_MAX
        || m_primaryMetadataSrvIndex == UINT32_MAX || m_primaryMetadataUavIndex == UINT32_MAX)
    {
        outError = "failed to allocate DXR primary debug output descriptors";
        DestroyOutputResource(
            m_primaryOutputResource,
            m_primaryOutputAllocation,
            m_primaryOutputSrvIndex,
            m_primaryOutputUavIndex);
        DestroyOutputResource(
            m_primaryMetadataResource,
            m_primaryMetadataAllocation,
            m_primaryMetadataSrvIndex,
            m_primaryMetadataUavIndex);
        return false;
    }

    CreatePrimaryOutputDescriptors();
    if (!EnsurePathTracerGuides(width, height, outError))
    {
        return false;
    }
    return true;
}

bool DxrDispatchContext::EnsurePathTracerGuides(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid path tracer guide dimensions";
        return false;
    }

    if (m_ptDepthTexture.resource != nullptr && m_ptNormalRoughnessTexture.resource != nullptr
        && m_ptPsrThroughputTexture.resource != nullptr
        && m_ptPsrMetadataTexture.resource != nullptr
        && m_ptSpecularMotionTexture.resource != nullptr
        && m_ptRrPrimaryOwnerTexture.resource != nullptr
        && m_ptRrTransmissionOwnerTexture.resource != nullptr
        && m_ptOpticalTransmissionOutputTexture.resource != nullptr
        && m_ptOpticalTransmissionNormalRoughnessTexture.resource != nullptr
        && m_ptDirectTexture.resource != nullptr
        && m_ptPrevDepthTexture.resource != nullptr
        && m_ptPrevNormalRoughnessTexture.resource != nullptr
        && m_ptRestirSurfacePositionDepthTexture.resource != nullptr
        && m_ptRestirSurfaceMaterialTexture.resource != nullptr
        && m_ptRestirSurfaceAlbedoMetallicTexture.resource != nullptr
        && m_ptPrevRestirSurfacePositionDepthTexture.resource != nullptr
        && m_ptPrevRestirSurfaceMaterialTexture.resource != nullptr
        && m_ptPrevRestirSurfaceAlbedoMetallicTexture.resource != nullptr
        && m_primaryOutputWidth == width && m_primaryOutputHeight == height)
    {
        return EnsureRestirBuffers(width, height, outError);
    }

    RetireOrDestroyReflectionTexture(m_ptDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptPsrThroughputTexture);
    RetireOrDestroyReflectionTexture(m_ptPsrMetadataTexture);
    RetireOrDestroyReflectionTexture(m_ptSpecularMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptRrPrimaryOwnerTexture);
    RetireOrDestroyReflectionTexture(m_ptRrTransmissionOwnerTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionOutputTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionMotionTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDiffuseAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionSpecularAlbedoTexture);
    RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptDirectTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
    RetireOrDestroyReflectionTexture(m_ptRestirSurfacePositionDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptRestirSurfaceMaterialTexture);
    RetireOrDestroyReflectionTexture(m_ptRestirSurfaceAlbedoMetallicTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfacePositionDepthTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceMaterialTexture);
    RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceAlbedoMetallicTexture);
    m_ptPrevSurfaceHistoryValid = false;

    // Formats match the RR internal targets they are copied into (rr_guides parity —
    // devdoc/dxr/pt/full-rr-guides.md buffer contract). Prev-surface history (G4) mirrors depth +
    // normal/roughness for ReSTIR temporal validation. R2 direct is HDR bounce-0 lighting only.
    struct GuideDesc
    {
        DXGI_FORMAT format;
        ReflectionTexture* texture;
    };
    const GuideDesc guides[] = {
        {DXGI_FORMAT_R32_FLOAT, &m_ptDepthTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptMotionTexture},
        {DXGI_FORMAT_R8G8B8A8_UNORM, &m_ptDiffuseAlbedoTexture},
        {DXGI_FORMAT_R8G8B8A8_UNORM, &m_ptSpecularAlbedoTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptNormalRoughnessTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptPsrThroughputTexture},
        {DXGI_FORMAT_R32_UINT, &m_ptPsrMetadataTexture},
        {DXGI_FORMAT_R16G16_FLOAT, &m_ptSpecularMotionTexture},
        {DXGI_FORMAT_R32_UINT, &m_ptRrPrimaryOwnerTexture},
        {DXGI_FORMAT_R32_UINT, &m_ptRrTransmissionOwnerTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptOpticalTransmissionOutputTexture},
        {DXGI_FORMAT_R32_FLOAT, &m_ptOpticalTransmissionDepthTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptOpticalTransmissionMotionTexture},
        {DXGI_FORMAT_R8G8B8A8_UNORM, &m_ptOpticalTransmissionDiffuseAlbedoTexture},
        {DXGI_FORMAT_R8G8B8A8_UNORM, &m_ptOpticalTransmissionSpecularAlbedoTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptOpticalTransmissionNormalRoughnessTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptDirectTexture},
        {DXGI_FORMAT_R32_FLOAT, &m_ptPrevDepthTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptPrevNormalRoughnessTexture},
        {DXGI_FORMAT_R32G32B32A32_FLOAT, &m_ptRestirSurfacePositionDepthTexture},
        {DXGI_FORMAT_R32G32B32A32_UINT, &m_ptRestirSurfaceMaterialTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptRestirSurfaceAlbedoMetallicTexture},
        {DXGI_FORMAT_R32G32B32A32_FLOAT, &m_ptPrevRestirSurfacePositionDepthTexture},
        {DXGI_FORMAT_R32G32B32A32_UINT, &m_ptPrevRestirSurfaceMaterialTexture},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &m_ptPrevRestirSurfaceAlbedoMetallicTexture},
    };
    for (const GuideDesc& guide : guides)
    {
        if (!CreateReflectionTexture(
                width,
                height,
                static_cast<std::uint32_t>(guide.format),
                *guide.texture,
                outError))
        {
            RetireOrDestroyReflectionTexture(m_ptDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptMotionTexture);
            RetireOrDestroyReflectionTexture(m_ptDiffuseAlbedoTexture);
            RetireOrDestroyReflectionTexture(m_ptSpecularAlbedoTexture);
            RetireOrDestroyReflectionTexture(m_ptNormalRoughnessTexture);
            RetireOrDestroyReflectionTexture(m_ptPsrThroughputTexture);
            RetireOrDestroyReflectionTexture(m_ptPsrMetadataTexture);
            RetireOrDestroyReflectionTexture(m_ptSpecularMotionTexture);
            RetireOrDestroyReflectionTexture(m_ptRrPrimaryOwnerTexture);
            RetireOrDestroyReflectionTexture(m_ptRrTransmissionOwnerTexture);
            RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionOutputTexture);
            RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionMotionTexture);
            RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionDiffuseAlbedoTexture);
            RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionSpecularAlbedoTexture);
            RetireOrDestroyReflectionTexture(m_ptOpticalTransmissionNormalRoughnessTexture);
            RetireOrDestroyReflectionTexture(m_ptDirectTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevNormalRoughnessTexture);
            RetireOrDestroyReflectionTexture(m_ptRestirSurfacePositionDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptRestirSurfaceMaterialTexture);
            RetireOrDestroyReflectionTexture(m_ptRestirSurfaceAlbedoMetallicTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfacePositionDepthTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceMaterialTexture);
            RetireOrDestroyReflectionTexture(m_ptPrevRestirSurfaceAlbedoMetallicTexture);
            m_ptPrevSurfaceHistoryValid = false;
            return false;
        }
    }

    return EnsureRestirBuffers(width, height, outError);
}

