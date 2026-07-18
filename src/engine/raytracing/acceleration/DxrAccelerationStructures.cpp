#include "engine/raytracing/acceleration/DxrAccelerationStructures.h"

#include "engine/rendering/scene/GpuScene.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/raytracing/acceleration/Blas.h"
#include "engine/raytracing/core/DxrContext.h"
#include "engine/raytracing/acceleration/DxrInstanceTransform.h"
#include "engine/raytracing/core/DxrTrace.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/GpuBuffer.h"

#include "engine/raytracing/core/DxrHeaders.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#include <glm/glm.hpp>

namespace
{
    constexpr std::uint32_t kMaterialFlagMetallicRoughnessMap = 1u;
    constexpr std::uint32_t kUv0OffsetFloats = 6u;
    constexpr std::uint32_t kUv1OffsetFloats = 8u;
    constexpr std::uint32_t kTangentOffsetFloats = 10u;
    constexpr std::uint32_t kMinTexturedStrideFloats = 8u;
    constexpr std::uint32_t kMinTangentStrideFloats = 14u;

    struct DxrRenderableInstance
    {
        std::uint32_t instanceId = 0;
        std::uint32_t materialId = 0;
        std::size_t objectIndex = 0;
        Mesh* mesh = nullptr;
        glm::mat4 world{1.0f};
    };

    struct DxrMeshGeometryRange
    {
        std::uint32_t vertexFloatOffset = 0;
        std::uint32_t vertexStrideFloats = 0;
        std::uint32_t indexUintOffset = 0;
    };

    std::vector<DxrRenderableInstance> BuildDxrRenderableInstances(const GpuScene& gpuScene)
    {
        std::vector<DxrRenderableInstance> instances;
        instances.reserve(gpuScene.GetInstances().size());

        for (const GpuSceneInstanceRecord& gpuInstance : gpuScene.GetInstances())
        {
            if (gpuInstance.meshId >= gpuScene.GetMeshAssets().size())
            {
                continue;
            }

            Mesh* mesh = gpuScene.GetMeshAssets()[gpuInstance.meshId].mesh;
            if (mesh == nullptr)
            {
                continue;
            }

            DxrRenderableInstance instance{};
            instance.instanceId = gpuInstance.instanceId;
            instance.materialId = gpuInstance.materialId;
            instance.objectIndex = gpuInstance.objectIndex;
            instance.mesh = mesh;
            instance.world = gpuInstance.world;
            instances.push_back(instance);
        }

        return instances;
    }

    void BuildEmissiveAliasTable(
        const std::vector<float>& weights,
        const std::uint32_t globalOffset,
        std::vector<DxrEmissiveAliasEntry>& outEntries)
    {
        const std::size_t count = weights.size();
        const std::size_t outputStart = outEntries.size();
        outEntries.resize(outputStart + count);
        if (count == 0)
        {
            return;
        }

        float weightSum = 0.0f;
        for (const float weight : weights)
        {
            weightSum += std::max(weight, 0.0f);
        }
        if (weightSum <= 0.0f)
        {
            for (std::size_t i = 0; i < count; ++i)
            {
                outEntries[outputStart + i] = {1.0f, globalOffset + static_cast<std::uint32_t>(i)};
            }
            return;
        }

        std::vector<float> scaled(count);
        std::vector<std::uint32_t> smallBuckets;
        std::vector<std::uint32_t> largeBuckets;
        smallBuckets.reserve(count);
        largeBuckets.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            scaled[i] = std::max(weights[i], 0.0f) * static_cast<float>(count) / weightSum;
            (scaled[i] < 1.0f ? smallBuckets : largeBuckets).push_back(i);
        }
        while (!smallBuckets.empty() && !largeBuckets.empty())
        {
            const std::uint32_t low = smallBuckets.back(); smallBuckets.pop_back();
            const std::uint32_t high = largeBuckets.back(); largeBuckets.pop_back();
            outEntries[outputStart + low] = {scaled[low], globalOffset + high};
            scaled[high] = scaled[high] + scaled[low] - 1.0f;
            (scaled[high] < 1.0f ? smallBuckets : largeBuckets).push_back(high);
        }
        for (const std::uint32_t index : smallBuckets)
        {
            outEntries[outputStart + index] = {1.0f, globalOffset + index};
        }
        for (const std::uint32_t index : largeBuckets)
        {
            outEntries[outputStart + index] = {1.0f, globalOffset + index};
        }
    }

    std::uint32_t UvOffsetFloatsForTexCoordSet(const int texCoordSet)
    {
        return texCoordSet == 1 ? kUv1OffsetFloats : kUv0OffsetFloats;
    }

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
    std::uint64_t ComputeDxrGeometryFingerprint(const GpuScene& gpuScene)
    {
        std::uint64_t fingerprint = HashCombine(0, gpuScene.GetInstances().size());
        fingerprint = HashCombine(fingerprint, gpuScene.GetMeshAssets().size());
        fingerprint = HashCombine(fingerprint, gpuScene.GetMaterials().size());

        for (const GpuSceneInstanceRecord& instance : gpuScene.GetInstances())
        {
            fingerprint = HashCombine(fingerprint, instance.instanceId);
            fingerprint = HashCombine(fingerprint, instance.meshId);
            fingerprint = HashCombine(fingerprint, instance.materialId);
            fingerprint = HashCombine(fingerprint, instance.flags);
        }

        for (const GpuSceneMeshAssetRecord& meshAsset : gpuScene.GetMeshAssets())
        {
            Mesh* mesh = meshAsset.mesh;
            fingerprint = HashCombine(fingerprint, reinterpret_cast<std::uintptr_t>(mesh));
            if (mesh == nullptr)
            {
                continue;
            }

            fingerprint = HashCombine(fingerprint, meshAsset.vertexCount);
            fingerprint = HashCombine(fingerprint, meshAsset.indexCount);
            fingerprint = HashCombine(fingerprint, meshAsset.floatsPerVertex);
        }

        for (const GpuSceneMaterialRecord& material : gpuScene.GetMaterials())
        {
            fingerprint = HashFloatBits(fingerprint, material.albedo[0]);
            fingerprint = HashFloatBits(fingerprint, material.albedo[1]);
            fingerprint = HashFloatBits(fingerprint, material.albedo[2]);
            fingerprint = HashFloatBits(fingerprint, material.emissive[0]);
            fingerprint = HashFloatBits(fingerprint, material.emissive[1]);
            fingerprint = HashFloatBits(fingerprint, material.emissive[2]);
            fingerprint = HashFloatBits(fingerprint, material.metallic);
            fingerprint = HashFloatBits(fingerprint, material.roughness);
            fingerprint = HashFloatBits(fingerprint, material.transmission);
            fingerprint = HashFloatBits(fingerprint, material.indexOfRefraction);
            fingerprint = HashFloatBits(fingerprint, material.thinWalled);
            fingerprint = HashCombine(fingerprint, material.albedoTexIndex);
            fingerprint = HashCombine(fingerprint, material.normalTexIndex);
            fingerprint = HashCombine(fingerprint, material.roughnessTexIndex);
            fingerprint = HashCombine(fingerprint, material.emissiveTexIndex);
            fingerprint = HashCombine(fingerprint, material.flags);
        }

        return fingerprint;
    }

    std::uint64_t ComputeDxrTlasTopologyFingerprint(
        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances)
    {
        std::uint64_t fingerprint = HashCombine(0, instances.size());
        for (const D3D12_RAYTRACING_INSTANCE_DESC& instance : instances)
        {
            fingerprint = HashCombine(fingerprint, instance.InstanceID);
            fingerprint = HashCombine(fingerprint, instance.AccelerationStructure);
            fingerprint = HashCombine(fingerprint, static_cast<std::uint64_t>(instance.Flags));
        }

        return fingerprint;
    }

    std::uint64_t ComputeDxrTlasTransformFingerprint(
        const std::vector<D3D12_RAYTRACING_INSTANCE_DESC>& instances)
    {
        std::uint64_t fingerprint = HashCombine(0, instances.size());
        for (const D3D12_RAYTRACING_INSTANCE_DESC& instance : instances)
        {
            for (std::uint32_t row = 0; row < 3; ++row)
            {
                for (std::uint32_t col = 0; col < 4; ++col)
                {
                    fingerprint = HashFloatBits(fingerprint, instance.Transform[row][col]);
                }
            }
        }

        return fingerprint;
    }
}

DxrAccelerationStructures::~DxrAccelerationStructures()
{
    Release();
}

bool DxrAccelerationStructures::ConsumeGeometryContentReupload()
{
    const bool pending = m_pendingGeometryContentReupload;
    m_pendingGeometryContentReupload = false;
    return pending;
}

void DxrAccelerationStructures::BumpPtSceneVersion()
{
    ++m_ptSceneVersion;
    if (m_ptSceneVersion == 0)
    {
        m_ptSceneVersion = 1;
    }
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

    for (const std::uint32_t srvIndex : m_prevTransformsSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    for (const std::uint32_t srvIndex : m_emissiveLightsSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    for (const std::uint32_t srvIndex : m_emissiveTrianglesSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }
    for (const std::uint32_t srvIndex : m_emissiveLightAliasSrvIndices)
    {
        if (srvIndex != UINT32_MAX) { GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex); }
    }
    for (const std::uint32_t srvIndex : m_emissiveTriangleAliasSrvIndices)
    {
        if (srvIndex != UINT32_MAX) { GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex); }
    }
    for (const std::uint32_t srvIndex : m_emissiveLightByInstanceSrvIndices)
    {
        if (srvIndex != UINT32_MAX) { GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex); }
    }

    m_geometryLookupSrvIndices.fill(UINT32_MAX);
    m_sceneVertexFloatsSrvIndices.fill(UINT32_MAX);
    m_sceneIndicesSrvIndices.fill(UINT32_MAX);
    m_materialSrvIndices.fill(UINT32_MAX);
    m_prevTransformsSrvIndices.fill(UINT32_MAX);
    m_emissiveLightsSrvIndices.fill(UINT32_MAX);
    m_emissiveTrianglesSrvIndices.fill(UINT32_MAX);
    m_emissiveLightAliasSrvIndices.fill(UINT32_MAX);
    m_emissiveTriangleAliasSrvIndices.fill(UINT32_MAX);
    m_emissiveLightByInstanceSrvIndices.fill(UINT32_MAX);

    m_geometryLookupStaging.Release();
    m_materialStaging.Release();
    m_sceneVertexFloatsStaging.Release();
    m_sceneIndicesStaging.Release();
    m_prevTransformsStaging.Release();
    m_emissiveLightsStaging.Release();
    m_emissiveTrianglesStaging.Release();
    m_emissiveLightAliasStaging.Release();
    m_emissiveTriangleAliasStaging.Release();
    m_emissiveLightByInstanceStaging.Release();
    m_geometryLookupGpu.Release();
    m_materialGpu.Release();
    m_sceneVertexFloatsGpu.Release();
    m_sceneIndicesGpu.Release();
    m_prevTransformsGpu.Release();
    m_emissiveLightsGpu.Release();
    m_emissiveTrianglesGpu.Release();
    m_emissiveLightAliasGpu.Release();
    m_emissiveTriangleAliasGpu.Release();
    m_emissiveLightByInstanceGpu.Release();
    m_uploadedGeometryFingerprint.fill(0);
    m_prevTransformsUploadFrame.fill(0);
    m_emissiveLightsUploadFrame.fill(0);
    m_emissiveTrianglesUploadFrame.fill(0);
    m_emissiveLightAliasUploadFrame.fill(0);
    m_emissiveTriangleAliasUploadFrame.fill(0);
    m_emissiveLightByInstanceUploadFrame.fill(0);
    m_prevTransformsCapacityCount = 0;
    m_emissiveLightsCapacityCount = 0;
    m_emissiveTrianglesCapacityCount = 0;
    m_emissiveLightAliasCapacityCount = 0;
    m_emissiveTriangleAliasCapacityCount = 0;
    m_emissiveLightByInstanceCapacityCount = 0;
    m_emissiveLightCount = 0;
    m_emissiveLightPickWeightSum = 0.0f;
    m_sceneHasTransmission = false;
    m_geometryObjectCount = 0;
}

void DxrAccelerationStructures::Release()
{
    m_blasCache.Release();
    m_tlas.Release();
    m_emptySceneMesh.reset();
    m_scratchBuffer.Release();
    ReleaseGeometryBuffers();
    m_scratchHighWaterMark = 0;
    m_scratchResourceState = 0;
    m_anyBlasBuiltThisFrame = false;
    m_builtTlasTopologyFingerprint = 0;
    m_builtTlasTransformFingerprint = 0;
}

Mesh* DxrAccelerationStructures::EnsureEmptySceneMesh()
{
    if (m_emptySceneMesh == nullptr)
    {
        // This triangle is never visible to a ray: its only TLAS instance has mask 0.  It exists
        // solely because DXR requires a non-empty BLAS/TLAS and valid structured-buffer SRVs for
        // DispatchRays.  Keeping the PT miss shader active preserves sky color and camera motion.
        constexpr float vertices[] = {
            0.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,
            1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,
            0.0f, 1.0f, 0.0f,  0.0f, 1.0f, 0.0f,
        };
        constexpr unsigned int indices[] = {0u, 1u, 2u};
        m_emptySceneMesh = std::make_unique<Mesh>(
            vertices,
            3u,
            Mesh::BasicVertexFloatCount,
            indices,
            3u);
    }

    return m_emptySceneMesh.get();
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
    const GpuScene& gpuScene,
    ID3D12GraphicsCommandList* commandList,
    std::string& outError)
{
    outError.clear();
    std::vector<DxrRenderableInstance> renderInstances = BuildDxrRenderableInstances(gpuScene);
    const bool emptyScene = renderInstances.empty();
    if (emptyScene)
    {
        renderInstances.push_back(DxrRenderableInstance{0u, 0u, 0u, EnsureEmptySceneMesh(), glm::mat4(1.0f)});
    }

    const std::uint64_t fingerprint = ComputeDxrGeometryFingerprint(gpuScene);
    const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
    const bool geometryContentChanged = m_uploadedGeometryFingerprint[frameIndex] != fingerprint
        || m_geometryLookupSrvIndices[frameIndex] == UINT32_MAX;
    if (!geometryContentChanged)
    {
        return true;
    }

    BumpPtSceneVersion();
    m_pendingGeometryContentReupload = true;

    std::vector<DxrGeometryLookupEntry> lookupEntries(renderInstances.size());
    std::vector<DxrMaterialEntry> materialEntries(
        std::max<std::size_t>(gpuScene.GetMaterials().size(), 1u));
    std::vector<bool> materialTexturedStrideValid(materialEntries.size(), true);
    std::vector<bool> materialTangentStrideValid(materialEntries.size(), true);
    std::vector<bool> materialUsed(materialEntries.size(), false);
    std::vector<float> vertexFloats;
    std::vector<std::uint32_t> indices;
    std::unordered_map<Mesh*, DxrMeshGeometryRange> geometryRanges;
    vertexFloats.reserve(1024);
    indices.reserve(1024);
    geometryRanges.reserve(gpuScene.GetMeshAssets().size());

    for (std::size_t instanceIndex = 0; instanceIndex < renderInstances.size(); ++instanceIndex)
    {
        const DxrRenderableInstance& renderInstance = renderInstances[instanceIndex];
        Mesh* mesh = renderInstance.mesh;

        const std::uint32_t vertexStrideFloats = mesh->GetFloatsPerVertex();
        if (vertexStrideFloats < 3)
        {
            continue;
        }
        if (renderInstance.materialId < materialEntries.size())
        {
            materialUsed[renderInstance.materialId] = true;
            materialTexturedStrideValid[renderInstance.materialId] =
                materialTexturedStrideValid[renderInstance.materialId]
                && vertexStrideFloats >= kMinTexturedStrideFloats;
            materialTangentStrideValid[renderInstance.materialId] =
                materialTangentStrideValid[renderInstance.materialId]
                && vertexStrideFloats >= kMinTangentStrideFloats;
        }

        DxrGeometryLookupEntry& entry = lookupEntries[instanceIndex];
        entry.materialId = renderInstance.materialId;

        const auto existingGeometry = geometryRanges.find(mesh);
        if (existingGeometry != geometryRanges.end())
        {
            entry.vertexFloatOffset = existingGeometry->second.vertexFloatOffset;
            entry.vertexStrideFloats = existingGeometry->second.vertexStrideFloats;
            entry.indexUintOffset = existingGeometry->second.indexUintOffset;
            continue;
        }

        mesh->EnsureGpuResources();
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

        const std::vector<unsigned int>& meshIndices = mesh->GetIndices();
        indices.insert(indices.end(), meshIndices.begin(), meshIndices.end());

        geometryRanges.emplace(
            mesh,
            DxrMeshGeometryRange{
                entry.vertexFloatOffset,
                entry.vertexStrideFloats,
                entry.indexUintOffset});
    }

    for (const GpuSceneMaterialRecord& gpuMaterial : gpuScene.GetMaterials())
    {
        if (gpuMaterial.materialId >= materialEntries.size())
        {
            continue;
        }

        DxrMaterialEntry& materialEntry = materialEntries[gpuMaterial.materialId];
        const bool texturedStrideValid =
            !materialUsed[gpuMaterial.materialId] || materialTexturedStrideValid[gpuMaterial.materialId];
        const bool tangentStrideValid =
            !materialUsed[gpuMaterial.materialId] || materialTangentStrideValid[gpuMaterial.materialId];
        materialEntry.albedo[0] = gpuMaterial.albedo[0];
        materialEntry.albedo[1] = gpuMaterial.albedo[1];
        materialEntry.albedo[2] = gpuMaterial.albedo[2];
        materialEntry.metallic = gpuMaterial.metallic;
        materialEntry.emissive[0] = gpuMaterial.emissive[0];
        materialEntry.emissive[1] = gpuMaterial.emissive[1];
        materialEntry.emissive[2] = gpuMaterial.emissive[2];
        materialEntry.roughness = gpuMaterial.roughness;
        materialEntry.transmission = gpuMaterial.transmission;
        materialEntry.indexOfRefraction = gpuMaterial.indexOfRefraction;
        materialEntry.thinWalled = gpuMaterial.thinWalled;
        materialEntry.materialFlags =
            texturedStrideValid ? (gpuMaterial.flags & kMaterialFlagMetallicRoughnessMap) : 0u;
        materialEntry.tangentOffsetFloats = tangentStrideValid ? kTangentOffsetFloats : UINT32_MAX;
        materialEntry.albedoTexIndex =
            texturedStrideValid ? gpuMaterial.albedoTexIndex : UINT32_MAX;
        materialEntry.albedoUvOffsetFloats = materialEntry.albedoTexIndex == UINT32_MAX
            ? UINT32_MAX
            : UvOffsetFloatsForTexCoordSet(static_cast<int>(gpuMaterial.albedoTexCoordSet));
        materialEntry.normalTexIndex =
            tangentStrideValid ? gpuMaterial.normalTexIndex : UINT32_MAX;
        materialEntry.normalUvOffsetFloats = materialEntry.normalTexIndex == UINT32_MAX
            ? UINT32_MAX
            : UvOffsetFloatsForTexCoordSet(static_cast<int>(gpuMaterial.normalTexCoordSet));
        materialEntry.roughnessTexIndex =
            texturedStrideValid ? gpuMaterial.roughnessTexIndex : UINT32_MAX;
        materialEntry.roughnessUvOffsetFloats = materialEntry.roughnessTexIndex == UINT32_MAX
            ? UINT32_MAX
            : UvOffsetFloatsForTexCoordSet(static_cast<int>(gpuMaterial.roughnessTexCoordSet));
        materialEntry.emissiveTexIndex =
            texturedStrideValid ? gpuMaterial.emissiveTexIndex : UINT32_MAX;
        materialEntry.emissiveUvOffsetFloats = materialEntry.emissiveTexIndex == UINT32_MAX
            ? UINT32_MAX
            : UvOffsetFloatsForTexCoordSet(static_cast<int>(gpuMaterial.emissiveTexCoordSet));
    }

    if (vertexFloats.empty() || indices.empty())
    {
        ReleaseGeometryBuffers();
        return true;
    }

    const std::size_t sceneGeometryObjectCount = emptyScene ? 0u : renderInstances.size();
    const bool sameLayout =
        m_geometryObjectCount == sceneGeometryObjectCount
        && m_geometryLookupStaging.GetCapacity()
            >= sizeof(DxrGeometryLookupEntry) * renderInstances.size()
        && m_materialStaging.GetCapacity() >= sizeof(DxrMaterialEntry) * materialEntries.size()
        && m_sceneVertexFloatsStaging.GetCapacity() >= vertexFloats.size() * sizeof(float)
        && m_sceneIndicesStaging.GetCapacity() >= indices.size() * sizeof(std::uint32_t)
        && m_geometryLookupGpu.GetCapacity() >= sizeof(DxrGeometryLookupEntry) * renderInstances.size()
        && m_materialGpu.GetCapacity() >= sizeof(DxrMaterialEntry) * materialEntries.size()
        && m_sceneVertexFloatsGpu.GetCapacity() >= vertexFloats.size() * sizeof(float)
        && m_sceneIndicesGpu.GetCapacity() >= indices.size() * sizeof(std::uint32_t);

    if (!sameLayout)
    {
        ReleaseGeometryBuffers();

        const std::uint64_t lookupBytes = sizeof(DxrGeometryLookupEntry) * renderInstances.size();
        const std::uint64_t materialBytes = sizeof(DxrMaterialEntry) * materialEntries.size();
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
            lookupSrvDesc.Buffer.NumElements = static_cast<UINT>(renderInstances.size());
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
            materialSrvDesc.Buffer.NumElements = static_cast<UINT>(materialEntries.size());
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

    m_geometryObjectCount = sceneGeometryObjectCount;
    m_uploadedGeometryFingerprint[frameIndex] = fingerprint;
    return true;
}

// P4b: per-frame upload (no fingerprint gate — matrices change whenever anything moves and the
// buffer must always describe the PREVIOUS frame). Rows are the transposed-agnostic encoding
// documented on DxrPrevInstanceTransformEntry.
bool DxrAccelerationStructures::UploadPrevInstanceTransforms(
    const std::vector<glm::mat4>& prevWorldMatrices,
    void* commandList)
{
    auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(commandList);
    if (d3dCommandList == nullptr || prevWorldMatrices.empty()
        || !GfxContext::Get().IsInitialized())
    {
        return false;
    }

    std::vector<DxrPrevInstanceTransformEntry> entries(prevWorldMatrices.size());
    for (std::size_t objectIndex = 0; objectIndex < prevWorldMatrices.size(); ++objectIndex)
    {
        const glm::mat4& m = prevWorldMatrices[objectIndex];
        DxrPrevInstanceTransformEntry& entry = entries[objectIndex];
        for (int column = 0; column < 4; ++column)
        {
            entry.row0[column] = m[column][0];
            entry.row1[column] = m[column][1];
            entry.row2[column] = m[column][2];
        }
    }

    const std::uint64_t byteSize = entries.size() * sizeof(DxrPrevInstanceTransformEntry);
    const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();

    const bool sameCapacity = m_prevTransformsCapacityCount == entries.size()
        && m_prevTransformsStaging.GetCapacity() >= byteSize
        && m_prevTransformsGpu.GetCapacity() >= byteSize
        && m_prevTransformsSrvIndices[frameIndex] != UINT32_MAX;
    if (!sameCapacity)
    {
        if (!m_prevTransformsStaging.EnsureCapacity(byteSize)
            || !m_prevTransformsGpu.EnsureCapacity(byteSize))
        {
            return false;
        }

        auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
        if (device == nullptr)
        {
            return false;
        }

        for (std::uint32_t slotIndex = 0; slotIndex < GfxContext::FrameCount; ++slotIndex)
        {
            if (m_prevTransformsSrvIndices[slotIndex] == UINT32_MAX)
            {
                m_prevTransformsSrvIndices[slotIndex] = GfxContext::Get().AllocateOffscreenSrv();
                if (m_prevTransformsSrvIndices[slotIndex] == UINT32_MAX)
                {
                    return false;
                }
            }

            D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
            srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(m_prevTransformsSrvIndices[slotIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = static_cast<UINT>(entries.size());
            srvDesc.Buffer.StructureByteStride = sizeof(DxrPrevInstanceTransformEntry);
            device->CreateShaderResourceView(
                m_prevTransformsGpu.Slot(slotIndex).resource, &srvDesc, srvHandle);
        }

        m_prevTransformsCapacityCount = entries.size();
    }

    DxrGpuResource& upload = m_prevTransformsStaging.Slot(frameIndex);
    void* mapped = nullptr;
    if (FAILED(upload.resource->Map(0, nullptr, &mapped)))
    {
        return false;
    }
    std::memcpy(mapped, entries.data(), byteSize);
    upload.resource->Unmap(0, nullptr);

    CopyDxrUploadToSrvBuffer(d3dCommandList, upload, m_prevTransformsGpu.Slot(frameIndex), byteSize);

    m_prevTransformsUploadFrame[frameIndex] = GfxContext::Get().GetSubmissionFrameNumber();
    return true;
}

namespace
{
    float EmissiveLuminance(const glm::vec3& emissive)
    {
        return 0.2126f * emissive.x + 0.7152f * emissive.y + 0.0722f * emissive.z;
    }

    float TriangleArea(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
    {
        return 0.5f * glm::length(glm::cross(b - a, c - a));
    }

    bool UploadStructuredBufferRing(
        ID3D12GraphicsCommandList* commandList,
        const void* data,
        std::uint64_t byteSize,
        std::uint32_t structureByteStride,
        std::uint32_t elementCount,
        DxrUploadRing& stagingRing,
        DxrSrvBufferRing& gpuRing,
        std::array<std::uint32_t, GfxContext::FrameCount>& srvIndices,
        std::array<std::uint64_t, GfxContext::FrameCount>& uploadFrame,
        std::size_t& capacityCount)
    {
        if (byteSize == 0 || elementCount == 0)
        {
            capacityCount = 0;
            const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
            uploadFrame[frameIndex] = GfxContext::Get().GetSubmissionFrameNumber();
            return true;
        }

        const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
        const bool sameCapacity = capacityCount == elementCount
            && stagingRing.GetCapacity() >= byteSize
            && gpuRing.GetCapacity() >= byteSize
            && srvIndices[frameIndex] != UINT32_MAX;
        if (!sameCapacity)
        {
            if (!stagingRing.EnsureCapacity(byteSize) || !gpuRing.EnsureCapacity(byteSize))
            {
                return false;
            }

            auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
            if (device == nullptr)
            {
                return false;
            }

            for (std::uint32_t slotIndex = 0; slotIndex < GfxContext::FrameCount; ++slotIndex)
            {
                if (srvIndices[slotIndex] == UINT32_MAX)
                {
                    srvIndices[slotIndex] = GfxContext::Get().AllocateOffscreenSrv();
                    if (srvIndices[slotIndex] == UINT32_MAX)
                    {
                        return false;
                    }
                }

                D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
                srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(srvIndices[slotIndex]);
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = elementCount;
                srvDesc.Buffer.StructureByteStride = structureByteStride;
                device->CreateShaderResourceView(
                    gpuRing.Slot(slotIndex).resource, &srvDesc, srvHandle);
            }

            capacityCount = elementCount;
        }

        DxrGpuResource& upload = stagingRing.Slot(frameIndex);
        void* mapped = nullptr;
        if (FAILED(upload.resource->Map(0, nullptr, &mapped)))
        {
            return false;
        }
        std::memcpy(mapped, data, byteSize);
        upload.resource->Unmap(0, nullptr);

        CopyDxrUploadToSrvBuffer(commandList, upload, gpuRing.Slot(frameIndex), byteSize);
        uploadFrame[frameIndex] = GfxContext::Get().GetSubmissionFrameNumber();
        return true;
    }
} // namespace

bool DxrAccelerationStructures::UploadEmissiveLights(
    const GpuScene& gpuScene,
    void* commandList)
{
    auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(commandList);
    if (d3dCommandList == nullptr || !GfxContext::Get().IsInitialized())
    {
        m_emissiveLightCount = 0;
        m_emissiveLightPickWeightSum = 0.0f;
        m_sceneHasTransmission = false;
        m_diagnostics.emissiveLightCount = 0;
        m_diagnostics.emissiveTriangleCount = 0;
        m_diagnostics.emissiveLightPickWeightSum = 0.0f;
        return false;
    }

    static constexpr float kEmissiveThreshold = 0.01f;

    std::vector<DxrEmissiveLightEntry> entries;
    std::vector<DxrEmissiveTriangleEntry> triangles;
    const std::vector<DxrRenderableInstance> renderInstances = BuildDxrRenderableInstances(gpuScene);
    entries.reserve(renderInstances.size());

    float pickWeightSum = 0.0f;
    bool sceneHasTransmission = false;
    for (const DxrRenderableInstance& renderInstance : renderInstances)
    {
        if (renderInstance.materialId >= gpuScene.GetMaterials().size())
        {
            continue;
        }

        Mesh* mesh = renderInstance.mesh;

        const GpuSceneMaterialRecord& material = gpuScene.GetMaterials()[renderInstance.materialId];
        // Match DielectricWeight in pt_dielectric.hlsli — any glass that can refract NEE shadows.
        const float dielectricWeight =
            std::clamp(material.transmission, 0.0f, 1.0f)
            * (1.0f - std::clamp(material.metallic, 0.0f, 1.0f));
        if (dielectricWeight > 0.0f)
        {
            sceneHasTransmission = true;
        }

        const glm::vec3 emissive(material.emissive[0], material.emissive[1], material.emissive[2]);
        const float luminance = EmissiveLuminance(emissive);
        if (luminance < kEmissiveThreshold)
        {
            continue;
        }

        const std::vector<glm::vec3>& positions = mesh->GetPositions();
        const std::vector<unsigned int>& indices = mesh->GetIndices();
        if (positions.empty() || indices.size() < 3)
        {
            continue;
        }

        const glm::mat4 worldMatrix = renderInstance.world;
        const std::uint32_t triangleOffset = static_cast<std::uint32_t>(triangles.size());
        float instanceArea = 0.0f;
        std::uint32_t triangleCount = 0;

        for (std::size_t indexOffset = 0; indexOffset + 2 < indices.size(); indexOffset += 3)
        {
            const glm::vec3 localV0 = positions[indices[indexOffset]];
            const glm::vec3 localV1 = positions[indices[indexOffset + 1]];
            const glm::vec3 localV2 = positions[indices[indexOffset + 2]];
            const glm::vec3 worldV0 = glm::vec3(worldMatrix * glm::vec4(localV0, 1.0f));
            const glm::vec3 worldV1 = glm::vec3(worldMatrix * glm::vec4(localV1, 1.0f));
            const glm::vec3 worldV2 = glm::vec3(worldMatrix * glm::vec4(localV2, 1.0f));

            const float triangleArea = TriangleArea(worldV0, worldV1, worldV2);
            if (triangleArea < 1e-8f)
            {
                continue;
            }

            glm::vec3 faceNormal = glm::cross(worldV1 - worldV0, worldV2 - worldV0);
            const float normalLength = glm::length(faceNormal);
            if (normalLength < 1e-8f)
            {
                continue;
            }
            faceNormal /= normalLength;

            const float pickWeight = luminance * triangleArea;
            instanceArea += triangleArea;
            ++triangleCount;

            DxrEmissiveTriangleEntry& triangleEntry = triangles.emplace_back();
            triangleEntry.v0[0] = worldV0.x;
            triangleEntry.v0[1] = worldV0.y;
            triangleEntry.v0[2] = worldV0.z;
            triangleEntry.pickWeight = pickWeight;
            triangleEntry.v1[0] = worldV1.x;
            triangleEntry.v1[1] = worldV1.y;
            triangleEntry.v1[2] = worldV1.z;
            triangleEntry.triangleArea = triangleArea;
            triangleEntry.v2[0] = worldV2.x;
            triangleEntry.v2[1] = worldV2.y;
            triangleEntry.v2[2] = worldV2.z;
            triangleEntry.faceNormal[0] = faceNormal.x;
            triangleEntry.faceNormal[1] = faceNormal.y;
            triangleEntry.faceNormal[2] = faceNormal.z;
            triangleEntry.primitiveIndex = static_cast<std::uint32_t>(indexOffset / 3);
        }

        if (triangleCount == 0)
        {
            continue;
        }

        const float instancePickWeight = luminance * instanceArea;
        pickWeightSum += instancePickWeight;

        DxrEmissiveLightEntry& entry = entries.emplace_back();
        entry.emissive[0] = emissive.x;
        entry.emissive[1] = emissive.y;
        entry.emissive[2] = emissive.z;
        entry.pickWeight = instancePickWeight;
        entry.instanceId = renderInstance.instanceId;
        entry.triangleOffset = triangleOffset;
        entry.triangleCount = triangleCount;
        entry.surfaceArea = std::max(instanceArea, 1e-4f);
    }

    m_emissiveLightCount = static_cast<std::uint32_t>(entries.size());
    m_emissiveLightPickWeightSum = pickWeightSum;
    m_sceneHasTransmission = sceneHasTransmission;
    m_diagnostics.emissiveLightCount = m_emissiveLightCount;
    m_diagnostics.emissiveTriangleCount = static_cast<std::uint32_t>(triangles.size());
    m_diagnostics.emissiveLightPickWeightSum = pickWeightSum;

    std::vector<DxrEmissiveAliasEntry> lightAliases;
    std::vector<float> lightWeights;
    lightWeights.reserve(entries.size());
    for (const DxrEmissiveLightEntry& entry : entries)
    {
        lightWeights.push_back(entry.pickWeight);
    }
    BuildEmissiveAliasTable(lightWeights, 0u, lightAliases);

    std::vector<DxrEmissiveAliasEntry> triangleAliases;
    triangleAliases.reserve(triangles.size());
    for (const DxrEmissiveLightEntry& entry : entries)
    {
        std::vector<float> triangleWeights;
        triangleWeights.reserve(entry.triangleCount);
        for (std::uint32_t triangleIndex = 0; triangleIndex < entry.triangleCount; ++triangleIndex)
        {
            triangleWeights.push_back(triangles[entry.triangleOffset + triangleIndex].pickWeight);
        }
        BuildEmissiveAliasTable(triangleWeights, entry.triangleOffset, triangleAliases);
    }

    std::uint32_t maxInstanceId = 0;
    for (const DxrRenderableInstance& renderInstance : renderInstances)
    {
        maxInstanceId = std::max(maxInstanceId, renderInstance.instanceId);
    }
    std::vector<std::uint32_t> lightByInstance(
        renderInstances.empty() ? 0u : static_cast<std::size_t>(maxInstanceId) + 1u,
        UINT32_MAX);
    for (std::uint32_t lightIndex = 0; lightIndex < entries.size(); ++lightIndex)
    {
        lightByInstance[entries[lightIndex].instanceId] = lightIndex;
    }

    const std::uint64_t lightsByteSize = entries.size() * sizeof(DxrEmissiveLightEntry);
    const std::uint64_t trianglesByteSize = triangles.size() * sizeof(DxrEmissiveTriangleEntry);
    const std::uint64_t lightAliasByteSize = lightAliases.size() * sizeof(DxrEmissiveAliasEntry);
    const std::uint64_t triangleAliasByteSize = triangleAliases.size() * sizeof(DxrEmissiveAliasEntry);
    const std::uint64_t lightByInstanceByteSize = lightByInstance.size() * sizeof(std::uint32_t);
    const bool lightsOk = UploadStructuredBufferRing(
        d3dCommandList,
        entries.empty() ? nullptr : entries.data(),
        lightsByteSize,
        sizeof(DxrEmissiveLightEntry),
        static_cast<std::uint32_t>(entries.size()),
        m_emissiveLightsStaging,
        m_emissiveLightsGpu,
        m_emissiveLightsSrvIndices,
        m_emissiveLightsUploadFrame,
        m_emissiveLightsCapacityCount);
    const bool trianglesOk = UploadStructuredBufferRing(
        d3dCommandList,
        triangles.empty() ? nullptr : triangles.data(),
        trianglesByteSize,
        sizeof(DxrEmissiveTriangleEntry),
        static_cast<std::uint32_t>(triangles.size()),
        m_emissiveTrianglesStaging,
        m_emissiveTrianglesGpu,
        m_emissiveTrianglesSrvIndices,
        m_emissiveTrianglesUploadFrame,
        m_emissiveTrianglesCapacityCount);
    const bool lightAliasesOk = UploadStructuredBufferRing(
        d3dCommandList, lightAliases.empty() ? nullptr : lightAliases.data(), lightAliasByteSize,
        sizeof(DxrEmissiveAliasEntry), static_cast<std::uint32_t>(lightAliases.size()),
        m_emissiveLightAliasStaging, m_emissiveLightAliasGpu, m_emissiveLightAliasSrvIndices,
        m_emissiveLightAliasUploadFrame, m_emissiveLightAliasCapacityCount);
    const bool triangleAliasesOk = UploadStructuredBufferRing(
        d3dCommandList, triangleAliases.empty() ? nullptr : triangleAliases.data(), triangleAliasByteSize,
        sizeof(DxrEmissiveAliasEntry), static_cast<std::uint32_t>(triangleAliases.size()),
        m_emissiveTriangleAliasStaging, m_emissiveTriangleAliasGpu, m_emissiveTriangleAliasSrvIndices,
        m_emissiveTriangleAliasUploadFrame, m_emissiveTriangleAliasCapacityCount);
    const bool lightByInstanceOk = UploadStructuredBufferRing(
        d3dCommandList, lightByInstance.empty() ? nullptr : lightByInstance.data(), lightByInstanceByteSize,
        sizeof(std::uint32_t), static_cast<std::uint32_t>(lightByInstance.size()),
        m_emissiveLightByInstanceStaging, m_emissiveLightByInstanceGpu, m_emissiveLightByInstanceSrvIndices,
        m_emissiveLightByInstanceUploadFrame, m_emissiveLightByInstanceCapacityCount);
    if (!lightsOk || !trianglesOk || !lightAliasesOk || !triangleAliasesOk || !lightByInstanceOk)
    {
        m_emissiveLightCount = 0;
        m_emissiveLightPickWeightSum = 0.0f;
        m_diagnostics.emissiveLightCount = 0;
        m_diagnostics.emissiveTriangleCount = 0;
        m_diagnostics.emissiveLightPickWeightSum = 0.0f;
        return false;
    }

    return true;
}

void DxrAccelerationStructures::EnsureScene(
    const GpuScene& gpuScene,
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
    std::vector<DxrRenderableInstance> renderInstances = BuildDxrRenderableInstances(gpuScene);
    const bool emptyScene = renderInstances.empty();
    if (emptyScene)
    {
        renderInstances.push_back(DxrRenderableInstance{0u, 0u, 0u, EnsureEmptySceneMesh(), glm::mat4(1.0f)});
    }
    for (const DxrRenderableInstance& renderInstance : renderInstances)
    {
        uniqueMeshes.insert(renderInstance.mesh);
    }

    std::uint64_t maxScratchBytes = 0;
    DxrBreadcrumb("AS prebuild scratch sizing");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 != nullptr)
    {
        for (Mesh* mesh : uniqueMeshes)
        {
            const Blas* existing = m_blasCache.Find(mesh);
            if (existing != nullptr && existing->IsBuilt())
            {
                continue;
            }

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

        const std::uint32_t renderableCount = static_cast<std::uint32_t>(renderInstances.size());
        if (renderableCount > 0)
        {
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
            tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
            // Keep prebuild sizing consistent with Tlas::Build's retained-TLAS policy.
            tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
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
    instances.reserve(renderInstances.size());
    std::uint64_t referencedTriangles = 0;
    std::unordered_set<Mesh*> referencedMeshes;
    for (const DxrRenderableInstance& renderInstance : renderInstances)
    {
        Mesh* mesh = renderInstance.mesh;

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
        const glm::mat4 worldMatrix = renderInstance.world;
        WriteD3D12InstanceTransform(
            worldMatrix,
            reinterpret_cast<float*>(instanceDesc.Transform));
        instanceDesc.InstanceID = static_cast<UINT>(renderInstance.instanceId);
        // Split opaque vs dielectric so sun NEE can any-hit each class without paying
        // TraceTransmissiveVisibility when glass is not on the sun ray (see path_tracer.hlsl).
        // TraceRay inclusion = InstanceInclusionMask & InstanceMask != 0; primary rays still use 0xFF.
        UINT instanceMask = emptyScene ? 0u : 0x1u;
        if (!emptyScene && renderInstance.materialId < gpuScene.GetMaterials().size())
        {
            const GpuSceneMaterialRecord& material =
                gpuScene.GetMaterials()[renderInstance.materialId];
            const float dielectricWeight =
                std::clamp(material.transmission, 0.0f, 1.0f)
                * (1.0f - std::clamp(material.metallic, 0.0f, 1.0f));
            if (dielectricWeight > 0.0f)
            {
                instanceMask = 0x2u;
            }
        }
        instanceDesc.InstanceMask = instanceMask;
        instanceDesc.InstanceContributionToHitGroupIndex = 0;
        bool flipWinding = glm::determinant(glm::mat3(worldMatrix)) < 0.0f;
        const bool doubleSided =
            renderInstance.instanceId < gpuScene.GetInstances().size()
            && (gpuScene.GetInstances()[renderInstance.instanceId].flags & GpuSceneInstanceFlags::DoubleSided) != 0;
        if (doubleSided)
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

        const std::uint64_t topologyFingerprint = ComputeDxrTlasTopologyFingerprint(instances);
        const std::uint64_t transformFingerprint = ComputeDxrTlasTransformFingerprint(instances);
        const bool topologyChanged = topologyFingerprint != m_builtTlasTopologyFingerprint;
        const bool transformsChanged = transformFingerprint != m_builtTlasTransformFingerprint;
        const bool skipTlasBuild = !instances.empty()
            && m_tlas.IsBuilt()
            && !m_anyBlasBuiltThisFrame
            && !topologyChanged
            && transformFingerprint == m_builtTlasTransformFingerprint;

        if (!skipTlasBuild)
        {
            if (m_builtTlasTransformFingerprint != 0 && transformsChanged)
            {
                ++m_ptMotionVersion;
                if (m_ptMotionVersion == 0)
                {
                    m_ptMotionVersion = 1;
                }
            }
            if (topologyChanged || m_anyBlasBuiltThisFrame)
            {
                BumpPtSceneVersion();
            }

            if (!m_tlas.Build(commandList4, instances, m_scratchBuffer, error))
            {
                DxrBreadcrumb("AS tlas-build failed");
                m_diagnostics.buildStatus = "FAILED: " + error;
                return;
            }

            m_builtTlasTopologyFingerprint = topologyFingerprint;
            m_builtTlasTransformFingerprint = transformFingerprint;
        }

        DxrBreadcrumb("AS tlas-build end");
        tlasScope.Success();
    }

    const auto buildEnd = std::chrono::steady_clock::now();
    DxrBreadcrumb("AS complete ok");
    m_diagnostics.blasCount = m_blasCache.GetCount();
    m_diagnostics.tlasInstanceCount = emptyScene ? 0u : static_cast<std::uint32_t>(instances.size());
    m_diagnostics.totalRtTriangles = referencedTriangles;
    m_diagnostics.asGpuMemoryBytes =
        m_blasCache.GetTotalMemoryBytes() + m_tlas.GetSizeInBytes() + m_scratchHighWaterMark;
    m_diagnostics.buildStatus = "OK";
    m_diagnostics.lastBuildTimeMs =
        std::chrono::duration<double, std::milli>(buildEnd - buildStart).count();

    if (!EnsureGeometryBuffers(
            gpuScene,
            static_cast<ID3D12GraphicsCommandList*>(commandList4),
            error))
    {
        m_diagnostics.buildStatus = "FAILED: " + error;
        return;
    }
}
