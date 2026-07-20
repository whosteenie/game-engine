#include "engine/raytracing/dispatch/context/Detail.h"

bool DxrDispatchContext::DispatchPrimaryDebug(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const std::uintptr_t depthSrvCpuHandle,
    const std::uint32_t geometryLookupSrvIndex,
    const std::uint32_t sceneVertexFloatsSrvIndex,
    const std::uint32_t sceneIndicesSrvIndex,
    const int width,
    const int height,
    const DxrRootSignature::PrimaryDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR primary debug dispatch arguments";
        return false;
    }

    const std::uint32_t depthSrvIndex = DepthSrvIndexFromCpuHandle(depthSrvCpuHandle);
    if (depthSrvIndex == UINT32_MAX || geometryLookupSrvIndex == UINT32_MAX
        || sceneVertexFloatsSrvIndex == UINT32_MAX || sceneIndicesSrvIndex == UINT32_MAX)
    {
        outError = "DXR primary debug SRV bindings unavailable";
        return false;
    }

    if (!EnsurePrimaryOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR primary debug constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    if (m_primaryOutputResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_primaryOutputResource,
            static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (m_primaryMetadataResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_primaryMetadataResource,
            static_cast<D3D12_RESOURCE_STATES>(m_primaryMetadataResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_primaryMetadataResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    const std::uint32_t srvIndices[5] = {
        m_tlasSrvIndex,
        depthSrvIndex,
        geometryLookupSrvIndex,
        sceneVertexFloatsSrvIndex,
        sceneIndicesSrvIndex};
    recorder.BindSrvTables(1, srvIndices, 5);
    recorder.BindSrvTable(6, m_primaryOutputUavIndex);
    recorder.BindSrvTable(7, m_primaryMetadataUavIndex);

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryOutputResource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryMetadataResource);

    // Combined read state — these buffers are sampled by the pixel-shader debug blit.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_primaryMetadataResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryMetadataResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    DxrBreadcrumb("dispatch DispatchPrimaryDebug ok");
    return true;
}

bool DxrDispatchContext::DispatchPathTracer(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    const ReflectionDispatchInputs& inputs,
    const int width,
    const int height,
    const DxrRootSignature::ReflectionDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR path tracer dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[8] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR path tracer SRV bindings unavailable";
            return false;
        }
    }

    if (inputs.geometryLookupSrvIndex == UINT32_MAX
        || inputs.sceneVertexFloatsSrvIndex == UINT32_MAX
        || inputs.sceneIndicesSrvIndex == UINT32_MAX
        || inputs.materialSrvIndex == UINT32_MAX)
    {
        outError = "DXR path tracer geometry lookup SRVs unavailable";
        return false;
    }

    if (!EnsurePrimaryOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(inputs.tlasResource, inputs.tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const bool usePsrResolver =
        (static_cast<std::uint32_t>(constants.ptOpticalStabilityFlags + 0.5f) & 4u) != 0u;
    DxrRootSignature::ReflectionDispatchConstants shadingConstants = constants;
    shadingConstants.ptPsrParams[2] = usePsrResolver ? 2.0f : 0.0f;
    shadingConstants.ptPsrParams[3] = m_ptPsrResolvedHistoryValid ? 1.0f : 0.0f;
    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(shadingConstants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR path tracer constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    if (m_primaryOutputResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_primaryOutputResource,
            static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (m_primaryMetadataResourceState != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_primaryMetadataResource,
            static_cast<D3D12_RESOURCE_STATES>(m_primaryMetadataResourceState),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_primaryMetadataResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (m_ptDepthTexture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_ptDepthTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptDepthTexture.state),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_ptDepthTexture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (m_ptMotionTexture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            m_ptMotionTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptMotionTexture.state),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        m_ptMotionTexture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    ReflectionTexture* const ptGuideTextures[] = {
        &m_ptDiffuseAlbedoTexture,
        &m_ptSpecularAlbedoTexture,
        &m_ptNormalRoughnessTexture,
        &m_ptPsrThroughputTexture,
        &m_ptPsrMetadataTexture,
        &m_ptSpecularMotionTexture,
        &m_ptRrPrimaryOwnerTexture,
        &m_ptRrTransmissionOwnerTexture,
        &m_ptOpticalTransmissionOutputTexture,
        &m_ptOpticalTransmissionDepthTexture,
        &m_ptOpticalTransmissionMotionTexture,
        &m_ptOpticalTransmissionDiffuseAlbedoTexture,
        &m_ptOpticalTransmissionSpecularAlbedoTexture,
        &m_ptOpticalTransmissionNormalRoughnessTexture,
        &m_ptDirectTexture,
        &m_ptRestirSurfacePositionDepthTexture,
        &m_ptRestirSurfaceMaterialTexture,
        &m_ptRestirSurfaceAlbedoMetallicTexture};
    for (ReflectionTexture* guide : ptGuideTextures)
    {
        if (guide->state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                guide->resource,
                static_cast<D3D12_RESOURCE_STATES>(guide->state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            guide->state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);

    const int psrResolvedWriteIndex = m_ptPsrResolvedWriteIndex;
    const int psrResolvedPreviousIndex = 1 - psrResolvedWriteIndex;
    StructuredBufferUav& psrResolvedCurrent = m_ptPsrResolvedRecords[psrResolvedWriteIndex];
    StructuredBufferUav& psrResolvedPrevious = m_ptPsrResolvedRecords[psrResolvedPreviousIndex];
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    if (psrResolvedCurrent.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            psrResolvedCurrent.resource,
            static_cast<D3D12_RESOURCE_STATES>(psrResolvedCurrent.state),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        psrResolvedCurrent.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (psrResolvedPrevious.state != static_cast<std::uint32_t>(kAllShaderRead))
    {
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            psrResolvedPrevious.resource,
            static_cast<D3D12_RESOURCE_STATES>(psrResolvedPrevious.state),
            kAllShaderRead);
        psrResolvedPrevious.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    // Path-tracer root signature: t0-t22 plus the previous PSR resolver record at t23.
    constexpr std::uint32_t kPathTracerSrvCount = 24;
    const std::uint32_t giSrvIndex = inputs.giDenoisedSrvCpuHandle != 0
        ? DepthSrvIndexFromCpuHandle(inputs.giDenoisedSrvCpuHandle)
        : srvIndicesFromHandles[5];
    // t14 needs a valid descriptor even when instance motion is unavailable (the shader is told
    // via constants and never reads it then) — geometry lookup is a safe placeholder buffer SRV.
    const std::uint32_t prevTransformsSrvIndex =
        inputs.prevInstanceTransformsSrvIndex != UINT32_MAX
            ? inputs.prevInstanceTransformsSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveLightsSrvIndex =
        inputs.emissiveLightsSrvIndex != UINT32_MAX
            ? inputs.emissiveLightsSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveTrianglesSrvIndex =
        inputs.emissiveTrianglesSrvIndex != UINT32_MAX
            ? inputs.emissiveTrianglesSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveLightAliasSrvIndex =
        inputs.emissiveLightAliasSrvIndex != UINT32_MAX
            ? inputs.emissiveLightAliasSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveTriangleAliasSrvIndex =
        inputs.emissiveTriangleAliasSrvIndex != UINT32_MAX
            ? inputs.emissiveTriangleAliasSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t emissiveLightByInstanceSrvIndex =
        inputs.emissiveLightByInstanceSrvIndex != UINT32_MAX
            ? inputs.emissiveLightByInstanceSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t envImportanceCdfSrvIndex =
        inputs.envImportanceCdfSrvIndex != UINT32_MAX
            ? inputs.envImportanceCdfSrvIndex
            : inputs.geometryLookupSrvIndex;
    const std::uint32_t envEquirectSrvIndex =
        inputs.envEquirectSrvCpuHandle != 0
            ? DepthSrvIndexFromCpuHandle(inputs.envEquirectSrvCpuHandle)
            : srvIndicesFromHandles[6];
    const std::uint32_t psrBoundsSrvIndex = inputs.ptPsrInstanceBoundsSrvIndex != UINT32_MAX
        ? inputs.ptPsrInstanceBoundsSrvIndex
        : inputs.geometryLookupSrvIndex;
    const std::uint32_t srvHeapIndices[kPathTracerSrvCount] = {
        m_tlasSrvIndex,
        srvIndicesFromHandles[0],
        srvIndicesFromHandles[1],
        srvIndicesFromHandles[2],
        inputs.geometryLookupSrvIndex,
        inputs.sceneVertexFloatsSrvIndex,
        inputs.sceneIndicesSrvIndex,
        srvIndicesFromHandles[3],
        srvIndicesFromHandles[4],
        srvIndicesFromHandles[5],
        srvIndicesFromHandles[6],
        srvIndicesFromHandles[7],
        inputs.materialSrvIndex,
        giSrvIndex,
        prevTransformsSrvIndex,
        emissiveLightsSrvIndex,
        envImportanceCdfSrvIndex,
        envEquirectSrvIndex,
        emissiveTrianglesSrvIndex,
        emissiveLightAliasSrvIndex,
        emissiveTriangleAliasSrvIndex,
        emissiveLightByInstanceSrvIndex,
        psrBoundsSrvIndex,
        psrResolvedPrevious.srvIndex};

    if (inputs.giDenoisedSrvCpuHandle != 0 && giSrvIndex == UINT32_MAX)
    {
        outError = "DXR path tracer GI SRV binding unavailable";
        return false;
    }

    if (!HasRestirBuffers()
        || m_restirGiReservoirs[m_restirWriteIndex].uavIndex == UINT32_MAX
        || m_restirReservoirs[m_restirWriteIndex].uavIndex == UINT32_MAX
        || m_ptDirectTexture.uavIndex == UINT32_MAX
        || m_ptRestirSurfacePositionDepthTexture.uavIndex == UINT32_MAX
        || m_ptRestirSurfaceMaterialTexture.uavIndex == UINT32_MAX
        || m_ptRestirSurfaceAlbedoMetallicTexture.uavIndex == UINT32_MAX
        || m_ptOpticalTransmissionOutputTexture.uavIndex == UINT32_MAX
        || m_ptOpticalTransmissionNormalRoughnessTexture.uavIndex == UINT32_MAX
        || m_ptPsrThroughputTexture.uavIndex == UINT32_MAX
        || m_ptPsrMetadataTexture.uavIndex == UINT32_MAX
        || m_ptSpecularMotionTexture.uavIndex == UINT32_MAX
        || m_ptRrPrimaryOwnerTexture.uavIndex == UINT32_MAX
        || m_ptRrTransmissionOwnerTexture.uavIndex == UINT32_MAX
        || psrResolvedCurrent.uavIndex == UINT32_MAX
        || psrResolvedPrevious.srvIndex == UINT32_MAX)
    {
        outError = "DXR path tracer ReSTIR buffer UAVs unavailable";
        return false;
    }

    // u0-u23 established outputs; u24 is the compact deterministic resolver/shading record.
    constexpr std::uint32_t kPathTracerUavCount = 25;
    const std::uint32_t pathTracerUavIndices[kPathTracerUavCount] = {
        m_primaryOutputUavIndex,
        m_ptDepthTexture.uavIndex,
        m_primaryMetadataUavIndex,
        m_ptMotionTexture.uavIndex,
        m_ptDiffuseAlbedoTexture.uavIndex,
        m_ptSpecularAlbedoTexture.uavIndex,
        m_ptNormalRoughnessTexture.uavIndex,
        m_restirGiReservoirs[m_restirWriteIndex].uavIndex,
        m_restirReservoirs[m_restirWriteIndex].uavIndex,
        m_ptDirectTexture.uavIndex,
        m_ptRestirSurfacePositionDepthTexture.uavIndex,
        m_ptRestirSurfaceMaterialTexture.uavIndex,
        m_ptRestirSurfaceAlbedoMetallicTexture.uavIndex,
        m_ptOpticalTransmissionOutputTexture.uavIndex,
        m_ptOpticalTransmissionDepthTexture.uavIndex,
        m_ptOpticalTransmissionMotionTexture.uavIndex,
        m_ptOpticalTransmissionDiffuseAlbedoTexture.uavIndex,
        m_ptOpticalTransmissionSpecularAlbedoTexture.uavIndex,
        m_ptOpticalTransmissionNormalRoughnessTexture.uavIndex,
        m_ptPsrThroughputTexture.uavIndex,
        m_ptPsrMetadataTexture.uavIndex,
        m_ptSpecularMotionTexture.uavIndex,
        m_ptRrPrimaryOwnerTexture.uavIndex,
        m_ptRrTransmissionOwnerTexture.uavIndex,
        psrResolvedCurrent.uavIndex};
    const auto beginAndBind = [&](const std::uint64_t constantsAddress) {
        recorder.BeginDraw(stateObject, rootSignature, constantsAddress);
        recorder.BindSrvTables(1, srvHeapIndices, kPathTracerSrvCount);
        for (std::uint32_t uavIndex = 0; uavIndex < kPathTracerUavCount; ++uavIndex)
        {
            D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
            uavTableHandle.ptr = reinterpret_cast<UINT64>(
                GfxContext::Get().GetSrvHeapGpuHandle(pathTracerUavIndices[uavIndex]));
            commandList->SetComputeRootDescriptorTable(
                1 + kPathTracerSrvCount + uavIndex, uavTableHandle);
        }
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        const D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetComputeRootDescriptorTable(
            1 + kPathTracerSrvCount + kPathTracerUavCount, bindlessHandle);
    };

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    if (usePsrResolver)
    {
        DxrRootSignature::ReflectionDispatchConstants resolverConstants = constants;
        resolverConstants.ptPsrParams[2] = 1.0f;
        resolverConstants.ptPsrParams[3] = m_ptPsrResolvedHistoryValid ? 1.0f : 0.0f;
        const std::uint64_t resolverConstantsAddress =
            AllocateDispatchConstants(resolverConstants);
        if (resolverConstantsAddress == 0)
        {
            outError = "failed to allocate transient PSR resolver constants";
            return false;
        }

        beginAndBind(resolverConstantsAddress);
        static constexpr wchar_t kPsrResolverMarker[] = L"PT.PSR GBuffer Resolve";
        BeginPathTracerGpuEvent(
            commandList, kPsrResolverMarker, static_cast<UINT>(sizeof(kPsrResolverMarker)));
        recorder.DispatchRays(
            shaderBindingTable,
            width,
            height,
            shaderBindingTable.GetPathTracerPsrResolveRaygenGpuAddress());
        EndPathTracerGpuEvent(commandList);
        RecordDxrUavBarrier(
            static_cast<ID3D12GraphicsCommandList*>(commandList), psrResolvedCurrent.resource);
    }

    // Nsight's automatic CSV export identifies the resolver and stochastic shading separately.
    beginAndBind(constantsGpuAddress);
    static constexpr wchar_t kPathTracerDispatchMarker[] = L"PT.Megakernel";
    BeginPathTracerGpuEvent(
        commandList,
        kPathTracerDispatchMarker,
        static_cast<UINT>(sizeof(kPathTracerDispatchMarker)));
    recorder.DispatchRays(shaderBindingTable, width, height);
    EndPathTracerGpuEvent(commandList);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryOutputResource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_primaryMetadataResource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_ptDepthTexture.resource);
    RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), m_ptMotionTexture.resource);
    RecordDxrUavBarrier(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_restirGiReservoirs[m_restirWriteIndex].resource);
    RecordDxrUavBarrier(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_restirReservoirs[m_restirWriteIndex].resource);

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_ptDepthTexture.resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_ptDepthTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_ptMotionTexture.resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_ptMotionTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    for (ReflectionTexture* guide : ptGuideTextures)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), guide->resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            guide->resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        guide->state = static_cast<std::uint32_t>(kAllShaderRead);
    }
    RecordDxrUavBarrier(
        static_cast<ID3D12GraphicsCommandList*>(commandList), psrResolvedCurrent.resource);
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        psrResolvedCurrent.resource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    psrResolvedCurrent.state = static_cast<std::uint32_t>(kAllShaderRead);
    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    TransitionResource(
        static_cast<ID3D12GraphicsCommandList*>(commandList),
        m_primaryMetadataResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryMetadataResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    if (usePsrResolver)
    {
        m_ptPsrResolvedHistoryValid = true;
        m_ptPsrResolvedWriteIndex = psrResolvedPreviousIndex;
    }

    // G4 copy + R2 temporal are owned by DxrPathTracerDispatch after this returns so temporal
    // can run before prev-surface history is overwritten.
    DxrBreadcrumb("dispatch DispatchPathTracer ok");
    return true;
}
