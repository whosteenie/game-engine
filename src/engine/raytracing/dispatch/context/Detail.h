#include "engine/raytracing/dispatch/DxrDispatchContext.h"
#include "engine/platform/diagnostics/FrameDiagnostics.h"

#include "engine/raytracing/core/DxrContext.h"
#include "engine/raytracing/core/DxrGpuResource.h"
#include "engine/raytracing/pipeline/DxrPipeline.h"
#include "engine/raytracing/core/DxrTrace.h"
#include "engine/raytracing/restir/RestirTypes.h"
#include "engine/raytracing/pipeline/ShaderBindingTable.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/HresultFormat.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>

#include <cstdlib>
#include <cstring>

#pragma once

namespace
{
    bool ArePathTracerGpuEventsEnabled()
    {
        static const bool enabled = [] {
            const char* const value = std::getenv("GAME_ENGINE_PT_GPU_EVENTS");
            return value == nullptr || std::strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    void BeginPathTracerGpuEvent(
        ID3D12GraphicsCommandList* const commandList,
        const wchar_t* const name,
        const UINT nameSize)
    {
        if (ArePathTracerGpuEventsEnabled())
        {
            commandList->BeginEvent(0, name, nameSize);
        }
    }

    void EndPathTracerGpuEvent(ID3D12GraphicsCommandList* const commandList)
    {
        if (ArePathTracerGpuEventsEnabled())
        {
            commandList->EndEvent();
        }
    }

    void DestroyOutputResource(
        ID3D12Resource*& resource,
        D3D12MA::Allocation*& allocation,
        std::uint32_t& srvIndex,
        std::uint32_t& uavIndex)
    {
        // CRASH-01/CRASH-03: defer release + descriptor recycling until the covering fence
        // completes; an in-flight or currently recording command list may still reference them.
        if (srvIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(srvIndex);
            srvIndex = UINT32_MAX;
        }

        if (uavIndex != UINT32_MAX)
        {
            GfxContext::Get().DeferredFreeOffscreenSrv(uavIndex);
            uavIndex = UINT32_MAX;
        }

        if (allocation != nullptr || resource != nullptr)
        {
            GfxContext::Get().DeferredReleaseResource(allocation, resource);
            allocation = nullptr;
        }

        resource = nullptr;
    }
}


// DXR-03: dispatch constants are allocated per dispatch from the per-frame transient upload
// arena (256-byte aligned). A single persistent CB was previously overwritten by each dispatch,
// so with Scene View + Game View both dispatching in one frame, the GPU executed *both*
// DispatchRays with the last-written constants (wrong camera for the first view).
class DxrDispatchRecorder
{
public:
    explicit DxrDispatchRecorder(ID3D12GraphicsCommandList4* commandList) : m_commandList(commandList) {}

    void BeginDraw(
        ID3D12StateObject* stateObject,
        ID3D12RootSignature* rootSignature,
        const std::uint64_t constantsGpuAddress) const
    {
        m_commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        ID3D12DescriptorHeap* descriptorHeaps[] = {srvHeap};
        m_commandList->SetDescriptorHeaps(1, descriptorHeaps);
        m_commandList->SetPipelineState1(stateObject);
        m_commandList->SetComputeRootSignature(rootSignature);
        m_commandList->SetComputeRootConstantBufferView(0, constantsGpuAddress);
    }

    void BindSrvTable(const UINT rootParameterIndex, const std::uint32_t srvHeapIndex) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
        tableHandle.ptr = reinterpret_cast<UINT64>(GfxContext::Get().GetSrvHeapGpuHandle(srvHeapIndex));
        m_commandList->SetComputeRootDescriptorTable(rootParameterIndex, tableHandle);
    }

    void BindSrvTables(
        const UINT rootParameterStart,
        const std::uint32_t* srvHeapIndices,
        const UINT count) const
    {
        for (UINT tableOffset = 0; tableOffset < count; ++tableOffset)
        {
            BindSrvTable(rootParameterStart + tableOffset, srvHeapIndices[tableOffset]);
        }
    }

    void DispatchRays(const ShaderBindingTable& shaderBindingTable, const int width, const int height) const
    {
        D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
        dispatchDesc.RayGenerationShaderRecord.StartAddress = shaderBindingTable.GetRaygenGpuAddress();
        dispatchDesc.RayGenerationShaderRecord.SizeInBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
        dispatchDesc.MissShaderTable.StartAddress = shaderBindingTable.GetMissGpuAddress();
        dispatchDesc.MissShaderTable.SizeInBytes = shaderBindingTable.GetMissRecordStride();
        dispatchDesc.MissShaderTable.StrideInBytes = shaderBindingTable.GetMissRecordStride();
        dispatchDesc.HitGroupTable.StartAddress = shaderBindingTable.GetHitGroupGpuAddress();
        dispatchDesc.HitGroupTable.SizeInBytes = shaderBindingTable.GetHitGroupRecordStride();
        dispatchDesc.HitGroupTable.StrideInBytes = shaderBindingTable.GetHitGroupRecordStride();
        dispatchDesc.Width = static_cast<UINT>(width);
        dispatchDesc.Height = static_cast<UINT>(height);
        dispatchDesc.Depth = 1;
        m_commandList->DispatchRays(&dispatchDesc);
    }

private:
    ID3D12GraphicsCommandList4* m_commandList = nullptr;
};

namespace
{
    template <typename TConstants>
    std::uint64_t AllocateDispatchConstants(const TConstants& constants)
    {
        const GfxContext::TransientUploadAllocation allocation =
            GfxContext::Get().AllocateTransientUpload(&constants, sizeof(TConstants));
        return allocation.gpuAddress;
    }
}

