#include "app/scene/GpuScene.h"

#include "app/scene/Scene.h"
#include "engine/rendering/Material.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/scene/SceneObject.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace
{
    constexpr std::uint32_t kMaterialFlagMetallicRoughnessMap = 1u << 0;
    constexpr std::uint32_t kMaterialFlagDoubleSided = 1u << 1;
    constexpr std::uint32_t kUv0OffsetFloats = 6u;
    constexpr std::uint32_t kUv1OffsetFloats = 8u;

    struct GpuSceneInstanceGpu
    {
        float world[16] = {};
        float prevWorld[16] = {};
        std::uint32_t meshId = 0xFFFFFFFFu;
        std::uint32_t materialId = 0xFFFFFFFFu;
        std::uint32_t flags = 0;
        std::uint32_t objectIndex = 0xFFFFFFFFu;
        std::uint32_t editorObjectIdLow = 0;
        std::uint32_t editorObjectIdHigh = 0;
        std::uint32_t _pad0 = 0;
        std::uint32_t _pad1 = 0;
    };

    struct GpuSceneMeshAssetGpu
    {
        std::uint32_t vertexCount = 0;
        std::uint32_t indexCount = 0;
        std::uint32_t floatsPerVertex = 0;
        std::uint32_t meshletCount = 0;
        std::uint32_t meshletVertexReferenceCount = 0;
        std::uint32_t meshletTriangleCount = 0;
        std::uint32_t _pad0 = 0;
        std::uint32_t _pad1 = 0;
    };

    struct GpuSceneMaterialGpu
    {
        float albedo[3] = {0.5f, 0.5f, 0.5f};
        float metallic = 0.0f;
        float emissive[3] = {0.0f, 0.0f, 0.0f};
        float roughness = 1.0f;
        std::uint32_t albedoTexIndex = 0xFFFFFFFFu;
        std::uint32_t albedoUvOffsetFloats = 0xFFFFFFFFu;
        std::uint32_t normalTexIndex = 0xFFFFFFFFu;
        std::uint32_t normalUvOffsetFloats = 0xFFFFFFFFu;
        std::uint32_t roughnessTexIndex = 0xFFFFFFFFu;
        std::uint32_t roughnessUvOffsetFloats = 0xFFFFFFFFu;
        std::uint32_t emissiveTexIndex = 0xFFFFFFFFu;
        std::uint32_t emissiveUvOffsetFloats = 0xFFFFFFFFu;
        std::uint32_t flags = 0;
        float transmission = 0.0f;
        float indexOfRefraction = 1.5f;
        float thinWalled = 0.0f;
    };

    static_assert(sizeof(GpuSceneInstanceGpu) == 160);
    static_assert(sizeof(GpuSceneMeshAssetGpu) == 32);
    static_assert(sizeof(GpuSceneMaterialGpu) == 80);

    std::uint32_t ToTexCoordSet(const int texCoordSet)
    {
        return texCoordSet > 0 ? 1u : 0u;
    }

    std::uint32_t UvOffsetFloatsForTexCoordSet(const std::uint32_t texCoordSet)
    {
        return texCoordSet == 1u ? kUv1OffsetFloats : kUv0OffsetFloats;
    }

    void CopyMatrix(float (&target)[16], const glm::mat4& matrix)
    {
        std::memcpy(target, &matrix[0][0], sizeof(target));
    }

    GpuSceneInstanceGpu BuildGpuInstanceRecord(const GpuSceneInstanceRecord& instance)
    {
        GpuSceneInstanceGpu gpu{};
        CopyMatrix(gpu.world, instance.world);
        CopyMatrix(gpu.prevWorld, instance.prevWorld);
        gpu.meshId = instance.meshId;
        gpu.materialId = instance.materialId;
        gpu.flags = instance.flags;
        gpu.objectIndex = instance.objectIndex;
        gpu.editorObjectIdLow = static_cast<std::uint32_t>(instance.editorObjectId & 0xFFFFFFFFull);
        gpu.editorObjectIdHigh = static_cast<std::uint32_t>((instance.editorObjectId >> 32) & 0xFFFFFFFFull);
        return gpu;
    }

    GpuSceneMeshAssetGpu BuildGpuMeshAssetRecord(const GpuSceneMeshAssetRecord& meshAsset)
    {
        GpuSceneMeshAssetGpu gpu{};
        gpu.vertexCount = meshAsset.vertexCount;
        gpu.indexCount = meshAsset.indexCount;
        gpu.floatsPerVertex = meshAsset.floatsPerVertex;
        gpu.meshletCount = meshAsset.meshletCount;
        gpu.meshletVertexReferenceCount = meshAsset.meshletVertexReferenceCount;
        gpu.meshletTriangleCount = meshAsset.meshletTriangleCount;
        return gpu;
    }

    GpuSceneMaterialGpu BuildGpuMaterialRecord(const GpuSceneMaterialRecord& material)
    {
        GpuSceneMaterialGpu gpu{};
        gpu.albedo[0] = material.albedo[0];
        gpu.albedo[1] = material.albedo[1];
        gpu.albedo[2] = material.albedo[2];
        gpu.metallic = material.metallic;
        gpu.emissive[0] = material.emissive[0];
        gpu.emissive[1] = material.emissive[1];
        gpu.emissive[2] = material.emissive[2];
        gpu.roughness = material.roughness;
        gpu.albedoTexIndex = material.albedoTexIndex;
        gpu.albedoUvOffsetFloats = material.albedoTexIndex == 0xFFFFFFFFu
            ? 0xFFFFFFFFu
            : UvOffsetFloatsForTexCoordSet(material.albedoTexCoordSet);
        gpu.normalTexIndex = material.normalTexIndex;
        gpu.normalUvOffsetFloats = material.normalTexIndex == 0xFFFFFFFFu
            ? 0xFFFFFFFFu
            : UvOffsetFloatsForTexCoordSet(material.normalTexCoordSet);
        gpu.roughnessTexIndex = material.roughnessTexIndex;
        gpu.roughnessUvOffsetFloats = material.roughnessTexIndex == 0xFFFFFFFFu
            ? 0xFFFFFFFFu
            : UvOffsetFloatsForTexCoordSet(material.roughnessTexCoordSet);
        gpu.emissiveTexIndex = material.emissiveTexIndex;
        gpu.emissiveUvOffsetFloats = material.emissiveTexIndex == 0xFFFFFFFFu
            ? 0xFFFFFFFFu
            : UvOffsetFloatsForTexCoordSet(material.emissiveTexCoordSet);
        gpu.flags = material.flags;
        gpu.transmission = material.transmission;
        gpu.indexOfRefraction = material.indexOfRefraction;
        gpu.thinWalled = material.thinWalled;
        return gpu;
    }

    void CollectRenderableSelectionIndices(
        const Scene& scene,
        const int objectIndex,
        std::unordered_set<int>& selectedRenderableIndices)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            return;
        }

        if (objects[static_cast<std::size_t>(objectIndex)].IsRenderable())
        {
            selectedRenderableIndices.insert(objectIndex);
        }

        for (const int childIndex : scene.GetChildren(objectIndex))
        {
            CollectRenderableSelectionIndices(scene, childIndex, selectedRenderableIndices);
        }
    }

    int FindFirstRenderableSelectionIndex(const Scene& scene, const int objectIndex)
    {
        const std::vector<SceneObject>& objects = scene.GetObjects();
        if (objectIndex < 0 || static_cast<std::size_t>(objectIndex) >= objects.size())
        {
            return -1;
        }

        if (objects[static_cast<std::size_t>(objectIndex)].IsRenderable())
        {
            return objectIndex;
        }

        for (const int childIndex : scene.GetChildren(objectIndex))
        {
            const int descendantIndex = FindFirstRenderableSelectionIndex(scene, childIndex);
            if (descendantIndex >= 0)
            {
                return descendantIndex;
            }
        }

        return -1;
    }
}

GpuScene::~GpuScene()
{
    ReleaseGpuResources();
}

void GpuScene::Clear()
{
    m_instances.clear();
    m_meshAssets.clear();
    m_materials.clear();
    m_objectIndexToInstanceId.clear();
    m_editorObjectIdToInstanceIds.clear();
    m_meshToId.clear();
    m_materialToId.clear();
    m_diagnostics = {};
}

bool GpuScene::UploadGpuTables(void* commandList)
{
    m_gpuDiagnostics.uploadValid = false;
    m_gpuDiagnostics.instanceCount = static_cast<std::uint32_t>(m_instances.size());
    m_gpuDiagnostics.meshAssetCount = static_cast<std::uint32_t>(m_meshAssets.size());
    m_gpuDiagnostics.materialCount = static_cast<std::uint32_t>(m_materials.size());
    m_gpuDiagnostics.instanceBytes = sizeof(GpuSceneInstanceGpu) * m_instances.size();
    m_gpuDiagnostics.meshAssetBytes = sizeof(GpuSceneMeshAssetGpu) * m_meshAssets.size();
    m_gpuDiagnostics.materialBytes = sizeof(GpuSceneMaterialGpu) * m_materials.size();

    if (m_instances.empty() || m_meshAssets.empty() || m_materials.empty())
    {
        ReleaseGpuResources();
        m_gpuDiagnostics.uploadValid = true;
        return true;
    }

    if (commandList == nullptr || !EnsureGpuTableCapacity(
            m_gpuDiagnostics.instanceBytes,
            m_gpuDiagnostics.meshAssetBytes,
            m_gpuDiagnostics.materialBytes)
        || !EnsureSrvDescriptors())
    {
        return false;
    }

    std::vector<GpuSceneInstanceGpu> gpuInstances;
    gpuInstances.reserve(m_instances.size());
    for (const GpuSceneInstanceRecord& instance : m_instances)
    {
        gpuInstances.push_back(BuildGpuInstanceRecord(instance));
    }

    std::vector<GpuSceneMeshAssetGpu> gpuMeshAssets;
    gpuMeshAssets.reserve(m_meshAssets.size());
    for (const GpuSceneMeshAssetRecord& meshAsset : m_meshAssets)
    {
        gpuMeshAssets.push_back(BuildGpuMeshAssetRecord(meshAsset));
    }

    std::vector<GpuSceneMaterialGpu> gpuMaterials;
    gpuMaterials.reserve(m_materials.size());
    for (const GpuSceneMaterialRecord& material : m_materials)
    {
        gpuMaterials.push_back(BuildGpuMaterialRecord(material));
    }

    const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
    auto uploadTable = [frameIndex, commandList](DxrUploadRing& uploadRing, DxrSrvBufferRing& gpuRing, const auto& data) {
        using ValueType = typename std::decay_t<decltype(data)>::value_type;
        const std::uint64_t byteSize = sizeof(ValueType) * data.size();
        DxrGpuResource& upload = uploadRing.Slot(frameIndex);
        void* mapped = nullptr;
        if (FAILED(upload.resource->Map(0, nullptr, &mapped)))
        {
            return false;
        }

        std::memcpy(mapped, data.data(), static_cast<std::size_t>(byteSize));
        upload.resource->Unmap(0, nullptr);
        CopyDxrUploadToSrvBuffer(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            upload,
            gpuRing.Slot(frameIndex),
            byteSize);
        return true;
    };

    if (!uploadTable(m_instanceUploadRing, m_instanceGpuRing, gpuInstances)
        || !uploadTable(m_meshAssetUploadRing, m_meshAssetGpuRing, gpuMeshAssets)
        || !uploadTable(m_materialUploadRing, m_materialGpuRing, gpuMaterials))
    {
        return false;
    }

    CreateSrvDescriptorsForCurrentCounts();
    ++m_gpuDiagnostics.uploadFrameCount;
    m_gpuDiagnostics.uploadValid = true;
    m_gpuDiagnostics.instanceSrvIndex = m_instanceSrvIndices[frameIndex];
    m_gpuDiagnostics.meshAssetSrvIndex = m_meshAssetSrvIndices[frameIndex];
    m_gpuDiagnostics.materialSrvIndex = m_materialSrvIndices[frameIndex];
    return true;
}

std::uint64_t GpuScene::GetInstanceTableGpuAddress() const
{
    return m_instanceGpuRing.Slot(GfxContext::Get().GetFrameIndex()).GetGpuVirtualAddress();
}

std::uint64_t GpuScene::GetMeshAssetTableGpuAddress() const
{
    return m_meshAssetGpuRing.Slot(GfxContext::Get().GetFrameIndex()).GetGpuVirtualAddress();
}

std::uint64_t GpuScene::GetMaterialTableGpuAddress() const
{
    return m_materialGpuRing.Slot(GfxContext::Get().GetFrameIndex()).GetGpuVirtualAddress();
}

void GpuScene::ReleaseGpuResources()
{
    m_instanceUploadRing.Release();
    m_meshAssetUploadRing.Release();
    m_materialUploadRing.Release();
    m_instanceGpuRing.Release();
    m_meshAssetGpuRing.Release();
    m_materialGpuRing.Release();
    ReleaseSrvDescriptors();
    m_gpuDiagnostics = {};
}

void GpuScene::Build(const Scene& scene, const PreviousWorldMap& previousWorldByObjectId)
{
    Clear();

    const std::vector<SceneObject>& objects = scene.GetObjects();
    m_instances.reserve(objects.size());
    m_objectIndexToInstanceId.assign(objects.size(), 0xFFFFFFFFu);

    std::unordered_set<SceneObjectId> seenEditorObjectIds;
    seenEditorObjectIds.reserve(objects.size());

    for (std::size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
    {
        const SceneObject& object = objects[objectIndex];
        if (!object.IsRenderable())
        {
            continue;
        }

        Mesh* mesh = object.GetMesh();
        if (mesh == nullptr || !object.HasMaterial())
        {
            ++m_diagnostics.invalidRenderableCount;
            m_diagnostics.valid = false;
            continue;
        }

        const SceneObjectId editorObjectId = object.GetId();
        if (editorObjectId == kInvalidSceneObjectId)
        {
            ++m_diagnostics.invalidEditorObjectIdCount;
            m_diagnostics.valid = false;
        }
        else if (!seenEditorObjectIds.insert(editorObjectId).second)
        {
            ++m_diagnostics.duplicateEditorObjectIdCount;
            m_diagnostics.valid = false;
        }

        const Material& material = object.GetMaterial();
        GpuSceneInstanceRecord instance{};
        instance.instanceId = static_cast<std::uint32_t>(m_instances.size());
        instance.objectIndex = static_cast<std::uint32_t>(objectIndex);
        instance.editorObjectId = editorObjectId;
        instance.meshId = GetOrCreateMeshAssetId(*mesh);
        instance.materialId = GetOrCreateMaterialId(material);
        instance.world = scene.GetWorldMatrix(static_cast<int>(objectIndex));
        const auto previousWorld = previousWorldByObjectId.find(editorObjectId);
        if (previousWorld != previousWorldByObjectId.end())
        {
            instance.prevWorld = previousWorld->second;
            ++m_diagnostics.previousWorldResolvedCount;
        }
        else
        {
            instance.prevWorld = instance.world;
            ++m_diagnostics.previousWorldInitializedCount;
        }
        if (object.CastsShadow())
        {
            instance.flags |= GpuSceneInstanceFlags::CastsShadow;
        }
        if (object.ReceivesShadow())
        {
            instance.flags |= GpuSceneInstanceFlags::ReceivesShadow;
        }
        if (material.IsDoubleSided())
        {
            instance.flags |= GpuSceneInstanceFlags::DoubleSided;
        }

        m_objectIndexToInstanceId[objectIndex] = instance.instanceId;
        m_editorObjectIdToInstanceIds[editorObjectId].push_back(instance.instanceId);
        m_instances.push_back(instance);
    }
}

std::uint32_t GpuScene::GetOrCreateMeshAssetId(Mesh& mesh)
{
    const auto existing = m_meshToId.find(&mesh);
    if (existing != m_meshToId.end())
    {
        return existing->second;
    }

    GpuSceneMeshAssetRecord record{};
    record.meshId = static_cast<std::uint32_t>(m_meshAssets.size());
    record.mesh = &mesh;
    record.vertexCount = static_cast<std::uint32_t>(mesh.GetPositions().size());
    record.indexCount = static_cast<std::uint32_t>(mesh.GetIndices().size());
    record.floatsPerVertex = mesh.GetFloatsPerVertex();
    record.meshletCount = mesh.GetMeshletCount();
    record.meshletVertexReferenceCount = mesh.GetMeshletVertexReferenceCount();
    record.meshletTriangleCount = mesh.GetMeshletTriangleCount();

    m_meshToId.emplace(&mesh, record.meshId);
    m_meshAssets.push_back(record);
    return record.meshId;
}

std::uint32_t GpuScene::GetOrCreateMaterialId(const Material& material)
{
    const auto existing = m_materialToId.find(&material);
    if (existing != m_materialToId.end())
    {
        return existing->second;
    }

    GpuSceneMaterialRecord record{};
    record.materialId = static_cast<std::uint32_t>(m_materials.size());
    record.material = &material;

    const glm::vec3 albedo = material.GetAlbedo();
    const glm::vec3 emissive = material.GetEmissive();
    record.albedo[0] = albedo.x;
    record.albedo[1] = albedo.y;
    record.albedo[2] = albedo.z;
    record.metallic = material.GetMetallic();
    record.emissive[0] = emissive.x;
    record.emissive[1] = emissive.y;
    record.emissive[2] = emissive.z;
    record.roughness = material.GetRoughness();
    record.transmission = material.GetTransmission();
    record.indexOfRefraction = material.GetIndexOfRefraction();
    record.thinWalled = material.IsThinWalled() ? 1.0f : 0.0f;
    record.albedoTexIndex = material.GetAlbedoMapSrvIndex();
    record.normalTexIndex = material.GetNormalMapSrvIndex();
    record.roughnessTexIndex = material.GetRoughnessMapSrvIndex();
    record.emissiveTexIndex = material.GetEmissiveMapSrvIndex();
    record.albedoTexCoordSet = ToTexCoordSet(material.GetAlbedoTexCoordSet());
    record.normalTexCoordSet = ToTexCoordSet(material.GetNormalTexCoordSet());
    record.roughnessTexCoordSet = ToTexCoordSet(material.GetRoughnessTexCoordSet());
    record.emissiveTexCoordSet = ToTexCoordSet(material.GetEmissiveTexCoordSet());
    if (material.IsDoubleSided())
    {
        record.flags |= kMaterialFlagDoubleSided;
    }
    if (material.HasMetallicRoughnessMap())
    {
        record.flags |= kMaterialFlagMetallicRoughnessMap;
    }

    m_materialToId.emplace(&material, record.materialId);
    m_materials.push_back(record);
    return record.materialId;
}

std::uint32_t GpuScene::FindInstanceForObjectIndex(const std::uint32_t objectIndex) const
{
    if (objectIndex >= m_objectIndexToInstanceId.size())
    {
        return 0xFFFFFFFFu;
    }

    return m_objectIndexToInstanceId[objectIndex];
}

const GpuSceneInstanceRecord* GpuScene::FindInstance(const std::uint32_t instanceId) const
{
    if (instanceId >= m_instances.size())
    {
        return nullptr;
    }

    return &m_instances[instanceId];
}

std::vector<std::uint32_t> GpuScene::FindInstancesForEditorObjectId(const SceneObjectId editorObjectId) const
{
    const auto existing = m_editorObjectIdToInstanceIds.find(editorObjectId);
    if (existing == m_editorObjectIdToInstanceIds.end())
    {
        return {};
    }

    return existing->second;
}

GpuScenePickResult GpuScene::ResolvePickedInstanceId(const std::uint32_t instanceId) const
{
    GpuScenePickResult result{};
    const GpuSceneInstanceRecord* instance = FindInstance(instanceId);
    if (instance == nullptr)
    {
        return result;
    }

    result.valid = true;
    result.instanceId = instance->instanceId;
    result.objectIndex = instance->objectIndex;
    result.editorObjectId = instance->editorObjectId;
    result.meshId = instance->meshId;
    result.materialId = instance->materialId;
    return result;
}

std::uint32_t GpuScene::CountSelectedRenderInstances(const Scene& scene) const
{
    std::unordered_set<int> selectedObjectIndices;
    for (const int selectedIndex : scene.GetSelection().indices)
    {
        CollectRenderableSelectionIndices(scene, selectedIndex, selectedObjectIndices);
    }

    std::uint32_t selectedInstanceCount = 0;
    for (const int objectIndex : selectedObjectIndices)
    {
        if (objectIndex >= 0 && FindInstanceForObjectIndex(static_cast<std::uint32_t>(objectIndex)) != 0xFFFFFFFFu)
        {
            ++selectedInstanceCount;
        }
    }

    return selectedInstanceCount;
}

const GpuSceneInstanceRecord* GpuScene::FindPrimarySelectionInstance(const Scene& scene) const
{
    const int primaryRenderableIndex = FindFirstRenderableSelectionIndex(scene, scene.GetPrimarySelection());
    if (primaryRenderableIndex < 0)
    {
        return nullptr;
    }

    const std::uint32_t instanceId = FindInstanceForObjectIndex(static_cast<std::uint32_t>(primaryRenderableIndex));
    return FindInstance(instanceId);
}

bool GpuScene::EnsureGpuTableCapacity(
    const std::uint64_t instanceBytes,
    const std::uint64_t meshAssetBytes,
    const std::uint64_t materialBytes)
{
    const bool willResize =
        instanceBytes > m_instanceGpuRing.GetCapacity()
        || meshAssetBytes > m_meshAssetGpuRing.GetCapacity()
        || materialBytes > m_materialGpuRing.GetCapacity();

    if (!m_instanceUploadRing.EnsureCapacity(instanceBytes)
        || !m_meshAssetUploadRing.EnsureCapacity(meshAssetBytes)
        || !m_materialUploadRing.EnsureCapacity(materialBytes)
        || !m_instanceGpuRing.EnsureCapacity(instanceBytes)
        || !m_meshAssetGpuRing.EnsureCapacity(meshAssetBytes)
        || !m_materialGpuRing.EnsureCapacity(materialBytes))
    {
        ReleaseGpuResources();
        return false;
    }

    if (willResize)
    {
        ReleaseSrvDescriptors();
        ++m_gpuDiagnostics.resizeEventCount;
    }

    return true;
}

bool GpuScene::EnsureSrvDescriptors()
{
    for (std::uint32_t frameIndex = 0; frameIndex < GfxContext::FrameCount; ++frameIndex)
    {
        if (m_instanceSrvIndices[frameIndex] == 0xFFFFFFFFu)
        {
            m_instanceSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
        }
        if (m_meshAssetSrvIndices[frameIndex] == 0xFFFFFFFFu)
        {
            m_meshAssetSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
        }
        if (m_materialSrvIndices[frameIndex] == 0xFFFFFFFFu)
        {
            m_materialSrvIndices[frameIndex] = GfxContext::Get().AllocateOffscreenSrv();
        }

        if (m_instanceSrvIndices[frameIndex] == 0xFFFFFFFFu
            || m_meshAssetSrvIndices[frameIndex] == 0xFFFFFFFFu
            || m_materialSrvIndices[frameIndex] == 0xFFFFFFFFu)
        {
            ReleaseSrvDescriptors();
            return false;
        }
    }

    return true;
}

void GpuScene::CreateSrvDescriptorsForCurrentCounts()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        return;
    }

    for (std::uint32_t frameIndex = 0; frameIndex < GfxContext::FrameCount; ++frameIndex)
    {
        auto createStructuredSrv =
            [device](ID3D12Resource* resource, const std::uint32_t descriptorIndex, const std::uint32_t count, const std::uint32_t stride) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srvDesc.Format = DXGI_FORMAT_UNKNOWN;
                srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srvDesc.Buffer.FirstElement = 0;
                srvDesc.Buffer.NumElements = count;
                srvDesc.Buffer.StructureByteStride = stride;
                D3D12_CPU_DESCRIPTOR_HANDLE handle{};
                handle.ptr = GfxContext::Get().GetSrvCpuHandle(descriptorIndex);
                device->CreateShaderResourceView(resource, &srvDesc, handle);
            };

        createStructuredSrv(
            m_instanceGpuRing.Slot(frameIndex).resource,
            m_instanceSrvIndices[frameIndex],
            static_cast<std::uint32_t>(m_instances.size()),
            sizeof(GpuSceneInstanceGpu));
        createStructuredSrv(
            m_meshAssetGpuRing.Slot(frameIndex).resource,
            m_meshAssetSrvIndices[frameIndex],
            static_cast<std::uint32_t>(m_meshAssets.size()),
            sizeof(GpuSceneMeshAssetGpu));
        createStructuredSrv(
            m_materialGpuRing.Slot(frameIndex).resource,
            m_materialSrvIndices[frameIndex],
            static_cast<std::uint32_t>(m_materials.size()),
            sizeof(GpuSceneMaterialGpu));
    }
}

void GpuScene::ReleaseSrvDescriptors()
{
    for (std::uint32_t frameIndex = 0; frameIndex < GfxContext::FrameCount; ++frameIndex)
    {
        if (m_instanceSrvIndices[frameIndex] != 0xFFFFFFFFu)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(m_instanceSrvIndices[frameIndex]);
            m_instanceSrvIndices[frameIndex] = 0xFFFFFFFFu;
        }
        if (m_meshAssetSrvIndices[frameIndex] != 0xFFFFFFFFu)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(m_meshAssetSrvIndices[frameIndex]);
            m_meshAssetSrvIndices[frameIndex] = 0xFFFFFFFFu;
        }
        if (m_materialSrvIndices[frameIndex] != 0xFFFFFFFFu)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(m_materialSrvIndices[frameIndex]);
            m_materialSrvIndices[frameIndex] = 0xFFFFFFFFu;
        }
    }
}
