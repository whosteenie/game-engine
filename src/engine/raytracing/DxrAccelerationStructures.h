#pragma once

#include "engine/raytracing/BlasCache.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/Tlas.h"
#include "engine/rhi/GfxContext.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

class Scene;
class GpuScene;
class Mesh;
struct ID3D12Resource;

struct DxrGeometryLookupEntry
{
    std::uint32_t vertexFloatOffset = 0;
    std::uint32_t vertexStrideFloats = 0;
    std::uint32_t indexUintOffset = 0;
    // Compact render-instance row data. InstanceID() indexes this table, then materialId resolves
    // into the material table. Geometry offsets may be shared by every instance of one Mesh.
    std::uint32_t materialId = 0;
};

// Material constants for in-hit reflection shading. Indexed by
// g_GeometryLookup[InstanceID()].materialId. Layout must match MaterialEntry in
// assets/shaders/raytracing/common/hit_shading.hlsli.
struct DxrMaterialEntry
{
    float albedo[3] = {0.5f, 0.5f, 0.5f};
    float metallic = 0.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float roughness = 1.0f;
    // Bindless textures (absolute SRV heap index; UINT32_MAX = none) + UV float offsets
    // within the interleaved vertex stride (6 = UV0, 8 = UV1).
    std::uint32_t albedoTexIndex = UINT32_MAX;
    std::uint32_t albedoUvOffsetFloats = UINT32_MAX;
    std::uint32_t normalTexIndex = UINT32_MAX;
    std::uint32_t normalUvOffsetFloats = UINT32_MAX;
    std::uint32_t roughnessTexIndex = UINT32_MAX;
    std::uint32_t roughnessUvOffsetFloats = UINT32_MAX;
    std::uint32_t emissiveTexIndex = UINT32_MAX;
    std::uint32_t emissiveUvOffsetFloats = UINT32_MAX;
    std::uint32_t materialFlags = 0;
    std::uint32_t tangentOffsetFloats = UINT32_MAX;
    float transmission = 0.0f; // 0 = opaque, 1 = fully transmissive (PT-A glass)
    float indexOfRefraction = 1.5f;
    float thinWalled = 0.0f; // 1 = thin slab (window pane); 0 = solid volume (lens)
    float _padDielectric = 0.0f;
};
static_assert(sizeof(DxrMaterialEntry) == 88);

// P4b: per-instance previous-frame object-to-world transform, indexed by compact TLAS InstanceID.
// Stored as explicit rows (row_i = (m[0][i], m[1][i], m[2][i], m[3][i]))
// so the HLSL side reconstructs prevWorld = (dot(row0, p), dot(row1, p), dot(row2, p)) without
// depending on matrix packing rules. Layout must match PrevInstanceTransform in path_tracer.hlsl.
struct DxrPrevInstanceTransformEntry
{
    float row0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float row1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float row2[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

// F2/F2b emissive NEE: one entry per emissive scene instance (S5 step 14 / A1).
// Layout must match EmissiveLightEntry in assets/shaders/raytracing/path_tracing/path_tracer.hlsl.
struct DxrEmissiveLightEntry
{
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float pickWeight = 0.0f;
    std::uint32_t instanceId = 0;
    std::uint32_t triangleOffset = 0;
    std::uint32_t triangleCount = 0;
    float surfaceArea = 0.0f;
};
static_assert(sizeof(DxrEmissiveLightEntry) == 32);

// Per-triangle emissive geometry in world space (uploaded alongside lights, t18).
struct DxrEmissiveTriangleEntry
{
    float v0[3] = {0.0f, 0.0f, 0.0f};
    float pickWeight = 0.0f;
    float v1[3] = {0.0f, 0.0f, 0.0f};
    float triangleArea = 0.0f;
    float v2[3] = {0.0f, 0.0f, 0.0f};
    float _pad0 = 0.0f;
    float faceNormal[3] = {0.0f, 0.0f, 1.0f};
    std::uint32_t primitiveIndex = 0;
};
static_assert(sizeof(DxrEmissiveTriangleEntry) == 64);

struct DxrEmissiveAliasEntry
{
    float probability = 1.0f;
    std::uint32_t aliasIndex = 0;
};
static_assert(sizeof(DxrEmissiveAliasEntry) == 8);

class DxrAccelerationStructures
{
public:
    DxrAccelerationStructures() = default;
    ~DxrAccelerationStructures();

    DxrAccelerationStructures(const DxrAccelerationStructures&) = delete;
    DxrAccelerationStructures& operator=(const DxrAccelerationStructures&) = delete;

    void EnsureScene(const Scene& scene, const GpuScene& gpuScene, bool dxrEnabled, void* commandList);
    const DxrDiagnostics& GetDiagnostics() const { return m_diagnostics; }

    bool IsTlasBuilt() const { return m_tlas.IsBuilt(); }
    std::uint64_t GetTlasGpuVirtualAddress() const { return m_tlas.GetGpuVirtualAddress(); }
    ID3D12Resource* GetTlasResource() const { return m_tlas.GetResultResource(); }

    std::uint32_t GetGeometryLookupSrvIndex() const
    {
        return m_geometryLookupSrvIndices[GfxContext::Get().GetFrameIndex()];
    }

    std::uint32_t GetSceneVertexFloatsSrvIndex() const
    {
        return m_sceneVertexFloatsSrvIndices[GfxContext::Get().GetFrameIndex()];
    }

    std::uint32_t GetSceneIndicesSrvIndex() const
    {
        return m_sceneIndicesSrvIndices[GfxContext::Get().GetFrameIndex()];
    }

    std::uint32_t GetMaterialSrvIndex() const
    {
        return m_materialSrvIndices[GfxContext::Get().GetFrameIndex()];
    }

    bool HasGeometryLookup() const
    {
        return m_geometryLookupSrvIndices[GfxContext::Get().GetFrameIndex()] != UINT32_MAX;
    }
    std::size_t GetGeometryObjectCount() const { return m_geometryObjectCount; }

    // P4b: upload previous-frame object-to-world matrices (indexed by compact InstanceID) for
    // path-tracer object motion vectors. Must be recorded on the frame's command list BEFORE the
    // PT dispatch, with the matrices as they were BEFORE this frame's advance.
    // Returns false (and leaves the SRV stale) on allocation failure.
    bool UploadPrevInstanceTransforms(
        const std::vector<glm::mat4>& prevWorldMatrices,
        void* commandList);

    // Valid only for the frame UploadPrevInstanceTransforms ran; UINT32_MAX otherwise.
    std::uint32_t GetPrevInstanceTransformsSrvIndex() const
    {
        const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
        return m_prevTransformsUploadFrame[frameIndex] == GfxContext::Get().GetSubmissionFrameNumber()
            ? m_prevTransformsSrvIndices[frameIndex]
            : UINT32_MAX;
    }

    // F2: upload emissive instance list for path-tracer NEE. Must be recorded on this frame's
    // command list BEFORE the PT dispatch that reads it (t15).
    bool UploadEmissiveLights(const Scene& scene, const GpuScene& gpuScene, void* commandList);

    std::uint32_t GetEmissiveLightCount() const { return m_emissiveLightCount; }
    float GetEmissiveLightPickWeightSum() const { return m_emissiveLightPickWeightSum; }
    // True when any renderable has dielectricWeight > 0 (transmission through glass for NEE shadows).
    bool SceneHasTransmission() const { return m_sceneHasTransmission; }

    // True once after mesh/material geometry buffers were re-uploaded; clears on read.
    bool ConsumeGeometryContentReupload();

    // G1 (ReSTIR R0): monotonic scene version for PT temporal histories (reference accumulation,
    // DLSS-RR, future reservoirs). Bumped on geometry/material/topology edits; SceneRenderer bumps
    // on environment and PT-setting fingerprint changes.
    std::uint32_t GetPtSceneVersion() const { return m_ptSceneVersion; }
    std::uint32_t GetPtMotionVersion() const { return m_ptMotionVersion; }
    void BumpPtSceneVersion();

    // Valid only for the frame UploadEmissiveLights ran; UINT32_MAX otherwise.
    std::uint32_t GetEmissiveLightsSrvIndex() const
    {
        const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
        return m_emissiveLightsUploadFrame[frameIndex] == GfxContext::Get().GetSubmissionFrameNumber()
            ? m_emissiveLightsSrvIndices[frameIndex]
            : UINT32_MAX;
    }

    std::uint32_t GetEmissiveTrianglesSrvIndex() const
    {
        const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
        return m_emissiveTrianglesUploadFrame[frameIndex] == GfxContext::Get().GetSubmissionFrameNumber()
            ? m_emissiveTrianglesSrvIndices[frameIndex]
            : UINT32_MAX;
    }

    std::uint32_t GetEmissiveLightAliasSrvIndex() const
    {
        return GetFrameUploadedSrvIndex(m_emissiveLightAliasSrvIndices, m_emissiveLightAliasUploadFrame);
    }
    std::uint32_t GetEmissiveTriangleAliasSrvIndex() const
    {
        return GetFrameUploadedSrvIndex(m_emissiveTriangleAliasSrvIndices, m_emissiveTriangleAliasUploadFrame);
    }
    std::uint32_t GetEmissiveLightByInstanceSrvIndex() const
    {
        return GetFrameUploadedSrvIndex(m_emissiveLightByInstanceSrvIndices, m_emissiveLightByInstanceUploadFrame);
    }

    void Release();

private:
    std::uint32_t GetFrameUploadedSrvIndex(
        const std::array<std::uint32_t, GfxContext::FrameCount>& indices,
        const std::array<std::uint64_t, GfxContext::FrameCount>& uploadFrames) const
    {
        const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
        return uploadFrames[frameIndex] == GfxContext::Get().GetSubmissionFrameNumber()
            ? indices[frameIndex]
            : UINT32_MAX;
    }
    bool EnsureScratchBuffer(std::uint64_t requiredBytes, std::string& outError);
    Mesh* EnsureEmptySceneMesh();
    bool EnsureGeometryBuffers(
        const Scene& scene,
        const GpuScene& gpuScene,
        ID3D12GraphicsCommandList* commandList,
        std::string& outError);
    void ReleaseGeometryBuffers();

    DxrDiagnostics m_diagnostics{};
    BlasCache m_blasCache;
    Tlas m_tlas;
    // Kept outside the user scene.  Its sole TLAS instance uses mask 0, so it provides a valid
    // all-miss acceleration structure for an otherwise empty path-traced scene.
    std::unique_ptr<Mesh> m_emptySceneMesh;
    DxrGpuResource m_scratchBuffer{};
    std::uint64_t m_scratchHighWaterMark = 0;
    std::uint32_t m_scratchResourceState = 0;
    bool m_anyBlasBuiltThisFrame = false;

    DxrUploadRing m_geometryLookupStaging{};
    DxrUploadRing m_sceneVertexFloatsStaging{};
    DxrUploadRing m_sceneIndicesStaging{};
    DxrUploadRing m_materialStaging{};
    DxrSrvBufferRing m_geometryLookupGpu{};
    DxrSrvBufferRing m_sceneVertexFloatsGpu{};
    DxrSrvBufferRing m_sceneIndicesGpu{};
    DxrSrvBufferRing m_materialGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_geometryLookupSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    std::array<std::uint32_t, GfxContext::FrameCount> m_sceneVertexFloatsSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    std::array<std::uint32_t, GfxContext::FrameCount> m_sceneIndicesSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    std::array<std::uint32_t, GfxContext::FrameCount> m_materialSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    // P4b prev-instance transforms (t14): per-frame upload → default-heap SRV ring.
    DxrUploadRing m_prevTransformsStaging{};
    DxrSrvBufferRing m_prevTransformsGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_prevTransformsSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    std::array<std::uint64_t, GfxContext::FrameCount> m_prevTransformsUploadFrame{};
    std::size_t m_prevTransformsCapacityCount = 0;
    // F2 emissive NEE (t15): compact list of emissive instances rebuilt every PT frame.
    DxrUploadRing m_emissiveLightsStaging{};
    DxrSrvBufferRing m_emissiveLightsGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_emissiveLightsSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    std::array<std::uint64_t, GfxContext::FrameCount> m_emissiveLightsUploadFrame{};
    std::size_t m_emissiveLightsCapacityCount = 0;
    DxrUploadRing m_emissiveTrianglesStaging{};
    DxrSrvBufferRing m_emissiveTrianglesGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_emissiveTrianglesSrvIndices{
        UINT32_MAX,
        UINT32_MAX};
    std::array<std::uint64_t, GfxContext::FrameCount> m_emissiveTrianglesUploadFrame{};
    std::size_t m_emissiveTrianglesCapacityCount = 0;
    DxrUploadRing m_emissiveLightAliasStaging{};
    DxrSrvBufferRing m_emissiveLightAliasGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_emissiveLightAliasSrvIndices{UINT32_MAX, UINT32_MAX};
    std::array<std::uint64_t, GfxContext::FrameCount> m_emissiveLightAliasUploadFrame{};
    std::size_t m_emissiveLightAliasCapacityCount = 0;
    DxrUploadRing m_emissiveTriangleAliasStaging{};
    DxrSrvBufferRing m_emissiveTriangleAliasGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_emissiveTriangleAliasSrvIndices{UINT32_MAX, UINT32_MAX};
    std::array<std::uint64_t, GfxContext::FrameCount> m_emissiveTriangleAliasUploadFrame{};
    std::size_t m_emissiveTriangleAliasCapacityCount = 0;
    DxrUploadRing m_emissiveLightByInstanceStaging{};
    DxrSrvBufferRing m_emissiveLightByInstanceGpu{};
    std::array<std::uint32_t, GfxContext::FrameCount> m_emissiveLightByInstanceSrvIndices{UINT32_MAX, UINT32_MAX};
    std::array<std::uint64_t, GfxContext::FrameCount> m_emissiveLightByInstanceUploadFrame{};
    std::size_t m_emissiveLightByInstanceCapacityCount = 0;
    std::uint32_t m_emissiveLightCount = 0;
    float m_emissiveLightPickWeightSum = 0.0f;
    // Any renderable with dielectricWeight > 0 — selects opaque vs transmissive NEE shadows.
    bool m_sceneHasTransmission = false;
    std::size_t m_geometryObjectCount = 0;
    std::array<std::uint64_t, GfxContext::FrameCount> m_uploadedGeometryFingerprint{};
    std::uint64_t m_builtTlasTopologyFingerprint = 0;
    std::uint64_t m_builtTlasTransformFingerprint = 0;
    bool m_pendingGeometryContentReupload = false;
    std::uint32_t m_ptSceneVersion = 1;
    // Changes on any TLAS instance-transform change. Per-pixel motion/surface validation decides
    // whether an individual temporal ReSTIR sample can be reused.
    std::uint32_t m_ptMotionVersion = 1;

    void EnsureScratchBufferReadyForBuild(ID3D12GraphicsCommandList* commandList);
};
