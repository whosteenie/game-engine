#include "pt_test_harness.h"

#include "engine/lighting/IBL.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrInstanceTransform.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"

#include "primitives/Cube.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{
    struct DummyTexture
    {
        DxrGpuResource resource{};
        std::uint32_t srvIndex = UINT32_MAX;
        std::uint32_t resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        void Release()
        {
            if (srvIndex != UINT32_MAX)
            {
                GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
                srvIndex = UINT32_MAX;
            }

            resource.Release();
            resourceState = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
    };

    bool CreateDummyTexture2D(
        const int width,
        const int height,
        const DXGI_FORMAT format,
        DummyTexture& outTexture,
        std::string& outError)
    {
        outError.clear();
        outTexture.Release();

        D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
        auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
        if (allocator == nullptr || device == nullptr)
        {
            outError = "GfxContext unavailable for PT dummy texture";
            return false;
        }

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Width = static_cast<UINT>(std::max(width, 1));
        resourceDesc.Height = static_cast<UINT>(std::max(height, 1));
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = format;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        D3D12MA::ALLOCATION_DESC allocationDesc{};
        allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        const HRESULT createResult = allocator->CreateResource(
            &allocationDesc,
            &resourceDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            nullptr,
            &allocation,
            IID_PPV_ARGS(&resource));
        if (FAILED(createResult))
        {
            outError = "failed to create PT dummy texture";
            return false;
        }

        outTexture.resource.resource = resource;
        outTexture.resource.allocation = allocation;
        outTexture.resource.sizeInBytes = 0;
        outTexture.resourceState =
            static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        outTexture.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
        if (outTexture.srvIndex == UINT32_MAX)
        {
            outError = "failed to allocate SRV for PT dummy texture";
            outTexture.Release();
            return false;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle{};
        cpuHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outTexture.srvIndex);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = format;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(resource, &srvDesc, cpuHandle);
        return true;
    }

    std::uintptr_t CpuHandleFromSrvIndex(const std::uint32_t srvIndex)
    {
        return srvIndex == UINT32_MAX ? 0 : GfxContext::Get().GetSrvCpuHandle(srvIndex);
    }

    float HalfToFloat(const std::uint16_t half)
    {
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16;
        const std::uint32_t exponent = (half & 0x7C00u) >> 10;
        const std::uint32_t mantissa = half & 0x03FFu;
        std::uint32_t bits = 0;
        if (exponent == 0)
        {
            bits = mantissa == 0 ? sign : sign;
        }
        else if (exponent == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            bits = sign |
                static_cast<std::uint32_t>((exponent + (127 - 15)) << 23) |
                (mantissa << 13);
        }

        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    DxrMaterialEntry MakeGlassMaterial()
    {
        DxrMaterialEntry material{};
        material.albedo[0] = 1.0f;
        material.albedo[1] = 1.0f;
        material.albedo[2] = 1.0f;
        material.roughness = 0.02f;
        material.transmission = 1.0f;
        material.indexOfRefraction = 1.5f;
        material.thinWalled = 1.0f;
        return material;
    }

    DxrMaterialEntry MakeBackdropMaterial()
    {
        DxrMaterialEntry material{};
        material.albedo[0] = 0.85f;
        material.albedo[1] = 0.12f;
        material.albedo[2] = 0.08f;
        material.roughness = 0.35f;
        material.transmission = 0.0f;
        return material;
    }

    bool UploadGeometryBuffersForInstances(
        const std::vector<MinimalPtGlassScene::InstanceDesc>& instances,
        ID3D12GraphicsCommandList* commandList,
        DxrUploadRing& geometryLookupStaging,
        DxrUploadRing& sceneVertexFloatsStaging,
        DxrUploadRing& sceneIndicesStaging,
        DxrUploadRing& materialStaging,
        DxrSrvBufferRing& geometryLookupGpu,
        DxrSrvBufferRing& sceneVertexFloatsGpu,
        DxrSrvBufferRing& sceneIndicesGpu,
        DxrSrvBufferRing& materialGpu,
        std::array<std::uint32_t, 2>& geometryLookupSrvIndices,
        std::array<std::uint32_t, 2>& sceneVertexFloatsSrvIndices,
        std::array<std::uint32_t, 2>& sceneIndicesSrvIndices,
        std::array<std::uint32_t, 2>& materialSrvIndices,
        std::string& outError)
    {
        outError.clear();
        const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
        const std::size_t objectCount = instances.size();

        std::vector<DxrGeometryLookupEntry> lookupEntries(objectCount);
        std::vector<DxrMaterialEntry> materialEntries(objectCount);
        std::vector<float> vertexFloats;
        std::vector<std::uint32_t> indices;

        for (std::size_t objectIndex = 0; objectIndex < objectCount; ++objectIndex)
        {
            Mesh* mesh = instances[objectIndex].mesh;
            if (mesh == nullptr)
            {
                outError = "PT scene instance missing mesh";
                return false;
            }

            mesh->EnsureGpuResources();
            const std::uint32_t vertexStrideFloats = mesh->GetFloatsPerVertex();
            const std::vector<float>& meshVertexData = mesh->GetVertexData();
            const std::size_t vertexCount = mesh->GetPositions().size();
            const std::size_t vertexFloatCount = vertexCount * vertexStrideFloats;
            const std::vector<std::uint32_t>& meshIndices = mesh->GetIndices();

            DxrGeometryLookupEntry& entry = lookupEntries[objectIndex];
            entry.vertexFloatOffset = static_cast<std::uint32_t>(vertexFloats.size());
            entry.vertexStrideFloats = vertexStrideFloats;
            entry.indexUintOffset = static_cast<std::uint32_t>(indices.size());

            vertexFloats.resize(vertexFloats.size() + vertexFloatCount, 0.0f);
            if (meshVertexData.size() >= vertexFloatCount)
            {
                std::memcpy(
                    vertexFloats.data() + entry.vertexFloatOffset,
                    meshVertexData.data(),
                    vertexFloatCount * sizeof(float));
            }

            indices.insert(indices.end(), meshIndices.begin(), meshIndices.end());
            materialEntries[objectIndex] = instances[objectIndex].material;
        }

        const std::uint64_t lookupBytes = lookupEntries.size() * sizeof(DxrGeometryLookupEntry);
        const std::uint64_t materialBytes = materialEntries.size() * sizeof(DxrMaterialEntry);
        const std::uint64_t vertexBytes = vertexFloats.size() * sizeof(float);
        const std::uint64_t indexBytes = indices.size() * sizeof(std::uint32_t);

        if (!geometryLookupStaging.EnsureCapacity(lookupBytes)
            || !materialStaging.EnsureCapacity(materialBytes)
            || !sceneVertexFloatsStaging.EnsureCapacity(vertexBytes)
            || !sceneIndicesStaging.EnsureCapacity(indexBytes)
            || !geometryLookupGpu.EnsureCapacity(lookupBytes)
            || !materialGpu.EnsureCapacity(materialBytes)
            || !sceneVertexFloatsGpu.EnsureCapacity(vertexBytes)
            || !sceneIndicesGpu.EnsureCapacity(indexBytes))
        {
            outError = "failed to allocate PT geometry upload buffers";
            return false;
        }

        auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
        if (device == nullptr)
        {
            outError = "GfxContext unavailable for PT geometry SRV creation";
            return false;
        }

        for (std::uint32_t ringIndex = 0; ringIndex < GfxContext::FrameCount; ++ringIndex)
        {
            if (geometryLookupSrvIndices[ringIndex] == UINT32_MAX)
            {
                geometryLookupSrvIndices[ringIndex] = GfxContext::Get().AllocateOffscreenSrv();
                sceneVertexFloatsSrvIndices[ringIndex] = GfxContext::Get().AllocateOffscreenSrv();
                sceneIndicesSrvIndices[ringIndex] = GfxContext::Get().AllocateOffscreenSrv();
                materialSrvIndices[ringIndex] = GfxContext::Get().AllocateOffscreenSrv();
            }

            if (geometryLookupSrvIndices[ringIndex] == UINT32_MAX
                || sceneVertexFloatsSrvIndices[ringIndex] == UINT32_MAX
                || sceneIndicesSrvIndices[ringIndex] == UINT32_MAX
                || materialSrvIndices[ringIndex] == UINT32_MAX)
            {
                outError = "failed to allocate PT geometry SRV descriptors";
                return false;
            }

            D3D12_CPU_DESCRIPTOR_HANDLE lookupHandle{};
            lookupHandle.ptr = GfxContext::Get().GetSrvCpuHandle(geometryLookupSrvIndices[ringIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC lookupSrvDesc{};
            lookupSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            lookupSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
            lookupSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            lookupSrvDesc.Buffer.FirstElement = 0;
            lookupSrvDesc.Buffer.NumElements = static_cast<UINT>(lookupEntries.size());
            lookupSrvDesc.Buffer.StructureByteStride = sizeof(DxrGeometryLookupEntry);
            device->CreateShaderResourceView(
                geometryLookupGpu.Slot(ringIndex).resource,
                &lookupSrvDesc,
                lookupHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE materialHandle{};
            materialHandle.ptr = GfxContext::Get().GetSrvCpuHandle(materialSrvIndices[ringIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC materialSrvDesc{};
            materialSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            materialSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
            materialSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            materialSrvDesc.Buffer.FirstElement = 0;
            materialSrvDesc.Buffer.NumElements = static_cast<UINT>(materialEntries.size());
            materialSrvDesc.Buffer.StructureByteStride = sizeof(DxrMaterialEntry);
            device->CreateShaderResourceView(
                materialGpu.Slot(ringIndex).resource,
                &materialSrvDesc,
                materialHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE vertexHandle{};
            vertexHandle.ptr = GfxContext::Get().GetSrvCpuHandle(sceneVertexFloatsSrvIndices[ringIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC vertexSrvDesc{};
            vertexSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            vertexSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            vertexSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            vertexSrvDesc.Buffer.FirstElement = 0;
            vertexSrvDesc.Buffer.NumElements = static_cast<UINT>(vertexFloats.size());
            device->CreateShaderResourceView(
                sceneVertexFloatsGpu.Slot(ringIndex).resource,
                &vertexSrvDesc,
                vertexHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE indexHandle{};
            indexHandle.ptr = GfxContext::Get().GetSrvCpuHandle(sceneIndicesSrvIndices[ringIndex]);
            D3D12_SHADER_RESOURCE_VIEW_DESC indexSrvDesc{};
            indexSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            indexSrvDesc.Format = DXGI_FORMAT_R32_UINT;
            indexSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            indexSrvDesc.Buffer.FirstElement = 0;
            indexSrvDesc.Buffer.NumElements = static_cast<UINT>(indices.size());
            device->CreateShaderResourceView(
                sceneIndicesGpu.Slot(ringIndex).resource,
                &indexSrvDesc,
                indexHandle);
        }

        DxrGpuResource& geometryLookupUpload = geometryLookupStaging.Slot(frameIndex);
        DxrGpuResource& materialUpload = materialStaging.Slot(frameIndex);
        DxrGpuResource& vertexUpload = sceneVertexFloatsStaging.Slot(frameIndex);
        DxrGpuResource& indexUpload = sceneIndicesStaging.Slot(frameIndex);

        void* mapped = nullptr;
        if (SUCCEEDED(geometryLookupUpload.resource->Map(0, nullptr, &mapped)))
        {
            std::memcpy(mapped, lookupEntries.data(), lookupBytes);
            geometryLookupUpload.resource->Unmap(0, nullptr);
        }

        if (SUCCEEDED(materialUpload.resource->Map(0, nullptr, &mapped)))
        {
            std::memcpy(mapped, materialEntries.data(), materialBytes);
            materialUpload.resource->Unmap(0, nullptr);
        }

        if (SUCCEEDED(vertexUpload.resource->Map(0, nullptr, &mapped)))
        {
            std::memcpy(mapped, vertexFloats.data(), vertexBytes);
            vertexUpload.resource->Unmap(0, nullptr);
        }

        if (SUCCEEDED(indexUpload.resource->Map(0, nullptr, &mapped)))
        {
            std::memcpy(mapped, indices.data(), indexBytes);
            indexUpload.resource->Unmap(0, nullptr);
        }

        CopyDxrUploadToSrvBuffer(
            commandList,
            geometryLookupUpload,
            geometryLookupGpu.Slot(frameIndex),
            lookupBytes);
        CopyDxrUploadToSrvBuffer(
            commandList,
            materialUpload,
            materialGpu.Slot(frameIndex),
            materialBytes);
        CopyDxrUploadToSrvBuffer(
            commandList,
            vertexUpload,
            sceneVertexFloatsGpu.Slot(frameIndex),
            vertexBytes);
        CopyDxrUploadToSrvBuffer(
            commandList,
            indexUpload,
            sceneIndicesGpu.Slot(frameIndex),
            indexBytes);

        return true;
    }
}

glm::mat4 MinimalPtGlassScene::MakeVerticalPlaneTransform(const float zPosition)
{
    return glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, zPosition))
        * glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
}

bool MinimalPtGlassScene::Build(
    ID3D12GraphicsCommandList4* commandList,
    DxrGpuResource& scratch,
    const bool includeGlassPane,
    std::string& outError)
{
    outError.clear();
    Release();

    if (commandList == nullptr)
    {
        outError = "invalid command list for PT glass scene";
        return false;
    }

    m_backdropMesh = CreateCubeMesh();
    if (m_backdropMesh == nullptr)
    {
        outError = "failed to create PT glass scene meshes";
        return false;
    }

    m_backdropMesh->EnsureGpuResources();
    if (!m_backdropBlas.Build(commandList, m_backdropMesh.get(), scratch, outError))
    {
        return false;
    }

    m_instances = {
        InstanceDesc{
            m_backdropMesh.get(),
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f))
                * glm::scale(glm::mat4(1.0f), glm::vec3(6.0f, 6.0f, 0.2f)),
            MakeBackdropMaterial()},
    };
    if (includeGlassPane)
    {
        m_instances.push_back(InstanceDesc{
            m_backdropMesh.get(),
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.5f))
                * glm::scale(glm::mat4(1.0f), glm::vec3(6.0f, 6.0f, 0.01f)),
            MakeGlassMaterial()});
    }

    if (!UploadGeometryBuffersForInstances(m_instances, commandList,
            m_geometryLookupStaging,
            m_sceneVertexFloatsStaging,
            m_sceneIndicesStaging,
            m_materialStaging,
            m_geometryLookupGpu,
            m_sceneVertexFloatsGpu,
            m_sceneIndicesGpu,
            m_materialGpu,
            m_geometryLookupSrvIndices,
            m_sceneVertexFloatsSrvIndices,
            m_sceneIndicesSrvIndices,
            m_materialSrvIndices,
            outError))
    {
        return false;
    }

    const std::uint32_t frameIndex = GfxContext::Get().GetFrameIndex();
    m_geometryLookupSrvIndex = m_geometryLookupSrvIndices[frameIndex];
    m_sceneVertexFloatsSrvIndex = m_sceneVertexFloatsSrvIndices[frameIndex];
    m_sceneIndicesSrvIndex = m_sceneIndicesSrvIndices[frameIndex];
    m_materialSrvIndex = m_materialSrvIndices[frameIndex];

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> tlasInstances(m_instances.size());
    for (std::size_t instanceIndex = 0; instanceIndex < m_instances.size(); ++instanceIndex)
    {
        WriteD3D12InstanceTransform(
            m_instances[instanceIndex].transform,
            reinterpret_cast<float*>(tlasInstances[instanceIndex].Transform));
        tlasInstances[instanceIndex].InstanceID = static_cast<UINT>(instanceIndex);
        tlasInstances[instanceIndex].InstanceMask = 0xFF;
        tlasInstances[instanceIndex].AccelerationStructure =
            m_backdropBlas.GetGpuVirtualAddress();
    }

    if (!m_tlas.Build(commandList, tlasInstances, scratch, outError))
    {
        return false;
    }

    return true;
}

void MinimalPtGlassScene::Release()
{
    m_tlas.Release();
    m_backdropBlas.Release();
    m_backdropMesh.reset();
    m_instances.clear();

    m_geometryLookupStaging.Release();
    m_sceneVertexFloatsStaging.Release();
    m_sceneIndicesStaging.Release();
    m_materialStaging.Release();
    m_geometryLookupGpu.Release();
    m_sceneVertexFloatsGpu.Release();
    m_sceneIndicesGpu.Release();
    m_materialGpu.Release();

    for (std::uint32_t srvIndex : m_geometryLookupSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }
    for (std::uint32_t srvIndex : m_sceneVertexFloatsSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }
    for (std::uint32_t srvIndex : m_sceneIndicesSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }
    for (std::uint32_t srvIndex : m_materialSrvIndices)
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
    m_geometryLookupSrvIndex = UINT32_MAX;
    m_sceneVertexFloatsSrvIndex = UINT32_MAX;
    m_sceneIndicesSrvIndex = UINT32_MAX;
    m_materialSrvIndex = UINT32_MAX;
}

bool PtDummyGbufferBindings::Create(std::string& outError)
{
    outError.clear();
    Release();

    const struct Desc
    {
        DXGI_FORMAT format;
        std::uintptr_t* outHandle;
    } descs[] = {
        {DXGI_FORMAT_R32_FLOAT, &depthSrvCpuHandle},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &normalSrvCpuHandle},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &material0SrvCpuHandle},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &directSrvCpuHandle},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &sunShadowSrvCpuHandle},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &indirectSrvCpuHandle},
        {DXGI_FORMAT_R16G16B16A16_FLOAT, &velocitySrvCpuHandle},
    };

    m_ownedTextures.reserve(std::size(descs));
    m_ownedSrvIndices.reserve(std::size(descs));
    for (const Desc& desc : descs)
    {
        DummyTexture texture{};
        if (!CreateDummyTexture2D(1, 1, desc.format, texture, outError))
        {
            Release();
            return false;
        }

        *desc.outHandle = CpuHandleFromSrvIndex(texture.srvIndex);
        m_ownedTextures.push_back(texture.resource);
        m_ownedSrvIndices.push_back(texture.srvIndex);
        texture.resource = {};
        texture.srvIndex = UINT32_MAX;
    }

    return true;
}

void PtDummyGbufferBindings::Release()
{
    depthSrvCpuHandle = 0;
    normalSrvCpuHandle = 0;
    material0SrvCpuHandle = 0;
    directSrvCpuHandle = 0;
    sunShadowSrvCpuHandle = 0;
    indirectSrvCpuHandle = 0;
    velocitySrvCpuHandle = 0;

    for (std::uint32_t srvIndex : m_ownedSrvIndices)
    {
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
        }
    }

    for (DxrGpuResource& texture : m_ownedTextures)
    {
        texture.Release();
    }

    m_ownedTextures.clear();
    m_ownedSrvIndices.clear();
}

bool PtDispatchStack::EnsureReady(std::string& outError)
{
    outError.clear();
    if (!pipeline.CreatePathTracerPipeline(outError))
    {
        return false;
    }

    return shaderBindingTable.BuildPathTracerTable(pipeline.GetProperties(), outError);
}

void PtDispatchStack::Release()
{
    shaderBindingTable.Release();
    pipeline.Release();
    dispatchContext.Release();
}

bool DispatchMinimalPathTracerFrame(const PtFrameDispatchParams& params, std::string& outError)
{
    outError.clear();
    if (params.scene == nullptr || params.gbuffer == nullptr || params.stack == nullptr
        || params.environmentIbl == nullptr || params.camera == nullptr || params.width <= 0
        || params.height <= 0 || !params.scene->IsReady())
    {
        outError = "invalid PT frame dispatch parameters";
        return false;
    }

    if (!params.environmentIbl->IsReady())
    {
        outError = "PT environment IBL is not ready";
        return false;
    }

    auto* commandList4 = DxrContext::Get().QueryCommandList4(GfxContext::Get().GetCommandList());
    if (commandList4 == nullptr)
    {
        outError = "CommandList4 unavailable for PT dispatch";
        return false;
    }

    if (!params.stack->EnsureReady(outError))
    {
        return false;
    }

    const glm::mat4 viewMatrix = params.camera->GetViewMatrix();
    const glm::mat4 projectionMatrix = params.camera->GetProjectionMatrix();
    const glm::mat4 unjitteredProjection = params.camera->GetUnjitteredProjectionMatrix();
    const glm::mat4 viewProj = projectionMatrix * viewMatrix;
    const glm::mat4 unjitteredViewProj = unjitteredProjection * viewMatrix;
    const glm::mat4 invViewProj = glm::inverse(viewProj);
    const glm::mat4 prevViewProj = params.motionHistoryValid
        ? params.prevViewProjection
        : unjitteredViewProj;
    // Glass virtual-motion replay matrix: prev VIEW + CURRENT jittered projection (mirrors
    // DxrPathTracerDispatch — jitter cancels out of the virtual MV; static glass MV stays 0).
    const glm::mat4 prevInvViewProj = glm::inverse(
        params.motionHistoryValid
            ? projectionMatrix * params.prevView
            : viewProj);
    const glm::vec3 cameraPos = params.camera->GetPosition();
    const glm::vec3 prevCameraPos =
        params.motionHistoryValid ? params.prevCameraPos : cameraPos;

    DxrRootSignature::ReflectionDispatchConstants constants{};
    constants.outputWidth = static_cast<std::uint32_t>(params.width);
    constants.outputHeight = static_cast<std::uint32_t>(params.height);
    constants.gbufferWidth = static_cast<std::uint32_t>(params.width);
    constants.gbufferHeight = static_cast<std::uint32_t>(params.height);
    std::memcpy(constants.invViewProj, glm::value_ptr(invViewProj), sizeof(constants.invViewProj));
    std::memcpy(constants.viewProj, glm::value_ptr(viewProj), sizeof(constants.viewProj));
    std::memcpy(constants.worldToView, glm::value_ptr(viewMatrix), sizeof(constants.worldToView));
    std::memcpy(
        constants.unjitteredViewProj,
        glm::value_ptr(unjitteredViewProj),
        sizeof(constants.unjitteredViewProj));
    std::memcpy(constants.prevViewProj, glm::value_ptr(prevViewProj), sizeof(constants.prevViewProj));
    std::memcpy(
        constants.prevInvViewProj,
        glm::value_ptr(prevInvViewProj),
        sizeof(constants.prevInvViewProj));
    constants.cameraPos[0] = cameraPos.x;
    constants.cameraPos[1] = cameraPos.y;
    constants.cameraPos[2] = cameraPos.z;
    constants.prevCameraPos[0] = prevCameraPos.x;
    constants.prevCameraPos[1] = prevCameraPos.y;
    constants.prevCameraPos[2] = prevCameraPos.z;
    constants.maxTraceDistance = 100.0f;
    constants.environmentIntensity = params.environmentIbl->GetEnvironmentIntensity();
    constants.maxReflectionLod = params.environmentIbl->GetMaxReflectionLod();
    constants.frameIndex = params.frameIndex;
    constants.samplesPerPixel = 1;
    constants.roughnessCutoff = 1.0f;
    constants.paddingUnjitteredViewProj[3] = params.motionHistoryValid ? 1.0f : 0.0f;
    constants.paddingUnjitteredViewProj[2] =
        2.0f * std::tan(glm::radians(params.camera->GetFov()) * 0.5f)
        / static_cast<float>(std::max(params.height, 1));

    DxrDispatchContext::ReflectionDispatchInputs dispatchInputs{};
    dispatchInputs.tlasResource = params.scene->GetTlasResource();
    dispatchInputs.tlasGpuVirtualAddress = params.scene->GetTlasGpuVirtualAddress();
    dispatchInputs.depthSrvCpuHandle = params.gbuffer->depthSrvCpuHandle;
    dispatchInputs.normalSrvCpuHandle = params.gbuffer->normalSrvCpuHandle;
    dispatchInputs.material0SrvCpuHandle = params.gbuffer->material0SrvCpuHandle;
    dispatchInputs.directSrvCpuHandle = params.gbuffer->directSrvCpuHandle;
    dispatchInputs.sunShadowSrvCpuHandle = params.gbuffer->sunShadowSrvCpuHandle;
    dispatchInputs.indirectSrvCpuHandle = params.gbuffer->indirectSrvCpuHandle;
    dispatchInputs.prefilterSrvCpuHandle = params.environmentIbl->GetPrefilterMapSrvCpuHandle();
    dispatchInputs.velocitySrvCpuHandle = params.gbuffer->velocitySrvCpuHandle;
    dispatchInputs.geometryLookupSrvIndex = params.scene->GetGeometryLookupSrvIndex();
    dispatchInputs.sceneVertexFloatsSrvIndex = params.scene->GetSceneVertexFloatsSrvIndex();
    dispatchInputs.sceneIndicesSrvIndex = params.scene->GetSceneIndicesSrvIndex();
    dispatchInputs.materialSrvIndex = params.scene->GetMaterialSrvIndex();

    return params.stack->dispatchContext.DispatchPathTracer(
        commandList4,
        params.stack->pipeline.GetStateObject(),
        params.stack->pipeline.GetGlobalRootSignature(),
        params.stack->shaderBindingTable,
        dispatchInputs,
        params.width,
        params.height,
        constants,
        outError);
}

bool ReadbackPtGuideCenterPixel(
    ID3D12Resource* textureResource,
    const std::uint32_t resourceState,
    const int width,
    const int height,
    const DXGI_FORMAT format,
    float outRgba[4])
{
    if (textureResource == nullptr || width <= 0 || height <= 0)
    {
        return false;
    }

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc = textureResource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    footprint.Offset = 0;
    footprint.Footprint.Width = 1;
    footprint.Footprint.Height = 1;
    footprint.Footprint.Depth = 1;
    footprint.Footprint.Format = format;
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);
    footprint.Footprint.RowPitch = static_cast<UINT>(rowSize);

    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = totalBytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12MA::ALLOCATION_DESC readbackAllocDesc{};
    readbackAllocDesc.HeapType = D3D12_HEAP_TYPE_READBACK;

    ID3D12Resource* readbackResource = nullptr;
    D3D12MA::Allocation* readbackAllocation = nullptr;
    if (FAILED(allocator->CreateResource(
            &readbackAllocDesc,
            &readbackDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            &readbackAllocation,
            IID_PPV_ARGS(&readbackResource))))
    {
        return false;
    }

    const int centerX = width / 2;
    const int centerY = height / 2;
    const D3D12_RESOURCE_STATES stateBefore = static_cast<D3D12_RESOURCE_STATES>(resourceState);

    GfxContext::Get().ExecuteImmediate([&](void* commandListPtr) {
        auto* commandList = static_cast<ID3D12GraphicsCommandList*>(commandListPtr);

        TransitionResource(
            commandList,
            textureResource,
            stateBefore,
            D3D12_RESOURCE_STATE_COPY_SOURCE);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = textureResource;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readbackResource;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;

        D3D12_BOX sourceBox{};
        sourceBox.left = static_cast<UINT>(centerX);
        sourceBox.top = static_cast<UINT>(centerY);
        sourceBox.front = 0;
        sourceBox.right = static_cast<UINT>(centerX + 1);
        sourceBox.bottom = static_cast<UINT>(centerY + 1);
        sourceBox.back = 1;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, &sourceBox);

        TransitionResource(
            commandList,
            textureResource,
            D3D12_RESOURCE_STATE_COPY_SOURCE,
            stateBefore);
    });

    D3D12_RANGE readRange{0, static_cast<SIZE_T>(totalBytes)};
    void* mapped = nullptr;
    if (FAILED(readbackResource->Map(0, &readRange, &mapped)))
    {
        readbackAllocation->Release();
        readbackResource->Release();
        return false;
    }

    if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(mapped);
        for (int channel = 0; channel < 4; ++channel)
        {
            outRgba[channel] = static_cast<float>(bytes[channel]) / 255.0f;
        }
    }
    else if (format == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        const auto* halfChannels = static_cast<const std::uint16_t*>(mapped);
        for (int channel = 0; channel < 4; ++channel)
        {
            outRgba[channel] = HalfToFloat(halfChannels[channel]);
        }
    }
    else
    {
        readbackResource->Unmap(0, nullptr);
        readbackAllocation->Release();
        readbackResource->Release();
        return false;
    }

    readbackResource->Unmap(0, nullptr);
    readbackAllocation->Release();
    readbackResource->Release();
    return true;
}
