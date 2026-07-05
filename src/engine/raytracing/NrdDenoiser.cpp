#include "engine/raytracing/NrdDenoiser.h"

#include "engine/platform/EngineLog.h"
#include "engine/platform/SceneRenderTrace.h"
#include "engine/raytracing/DxrGpuResource.h"
#include "engine/raytracing/DxrTrace.h"
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
    constexpr nrd::Identifier kSpecularIdentifier = 0;

    DXGI_FORMAT NrdFormatToDxgi(const nrd::Format format)
    {
        switch (format)
        {
        case nrd::Format::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
        case nrd::Format::R8_SNORM: return DXGI_FORMAT_R8_SNORM;
        case nrd::Format::R8_UINT: return DXGI_FORMAT_R8_UINT;
        case nrd::Format::R8_SINT: return DXGI_FORMAT_R8_SINT;
        case nrd::Format::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
        case nrd::Format::RG8_SNORM: return DXGI_FORMAT_R8G8_SNORM;
        case nrd::Format::RG8_UINT: return DXGI_FORMAT_R8G8_UINT;
        case nrd::Format::RG8_SINT: return DXGI_FORMAT_R8G8_SINT;
        case nrd::Format::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
        case nrd::Format::RGBA8_SNORM: return DXGI_FORMAT_R8G8B8A8_SNORM;
        case nrd::Format::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
        case nrd::Format::RGBA8_SINT: return DXGI_FORMAT_R8G8B8A8_SINT;
        case nrd::Format::RGBA8_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case nrd::Format::R16_UNORM: return DXGI_FORMAT_R16_UNORM;
        case nrd::Format::R16_SNORM: return DXGI_FORMAT_R16_SNORM;
        case nrd::Format::R16_UINT: return DXGI_FORMAT_R16_UINT;
        case nrd::Format::R16_SINT: return DXGI_FORMAT_R16_SINT;
        case nrd::Format::R16_SFLOAT: return DXGI_FORMAT_R16_FLOAT;
        case nrd::Format::RG16_UNORM: return DXGI_FORMAT_R16G16_UNORM;
        case nrd::Format::RG16_SNORM: return DXGI_FORMAT_R16G16_SNORM;
        case nrd::Format::RG16_UINT: return DXGI_FORMAT_R16G16_UINT;
        case nrd::Format::RG16_SINT: return DXGI_FORMAT_R16G16_SINT;
        case nrd::Format::RG16_SFLOAT: return DXGI_FORMAT_R16G16_FLOAT;
        case nrd::Format::RGBA16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
        case nrd::Format::RGBA16_SNORM: return DXGI_FORMAT_R16G16B16A16_SNORM;
        case nrd::Format::RGBA16_UINT: return DXGI_FORMAT_R16G16B16A16_UINT;
        case nrd::Format::RGBA16_SINT: return DXGI_FORMAT_R16G16B16A16_SINT;
        case nrd::Format::RGBA16_SFLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case nrd::Format::R32_UINT: return DXGI_FORMAT_R32_UINT;
        case nrd::Format::R32_SINT: return DXGI_FORMAT_R32_SINT;
        case nrd::Format::R32_SFLOAT: return DXGI_FORMAT_R32_FLOAT;
        case nrd::Format::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
        case nrd::Format::RG32_SINT: return DXGI_FORMAT_R32G32_SINT;
        case nrd::Format::RG32_SFLOAT: return DXGI_FORMAT_R32G32_FLOAT;
        case nrd::Format::RGB32_UINT: return DXGI_FORMAT_R32G32B32_UINT;
        case nrd::Format::RGB32_SINT: return DXGI_FORMAT_R32G32B32_SINT;
        case nrd::Format::RGB32_SFLOAT: return DXGI_FORMAT_R32G32B32_FLOAT;
        case nrd::Format::RGBA32_UINT: return DXGI_FORMAT_R32G32B32A32_UINT;
        case nrd::Format::RGBA32_SINT: return DXGI_FORMAT_R32G32B32A32_SINT;
        case nrd::Format::RGBA32_SFLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case nrd::Format::R10_G10_B10_A2_UNORM: return DXGI_FORMAT_R10G10B10A2_UNORM;
        case nrd::Format::R10_G10_B10_A2_UINT: return DXGI_FORMAT_R10G10B10A2_UINT;
        case nrd::Format::R11_G11_B10_UFLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
        case nrd::Format::R9_G9_B9_E5_UFLOAT: return DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        default: return DXGI_FORMAT_UNKNOWN;
        }
    }

    void TransitionTracked(
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* resource,
        std::uint32_t& state,
        const D3D12_RESOURCE_STATES newState)
    {
        const auto current = static_cast<D3D12_RESOURCE_STATES>(state);
        if (resource == nullptr || current == newState)
        {
            return;
        }

        TransitionResource(commandList, resource, current, newState);
        state = static_cast<std::uint32_t>(newState);
    }
}

NrdDenoiser::~NrdDenoiser()
{
    Release();
}

void NrdDenoiser::Release()
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

void NrdDenoiser::ReleasePoolTextures()
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

bool NrdDenoiser::EnsureInstance(const int resourceWidth, const int resourceHeight, std::string& outError)
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
        // Resolution changed: pools are sized from the resource size, recreate everything.
        // (Instance itself is resolution-independent in NRD 4.x, but a full rebuild is the
        // simplest correct behavior and resizes are rare.)
        Release();
    }

    DxrBreadcrumb("nrd EnsureInstance begin");

    nrd::DenoiserDesc denoiserDesc{};
    denoiserDesc.identifier = kSpecularIdentifier;
    denoiserDesc.denoiser = nrd::Denoiser::RELAX_SPECULAR;

    nrd::InstanceCreationDesc creationDesc{};
    creationDesc.denoisers = &denoiserDesc;
    creationDesc.denoisersNum = 1;

    if (nrd::CreateInstance(creationDesc, m_instance) != nrd::Result::SUCCESS || m_instance == nullptr)
    {
        m_instance = nullptr;
        m_creationFailed = true;
        m_lastError = "nrd::CreateInstance failed";
        outError = m_lastError;
        return false;
    }

    if (!CreatePipelines(outError) || !CreatePoolTextures(resourceWidth, resourceHeight, outError))
    {
        m_lastError = outError;
        m_creationFailed = true;
        Release();
        m_creationFailed = true; // Release() clears the flag
        return false;
    }

    m_resourceWidth = resourceWidth;
    m_resourceHeight = resourceHeight;
    EngineLog::Info(
        "nrd",
        "NRD RELAX_SPECULAR ready (" + std::to_string(resourceWidth) + "x"
            + std::to_string(resourceHeight) + ", " + std::to_string(m_pipelines.size())
            + " pipelines)");
    DxrBreadcrumb("nrd EnsureInstance ok");
    return true;
}

bool NrdDenoiser::CreatePipelines(std::string& outError)
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        outError = "GfxContext unavailable for NRD pipelines";
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

    // Shared root signature (NRD-recommended layout): root CBV + one table holding the
    // per-set maximum of SRVs followed by UAVs at a fixed offset + static samplers.
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
        outError = "NRD root signature serialization failed (HRESULT=" + HresultFormat::Format(hr) + ")";
        return false;
    }

    hr = device->CreateRootSignature(
        0,
        signatureBlob->GetBufferPointer(),
        signatureBlob->GetBufferSize(),
        IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr))
    {
        outError = "NRD root signature creation failed (HRESULT=" + HresultFormat::Format(hr) + ")";
        return false;
    }

    m_pipelines.resize(instanceDesc->pipelinesNum, nullptr);
    for (std::uint32_t pipelineIndex = 0; pipelineIndex < instanceDesc->pipelinesNum; ++pipelineIndex)
    {
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[pipelineIndex];
        if (pipelineDesc.computeShaderDXIL.bytecode == nullptr || pipelineDesc.computeShaderDXIL.size == 0)
        {
            outError = "NRD pipeline has no embedded DXIL (check NRD_EMBEDS_DXIL_SHADERS)";
            return false;
        }

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = m_rootSignature;
        psoDesc.CS.pShaderBytecode = pipelineDesc.computeShaderDXIL.bytecode;
        psoDesc.CS.BytecodeLength = static_cast<SIZE_T>(pipelineDesc.computeShaderDXIL.size);

        hr = device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelines[pipelineIndex]));
        if (FAILED(hr))
        {
            outError = std::string("NRD compute PSO creation failed for '")
                + pipelineDesc.shaderIdentifier + "' (HRESULT=" + HresultFormat::Format(hr) + ")";
            return false;
        }
    }

    return true;
}

bool NrdDenoiser::CreatePoolTextures(const int resourceWidth, const int resourceHeight, std::string& outError)
{
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();
    if (allocator == nullptr)
    {
        outError = "memory allocator unavailable for NRD pools";
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
            texture.dxgiFormat = static_cast<std::uint32_t>(NrdFormatToDxgi(textureDesc.format));
            if (texture.dxgiFormat == DXGI_FORMAT_UNKNOWN)
            {
                outError = "unsupported NRD pool texture format";
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
                outError = "NRD pool texture allocation failed (HRESULT=" + HresultFormat::Format(hr) + ")";
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

bool NrdDenoiser::ResolveResource(
    const void* nrdResourceDescPtr,
    const DxrDispatchContext::ReflectionNrdResources& resources,
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
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM);
        return true;
    case nrd::ResourceType::IN_VIEWZ:
        outResource = resources.viewZ;
        outState = resources.viewZState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R32_FLOAT);
        return true;
    case nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST:
        outResource = resources.radianceHitDist;
        outState = resources.radianceState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
        return true;
    case nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST:
        outResource = resources.denoisedOutput;
        outState = resources.denoisedState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
        return true;
    default:
        return false;
    }
}

bool NrdDenoiser::Denoise(
    ID3D12GraphicsCommandList* commandList,
    const DxrDispatchContext::ReflectionNrdResources& resources,
    const FrameParameters& frame,
    std::string& outError)
{
    outError.clear();
    if (commandList == nullptr || resources.radianceHitDist == nullptr
        || resources.denoisedOutput == nullptr || resources.textureWidth <= 0
        || resources.dispatchWidth <= 0)
    {
        outError = "invalid NRD denoise arguments";
        return false;
    }

    if (!EnsureInstance(resources.textureWidth, resources.textureHeight, outError))
    {
        return false;
    }

    SceneRenderTrace::Scope nrdScope("dxr-nrd-relax-specular");

    // Common settings. Matrices: glm is column-major with column vectors — NRD's exact
    // expectation, so a straight memcpy is correct.
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
        outError = "nrd::SetCommonSettings failed";
        return false;
    }

    // Engine temporalBlend (0..0.98) -> accumulated frames: frames = blend / (1 - blend).
    const float blend = std::clamp(frame.temporalBlend, 0.0f, 0.98f);
    const auto accumulatedFrames = static_cast<std::uint32_t>(
        std::clamp(blend / std::max(1.0f - blend, 0.02f), 0.0f, 63.0f));

    nrd::RelaxSettings relaxSettings{};
    relaxSettings.specularMaxAccumulatedFrameNum = std::max(accumulatedFrames, 1u);
    relaxSettings.specularMaxFastAccumulatedFrameNum = std::max(accumulatedFrames / 5u, 1u);
    relaxSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
    relaxSettings.enableAntiFirefly = true;
    // Explicit NRD defaults — brace-init would zero these and disable the prepass spatial reuse.
    relaxSettings.specularPrepassBlurRadius = 50.0f;
    relaxSettings.diffusePrepassBlurRadius = 30.0f;

    if (nrd::SetDenoiserSettings(*m_instance, kSpecularIdentifier, &relaxSettings) != nrd::Result::SUCCESS)
    {
        outError = "nrd::SetDenoiserSettings failed";
        return false;
    }

    const nrd::DispatchDesc* dispatchDescs = nullptr;
    std::uint32_t dispatchDescsNum = 0;
    const nrd::Identifier identifiers[] = {kSpecularIdentifier};
    if (nrd::GetComputeDispatches(*m_instance, identifiers, 1, dispatchDescs, dispatchDescsNum)
            != nrd::Result::SUCCESS
        || dispatchDescs == nullptr)
    {
        outError = "nrd::GetComputeDispatches failed";
        return false;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    const nrd::InstanceDesc* instanceDesc = nrd::GetInstanceDesc(*m_instance);

    for (std::uint32_t dispatchIndex = 0; dispatchIndex < dispatchDescsNum; ++dispatchIndex)
    {
        const nrd::DispatchDesc& dispatchDesc = dispatchDescs[dispatchIndex];
        const nrd::PipelineDesc& pipelineDesc = instanceDesc->pipelines[dispatchDesc.pipelineIndex];

        // One transient table per dispatch: SRVs at [0..srvMax), UAVs at [srvMax..).
        const std::uint32_t tableSize = std::max(m_srvTableSize, 1u) + std::max(m_uavTableSize, 1u);
        const GfxContext::TransientDescriptorRange table =
            GfxContext::Get().AllocateTransientSrvRange(tableSize);
        if (table.baseIndex == UINT32_MAX)
        {
            outError = "transient descriptor region exhausted during NRD dispatches";
            return false;
        }

        // Walk resource ranges: range order matches the concatenated "resources" array.
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
                    outError = "NRD dispatch references an unsupported resource type";
                    return false;
                }

                TransitionTracked(
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
                outError = "transient upload arena exhausted during NRD dispatches";
                return false;
            }
            commandList->SetComputeRootConstantBufferView(0, constants.gpuAddress);
        }

        D3D12_GPU_DESCRIPTOR_HANDLE tableHandle{};
        tableHandle.ptr = table.gpuHandle;
        commandList->SetComputeRootDescriptorTable(1, tableHandle);

        commandList->Dispatch(dispatchDesc.gridWidth, dispatchDesc.gridHeight, 1);

        // Order dependent UAV writes between NRD passes.
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

    // Leave the denoised output AND the raw radiance buffer readable for the pixel-shader
    // debug blit / D6 resolve. NRD read the raw buffer as a compute (NON_PIXEL) SRV, so it
    // must be returned to the combined read state or the resolve pass sees a stale state.
    constexpr D3D12_RESOURCE_STATES kAllShaderRead =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionTracked(commandList, resources.denoisedOutput, *resources.denoisedState, kAllShaderRead);
    TransitionTracked(commandList, resources.radianceHitDist, *resources.radianceState, kAllShaderRead);

    nrdScope.Success();
    return true;
}
