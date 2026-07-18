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

#include "engine/raytracing/dispatch/context/Detail.h"

bool DxrDispatchContext::CreateReflectionTexture(
    const int width,
    const int height,
    const std::uint32_t dxgiFormat,
    ReflectionTexture& outTexture,
    std::string& outError)
{
    outError.clear();

    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (allocator == nullptr || device == nullptr)
    {
        outError = "GfxContext unavailable for DXR reflection texture";
        return false;
    }

    D3D12_RESOURCE_DESC resourceDesc{};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Width = static_cast<UINT64>(width);
    resourceDesc.Height = static_cast<UINT>(height);
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12MA::ALLOCATION_DESC allocationDesc{};
    allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

    const HRESULT createResult = allocator->CreateResource(
        &allocationDesc,
        &resourceDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        &outTexture.allocation,
        IID_PPV_ARGS(&outTexture.resource));
    if (FAILED(createResult))
    {
        outError = "failed to allocate DXR reflection texture ("
            + std::to_string(width) + "x" + std::to_string(height)
            + ", HRESULT=" + HresultFormat::Format(createResult) + ")";
        return false;
    }

    outTexture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);

    outTexture.srvIndex = GfxContext::Get().AllocateOffscreenSrv();
    outTexture.uavIndex = GfxContext::Get().AllocateOffscreenSrv();
    if (outTexture.srvIndex == UINT32_MAX || outTexture.uavIndex == UINT32_MAX)
    {
        outError = "failed to allocate DXR reflection texture descriptors";
        DestroyOutputResource(
            outTexture.resource, outTexture.allocation, outTexture.srvIndex, outTexture.uavIndex);
        return false;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle{};
    srvHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outTexture.srvIndex);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    GfxContext::Get().CreateShaderResourceView(outTexture.resource, &srvDesc, outTexture.srvIndex);
    outTexture.srvCpuHandle = srvHandle.ptr;

    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle{};
    uavHandle.ptr = GfxContext::Get().GetSrvCpuHandle(outTexture.uavIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
    GfxContext::Get().CreateUnorderedAccessView(outTexture.resource, nullptr, &uavDesc, outTexture.uavIndex);
    return true;
}

void DxrDispatchContext::RetireOrDestroyReflectionTexture(ReflectionTexture& texture)
{
    if (texture.resource == nullptr)
    {
        return;
    }

    if (GfxContext::Get().IsFrameRecording())
    {
        RetiredOutput retired{};
        retired.resource = texture.resource;
        retired.allocation = texture.allocation;
        retired.srvIndex = texture.srvIndex;
        retired.uavIndex = texture.uavIndex;
        m_retiredReflectionOutputs.push_back(retired);
    }
    else
    {
        DestroyOutputResource(texture.resource, texture.allocation, texture.srvIndex, texture.uavIndex);
    }

    texture = ReflectionTexture{};
}

bool DxrDispatchContext::EnsureReflectionOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid reflection output dimensions";
        return false;
    }

    ReleaseRetiredReflectionOutputs();

    const bool haveTextures = m_reflectionTextures[0].resource != nullptr;
    if (haveTextures && m_reflectionOutputWidth == width && m_reflectionOutputHeight == height)
    {
        return true;
    }

    // Keep a larger allocation alive (avoids churn when quality shrinks or viewports differ);
    // consumers must respect the dispatch/texture UV-scale contract (dxr-reflections.md).
    if (haveTextures && GfxContext::Get().IsFrameRecording()
        && m_reflectionOutputWidth >= width && m_reflectionOutputHeight >= height)
    {
        return true;
    }

    if (haveTextures && m_reflectionOutputWidth >= width && m_reflectionOutputHeight >= height
        && !GfxContext::Get().IsFrameRecording())
    {
        return true;
    }

    for (ReflectionTexture& texture : m_reflectionTextures)
    {
        RetireOrDestroyReflectionTexture(texture);
    }
    m_reflectionOutputSrvCpuHandle = 0;
    m_reflectionDenoisedSrvCpuHandle = 0;
    m_reflectionOutputWidth = 0;
    m_reflectionOutputHeight = 0;

    // [0] radiance+hitDist, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    // [2] must match NRD_NORMAL_ENCODING in CMakeLists (3 = RGBA16_UNORM: 8-bit normals
    // quantize on curved surfaces and defeat RELAX's normal edge-stopping).
    const std::uint32_t formats[kReflectionTextureCount] = {
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT)};

    for (int textureIndex = 0; textureIndex < kReflectionTextureCount; ++textureIndex)
    {
        if (!CreateReflectionTexture(
                width, height, formats[textureIndex], m_reflectionTextures[textureIndex], outError))
        {
            for (ReflectionTexture& texture : m_reflectionTextures)
            {
                RetireOrDestroyReflectionTexture(texture);
            }
            return false;
        }
    }

    m_reflectionOutputSrvCpuHandle = m_reflectionTextures[0].srvCpuHandle;
    m_reflectionDenoisedSrvCpuHandle = m_reflectionTextures[4].srvCpuHandle;
    m_reflectionOutputWidth = width;
    m_reflectionOutputHeight = height;
    return true;
}

DxrDispatchContext::ReflectionNrdResources DxrDispatchContext::GetReflectionNrdResources()
{
    ReflectionNrdResources resources{};
    resources.radianceHitDist = m_reflectionTextures[0].resource;
    resources.viewZ = m_reflectionTextures[1].resource;
    resources.normalRoughness = m_reflectionTextures[2].resource;
    resources.motion = m_reflectionTextures[3].resource;
    resources.denoisedOutput = m_reflectionTextures[4].resource;
    resources.radianceState = &m_reflectionTextures[0].state;
    resources.viewZState = &m_reflectionTextures[1].state;
    resources.normalRoughnessState = &m_reflectionTextures[2].state;
    resources.motionState = &m_reflectionTextures[3].state;
    resources.denoisedState = &m_reflectionTextures[4].state;
    resources.textureWidth = m_reflectionOutputWidth;
    resources.textureHeight = m_reflectionOutputHeight;
    resources.dispatchWidth = m_reflectionDispatchWidth;
    resources.dispatchHeight = m_reflectionDispatchHeight;
    return resources;
}

bool DxrDispatchContext::DispatchReflections(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    const ReflectionDispatchInputs& inputs,
    const int width,
    const int height,
    const DxrRootSignature::ReflectionDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR reflection dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[8] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR reflection SRV bindings unavailable";
            return false;
        }
    }

    if (inputs.geometryLookupSrvIndex == UINT32_MAX
        || inputs.sceneVertexFloatsSrvIndex == UINT32_MAX
        || inputs.sceneIndicesSrvIndex == UINT32_MAX
        || inputs.materialSrvIndex == UINT32_MAX)
    {
        outError = "DXR reflection geometry lookup SRVs unavailable";
        return false;
    }

    if (!EnsureReflectionOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(inputs.tlasResource, inputs.tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR reflection constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // Trace writes textures [0..3] (radiance, viewZ, normal+roughness, motion).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_reflectionTextures[textureIndex];
        if (texture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                texture.resource,
                static_cast<D3D12_RESOURCE_STATES>(texture.state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Root params 1..14 = SRV tables t0..t13 (see SerializeReflectionGlobalRootSignature).
    constexpr std::uint32_t kReflectionSrvCount = 14;
    const std::uint32_t giSrvIndex = inputs.giDenoisedSrvCpuHandle != 0
        ? DepthSrvIndexFromCpuHandle(inputs.giDenoisedSrvCpuHandle)
        : srvIndicesFromHandles[5]; // harmless fallback: RT1 indirect
    const std::uint32_t srvHeapIndices[kReflectionSrvCount] = {
        m_tlasSrvIndex,                     // t0 TLAS
        srvIndicesFromHandles[0],           // t1 depth
        srvIndicesFromHandles[1],           // t2 shading normal
        srvIndicesFromHandles[2],           // t3 material0
        inputs.geometryLookupSrvIndex,      // t4
        inputs.sceneVertexFloatsSrvIndex,   // t5
        inputs.sceneIndicesSrvIndex,        // t6
        srvIndicesFromHandles[3],           // t7 direct RT0
        srvIndicesFromHandles[4],           // t8 sun shadow RT3
        srvIndicesFromHandles[5],           // t9 indirect RT1
        srvIndicesFromHandles[6],           // t10 prefiltered env cube
        srvIndicesFromHandles[7],           // t11 velocity RT4
        inputs.materialSrvIndex,            // t12 per-object material table
        giSrvIndex};                        // t13 GI denoised (optional)

    if (inputs.giDenoisedSrvCpuHandle != 0 && giSrvIndex == UINT32_MAX)
    {
        outError = "DXR reflection GI SRV binding unavailable";
        return false;
    }

    recorder.BindSrvTables(1, srvHeapIndices, kReflectionSrvCount);

    // Root params 14..17 = UAV tables u0..u3 (base = 1 + kReflectionSrvCount).
    constexpr std::uint32_t kReflectionUavCount = 4;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(m_reflectionTextures[textureIndex].uavIndex));
        commandList->SetComputeRootDescriptorTable(1 + kReflectionSrvCount + textureIndex, uavTableHandle);
    }

    // Root param 18 = bindless SRV table (space1) over the whole heap. The base must be the
    // PHYSICAL heap start (descriptor 0) so g_BindlessTextures[absoluteHeapIndex] resolves
    // correctly. GetSrvHeapGpuHandle(0) can't be used: the SRV allocator reserves the first
    // descriptors (index offset), so index 0 is "invalid" and would return null.
    {
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        const D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetComputeRootDescriptorTable(
            1 + kReflectionSrvCount + kReflectionUavCount, bindlessHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    // Combined read state: pixel-shader debug blit + NRD compute reads (D5).
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_reflectionTextures[textureIndex];
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), texture.resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            texture.resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        texture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    m_reflectionDispatchWidth = width;
    m_reflectionDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchReflections ok");
    return true;
}

bool DxrDispatchContext::EnsureShadowOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid shadow output dimensions";
        return false;
    }

    ReleaseRetiredShadowOutputs();

    const bool haveTextures = m_shadowTextures[0].resource != nullptr;
    if (haveTextures && m_shadowOutputWidth == width && m_shadowOutputHeight == height)
    {
        return true;
    }

    // Keep a larger allocation alive (avoids churn when viewports differ); the shadow pass runs
    // at full render resolution so the dispatch/texture UV scale is normally 1.
    if (haveTextures && m_shadowOutputWidth >= width && m_shadowOutputHeight >= height)
    {
        return true;
    }

    for (ReflectionTexture& texture : m_shadowTextures)
    {
        RetireOrDestroyReflectionTexture(texture);
    }
    m_shadowPenumbraSrvCpuHandle = 0;
    m_shadowDenoisedSrvCpuHandle = 0;
    m_shadowOutputWidth = 0;
    m_shadowOutputHeight = 0;

    // [0] penumbra, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    // [2] must match NRD_NORMAL_ENCODING=3 (RGBA16_UNORM). [4] OUT_SHADOW_TRANSLUCENCY is R8+;
    // R16F keeps precision for the squared-shadow unpack and doubles as SIGMA's history buffer.
    const std::uint32_t formats[kShadowTextureCount] = {
        static_cast<std::uint32_t>(DXGI_FORMAT_R16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16_FLOAT)};

    for (int textureIndex = 0; textureIndex < kShadowTextureCount; ++textureIndex)
    {
        if (!CreateReflectionTexture(
                width, height, formats[textureIndex], m_shadowTextures[textureIndex], outError))
        {
            for (ReflectionTexture& texture : m_shadowTextures)
            {
                RetireOrDestroyReflectionTexture(texture);
            }
            return false;
        }
    }

    m_shadowPenumbraSrvCpuHandle = m_shadowTextures[0].srvCpuHandle;
    m_shadowDenoisedSrvCpuHandle = m_shadowTextures[4].srvCpuHandle;
    m_shadowOutputWidth = width;
    m_shadowOutputHeight = height;
    return true;
}

DxrDispatchContext::ShadowNrdResources DxrDispatchContext::GetShadowNrdResources()
{
    ShadowNrdResources resources{};
    resources.penumbra = m_shadowTextures[0].resource;
    resources.viewZ = m_shadowTextures[1].resource;
    resources.normalRoughness = m_shadowTextures[2].resource;
    resources.motion = m_shadowTextures[3].resource;
    resources.denoisedOutput = m_shadowTextures[4].resource;
    resources.penumbraState = &m_shadowTextures[0].state;
    resources.viewZState = &m_shadowTextures[1].state;
    resources.normalRoughnessState = &m_shadowTextures[2].state;
    resources.motionState = &m_shadowTextures[3].state;
    resources.denoisedState = &m_shadowTextures[4].state;
    resources.textureWidth = m_shadowOutputWidth;
    resources.textureHeight = m_shadowOutputHeight;
    resources.dispatchWidth = m_shadowDispatchWidth;
    resources.dispatchHeight = m_shadowDispatchHeight;
    return resources;
}

bool DxrDispatchContext::DispatchShadows(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    const ShadowDispatchInputs& inputs,
    const int width,
    const int height,
    const DxrRootSignature::ShadowDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR shadow dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[4] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR shadow SRV bindings unavailable";
            return false;
        }
    }

    if (!EnsureShadowOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(inputs.tlasResource, inputs.tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR shadow constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // Raygen writes textures [0..3] (penumbra, viewZ, normal+roughness, motion).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_shadowTextures[textureIndex];
        if (texture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                texture.resource,
                static_cast<D3D12_RESOURCE_STATES>(texture.state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Root params 1..5 = SRV tables t0..t4 (see SerializeShadowGlobalRootSignature).
    constexpr std::uint32_t kShadowSrvCount = 5;
    const std::uint32_t srvHeapIndices[kShadowSrvCount] = {
        m_tlasSrvIndex,           // t0 TLAS
        srvIndicesFromHandles[0], // t1 depth
        srvIndicesFromHandles[1], // t2 shading normal
        srvIndicesFromHandles[2], // t3 material0 (roughness)
        srvIndicesFromHandles[3]};// t4 velocity

    recorder.BindSrvTables(1, srvHeapIndices, kShadowSrvCount);

    // Root params 6..9 = UAV tables u0..u3 (base = 1 + kShadowSrvCount).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(m_shadowTextures[textureIndex].uavIndex));
        commandList->SetComputeRootDescriptorTable(1 + kShadowSrvCount + textureIndex, uavTableHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    // Combined read state: pixel-shader debug blit + NRD compute reads.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_shadowTextures[textureIndex];
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), texture.resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            texture.resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        texture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    m_shadowDispatchWidth = width;
    m_shadowDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchShadows ok");
    return true;
}

bool DxrDispatchContext::EnsureGiOutput(const int width, const int height, std::string& outError)
{
    outError.clear();
    if (width <= 0 || height <= 0)
    {
        outError = "invalid GI output dimensions";
        return false;
    }

    ReleaseRetiredGiOutputs();

    const bool haveTextures = m_giTextures[0].resource != nullptr;
    if (haveTextures && m_giOutputWidth == width && m_giOutputHeight == height)
    {
        return true;
    }

    // Keep a larger allocation alive (avoids churn when quality shrinks / viewports differ);
    // consumers respect the dispatch/texture UV-scale contract.
    if (haveTextures && m_giOutputWidth >= width && m_giOutputHeight >= height)
    {
        return true;
    }

    for (ReflectionTexture& texture : m_giTextures)
    {
        RetireOrDestroyReflectionTexture(texture);
    }
    m_giOutputSrvCpuHandle = 0;
    m_giDenoisedSrvCpuHandle = 0;
    m_giOutputWidth = 0;
    m_giOutputHeight = 0;

    // [0] radiance+hitDist, [1] viewZ, [2] normal+roughness, [3] motion, [4] denoised.
    // [2] must match NRD_NORMAL_ENCODING=3 (RGBA16_UNORM), same as the reflection set.
    const std::uint32_t formats[kGiTextureCount] = {
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT),
        static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT)};

    for (int textureIndex = 0; textureIndex < kGiTextureCount; ++textureIndex)
    {
        if (!CreateReflectionTexture(
                width, height, formats[textureIndex], m_giTextures[textureIndex], outError))
        {
            for (ReflectionTexture& texture : m_giTextures)
            {
                RetireOrDestroyReflectionTexture(texture);
            }
            return false;
        }
    }

    m_giOutputSrvCpuHandle = m_giTextures[0].srvCpuHandle;
    m_giDenoisedSrvCpuHandle = m_giTextures[4].srvCpuHandle;
    m_giOutputWidth = width;
    m_giOutputHeight = height;
    return true;
}

DxrDispatchContext::ReflectionNrdResources DxrDispatchContext::GetGiNrdResources()
{
    ReflectionNrdResources resources{};
    resources.radianceHitDist = m_giTextures[0].resource;
    resources.viewZ = m_giTextures[1].resource;
    resources.normalRoughness = m_giTextures[2].resource;
    resources.motion = m_giTextures[3].resource;
    resources.denoisedOutput = m_giTextures[4].resource;
    resources.radianceState = &m_giTextures[0].state;
    resources.viewZState = &m_giTextures[1].state;
    resources.normalRoughnessState = &m_giTextures[2].state;
    resources.motionState = &m_giTextures[3].state;
    resources.denoisedState = &m_giTextures[4].state;
    resources.textureWidth = m_giOutputWidth;
    resources.textureHeight = m_giOutputHeight;
    resources.dispatchWidth = m_giDispatchWidth;
    resources.dispatchHeight = m_giDispatchHeight;
    return resources;
}

bool DxrDispatchContext::DispatchGi(
    ID3D12GraphicsCommandList4* commandList,
    ID3D12StateObject* stateObject,
    ID3D12RootSignature* rootSignature,
    const ShaderBindingTable& shaderBindingTable,
    const ReflectionDispatchInputs& inputs,
    const int width,
    const int height,
    const DxrRootSignature::ReflectionDispatchConstants& constants,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || stateObject == nullptr || rootSignature == nullptr)
    {
        outError = "invalid DXR GI dispatch arguments";
        return false;
    }

    const std::uint32_t srvIndicesFromHandles[8] = {
        DepthSrvIndexFromCpuHandle(inputs.depthSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.normalSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.material0SrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.directSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.sunShadowSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.indirectSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.prefilterSrvCpuHandle),
        DepthSrvIndexFromCpuHandle(inputs.velocitySrvCpuHandle)};
    for (const std::uint32_t index : srvIndicesFromHandles)
    {
        if (index == UINT32_MAX)
        {
            outError = "DXR GI SRV bindings unavailable";
            return false;
        }
    }

    if (inputs.geometryLookupSrvIndex == UINT32_MAX
        || inputs.sceneVertexFloatsSrvIndex == UINT32_MAX
        || inputs.sceneIndicesSrvIndex == UINT32_MAX
        || inputs.materialSrvIndex == UINT32_MAX)
    {
        outError = "DXR GI geometry lookup SRVs unavailable";
        return false;
    }

    if (!EnsureGiOutput(width, height, outError))
    {
        return false;
    }

    if (!CreateTlasSrv(inputs.tlasResource, inputs.tlasGpuVirtualAddress, outError))
    {
        return false;
    }

    const std::uint64_t constantsGpuAddress = AllocateDispatchConstants(constants);
    if (constantsGpuAddress == 0)
    {
        outError = "failed to allocate transient DXR GI constants";
        return false;
    }

    commandList->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

    // Trace writes textures [0..3] (radiance, viewZ, normal+roughness, motion).
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_giTextures[textureIndex];
        if (texture.state != static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        {
            TransitionResource(
                static_cast<ID3D12GraphicsCommandList*>(commandList),
                texture.resource,
                static_cast<D3D12_RESOURCE_STATES>(texture.state),
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
    }

    const DxrDispatchRecorder recorder(commandList);
    recorder.BeginDraw(stateObject, rootSignature, constantsGpuAddress);

    // Root params 1..14 = SRV tables t0..t13 (reflection global root signature layout).
    constexpr std::uint32_t kGiSrvCount = 14;
    const std::uint32_t srvHeapIndices[kGiSrvCount] = {
        m_tlasSrvIndex,                     // t0 TLAS
        srvIndicesFromHandles[0],           // t1 depth
        srvIndicesFromHandles[1],           // t2 shading normal
        srvIndicesFromHandles[2],           // t3 material0
        inputs.geometryLookupSrvIndex,      // t4
        inputs.sceneVertexFloatsSrvIndex,   // t5
        inputs.sceneIndicesSrvIndex,        // t6
        srvIndicesFromHandles[3],           // t7 direct RT0 (unused by GI shader; bound for parity)
        srvIndicesFromHandles[4],           // t8 sun shadow RT3 (unused)
        srvIndicesFromHandles[5],           // t9 indirect RT1 (unused)
        srvIndicesFromHandles[6],           // t10 prefiltered env cube
        srvIndicesFromHandles[7],           // t11 velocity RT4
        inputs.materialSrvIndex,            // t12 per-object material table
        srvIndicesFromHandles[5]};          // t13 GI unused by GI shader; bind RT1 for parity

    recorder.BindSrvTables(1, srvHeapIndices, kGiSrvCount);

    // Root params 14..17 = UAV tables u0..u3 (GI texture set).
    constexpr std::uint32_t kGiUavCount = 4;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE uavTableHandle{};
        uavTableHandle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(m_giTextures[textureIndex].uavIndex));
        commandList->SetComputeRootDescriptorTable(1 + kGiSrvCount + textureIndex, uavTableHandle);
    }

    // Root param 18 = bindless SRV table (space1) over the whole heap (physical heap start).
    {
        auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
        const D3D12_GPU_DESCRIPTOR_HANDLE bindlessHandle =
            srvHeap->GetGPUDescriptorHandleForHeapStart();
        commandList->SetComputeRootDescriptorTable(
            1 + kGiSrvCount + kGiUavCount, bindlessHandle);
    }

    if (inputs.tlasResource != nullptr)
    {
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), inputs.tlasResource);
    }

    recorder.DispatchRays(shaderBindingTable, width, height);

    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    for (int textureIndex = 0; textureIndex < 4; ++textureIndex)
    {
        ReflectionTexture& texture = m_giTextures[textureIndex];
        RecordDxrUavBarrier(static_cast<ID3D12GraphicsCommandList*>(commandList), texture.resource);
        TransitionResource(
            static_cast<ID3D12GraphicsCommandList*>(commandList),
            texture.resource,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            kAllShaderRead);
        texture.state = static_cast<std::uint32_t>(kAllShaderRead);
    }

    m_giDispatchWidth = width;
    m_giDispatchHeight = height;

    DxrBreadcrumb("dispatch DispatchGi ok");
    return true;
}

std::uint32_t DxrDispatchContext::DepthSrvIndexFromCpuHandle(
    const std::uintptr_t depthSrvCpuHandle) const
{
    if (depthSrvCpuHandle == 0)
    {
        return UINT32_MAX;
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    if (srvHeap == nullptr)
    {
        return UINT32_MAX;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart = srvHeap->GetCPUDescriptorHandleForHeapStart();
    const std::uint32_t descriptorSize = GfxContext::Get().GetSrvDescriptorSize();
    return static_cast<std::uint32_t>((depthSrvCpuHandle - heapStart.ptr) / descriptorSize);
}

