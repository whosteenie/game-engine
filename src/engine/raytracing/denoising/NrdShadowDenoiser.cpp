#include "engine/raytracing/denoising/NrdShadowDenoiser.h"

#include "engine/platform/diagnostics/EngineLog.h"
#include "engine/platform/diagnostics/SceneRenderTrace.h"
#include "engine/raytracing/core/DxrGpuResource.h"
#include "engine/raytracing/core/DxrTrace.h"
#include "engine/raytracing/denoising/NrdCommon.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/HresultFormat.h"

#include <NRD.h>

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <glm/gtc/type_ptr.hpp>

namespace
{
    constexpr nrd::Identifier kShadowIdentifier = 0;
}

NrdShadowDenoiser::~NrdShadowDenoiser()
{
    Release();
}

void NrdShadowDenoiser::Release()
{
    ReleasePoolTextures();

    for (ID3D12PipelineState* pipeline : m_pipelines)
    {
        if (pipeline != nullptr)
        {
            pipeline->Release();
        }
    }
    m_pipelines.clear();

    if (m_rootSignature != nullptr)
    {
        m_rootSignature->Release();
        m_rootSignature = nullptr;
    }

    if (m_instance != nullptr)
    {
        nrd::DestroyInstance(*m_instance);
        m_instance = nullptr;
    }

    m_engineResourceFormats.clear();
    m_resourceWidth = 0;
    m_resourceHeight = 0;
    m_creationFailed = false;
}

void NrdShadowDenoiser::ReleasePoolTextures()
{
    auto releasePool = [](std::vector<PoolTexture>& pool) {
        for (PoolTexture& texture : pool)
        {
            if (texture.allocation != nullptr || texture.resource != nullptr)
            {
                GfxContext::Get().DeferredReleaseResource(texture.allocation, texture.resource);
            }
            texture = PoolTexture{};
        }
        pool.clear();
    };

    releasePool(m_permanentPool);
    releasePool(m_transientPool);
}

bool NrdShadowDenoiser::EnsureInstance(
    const int resourceWidth, const int resourceHeight, std::string& outError)
{
    outError.clear();

    if (m_creationFailed)
    {
        outError = m_lastError;
        return false;
    }

    if (m_instance != nullptr && m_resourceWidth == resourceWidth && m_resourceHeight == resourceHeight)
    {
        return true;
    }

    if (m_instance != nullptr)
    {
        Release();
    }

    DxrBreadcrumb("nrd-shadow EnsureInstance begin");

    nrd::DenoiserDesc denoiserDesc{};
    denoiserDesc.identifier = kShadowIdentifier;
    denoiserDesc.denoiser = nrd::Denoiser::SIGMA_SHADOW;

    nrd::InstanceCreationDesc creationDesc{};
    creationDesc.denoisers = &denoiserDesc;
    creationDesc.denoisersNum = 1;

    if (nrd::CreateInstance(creationDesc, m_instance) != nrd::Result::SUCCESS || m_instance == nullptr)
    {
        m_instance = nullptr;
        m_creationFailed = true;
        m_lastError = "nrd::CreateInstance failed (SIGMA_SHADOW)";
        outError = m_lastError;
        return false;
    }

    if (!CreatePipelines(outError) || !CreatePoolTextures(resourceWidth, resourceHeight, outError))
    {
        m_lastError = outError;
        Release();
        m_creationFailed = true; // Release() clears the flag
        return false;
    }

    m_resourceWidth = resourceWidth;
    m_resourceHeight = resourceHeight;
    EngineLog::Info(
        "nrd",
        "NRD SIGMA_SHADOW ready (" + std::to_string(resourceWidth) + "x"
            + std::to_string(resourceHeight) + ", " + std::to_string(m_pipelines.size())
            + " pipelines)");
    DxrBreadcrumb("nrd-shadow EnsureInstance ok");
    return true;
}

bool NrdShadowDenoiser::CreatePipelines(std::string& outError)
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        outError = "GfxContext unavailable for NRD shadow pipelines";
        return false;
    }

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_instance);
    if (instanceDesc == nullptr)
    {
        outError = "nrd::GetInstanceDesc failed";
        return false;
    }

    m_constantBufferMaxDataSize = instanceDesc->constantBufferMaxDataSize;
    m_cbRegister = instanceDesc->constantBufferRegisterIndex;
    m_cbSpace = instanceDesc->constantBufferAndSamplersSpaceIndex;
    m_samplersBaseRegister = instanceDesc->samplersBaseRegisterIndex;
    m_resourcesBaseRegister = instanceDesc->resourcesBaseRegisterIndex;
    m_resourcesSpace = instanceDesc->resourcesSpaceIndex;
    m_srvTableSize = instanceDesc->descriptorPoolDesc.perSetTexturesMaxNum;
    m_uavTableSize = instanceDesc->descriptorPoolDesc.perSetStorageTexturesMaxNum;

    D3D12_DESCRIPTOR_RANGE1 ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = std::max(m_srvTableSize, 1u);
    ranges[0].BaseShaderRegister = m_resourcesBaseRegister;
    ranges[0].RegisterSpace = m_resourcesSpace;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = std::max(m_uavTableSize, 1u);
    ranges[1].BaseShaderRegister = m_resourcesBaseRegister;
    ranges[1].RegisterSpace = m_resourcesSpace;
    ranges[1].OffsetInDescriptorsFromTableStart = std::max(m_srvTableSize, 1u);
    ranges[1].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

    D3D12_ROOT_PARAMETER1 rootParams[2]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = m_cbRegister;
    rootParams[0].Descriptor.RegisterSpace = m_cbSpace;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1].DescriptorTable.NumDescriptorRanges = 2;
    rootParams[1].DescriptorTable.pDescriptorRanges = ranges;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    std::vector<D3D12_STATIC_SAMPLER_DESC> samplers(instanceDesc->samplersNum);
    for (std::uint32_t samplerIndex = 0; samplerIndex < instanceDesc->samplersNum; ++samplerIndex)
    {
        D3D12_STATIC_SAMPLER_DESC& sampler = samplers[samplerIndex];
        sampler = {};
        sampler.Filter = instanceDesc->samplers[samplerIndex] == nrd::Sampler::LINEAR_CLAMP
            ? D3D12_FILTER_MIN_MAG_MIP_LINEAR
            : D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = m_samplersBaseRegister + samplerIndex;
        sampler.RegisterSpace = m_cbSpace;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.NumParameters = 2;
    rootDesc.pParameters = rootParams;
    rootDesc.NumStaticSamplers = static_cast<UINT>(samplers.size());
    rootDesc.pStaticSamplers = samplers.empty() ? nullptr : samplers.data();

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1 = rootDesc;

    Microsoft::WRL::ComPtr<ID3DBlob> signatureBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> signatureError;
    HRESULT hr = D3D12SerializeVersionedRootSignature(&versionedDesc, &signatureBlob, &signatureError);
    if (FAILED(hr))
    {
        outError = "NRD shadow root signature serialization failed (HRESULT="
            + HresultFormat::Format(hr) + ")";
        return false;
    }

    hr = device->CreateRootSignature(
        0,
        signatureBlob->GetBufferPointer(),
        signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr))
    {
        outError = "NRD shadow root signature creation failed (HRESULT="
            + HresultFormat::Format(hr) + ")";
        return false;
    }

    m_pipelines.resize(instanceDesc->pipelinesNum, nullptr);
    for (std::uint32_t pipelineIndex = 0; pipelineIndex < instanceDesc->pipelinesNum; ++pipelineIndex)
    {
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[pipelineIndex];
        if (pipelineDesc.computeShaderDXIL.bytecode == nullptr || pipelineDesc.computeShaderDXIL.size == 0)
        {
            outError = "NRD shadow pipeline has no embedded DXIL (check NRD_EMBEDS_DXIL_SHADERS)";
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSignature;
        psoDesc.CS.pShaderBytecode = pipelineDesc.computeShaderDXIL.bytecode;
        psoDesc.CS.BytecodeLength = static_cast<SIZE_T>(pipelineDesc.computeShaderDXIL.size);

        hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelines[pipelineIndex]));
        if (FAILED(hr))
        {
            outError = std::string("NRD shadow compute PSO creation failed for '")
                + pipelineDesc.shaderIdentifier + "' (HRESULT=" + HresultFormat::Format(hr) + ")";
            return false;
        }
    }

    return true;
}

bool NrdShadowDenoiser::CreatePoolTextures(
    const int resourceWidth, const int resourceHeight, std::string& outError)
{
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        outError = "memory allocator unavailable for NRD shadow pools";
        return false;
    }

    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_instance);

    auto createPool = [&](const nrd::TextureDesc* descs,
                          const std::uint32_t count,
                          std::vector<PoolTexture>& pool) -> bool {
        pool.resize(count);
        for (std::uint32_t textureIndex = 0; textureIndex < count; ++textureIndex)
        {
            const nrd::TextureDesc& textureDesc = descs[textureIndex];
            const int downsample = std::max<int>(1, textureDesc.downsampleFactor);
            PoolTexture& texture = pool[textureIndex];
            texture.width = std::max(1, (resourceWidth + downsample - 1) / downsample);
            texture.height = std::max(1, (resourceHeight + downsample - 1) / downsample);
            texture.dxgiFormat = NrdCommon::NrdFormatToDxgi(textureDesc.format);
            if (texture.dxgiFormat == DXGI_FORMAT_UNKNOWN)
            {
                outError = "unsupported NRD shadow pool texture format";
                return false;
            }

            D3D12_RESOURCE_DESC resourceDesc{};
            resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            resourceDesc.Width = static_cast<UINT64>(texture.width);
            resourceDesc.Height = static_cast<UINT>(texture.height);
            resourceDesc.DepthOrArraySize = 1;
            resourceDesc.MipLevels = 1;
            resourceDesc.Format = static_cast<DXGI_FORMAT>(texture.dxgiFormat);
            resourceDesc.SampleDesc.Count = 1;
            resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            D3D12MA::ALLOCATION_DESC allocationDesc{};
            allocationDesc.HeapType = D3D12_HEAP_TYPE_DEFAULT;

            const HRESULT hr = allocator->CreateResource(
                &allocationDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                &texture.allocation,
                IID_PPV_ARGS(&texture.resource));
            if (FAILED(hr))
            {
                outError = "NRD shadow pool texture allocation failed (HRESULT="
                    + HresultFormat::Format(hr) + ")";
                return false;
            }

            texture.state = static_cast<std::uint32_t>(D3D12_RESOURCE_STATE_COMMON);
        }

        return true;
    };

    if (!createPool(instanceDesc->permanentPool, instanceDesc->permanentPoolSize, m_permanentPool))
    {
        return false;
    }

    return createPool(instanceDesc->transientPool, instanceDesc->transientPoolSize, m_transientPool);
}

bool NrdShadowDenoiser::ResolveResource(
    const void* nrdResourceDescPtr,
    const DxrDispatchContext::ShadowNrdResources& resources,
    ID3D12Resource*& outResource,
    std::uint32_t*& outState,
    std::uint32_t& outDxgiFormat) const
{
    const auto& resourceDesc = *static_cast<const nrd::ResourceDesc*>(nrdResourceDescPtr);
    outResource = nullptr;
    outState = nullptr;
    outDxgiFormat = 0;

    switch (resourceDesc.type)
    {
    case nrd::ResourceType::TRANSIENT_POOL:
    {
        auto& texture = const_cast<PoolTexture&>(m_transientPool[resourceDesc.indexInPool]);
        outResource = texture.resource;
        outState = &texture.state;
        outDxgiFormat = texture.dxgiFormat;
        return true;
    }
    case nrd::ResourceType::PERMANENT_POOL:
    {
        auto& texture = const_cast<PoolTexture&>(m_permanentPool[resourceDesc.indexInPool]);
        outResource = texture.resource;
        outState = &texture.state;
        outDxgiFormat = texture.dxgiFormat;
        return true;
    }
    case nrd::ResourceType::IN_MV:
        outResource = resources.motion;
        outState = resources.motionState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16_FLOAT);
        return true;
    case nrd::ResourceType::IN_NORMAL_ROUGHNESS:
        outResource = resources.normalRoughness;
        outState = resources.normalRoughnessState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM);
        return true;
    case nrd::ResourceType::IN_VIEWZ:
        outResource = resources.viewZ;
        outState = resources.viewZState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT);
        return true;
    case nrd::ResourceType::IN_PENUMBRA:
        outResource = resources.penumbra;
        outState = resources.penumbraState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16_FLOAT);
        return true;
    case nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY:
        outResource = resources.denoisedOutput;
        outState = resources.denoisedState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16_FLOAT);
        return true;
    default:
        return false;
    }
}

bool NrdShadowDenoiser::Denoise(
    ID3D12GraphicsCommandList* commandList,
    const DxrDispatchContext::ShadowNrdResources& resources,
    const FrameParameters& frame,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || resources.penumbra == nullptr
        || resources.denoisedOutput == nullptr || resources.textureWidth <= 0
        || resources.dispatchWidth <= 0)
    {
        outError = "invalid NRD shadow denoise arguments";
        return false;
    }

    if (!EnsureInstance(resources.textureWidth, resources.textureHeight, outError))
    {
        return false;
    }

    SceneRenderTrace::Scope nrdScope("dxr-nrd-sigma-shadow");

    // Common settings. glm is column-major with column vectors — NRD's exact expectation, so a
    // straight memcpy is correct.
    nrd::CommonSettings commonSettings{};
    std::memcpy(commonSettings.viewToClipMatrix, glm::value_ptr(frame.viewToClip), sizeof(float) * 16);
    std::memcpy(commonSettings.viewToClipMatrixPrev, glm::value_ptr(frame.viewToClipPrev), sizeof(float) * 16);
    std::memcpy(commonSettings.worldToViewMatrix, glm::value_ptr(frame.worldToView), sizeof(float) * 16);
    std::memcpy(commonSettings.worldToViewMatrixPrev, glm::value_ptr(frame.worldToViewPrev), sizeof(float) * 16);
    commonSettings.motionVectorScale[0] = 1.0f; // IN_MV already stores uvPrev - uvCurr
    commonSettings.motionVectorScale[1] = 1.0f;
    commonSettings.motionVectorScale[2] = 0.0f;
    commonSettings.cameraJitter[0] = frame.cameraJitterUv.x;
    commonSettings.cameraJitter[1] = frame.cameraJitterUv.y;
    commonSettings.cameraJitterPrev[0] = frame.cameraJitterUvPrev.x;
    commonSettings.cameraJitterPrev[1] = frame.cameraJitterUvPrev.y;
    commonSettings.resourceSize[0] = static_cast<std::uint16_t>(resources.textureWidth);
    commonSettings.resourceSize[1] = static_cast<std::uint16_t>(resources.textureHeight);
    commonSettings.resourceSizePrev[0] = static_cast<std::uint16_t>(resources.textureWidth);
    commonSettings.resourceSizePrev[1] = static_cast<std::uint16_t>(resources.textureHeight);
    commonSettings.rectSize[0] = static_cast<std::uint16_t>(resources.dispatchWidth);
    commonSettings.rectSize[1] = static_cast<std::uint16_t>(resources.dispatchHeight);
    commonSettings.rectSizePrev[0] = static_cast<std::uint16_t>(resources.dispatchWidth);
    commonSettings.rectSizePrev[1] = static_cast<std::uint16_t>(resources.dispatchHeight);
    commonSettings.denoisingRange = frame.denoisingRange;
    commonSettings.frameIndex = frame.frameIndex;
    commonSettings.accumulationMode =
        frame.resetHistory ? nrd::AccumulationMode::RESTART : nrd::AccumulationMode::CONTINUE;
    commonSettings.isMotionVectorInWorldSpace = false;

    if (nrd::SetCommonSettings(*m_instance, commonSettings) != nrd::Result::SUCCESS)
    {
        outError = "nrd::SetCommonSettings failed (shadow)";
        return false;
    }

    nrd::SigmaSettings sigmaSettings{};
    sigmaSettings.lightDirection[0] = frame.lightDirection.x;
    sigmaSettings.lightDirection[1] = frame.lightDirection.y;
    sigmaSettings.lightDirection[2] = frame.lightDirection.z;
    sigmaSettings.maxStabilizedFrameNum = nrd::SIGMA_MAX_HISTORY_FRAME_NUM;

    if (nrd::SetDenoiserSettings(*m_instance, kShadowIdentifier, &sigmaSettings) != nrd::Result::SUCCESS)
    {
        outError = "nrd::SetDenoiserSettings failed (shadow)";
        return false;
    }

    const nrd::DispatchDesc* dispatchDescs = nullptr;
    std::uint32_t dispatchDescsNum = 0;
    const nrd::Identifier identifiers[] = {kShadowIdentifier};
    if (nrd::GetComputeDispatches(*m_instance, identifiers, 1, dispatchDescs, dispatchDescsNum)
            != nrd::Result::SUCCESS
        || dispatchDescs == nullptr)
    {
        outError = "nrd::GetComputeDispatches failed (shadow)";
        return false;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_instance);

    for (std::uint32_t dispatchIndex = 0; dispatchIndex < dispatchDescsNum; ++dispatchIndex)
    {
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIndex];
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[dispatchDesc.pipelineIndex];

        const std::uint32_t tableSize = std::max(m_srvTableSize, 1u) + std::max(m_uavTableSize, 1u);
        const GfxContext::TransientDescriptorRange table =
            GfxContext::Get().AllocateTransientSrvRange(tableSize);
        if (table.baseIndex == UINT32_MAX)
        {
            outError = "transient descriptor region exhausted during NRD shadow dispatches";
            return false;
        }

        std::uint32_t resourceCursor = 0;
        for (std::uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex)
        {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[rangeIndex];
            const bool isStorage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            const std::uint32_t tableOffset = isStorage ? std::max(m_srvTableSize, 1u) : 0u;

            for (std::uint32_t descriptorIndex = 0; descriptorIndex < range.descriptorsNum;
                 ++descriptorIndex, ++resourceCursor)
            {
                ID3D12Resource* resource = nullptr;
                std::uint32_t* state = nullptr;
                std::uint32_t dxgiFormat = 0;
                if (!ResolveResource(
                        &dispatchDesc.resources[resourceCursor], resources, resource, state, dxgiFormat))
                {
                    outError = "NRD shadow dispatch references an unsupported resource type";
                    return false;
                }

                NrdCommon::TransitionTracked(
                    commandList,
                    resource,
                    *state,
                    isStorage ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                              : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

                D3D12_CPU_DESCRIPTOR_HANDLE handle{};
                handle.ptr = table.cpuHandle
                    + static_cast<SIZE_T>(tableOffset + descriptorIndex) * table.descriptorSize;

                if (isStorage)
                {
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
                    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    uavDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
                    device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
                }
                else
                {
                    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
                    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    srvDesc.Format = static_cast<DXGI_FORMAT>(dxgiFormat);
                    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    srvDesc.Texture2D.MipLevels = 1;
                    device->CreateShaderResourceView(resource, &srvDesc, handle);
                }
            }
        }

        commandList->SetComputeRootSignature(m_rootSignature);
        commandList->SetPipelineState(m_pipelines[dispatchDesc.pipelineIndex]);

        if (dispatchDesc.constantBufferDataSize > 0 && dispatchDesc.constantBufferData != nullptr)
        {
            const GfxContext::TransientUploadAllocation constants =
                GfxContext::Get().AllocateTransientUpload(
                    dispatchDesc.constantBufferData, dispatchDesc.constantBufferDataSize);
            if (constants.gpuAddress == 0)
            {
                outError = "transient upload arena exhausted during NRD shadow dispatches";
                return false;
            }
            commandList->SetComputeRootConstantBufferView(0, constants.gpuAddress);
        }

        D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
        tableHandle.ptr = table.gpuHandle;
        commandList->SetComputeRootDescriptorTable(1, tableHandle);

        commandList->Dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);

        resourceCursor = 0;
        for (std::uint32_t rangeIndex = 0; rangeIndex < pipelineDesc.resourceRangesNum; ++rangeIndex)
        {
            const nrd::ResourceRangeDesc& range = pipelineDesc.resourceRanges[rangeIndex];
            const bool isStorage = range.descriptorType == nrd::DescriptorType::STORAGE_TEXTURE;
            for (std::uint32_t descriptorIndex = 0; descriptorIndex < range.descriptorsNum;
                 ++descriptorIndex, ++resourceCursor)
            {
                if (!isStorage)
                {
                    continue;
                }

                ID3D12Resource* resource = nullptr;
                std::uint32_t* state = nullptr;
                std::uint32_t dxgiFormat = 0;
                if (ResolveResource(
                        &dispatchDesc.resources[resourceCursor], resources, resource, state, dxgiFormat))
                {
                    RecordDxrUavBarrier(commandList, resource);
                }
            }
        }
    }

    // Leave the denoised shadow output AND the penumbra buffer readable for the pixel-shader
    // composite / debug blit.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    NrdCommon::TransitionTracked(commandList, resources.denoisedOutput, *resources.denoisedState, kAllShaderRead);
    NrdCommon::TransitionTracked(commandList, resources.penumbra, *resources.penumbraState, kAllShaderRead);

    nrdScope.Success();
    return true;
}
