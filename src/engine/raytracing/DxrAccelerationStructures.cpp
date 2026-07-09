#include "engine/raytracing/DxrAccelerationStructures.h"

#include "app/scene/Scene.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/Blas.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrInstanceTransform.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/GpuBuffer.h"
#include "engine/scene/SceneObject.h"

#include "engine/raytracing/DxrHeaders.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

#include <glm/glm.hpp>

namespace
{
    std::uint64_t HashCombine(const std::uint64_t seed, const std::uint64_t value)
    {
        return seed ^ (value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2));
    }

    std::uint64_t HashFloatBits(const std::uint64_t seed, const float value)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return HashCombine(seed, static_cast<std::uint64_t>(bits));
    }

    // Fingerprint of mesh/material/layout inputs to EnsureGeometryBuffers. Transform changes
    // are excluded so moving objects does not trigger a full geometry re-upload (DXR-05).
    std::uint64_t ComputeDxrGeometryFingerprint(const Scene& scene)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        std::uint64_t fingerprint = HashCombine(0, objects.size());

        for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            const SceneObject& object = objects[objectIndex];
            fingerprint = HashCombine(fingerprint, objectIndex);
            fingerprint = HashCombine(fingerprint, object.IsRenderable() ? 1u : 0u);
            if (!object.IsRenderable())
            {
                continue;
            }

            Mesh* mesh = object.GetMesh();
            fingerprint = HashCombine(
                fingerprint,
                reinterpret_cast<std::uintptr_t>(mesh));
            if (mesh == nullptr)
            {
                continue;
            }

            fingerprint = HashCombine(fingerprint, mesh->GetPositions().size());
            fingerprint = HashCombine(fingerprint, mesh->GetIndices().size());
            fingerprint = HashCombine(fingerprint, mesh->GetFloatsPerVertex());

            const Material& material = object.GetMaterial();
            const glm::vec3 albedo = material.GetAlbedo();
            const glm::vec3 emissive = material.GetEmissive();
            fingerprint = HashFloatBits(fingerprint, albedo.x);
            fingerprint = HashFloatBits(fingerprint, albedo.y);
            fingerprint = HashFloatBits(fingerprint, albedo.z);
            fingerprint = HashFloatBits(fingerprint, emissive.x);
            fingerprint = HashFloatBits(fingerprint, emissive.y);
            fingerprint = HashFloatBits(fingerprint, emissive.z);
            fingerprint = HashFloatBits(fingerprint, material.GetMetallic());
            fingerprint = HashFloatBits(fingerprint, material.GetRoughness());
            fingerprint = HashCombine(fingerprint, material.GetAlbedoMapSrvIndex());
        }

        return fingerprint;
    }
}

DxrAccelerationStructures::~DxrAccelerationStructures()
{
    Release();
}

void DxrAccelerationStructures::ReleaseGeometryBuffers()
{
    // CRASH-03: defer descriptor-slot recycling — in-flight rays may still read these.
    for (const std::uint32_t srvIndex : m_geometryLookupSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    for (const std::uint32_t srvIndex : m_sceneVertexFloatsSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    for (const std::uint32_t srvIndex : m_sceneIndicesSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    for (const std::uint32_t srvIndex : m_materialSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    m_geometryLookupSrvIndices.fill(UINT32_MAX);
    m_sceneVertexFloatsSrvIndices.fill(UINT32_MAX);
    m_sceneIndicesSrvIndices.fill(UINT32_MAX);
    m_materialSrvIndices.fill(UINT32_MAX);

    m_geometryLookupStaging.Release();
    m_materialStaging.Release();
    m_sceneVertexFloatsStaging.Release();
    m_sceneIndicesStaging.Release();
    m_geometryLookupGpu.Release();
    m_materialGpu.Release();
    m_sceneVertexFloatsGpu.Release();
    m_sceneIndicesGpu.Release();
    m_uploadedGeometryFingerprint.fill(0);
    m_geometryObjectCount = 0;
}

void DxrAccelerationStructures::Release()
{
    m_blasCache.Release();
    m_tlas.Release();
    m_scratchBuffer.Release();
    ReleaseGeometryBuffers();
    m_scratchHighWaterMark = 0;
    m_scratchResourceState = 0;
    m_anyBlasBuiltThisFrame = false;
}

void DxrAccelerationStructures::EnsureScratchBufferReadyForBuild(
    ID3D12GraphicsCommandList* commandList)
{
    if (commandList == nullptr || m_scratchBuffer.resource == nullptr)
    {
        return;
    }

    const auto targetState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (m_scratchResourceState == targetState)
    {
        return;
    }

    TransitionResource(
        commandList,
        m_scratchBuffer.resource,
        static_cast<D3D12_RESOURCE_STATES>(m_scratchResourceState),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    m_scratchResourceState = targetState;
}

bool DxrAccelerationStructures::EnsureScratchBuffer(
    const std::uint64_t requiredBytes,
    std::string& outError)
{
    outError.clear();
    if (requiredBytes == 0)
    {
        return true;
    }

    if (m_scratchBuffer.sizeInBytes >= requiredBytes)
    {
        return true;
    }

    m_scratchBuffer.Release();
    if (!CreateDxrScratchBuffer(requiredBytes, m_scratchBuffer))
    {
        outError = "failed to allocate DXR scratch buffer";
        return false;
    }
    m_scratchResourceState =
        static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (requiredBytes > m_scratchHighWaterMark)
    {
        m_scratchHighWaterMark = requiredBytes;
    }

    return true;
}

bool DxrAccelerationStructures::EnsureGeometryBuffers(
    const Scene& scene,
    ID3D12GraphicsCommandList* commandList,
    std::string& outError)
{
    outError.clear();
    const std::vector<SceneObject>& objects = scene.GetObjects();
    if (objects.empty())
    {
        ReleaseGeometryBuffers();
        return true;
    }

    const std::uint64_t fingerprint = ComputeDxrGeometryFingerprint(scene);
    const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
    if (m_uploadedGeometryFingerprint[frameIndex] == fingerprint
        && m_geometryLookupSrvIndices[frameIndex] != UINT32_MAX)
    {
        return true;
    }

    std::vector<DxrGeometryLookupEntry> lookupEntries(objects.size());
    std::vector<DxrMaterialEntry> materialEntries(objects.size());
    std::vector<float> vertexFloats;
    std::vector<std::uint32_t> indices;
    vertexFloats.reserve(1024);
    indices.reserve(1024);

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh == nullptr)
        {
            continue;
        }

        mesh->EnsureGpuResources();
        const std::uint32_t vertexStrideFloats = mesh->GetFloatsPerVertex();
        if (vertexStrideFloats < 3)
        {
            continue;
        }

        DxrGeometryLookupEntry& entry = lookupEntries[objectIndex];
        entry.vertexFloatOffset = static_cast<std::uint32_t>(vertexFloats.size());
        entry.vertexStrideFloats = vertexStrideFloats;
        entry.indexUintOffset = static_cast<std::uint32_t>(indices.size());

        // Upload the FULL interleaved vertex stride (position + normal + ...) so the reflection
        // closest-hit can read smooth vertex normals at float offset 3, not just positions.
        const std::vector<glm::vec3>& positions = mesh->GetPositions();
        const std::vector<float>& meshVertexData = mesh->GetVertexData();
        const std::size_t vertexCount = positions.size();
        const std::size_t vertexFloatCount = vertexCount * vertexStrideFloats;
        vertexFloats.resize(vertexFloats.size() + vertexFloatCount, 0.0f);
        if (meshVertexData.size() >= vertexFloatCount)
        {
            std::memcpy(
                vertexFloats.data() + entry.vertexFloatOffset,
                meshVertexData.data(),
                vertexFloatCount * sizeof(float));
        }
        else
        {
            for (std::size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
            {
                const std::size_t base =
                    static_cast<std::size_t>(entry.vertexFloatOffset) + vertexIndex * vertexStrideFloats;
                vertexFloats[base + 0] = positions[vertexIndex].x;
                vertexFloats[base + 1] = positions[vertexIndex].y;
                vertexFloats[base + 2] = positions[vertexIndex].z;
            }
        }

        const Material& material = object.GetMaterial();
        DxrMaterialEntry& materialEntry = materialEntries[objectIndex];
        const glm::vec3 albedo = material.GetAlbedo();
        const glm::vec3 emissive = material.GetEmissive();
        materialEntry.albedo[0] = albedo.x;
        materialEntry.albedo[1] = albedo.y;
        materialEntry.albedo[2] = albedo.z;
        materialEntry.metallic = material.GetMetallic();
        materialEntry.emissive[0] = emissive.x;
        materialEntry.emissive[1] = emissive.y;
        materialEntry.emissive[2] = emissive.z;
        materialEntry.roughness = material.GetRoughness();

        // Bindless albedo texture: textured meshes carry their color in the albedo map, not the
        // constant. UV0 sits at float offset 6 in the interleaved stride (pos3 + normal3 + uv0).
        const std::uint32_t albedoTexIndex = material.GetAlbedoMapSrvIndex();
        if (albedoTexIndex != UINT32_MAX && vertexStrideFloats >= 8)
        {
            materialEntry.albedoTexIndex = albedoTexIndex;
            materialEntry.albedoUvOffsetFloats = 6;
        }

        const std::vector<unsigned int>& meshIndices = mesh->GetIndices();
        indices.insert(indices.end(), meshIndices.begin(), meshIndices.end());
    }

    if (vertexFloats.empty() || indices.empty())
    {
        ReleaseGeometryBuffers();
        return true;
    }

    const bool sameLayout =
        m_geometryObjectCount == objects.size()
        && m_geometryLookupStaging.GetCapacity()
            >= sizeof(DxrGeometryLookupEntry) * objects.size()
        && m_materialStaging.GetCapacity() >= sizeof(DxrMaterialEntry) * objects.size()
        && m_sceneVertexFloatsStaging.GetCapacity() >= vertexFloats.size() * sizeof(float)
        && m_sceneIndicesStaging.GetCapacity() >= indices.size() * sizeof(std::uint32_t)
        && m_geometryLookupGpu.GetCapacity() >= sizeof(DxrGeometryLookupEntry) * objects.size()
        && m_materialGpu.GetCapacity() >= sizeof(DxrMaterialEntry) * objects.size()
        && m_sceneVertexFloatsGpu.GetCapacity() >= vertexFloats.size() * sizeof(float)
        && m_sceneIndicesGpu.GetCapacity() >= indices.size() * sizeof(std::uint32_t);

    if (!sameLayout)
    {
        ReleaseGeometryBuffers();

        const std::uint64_t lookupBytes = sizeof(DxrGeometryLookupEntry) * objects.size();
        const std::uint64_t materialBytes = sizeof(DxrMaterialEntry) * objects.size();
        const std::uint64_t vertexBytes = vertexFloats.size() * sizeof(float);
        const std::uint64_t indexBytes = indices.size() * sizeof(std::uint32_t);

        if (!m_geometryLookupStaging.EnsureCapacity(lookupBytes)
            || !m_materialStaging.EnsureCapacity(materialBytes)
            || !m_sceneVertexFloatsStaging.EnsureCapacity(vertexBytes)
            || !m_sceneIndicesStaging.EnsureCapacity(indexBytes)
            || !m_geometryLookupGpu.EnsureCapacity(lookupBytes)
            || !m_materialGpu.EnsureCapacity(materialBytes)
            || !m_sceneVertexFloatsGpu.EnsureCapacity(vertexBytes)
            || !m_sceneIndicesGpu.EnsureCapacity(indexBytes))
        {
            outError = "failed to allocate DXR geometry lookup buffers";
            ReleaseGeometryBuffers();
            return false;
        }

        for (std::uint32_t frameIndex = 0; frameIndex < GfxContext::FrameCount; ++frameIndex)
        {
            m_geometryLookupSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
            m_materialSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
            m_sceneVertexFloatsSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
            m_sceneIndicesSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
            if (m_geometryLookupSrvIndices[frameIndex] == UINT32_MAX
                || m_materialSrvIndices[frameIndex] == UINT32_MAX
                || m_sceneVertexFloatsSrvIndices[frameIndex] == UINT32_MAX
                || m_sceneIndicesSrvIndices[frameIndex] == UINT32_MAX)
            {
                outError = "failed to allocate DXR geometry lookup SRV descriptors";
                ReleaseGeometryBuffers();
                return false;
            }
        }

        auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
        if (device == nullptr)
        {
            outError = "GfxContext unavailable for DXR geometry lookup SRV creation";
            ReleaseGeometryBuffers();
            return false;
        }

        for (std::uint32_t frameIndex = 0; frameIndex < GfxContext::FrameCount; ++frameIndex)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE lookupHandle{};
            lookupHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_geometryLookupSrvIndices[frameIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC lookupSrvDesc{};
            lookupSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            lookupSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
            lookupSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            lookupSrvDesc.Buffer.FirstElement = 0;
            lookupSrvDesc.Buffer.NumElements = static_cast<UINT>(objects.size());
            lookupSrvDesc.Buffer.StructureByteStride = sizeof(DxrGeometryLookupEntry);
            device->CreateShaderResourceView(
                m_geometryLookupGpu.Slot(frameIndex).resource,
                &lookupSrvDesc,
                lookupHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE materialHandle{};
            materialHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_materialSrvIndices[frameIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC materialSrvDesc{};
            materialSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            materialSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
            materialSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            materialSrvDesc.Buffer.FirstElement = 0;
            materialSrvDesc.Buffer.NumElements = static_cast<UINT>(objects.size());
            materialSrvDesc.Buffer.StructureByteStride = sizeof(DxrMaterialEntry);
            device->CreateShaderResourceView(
                m_materialGpu.Slot(frameIndex).resource,
                &materialSrvDesc,
                materialHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE vertexHandle{};
            vertexHandle.ptr =
                GfxContext::Get().GetSrvCpuHandle(m_sceneVertexFloatsSrvIndices[frameIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrvDesc{};
            vertexSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            vertexSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            vertexSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            vertexSrvDesc.Buffer.FirstElement = 0;
            vertexSrvDesc.Buffer.NumElements = static_cast<UINT>(vertexFloats.size());
            device->CreateShaderResourceView(
                m_sceneVertexFloatsGpu.Slot(frameIndex).resource,
                &vertexSrvDesc,
                vertexHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE indexHandle{};
            indexHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_sceneIndicesSrvIndices[frameIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC indexSrvDesc{};
            indexSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            indexSrvDesc.Format = DXGI_FORMAT_R32_UINT;
            indexSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            indexSrvDesc.Buffer.FirstElement = 0;
            indexSrvDesc.Buffer.NumElements = static_cast<UINT>(indices.size());
            device->CreateShaderResourceView(
                m_sceneIndicesGpu.Slot(frameIndex).resource,
                &indexSrvDesc,
                indexHandle);
        }

        m_geometryObjectCount = objects.size();
    }

    DxrGpuResource& geometryLookupUpload = m_geometryLookupStaging.Slot(frameIndex);
    DxrGpuResource& materialUpload = m_materialStaging.Slot(frameIndex);
    DxrGpuResource& vertexUpload = m_sceneVertexFloatsStaging.Slot(frameIndex);
    DxrGpuResource& indexUpload = m_sceneIndicesStaging.Slot(frameIndex);

    const std::uint64_t lookupBytes = lookupEntries.size() * sizeof(DxrGeometryLookupEntry);
    const std::uint64_t materialBytes = materialEntries.size() * sizeof(DxrMaterialEntry);
    const std::uint64_t vertexBytes = vertexFloats.size() * sizeof(float);
    const std::uint64_t indexBytes = indices.size() * sizeof(std::uint32_t);

    void* mapped = nullptr;
    if (SUCCEEDED(geometryLookupUpload.resource->Map(0, nullptr, &mapped)))
    {
        std::memcpy(mapped, lookupEntries.data(), lookupEntries.size() * sizeof(DxrGeometryLookupEntry));
        geometryLookupUpload.resource->Unmap(0, nullptr);
    }

    if (SUCCEEDED(materialUpload.resource->Map(0, nullptr, &mapped)))
    {
        std::memcpy(mapped, materialEntries.data(), materialEntries.size() * sizeof(DxrMaterialEntry));
        materialUpload.resource->Unmap(0, nullptr);
    }

    if (SUCCEEDED(vertexUpload.resource->Map(0, nullptr, &mapped)))
    {
        std::memcpy(mapped, vertexFloats.data(), vertexFloats.size() * sizeof(float));
        vertexUpload.resource->Unmap(0, nullptr);
    }

    if (SUCCEEDED(indexUpload.resource->Map(0, nullptr, &mapped)))
    {
        std::memcpy(mapped, indices.data(), indices.size() * sizeof(std::uint32_t));
        indexUpload.resource->Unmap(0, nullptr);
    }

    CopyDxrUploadToSrvBuffer(
        commandList,
        geometryLookupUpload,
        m_geometryLookupGpu.Slot(frameIndex),
        lookupBytes);
    CopyDxrUploadToSrvBuffer(
        commandList,
        materialUpload,
        m_materialGpu.Slot(frameIndex),
        materialBytes);
    CopyDxrUploadToSrvBuffer(
        commandList,
        vertexUpload,
        m_sceneVertexFloatsGpu.Slot(frameIndex),
        vertexBytes);
    CopyDxrUploadToSrvBuffer(
        commandList,
        indexUpload,
        m_sceneIndicesGpu.Slot(frameIndex),
        indexBytes);

    m_uploadedGeometryFingerprint[frameIndex] = fingerprint;
    return true;
}

void DxrAccelerationStructures::EnsureScene(
    const Scene& scene,
    const bool dxrEnabled,
    void* commandList)
{
    const auto buildStart = std::chrono::steady_clock::now();
    m_anyBlasBuiltThisFrame = false;

    if (!GfxContext::Get().IsInitialized())
    {
        DxrBreadcrumb("AS skipped: GfxContext not initialized");
        m_diagnostics = DxrDiagnostics{};
        m_diagnostics.buildStatus = "SKIPPED (RT off)";
        return;
    }

    if (GfxContext::Get().IsDeviceRemoved())
    {
        Release();
        m_diagnostics = DxrDiagnostics{};
        m_diagnostics.buildStatus = "FAILED: device removed";
        return;
    }

    if (!GfxContext::Get().IsRaytracingSupported() || !dxrEnabled)
    {
        DxrBreadcrumbOnce("as-skipped", "AS skipped: RT off or unsupported");
        m_diagnostics.blasCount = 0;
        m_diagnostics.tlasInstanceCount = 0;
        m_diagnostics.totalRtTriangles = 0;
        m_diagnostics.asGpuMemoryBytes = 0;
        m_diagnostics.buildStatus = "SKIPPED (RT off)";
        m_diagnostics.lastBuildTimeMs = 0.0;
        return;
    }

    DxrBreadcrumb("AS begin");
    ID3D12GraphicsCommandList4* commandList4 = DxrContext::Get().QueryCommandList4(commandList);
    if (commandList4 == nullptr)
    {
        DxrBreadcrumb("AS failed: CommandList4 unavailable");
        m_diagnostics.buildStatus = "FAILED: ID3D12GraphicsCommandList4 unavailable";
        return;
    }

    std::string error;
    std::unordered_set<Mesh*> uniqueMeshes;
    const std::vector<SceneObject>& objects = scene.GetObjects();
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh != nullptr)
        {
            uniqueMeshes.insert(mesh);
        }
    }

    std::uint64_t maxScratchBytes = 0;
    DxrBreadcrumb("AS prebuild scratch sizing");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 != nullptr)
    {
        for (Mesh* mesh : uniqueMeshes)
        {
            mesh->EnsureGpuResources();
            const std::uint32_t indexCount = static_cast<std::uint32_t>(mesh->GetIndices().size());
            if (indexCount < 3)
            {
                continue;
            }

            const GpuBuffer& vertexBuffer = mesh->GetVertexBuffer();
            const GpuBuffer& indexBuffer = mesh->GetIndexBuffer();
            if (!vertexBuffer.IsValid() || !indexBuffer.IsValid())
            {
                continue;
            }

            const std::uint32_t vertexStride =
                mesh->GetFloatsPerVertex() * static_cast<std::uint32_t>(sizeof(float));

            D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
            geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geometryDesc.Triangles.IndexCount = indexCount;
            geometryDesc.Triangles.VertexCount = static_cast<UINT>(mesh->GetPositions().size());
            geometryDesc.Triangles.IndexBuffer =
                static_cast<ID3D12Resource*>(indexBuffer.GetResource())->GetGPUVirtualAddress();
            geometryDesc.Triangles.VertexBuffer.StartAddress =
                static_cast<ID3D12Resource*>(vertexBuffer.GetResource())->GetGPUVirtualAddress();
            geometryDesc.Triangles.VertexBuffer.StrideInBytes = vertexStride;

            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
            blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            blasInputs.NumDescs = 1;
            blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            blasInputs.pGeometryDescs = &geometryDesc;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuildInfo{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &prebuildInfo);
            maxScratchBytes = std::max(maxScratchBytes, prebuildInfo.ScratchDataSizeInBytes);
        }

        const std::uint32_t renderableCount = static_cast<std::uint32_t>(std::count_if(
            objects.begin(),
            objects.end(),
            [](const SceneObject& object) { return object.IsRenderable(); }));
        if (renderableCount > 0)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
            tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            tlasInputs.NumDescs = renderableCount;
            tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuild{};
            device5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuild);
            maxScratchBytes = std::max(maxScratchBytes, tlasPrebuild.ScratchDataSizeInBytes);
        }
    }

    if (!EnsureScratchBuffer(maxScratchBytes, error))
    {
        m_diagnostics.buildStatus = "FAILED: " + error;
        return;
    }

    EnsureScratchBufferReadyForBuild(
        static_cast<ID3D12GraphicsCommandList*>(commandList4));

    {
        SceneRenderTrace::Scope blasScope("dxr-blas-ensure");
        DxrBreadcrumb("AS blas-ensure begin");
        for (Mesh* mesh : uniqueMeshes)
        {
            Blas* existing = m_blasCache.Find(mesh);
            if (existing != nullptr && existing->IsBuilt())
            {
                continue;
            }

            if (!m_blasCache.Ensure(commandList4, mesh, m_scratchBuffer, error))
            {
                m_diagnostics.buildStatus = "FAILED: " + error;
                return;
            }

            m_anyBlasBuiltThisFrame = true;
        }
        DxrBreadcrumb("AS blas-ensure end");
        blasScope.Success();
    }

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.reserve(objects.size());
    std::uint64_t referencedTriangles = 0;
    std::unordered_set<Mesh*> referencedMeshes;
    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh == nullptr)
        {
            continue;
        }

        Blas* blas = m_blasCache.Find(mesh);
        if (blas == nullptr || !blas->IsBuilt())
        {
            continue;
        }

        if (referencedMeshes.insert(mesh).second)
        {
            referencedTriangles += blas->GetTriangleCount();
        }

        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc{};
        const glm::mat4 worldMatrix = scene.GetWorldMatrix(static_cast<int>(objectIndex));
        WriteD3D12InstanceTransform(
            worldMatrix,
            reinterpret_cast<float*>(instanceDesc.Transform));
        instanceDesc.InstanceID = static_cast<UINT>(objectIndex);
        instanceDesc.InstanceMask = 0xFF;
        instanceDesc.InstanceContributionToHitGroupIndex = 0;
        bool flipWinding = glm::determinant(glm::mat3(worldMatrix)) < 0.0f;
        if (object.GetMaterial().IsDoubleSided())
        {
            flipWinding = !flipWinding;
        }
        instanceDesc.Flags = flipWinding
            ? D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE
            : D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
        instanceDesc.AccelerationStructure = blas->GetGpuVirtualAddress();
        instances.push_back(instanceDesc);
    }

    if (m_anyBlasBuiltThisFrame)
    {
        for (Mesh* mesh : uniqueMeshes)
        {
            Blas* blas = m_blasCache.Find(mesh);
            if (blas != nullptr && blas->IsBuilt() && blas->GetResultResource() != nullptr)
            {
                RecordDxrUavBarrier(
                    static_cast<ID3D12GraphicsCommandList*>(commandList4),
                    blas->GetResultResource());
            }
        }
    }

    {
        SceneRenderTrace::Scope tlasScope("dxr-tlas-build");
        DxrBreadcrumb("AS tlas-build begin");
        if (!m_tlas.Build(commandList4, instances, m_scratchBuffer, error))
        {
            DxrBreadcrumb("AS tlas-build failed");
            m_diagnostics.buildStatus = "FAILED: " + error;
            return;
        }
        DxrBreadcrumb("AS tlas-build end");
        tlasScope.Success();
    }

    const auto buildEnd = std::chrono::steady_clock::now();
    DxrBreadcrumb("AS complete ok");
    m_diagnostics.blasCount = m_blasCache.GetCount();
    m_diagnostics.tlasInstanceCount = static_cast<std::uint32_t>(instances.size());
    m_diagnostics.totalRtTriangles = referencedTriangles;
    m_diagnostics.asGpuMemoryBytes =
        m_blasCache.GetTotalMemoryBytes() + m_tlas.GetSizeInBytes() + m_scratchHighWaterMark;
    m_diagnostics.buildStatus = "OK";
    m_diagnostics.lastBuildTimeMs =
        std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();

    if (!EnsureGeometryBuffers(
            scene,
            static_cast<ID3D12GraphicsCommandList*>(commandList4),
            error))
    {
        m_diagnostics.buildStatus = "FAILED: " + error;
        return;
    }
}
