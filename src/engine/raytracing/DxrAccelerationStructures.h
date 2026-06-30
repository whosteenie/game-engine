#pragma once

#include "engine/raytracing/BlasCache.h"
#include "engine/raytracing/DxrDiagnostics.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/Tlas.h"

#include <cstdint>
#include <string>

class Scene;
struct ID3D12Resource;

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

    void Release();

private:
    bool EnsureScratchBuffer(std::uint64_t requiredBytes, std::string& outError);

    DxrDiagnostics m_diagnostics{};
    BlasCache m_blasCache;
    Tlas m_tlas;
    DxrGpuResource m_scratchBuffer{};
    std::uint64_t m_scratchHighWaterMark = 0;
    std::uint32_t m_scratchResourceState = 0;
    bool m_anyBlasBuiltThisFrame = false;

    void EnsureScratchBufferReadyForBuild(ID3D12GraphicsCommandList* commandList);
};
