#pragma once

#include "engine/scene/SceneObjectId.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/rhi/GfxContext.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

class Material;
class Mesh;
class Scene;

struct GpuSceneInstanceFlags
{
    static constexpr std::uint32_t CastsShadow = 1u << 0;
    static constexpr std::uint32_t ReceivesShadow = 1u << 1;
    static constexpr std::uint32_t DoubleSided = 1u << 2;
};

struct GpuSceneInstanceRecord
{
    std::uint32_t instanceId = 0xFFFFFFFFu;
    std::uint32_t objectIndex = 0xFFFFFFFFu;
    SceneObjectId editorObjectId = kInvalidSceneObjectId;
    std::uint32_t meshId = 0xFFFFFFFFu;
    std::uint32_t materialId = 0xFFFFFFFFu;
    std::uint32_t flags = 0;
    glm::mat4 world{1.0f};
    glm::mat4 prevWorld{1.0f};
};

struct GpuSceneMeshAssetRecord
{
    std::uint32_t meshId = 0xFFFFFFFFu;
    Mesh* mesh = nullptr;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t floatsPerVertex = 0;
    std::uint32_t meshletCount = 0;
    std::uint32_t meshletVertexReferenceCount = 0;
    std::uint32_t meshletTriangleCount = 0;
};

struct GpuSceneMaterialRecord
{
    std::uint32_t materialId = 0xFFFFFFFFu;
    const Material* material = nullptr;
    float albedo[3] = {0.5f, 0.5f, 0.5f};
    float metallic = 0.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float roughness = 1.0f;
    float transmission = 0.0f;
    float indexOfRefraction = 1.5f;
    float thinWalled = 0.0f;
    std::uint32_t albedoTexIndex = 0xFFFFFFFFu;
    std::uint32_t normalTexIndex = 0xFFFFFFFFu;
    std::uint32_t roughnessTexIndex = 0xFFFFFFFFu;
    std::uint32_t emissiveTexIndex = 0xFFFFFFFFu;
    std::uint32_t albedoTexCoordSet = 0;
    std::uint32_t normalTexCoordSet = 0;
    std::uint32_t roughnessTexCoordSet = 0;
    std::uint32_t emissiveTexCoordSet = 0;
    std::uint32_t flags = 0;
};

struct GpuSceneBuildDiagnostics
{
    bool valid = true;
    std::uint32_t duplicateEditorObjectIdCount = 0;
    std::uint32_t invalidEditorObjectIdCount = 0;
    std::uint32_t invalidRenderableCount = 0;
    std::uint32_t previousWorldResolvedCount = 0;
    std::uint32_t previousWorldInitializedCount = 0;
};

struct GpuSceneGpuDiagnostics
{
    bool uploadValid = false;
    std::uint32_t uploadFrameCount = 0;
    std::uint32_t resizeEventCount = 0;
    std::uint32_t instanceSrvIndex = 0xFFFFFFFFu;
    std::uint32_t meshAssetSrvIndex = 0xFFFFFFFFu;
    std::uint32_t materialSrvIndex = 0xFFFFFFFFu;
    std::uint32_t instanceCount = 0;
    std::uint32_t meshAssetCount = 0;
    std::uint32_t materialCount = 0;
    std::uint64_t instanceBytes = 0;
    std::uint64_t meshAssetBytes = 0;
    std::uint64_t materialBytes = 0;
};

class GpuScene
{
public:
    using PreviousWorldMap = std::unordered_map<SceneObjectId, glm::mat4>;

    ~GpuScene();

    void Build(const Scene& scene, const PreviousWorldMap& previousWorldByObjectId);
    void Clear();
    bool UploadGpuTables(void* commandList);
    void ReleaseGpuResources();

    const std::vector<GpuSceneInstanceRecord>& GetInstances() const { return m_instances; }
    const std::vector<GpuSceneMeshAssetRecord>& GetMeshAssets() const { return m_meshAssets; }
    const std::vector<GpuSceneMaterialRecord>& GetMaterials() const { return m_materials; }
    const GpuSceneBuildDiagnostics& GetDiagnostics() const { return m_diagnostics; }
    const GpuSceneGpuDiagnostics& GetGpuDiagnostics() const { return m_gpuDiagnostics; }

    std::uint32_t GetInstanceTableSrvIndex() const { return m_instanceSrvIndices[GfxContext::Get().GetFrameIndex()]; }
    std::uint32_t GetMeshAssetTableSrvIndex() const { return m_meshAssetSrvIndices[GfxContext::Get().GetFrameIndex()]; }
    std::uint32_t GetMaterialTableSrvIndex() const { return m_materialSrvIndices[GfxContext::Get().GetFrameIndex()]; }
    std::uint64_t GetInstanceTableGpuAddress() const;
    std::uint64_t GetMeshAssetTableGpuAddress() const;
    std::uint64_t GetMaterialTableGpuAddress() const;

    std::uint32_t FindInstanceForObjectIndex(std::uint32_t objectIndex) const;
    const GpuSceneInstanceRecord* FindInstance(std::uint32_t instanceId) const;
    std::vector<std::uint32_t> FindInstancesForEditorObjectId(SceneObjectId editorObjectId) const;

    std::uint32_t CountSelectedRenderInstances(const Scene& scene) const;
    const GpuSceneInstanceRecord* FindPrimarySelectionInstance(const Scene& scene) const;

private:
    std::uint32_t GetOrCreateMeshAssetId(Mesh& mesh);
    std::uint32_t GetOrCreateMaterialId(const Material& material);
    bool EnsureGpuTableCapacity(
        std::uint64_t instanceBytes,
        std::uint64_t meshAssetBytes,
        std::uint64_t materialBytes);
    bool EnsureSrvDescriptors();
    void CreateSrvDescriptorsForCurrentCounts();
    void ReleaseSrvDescriptors();

    std::vector<GpuSceneInstanceRecord> m_instances;
    std::vector<GpuSceneMeshAssetRecord> m_meshAssets;
    std::vector<GpuSceneMaterialRecord> m_materials;
    std::vector<std::uint32_t> m_objectIndexToInstanceId;
    std::unordered_map<SceneObjectId, std::vector<std::uint32_t>> m_editorObjectIdToInstanceIds;
    std::unordered_map<const Mesh*, std::uint32_t> m_meshToId;
    std::unordered_map<const Material*, std::uint32_t> m_materialToId;
    GpuSceneBuildDiagnostics m_diagnostics{};
    GpuSceneGpuDiagnostics m_gpuDiagnostics{};
    DxrUploadRing m_instanceUploadRing{};
    DxrUploadRing m_meshAssetUploadRing{};
    DxrUploadRing m_materialUploadRing{};
    DxrSrvBufferRing m_instanceGpuRing{};
    DxrSrvBufferRing m_meshAssetGpuRing{};
    DxrSrvBufferRing m_materialGpuRing{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_instanceSrvIndices{0xFFFFFFFFu, 0xFFFFFFFFu};
    std::array<std::uint32_t, GfxContext::FrameCount> m_meshAssetSrvIndices{0xFFFFFFFFu, 0xFFFFFFFFu};
    std::array<std::uint32_t, GfxContext::FrameCount> m_materialSrvIndices{0xFFFFFFFFu, 0xFFFFFFFFu};
};
