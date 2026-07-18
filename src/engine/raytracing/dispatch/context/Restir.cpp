#include "engine/raytracing/dispatch/context/Detail.h"

bool DxrDispatchContext::EnsureRestirBuffers(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid ReSTIR buffer dimensions";
        return false;
    }

    const std::uint32_t elementCount = static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height);
    if (HasRestirBuffers() && m_restirBufferWidth == width && m_restirBufferHeight == height
        && m_restirElementCount == elementCount)
    {
        return true;
    }

    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[0]);
    RetireOrDestroyStructuredBufferUav(m_restirReservoirs[1]);
    RetireOrDestroyStructuredBufferUav(m_restirGiReservoirs[0]);
    RetireOrDestroyStructuredBufferUav(m_restirGiReservoirs[1]);
    m_restirWriteIndex = 0;
    m_restirReservoirHistoryValid = false;
    m_restirLastSceneVersion = 0;
    m_restirLastMotionVersion = 0;
    m_restirBufferWidth = 0;
    m_restirBufferHeight = 0;
    m_restirElementCount = 0;

    if (!CreateStructuredBufferUav(
            elementCount, sizeof(RestirDiReservoirSet), m_restirReservoirs[0], outError)
        || !CreateStructuredBufferUav(
            elementCount, sizeof(RestirDiReservoirSet), m_restirReservoirs[1], outError)
        || !CreateStructuredBufferUav(
            elementCount, sizeof(RestirGiReservoir), m_restirGiReservoirs[0], outError)
        || !CreateStructuredBufferUav(
            elementCount, sizeof(RestirGiReservoir), m_restirGiReservoirs[1], outError))
    {
        RetireOrDestroyStructuredBufferUav(m_restirReservoirs[0]);
        RetireOrDestroyStructuredBufferUav(m_restirReservoirs[1]);
        RetireOrDestroyStructuredBufferUav(m_restirGiReservoirs[0]);
        RetireOrDestroyStructuredBufferUav(m_restirGiReservoirs[1]);
        return false;
    }

    m_restirBufferWidth = width;
    m_restirBufferHeight = height;
    m_restirElementCount = elementCount;
    return true;
}

bool DxrDispatchContext::CreateStructuredBufferUav(
    const std::uint32_t elementCount,
    const std::uint32_t structureByteStride,
    StructuredBufferUav& outBuffer,
    std::string& outError)
{
    outError.clear();
    outBuffer = StructuredBufferUav{};

    if (elementCount == 0 || structureByteStride == 0)
    {
        outError = "invalid structured buffer parameters";
        return false;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr || !GfxContext::Get().IsInitialized())
    {
        outError = "GfxContext unavailable for ReSTIR structured buffer";
        return false;
    }

    DxrGpuResource gpuResource{};
    const std::uint64_t byteSize =
        static_cast<std::uint64_t>(elementCount) * static_cast<std::uint64_t>(structureByteStride);
    if (!CreateDxrDefaultBuffer(
            byteSize, true, gpuResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    {
        outError = "failed to allocate ReSTIR structured buffer ("
            + std::to_string(elementCount) + " x " + std::to_string(structureByteStride) + ")";
        return false;
    }

    outBuffer.resource = gpuResource.resource;
    outBuffer.allocation = gpuResource.allocation;
    outBuffer.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    outBuffer.elementCount = elementCount;
    outBuffer.structureByteStride = structureByteStride;
    // Ownership transferred into outBuffer; clear so DxrGpuResource destructor does not release.
    gpuResource.resource = nullptr;
    gpuResource.allocation = nullptr;
    gpuResource.sizeInBytes = 0;

    outBuffer.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    outBuffer.uavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (outBuffer.srvIndex == UINT32_MAX || outBuffer.uavIndex == UINT32_MAX)
    {
        outError = "failed to allocate ReSTIR structured buffer descriptors";
        DestroyOutputResource(
            outBuffer.resource, outBuffer.allocation, outBuffer.srvIndex, outBuffer.uavIndex);
        outBuffer = StructuredBufferUav{};
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outBuffer.srvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.FirstElement = 0;
    srvDesc.Buffer.NumElements = elementCount;
    srvDesc.Buffer.StructureByteStride = structureByteStride;
    GfxContext::Get().CreateShaderResourceView(outBuffer.resource, &srvDesc, outBuffer.srvIndex);
    outBuffer.srvCpuHandle = srvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{};
    uavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outBuffer.uavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = elementCount;
    uavDesc.Buffer.StructureByteStride = structureByteStride;
    GfxContext::Get().CreateUnorderedAccessView(outBuffer.resource, nullptr, &uavDesc, outBuffer.uavIndex);
    outBuffer.uavCpuHandle = uavHandle.ptr;
    return true;
}

void DxrDispatchContext::RetireOrDestroyStructuredBufferUav(StructuredBufferUav& buffer)
{
    if (buffer.resource == nullptr)
    {
        return;
    }

    if (GfxContext::Get().IsFrameRecording())
    {
        RetiredOutput retired{};
        retired.resource = buffer.resource;
        retired.allocation = buffer.allocation;
        retired.srvIndex = buffer.srvIndex;
        retired.uavIndex = buffer.uavIndex;
        m_retiredRestirBuffers.push_back(retired);
        buffer = StructuredBufferUav{};
        return;
    }

    DestroyOutputResource(buffer.resource, buffer.allocation, buffer.srvIndex, buffer.uavIndex);
    buffer = StructuredBufferUav{};
}

void DxrDispatchContext::CopyPathTracerSurfaceHistory(ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr || m_ptDepthTexture.resource == nullptr
        || m_ptNormalRoughnessTexture.resource == nullptr || m_ptPrevDepthTexture.resource == nullptr
        || m_ptPrevNormalRoughnessTexture.resource == nullptr
        || m_ptRestirSurfacePositionDepthTexture.resource == nullptr
        || m_ptRestirSurfaceMaterialTexture.resource == nullptr
        || m_ptPrevRestirSurfacePositionDepthTexture.resource == nullptr
        || m_ptPrevRestirSurfaceMaterialTexture.resource == nullptr
        || m_ptPrevRestirSurfaceAlbedoMetallicTexture.resource == nullptr)
    {
        return;
    }

    // Keep the history copies separate from ReSTIR reuse in captures. This scope deliberately
    // includes only the guide transitions and copies recorded by this routine.
    static constexpr wchar_t kPathTracerSurfaceHistoryMarker[] = L"PT.SurfaceHistory";
    BeginPathTracerGpuEvent(
        commandList,
        kPathTracerSurfaceHistoryMarker,
        static_cast<UINT>(sizeof(kPathTracerSurfaceHistoryMarker)));

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    auto copyGuide = [&](ReflectionTexture& current, ReflectionTexture& prev) {
        TransitionResource(
            commandList,
            current.resource,
            static_cast<D3D12_RESOURCE_STATES>(current.state),
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        current.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_SOURCE);

        const D3D12_RESOURCE_STATES prevBefore =
            prev.state == 0
                ? D3D12_RESOURCE_STATE_COMMON
                : static_cast<D3D12_RESOURCE_STATES>(prev.state);
        TransitionResource(
            commandList,
            prev.resource,
            prevBefore,
            D3D12_RESOURCE_STATE_COPY_DEST);
        prev.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COPY_DEST);

        commandList->CopyResource(prev.resource, current.resource);

        TransitionResource(
            commandList,
            current.resource,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            kAllShaderRead);
        current.state = static_cast<std::uint32_t>(kAllShaderRead);
        TransitionResource(
            commandList,
            prev.resource,
            D3D12_RESOURCE_STATE_COPY_DEST,
            kAllShaderRead);
        prev.state = static_cast<std::uint32_t>(kAllShaderRead);
    };

    copyGuide(m_ptDepthTexture, m_ptPrevDepthTexture);
    copyGuide(m_ptNormalRoughnessTexture, m_ptPrevNormalRoughnessTexture);
    copyGuide(m_ptRestirSurfacePositionDepthTexture, m_ptPrevRestirSurfacePositionDepthTexture);
    copyGuide(m_ptRestirSurfaceMaterialTexture, m_ptPrevRestirSurfaceMaterialTexture);
    copyGuide(m_ptRestirSurfaceAlbedoMetallicTexture, m_ptPrevRestirSurfaceAlbedoMetallicTexture);
    m_ptPrevSurfaceHistoryValid = true;
    EndPathTracerGpuEvent(commandList);
}

void DxrDispatchContext::FinalizePathTracerSurfaceHistory(ID3D12GraphicsCommandList* commandList)
{
    CopyPathTracerSurfaceHistory(commandList);
}

void DxrDispatchContext::InvalidateRestirHistoryIfSceneChanged(
    const std::uint32_t sceneVersion,
    const std::uint32_t motionVersion)
{
    if (sceneVersion != m_restirLastSceneVersion)
    {
        m_restirReservoirHistoryValid = false;
    }

    m_restirLastSceneVersion = sceneVersion;
    m_restirLastMotionVersion = motionVersion;
}

bool DxrDispatchContext::DispatchRestirTemporal(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const std::uint32_t emissiveLightsSrvIndex,
    const std::uint32_t emissiveTrianglesSrvIndex,
    const std::uint32_t envCdfSrvIndex,
    const std::uintptr_t envMapSrvCpuHandle,
    const DxrRootSignature::RestirTemporalConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid ReSTIR temporal dispatch arguments";
        return false;
    }
    if (!HasRestirBuffers() || m_primaryOutputResource == nullptr || m_primaryOutputUavIndex == UINT32_MAX
        || m_ptDirectTexture.resource == nullptr || m_ptDirectTexture.srvIndex == UINT32_MAX
        || m_ptRestirSurfaceAlbedoMetallicTexture.resource == nullptr
        || m_ptRestirSurfaceAlbedoMetallicTexture.srvIndex == UINT32_MAX
        || m_ptPrevRestirSurfaceAlbedoMetallicTexture.resource == nullptr
        || m_ptPrevRestirSurfaceAlbedoMetallicTexture.srvIndex == UINT32_MAX)
    {
        outError = "ReSTIR temporal buffers unavailable";
        return false;
    }
    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const int writeIndex = m_restirWriteIndex;
    const int prevIndex = 1 - writeIndex;
    const int width = m_restirBufferWidth;
    const int height = m_restirBufferHeight;

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // g_Output must be UAV for shade rewrite; guides/motion/direct already SRV-readable after PT.
    TransitionResource(
        commandList,
        m_primaryOutputResource,
        static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (m_ptDirectTexture.state != static_cast<std::uint32_t>(kAllShaderRead))
    {
        TransitionResource(
            commandList,
            m_ptDirectTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptDirectTexture.state),
            kAllShaderRead);
        m_ptDirectTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    auto ensureUav = [&](StructuredBufferUav& buffer) {
        if (buffer.resource == nullptr)
        {
            return;
        }
        const D3D12_RESOURCE_STATES before =
            buffer.state == 0 ? D3D12_RESOURCE_STATE_COMMON
                            : static_cast<D3D12_RESOURCE_STATES>(buffer.state);
        if (before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            TransitionResource(commandList, buffer.resource, before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            buffer.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    };
    ensureUav(m_restirReservoirs[writeIndex]);
    ensureUav(m_restirReservoirs[prevIndex]);
    ensureUav(m_restirGiReservoirs[writeIndex]);
    ensureUav(m_restirGiReservoirs[prevIndex]);

    DxrRootSignature::RestirTemporalConstants dispatchConstants = constants;
    const bool historyOk = m_ptPrevSurfaceHistoryValid && m_restirReservoirHistoryValid
        && dispatchConstants.historyValid != 0u;
    FrameDiagnostics::LogHistoryEvent(
        m_historyViewportId,
        "restir-temporal",
        historyOk ? "consume" : "request",
        "path-tracer",
        "pt-surface-history-v1",
        "none",
        "real-time",
        m_restirBufferWidth,
        m_restirBufferHeight,
        m_restirBufferWidth,
        m_restirBufferHeight,
        false,
        false,
        (!m_ptPrevSurfaceHistoryValid ? 1u : 0u)
            | (!m_restirReservoirHistoryValid ? 2u : 0u)
            | (dispatchConstants.historyValid == 0u ? 4u : 0u));
    dispatchConstants.historyValid = historyOk ? 1u : 0u;

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(dispatchConstants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate ReSTIR temporal constants";
        return false;
    }

    DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    const std::uint32_t envMapSrvIndex = DepthSrvIndexFromCpuHandle(envMapSrvCpuHandle);
    const std::uint32_t srvIndices[13] = {
        m_tlasSrvIndex,
        m_ptPrevRestirSurfacePositionDepthTexture.srvIndex,
        m_ptPrevRestirSurfaceMaterialTexture.srvIndex,
        m_ptRestirSurfacePositionDepthTexture.srvIndex,
        m_ptRestirSurfaceMaterialTexture.srvIndex,
        m_ptMotionTexture.srvIndex,
        m_ptDirectTexture.srvIndex,
        m_ptRestirSurfaceAlbedoMetallicTexture.srvIndex,
        m_ptPrevRestirSurfaceAlbedoMetallicTexture.srvIndex,
        emissiveLightsSrvIndex,
        emissiveTrianglesSrvIndex,
        envCdfSrvIndex,
        envMapSrvIndex};
    for (const std::uint32_t srvIndex : srvIndices)
    {
        if (srvIndex == UINT32_MAX)
        {
            outError = "ReSTIR temporal SRV bindings unavailable";
            return false;
        }
    }
    recorder.BindSrvTables(1, srvIndices, 13);

    const std::uint32_t uavIndices[5] = {
        m_restirReservoirs[writeIndex].uavIndex,
        m_restirReservoirs[prevIndex].uavIndex,
        m_restirGiReservoirs[writeIndex].uavIndex,
        m_primaryOutputUavIndex,
        m_restirGiReservoirs[prevIndex].uavIndex};
    for (std::uint32_t uavIndex = 0; uavIndex < 5; ++uavIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(uavIndices[uavIndex]));
        commandList->SetComputeRootDescriptorTable(14 + uavIndex, uavTableHandle);
    }

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    // Keep the temporal reuse dispatch independently visible in captures. The UI timer surrounds
    // this call too, but this narrow marker identifies the DispatchRays work alone.
    static constexpr wchar_t kRestirTemporalDispatchMarker[] = L"PT.ReSTIR.Temporal";
    BeginPathTracerGpuEvent(
        commandList,
        kRestirTemporalDispatchMarker,
        static_cast<UINT>(sizeof(kRestirTemporalDispatchMarker)));
    recorder.DispatchRays(shaderBindingTable, width, height);
    EndPathTracerGpuEvent(commandList);

    RecordDxrUavBarrier(commandList, m_primaryOutputResource);
    RecordDxrUavBarrier(commandList, m_restirReservoirs[writeIndex].resource);
    RecordDxrUavBarrier(commandList, m_restirGiReservoirs[writeIndex].resource);

    TransitionResource(
        commandList,
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    m_restirWriteIndex = prevIndex;
    m_restirReservoirHistoryValid = true;
    return true;
}

bool DxrDispatchContext::DispatchRestirSpatial(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    ID3D12Resource* tlasResource,
    const std::uint64_t tlasGpuVirtualAddress,
    const std::uint32_t emissiveLightsSrvIndex,
    const std::uint32_t emissiveTrianglesSrvIndex,
    const std::uint32_t envCdfSrvIndex,
    const std::uintptr_t envMapSrvCpuHandle,
    const DxrRootSignature::RestirTemporalConstants& constants,
    const bool dispatchGiBoilingFilterTiles,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid ReSTIR spatial dispatch arguments";
        return false;
    }
    if (!HasRestirBuffers() || m_primaryOutputResource == nullptr || m_primaryOutputUavIndex == UINT32_MAX
        || m_ptDirectTexture.resource == nullptr || m_ptDirectTexture.srvIndex == UINT32_MAX
        || m_ptRestirSurfaceAlbedoMetallicTexture.resource == nullptr
        || m_ptRestirSurfaceAlbedoMetallicTexture.srvIndex == UINT32_MAX)
    {
        outError = "ReSTIR spatial buffers unavailable";
        return false;
    }
    if (!CreateTlasSrv(tlasResource, tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    // After temporal: writeIndex is free; temporal result is in the other slot.
    const int writeIndex = m_restirWriteIndex;
    const int readIndex = 1 - writeIndex;
    const int width = m_restirBufferWidth;
    const int height = m_restirBufferHeight;

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    TransitionResource(
        commandList,
        m_primaryOutputResource,
        static_cast<D3D12_RESOURCE_STATES>(m_primaryOutputResourceState),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (m_ptDirectTexture.state != static_cast<std::uint32_t>(kAllShaderRead))
    {
        TransitionResource(
            commandList,
            m_ptDirectTexture.resource,
            static_cast<D3D12_RESOURCE_STATES>(m_ptDirectTexture.state),
            kAllShaderRead);
        m_ptDirectTexture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    auto ensureUav = [&](StructuredBufferUav& buffer) {
        if (buffer.resource == nullptr)
        {
            return;
        }
        const D3D12_RESOURCE_STATES before =
            buffer.state == 0 ? D3D12_RESOURCE_STATE_COMMON
                            : static_cast<D3D12_RESOURCE_STATES>(buffer.state);
        if (before != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            TransitionResource(commandList, buffer.resource, before, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            buffer.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    };
    ensureUav(m_restirReservoirs[writeIndex]);
    ensureUav(m_restirReservoirs[readIndex]);
    ensureUav(m_restirGiReservoirs[writeIndex]);
    ensureUav(m_restirGiReservoirs[readIndex]);

    // UAV barrier on the read buffer so this iteration sees the prior pass's writes.
    RecordDxrUavBarrier(commandList, m_restirReservoirs[readIndex].resource);
    RecordDxrUavBarrier(commandList, m_restirGiReservoirs[readIndex].resource);

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate ReSTIR spatial constants";
        return false;
    }

    DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    const std::uint32_t envMapSrvIndex = DepthSrvIndexFromCpuHandle(envMapSrvCpuHandle);
    const std::uint32_t srvIndices[13] = {
        m_tlasSrvIndex,
        m_ptPrevRestirSurfacePositionDepthTexture.srvIndex,
        m_ptPrevRestirSurfaceMaterialTexture.srvIndex,
        m_ptRestirSurfacePositionDepthTexture.srvIndex,
        m_ptRestirSurfaceMaterialTexture.srvIndex,
        m_ptMotionTexture.srvIndex,
        m_ptDirectTexture.srvIndex,
        m_ptRestirSurfaceAlbedoMetallicTexture.srvIndex,
        m_ptPrevRestirSurfaceAlbedoMetallicTexture.srvIndex,
        emissiveLightsSrvIndex,
        emissiveTrianglesSrvIndex,
        envCdfSrvIndex,
        envMapSrvIndex};
    for (const std::uint32_t srvIndex : srvIndices)
    {
        if (srvIndex == UINT32_MAX)
        {
            outError = "ReSTIR spatial SRV bindings unavailable";
            return false;
        }
    }
    recorder.BindSrvTables(1, srvIndices, 13);

    // u0 = spatial write, u1 = prior reservoirs (temporal / previous spatial iter).
    const std::uint32_t uavIndices[5] = {
        m_restirReservoirs[writeIndex].uavIndex,
        m_restirReservoirs[readIndex].uavIndex,
        m_restirGiReservoirs[writeIndex].uavIndex,
        m_primaryOutputUavIndex,
        m_restirGiReservoirs[readIndex].uavIndex};
    for (std::uint32_t uavIndex = 0; uavIndex < 5; ++uavIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(uavIndices[uavIndex]));
        commandList->SetComputeRootDescriptorTable(14 + uavIndex, uavTableHandle);
    }

    if (tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), tlasResource);
    }

    constexpr int kGiBoilingFilterTileSize = 16;
    const int dispatchWidth = dispatchGiBoilingFilterTiles
        ? (width + kGiBoilingFilterTileSize - 1) / kGiBoilingFilterTileSize
        : width;
    const int dispatchHeight = dispatchGiBoilingFilterTiles
        ? (height + kGiBoilingFilterTileSize - 1) / kGiBoilingFilterTileSize
        : height;
    static constexpr wchar_t kRestirSpatialDispatchMarker[] = L"PT.ReSTIR.Spatial";
    static constexpr wchar_t kRestirGiBoilingFilterMarker[] = L"PT.ReSTIR.GiBoilingFilter";
    const wchar_t* const marker = dispatchGiBoilingFilterTiles
        ? kRestirGiBoilingFilterMarker
        : kRestirSpatialDispatchMarker;
    const UINT markerSize = dispatchGiBoilingFilterTiles
        ? static_cast<UINT>(sizeof(kRestirGiBoilingFilterMarker))
        : static_cast<UINT>(sizeof(kRestirSpatialDispatchMarker));
    BeginPathTracerGpuEvent(commandList, marker, markerSize);
    recorder.DispatchRays(shaderBindingTable, dispatchWidth, dispatchHeight);
    EndPathTracerGpuEvent(commandList);

    RecordDxrUavBarrier(commandList, m_primaryOutputResource);
    RecordDxrUavBarrier(commandList, m_restirReservoirs[writeIndex].resource);
    RecordDxrUavBarrier(commandList, m_restirGiReservoirs[writeIndex].resource);

    TransitionResource(
        commandList,
        m_primaryOutputResource,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        kAllShaderRead);
    m_primaryOutputResourceState = static_cast<std::uint32_t>(kAllShaderRead);

    // Next PT overwrites the read slot; spatial output becomes temporal history.
    m_restirWriteIndex = readIndex;
    return true;
}

