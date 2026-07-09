#pragma once

#include "engine/raytracing/BlasCache.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/Tlas.h"
#include "engine/rhi/GfxContext.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>

class Scene;
struct ID3D12Resource;

struct DxrGeometryLookupEntry
{
    std::uint32_t vertexFloatOffset = 0;
    std::uint32_t vertexStrideFloats = 0;
    std::uint32_t indexUintOffset = 0;
    std::uint32_t pad0 = 0;
};

// Per-object material constants for in-hit reflection shading. Indexed by TLAS InstanceID
// (== scene object index). Layout must match MaterialEntry in assets/shaders/dxr/reflections.hlsl.
struct DxrMaterialEntry
{
    float albedo[3] = {0.5f, 0.5f, 0.5f};
    float metallic = 0.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};
    float roughness = 1.0f;
    // Bindless albedo texture (absolute SRV heap index; UINT32_MAX = none) + its UV float offset
    // within the interleaved vertex stride.
    std::uint32_t albedoTexIndex = UINT32_MAX;
    std::uint32_t albedoUvOffsetFloats = UINT32_MAX;
    std::uint32_t pad0 = 0;
    std::uint32_t pad1 = 0;
};

// P4b: per-instance previous-frame object-to-world transform, indexed by TLAS InstanceID
// (== scene object index). Stored as explicit rows (row_i = (m[0][i], m[1][i], m[2][i], m[3][i]))
// so the HLSL side reconstructs prevWorld = (dot(row0, p), dot(row1, p), dot(row2, p)) without
// depending on matrix packing rules. Layout must match PrevInstanceTransform in path_tracer.hlsl.
struct DxrPrevInstanceTransformEntry
{
    float row0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float row1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    float row2[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

class DxrAccelerationStructures
{
public:
    DxrAccelerationStructures() = default;
    ~DxrAccelerationStructures();

    DxrAccelerationStructures(const DxrAccelerationStructures&) = delete;
    DxrAccelerationStructures& operator=(const DxrAccelerationStructures&) = delete;

    void EnsureScene(const Scene& scene, bool dxrEnabled, void* commandList);
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

    // P4b: upload previous-frame object-to-world matrices (indexed by scene object index ==
    // InstanceID) for path-tracer object motion vectors. Must be recorded on the frame's command
    // list BEFORE the PT dispatch, with the matrices as they were BEFORE this frame's advance.
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

    void Release();

private:
    bool EnsureScratchBuffer(std::uint64_t requiredBytes, std::string& outError);
    bool EnsureGeometryBuffers(
        const Scene& scene,
        ID3D12GraphicsCommandList* commandList,
        std::string& outError);
    void ReleaseGeometryBuffers();

    DxrDiagnostics m_diagnostics{};
    BlasCache m_blasCache;
    Tlas m_tlas;
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
    std::size_t m_geometryObjectCount = 0;
    std::array<std::uint64_t, GfxContext::FrameCount> m_uploadedGeometryFingerprint{};
    std::uint64_t m_builtTlasTopologyFingerprint = 0;
    std::uint64_t m_builtTlasTransformFingerprint = 0;

    void EnsureScratchBufferReadyForBuild(ID3D12GraphicsCommandList* commandList);
};
