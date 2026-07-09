#pragma once

#include "engine/camera/Camera.h"
#include "engine/raytracing/Blas.h"
#include "engine/raytracing/DxrAccelerationStructures.h"
#include "engine/raytracing/DxrDispatchContext.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrPipeline.h"
#include "engine/raytracing/DxrRootSignature.h"
#include "engine/raytracing/ShaderBindingTable.h"
#include "engine/raytracing/Tlas.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class IBL;
class Mesh;

struct ID3D12GraphicsCommandList4;

// Minimal two-instance PT scene (glass pane + colored backdrop) without the app Scene class.
class MinimalPtGlassScene
{
public:
    struct InstanceDesc
    {
        Mesh* mesh = nullptr;
        glm::mat4 transform{1.0f};
        DxrMaterialEntry material{};
    };

    bool Build(ID3D12GraphicsCommandList4* commandList, DxrGpuResource& scratch, bool includeGlassPane, std::string& outError);
    void Release();

    bool IsReady() const { return m_tlas.IsBuilt() && m_geometryLookupSrvIndex != UINT32_MAX; }

    ID3D12Resource* GetTlasResource() const { return m_tlas.GetResultResource(); }
    std::uint64_t GetTlasGpuVirtualAddress() const { return m_tlas.GetGpuVirtualAddress(); }
    std::uint32_t GetGeometryLookupSrvIndex() const { return m_geometryLookupSrvIndex; }
    std::uint32_t GetSceneVertexFloatsSrvIndex() const { return m_sceneVertexFloatsSrvIndex; }
    std::uint32_t GetSceneIndicesSrvIndex() const { return m_sceneIndicesSrvIndex; }
    std::uint32_t GetMaterialSrvIndex() const { return m_materialSrvIndex; }

    static glm::mat4 MakeVerticalPlaneTransform(const float zPosition);

private:
    std::unique_ptr<Mesh> m_backdropMesh;
    Blas m_backdropBlas;
    Tlas m_tlas;
    DxrUploadRing m_geometryLookupStaging{};
    DxrUploadRing m_sceneVertexFloatsStaging{};
    DxrUploadRing m_sceneIndicesStaging{};
    DxrUploadRing m_materialStaging{};
    DxrSrvBufferRing m_geometryLookupGpu{};
    DxrSrvBufferRing m_sceneVertexFloatsGpu{};
    DxrSrvBufferRing m_sceneIndicesGpu{};
    DxrSrvBufferRing m_materialGpu{};
    std::array<std::uint32_t, 2> m_geometryLookupSrvIndices{UINT32_MAX, UINT32_MAX};
    std::array<std::uint32_t, 2> m_sceneVertexFloatsSrvIndices{UINT32_MAX, UINT32_MAX};
    std::array<std::uint32_t, 2> m_sceneIndicesSrvIndices{UINT32_MAX, UINT32_MAX};
    std::array<std::uint32_t, 2> m_materialSrvIndices{UINT32_MAX, UINT32_MAX};
    std::uint32_t m_geometryLookupSrvIndex = UINT32_MAX;
    std::uint32_t m_sceneVertexFloatsSrvIndex = UINT32_MAX;
    std::uint32_t m_sceneIndicesSrvIndex = UINT32_MAX;
    std::uint32_t m_materialSrvIndex = UINT32_MAX;
    std::vector<InstanceDesc> m_instances;
};

struct PtDummyGbufferBindings
{
    std::uintptr_t depthSrvCpuHandle = 0;
    std::uintptr_t normalSrvCpuHandle = 0;
    std::uintptr_t material0SrvCpuHandle = 0;
    std::uintptr_t directSrvCpuHandle = 0;
    std::uintptr_t sunShadowSrvCpuHandle = 0;
    std::uintptr_t indirectSrvCpuHandle = 0;
    std::uintptr_t velocitySrvCpuHandle = 0;

    bool Create(std::string& outError);
    void Release();

private:
    std::vector<DxrGpuResource> m_ownedTextures;
    std::vector<std::uint32_t> m_ownedSrvIndices;
};

struct PtDispatchStack
{
    DxrPipeline pipeline;
    ShaderBindingTable shaderBindingTable;
    DxrDispatchContext dispatchContext;

    bool EnsureReady(std::string& outError);
    void Release();
};

struct PtFrameDispatchParams
{
    MinimalPtGlassScene* scene = nullptr;
    PtDummyGbufferBindings* gbuffer = nullptr;
    PtDispatchStack* stack = nullptr;
    const IBL* environmentIbl = nullptr;
    Camera* camera = nullptr;
    int width = 0;
    int height = 0;
    glm::mat4 prevViewProjection{1.0f};
    glm::vec3 prevCameraPos{0.0f};
    bool motionHistoryValid = false;
    std::uint32_t frameIndex = 0;
};

bool DispatchMinimalPathTracerFrame(const PtFrameDispatchParams& params, std::string& outError);

bool ReadbackPtGuideCenterPixel(
    ID3D12Resource* textureResource,
    std::uint32_t resourceState,
    int width,
    int height,
    DXGI_FORMAT format,
    float outRgba[4]);
