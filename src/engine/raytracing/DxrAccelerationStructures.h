#pragma once

#include "engine/raytracing/BlasCache.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/Tlas.h"

#include <cstdint>
#include <string>
#include <vector>

class Scene;
struct ID3D12Resource;

struct DxrGeometryLookupEntry
{
    std::uint32_t vertexFloatOffset = 0;
    std::uint32_t vertexStrideFloats = 0;
    std::uint32_t indexUintOffset = 0;
    std::uint32_t pad0 = 0;
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

    std::uint32_t GetGeometryLookupSrvIndex() const { return m_geometryLookupSrvIndex; }
    std::uint32_t GetSceneVertexFloatsSrvIndex() const { return m_sceneVertexFloatsSrvIndex; }
    std::uint32_t GetSceneIndicesSrvIndex() const { return m_sceneIndicesSrvIndex; }
    bool HasGeometryLookup() const { return m_geometryLookupSrvIndex != UINT32_MAX; }

    void Release();

private:
    bool EnsureScratchBuffer(std::uint64_t requiredBytes, std::string& outError);
    bool EnsureGeometryBuffers(const Scene& scene, std::string& outError);
    void ReleaseGeometryBuffers();

    DxrDiagnostics m_diagnostics{};
    BlasCache m_blasCache;
    Tlas m_tlas;
    DxrGpuResource m_scratchBuffer{};
    std::uint64_t m_scratchHighWaterMark = 0;
    std::uint32_t m_scratchResourceState = 0;
    bool m_anyBlasBuiltThisFrame = false;

    DxrGpuResource m_geometryLookupBuffer{};
    DxrGpuResource m_sceneVertexFloatsBuffer{};
    DxrGpuResource m_sceneIndicesBuffer{};
    std::uint32_t m_geometryLookupSrvIndex = UINT32_MAX;
    std::uint32_t m_sceneVertexFloatsSrvIndex = UINT32_MAX;
    std::uint32_t m_sceneIndicesSrvIndex = UINT32_MAX;
    std::size_t m_geometryObjectCount = 0;

    void EnsureScratchBufferReadyForBuild(ID3D12GraphicsCommandList* commandList);
};
