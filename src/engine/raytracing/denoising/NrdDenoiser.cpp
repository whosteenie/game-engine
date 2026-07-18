#include "engine/raytracing/denoising/NrdDenoiser.h"

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
    // One denoiser per instance; identifier 0 addresses whichever flavor the instance was
    // created with (RELAX_SPECULAR for reflections, RELAX_DIFFUSE for the separate GI instance).
    constexpr nrd::Identifier kSpecularIdentifier = 0;
}

NrdDenoiser::~NrdDenoiser()
{
    Release();
}

void NrdDenoiser::SetSignal(const Signal signal)
{
    if (m_signal == signal)
    {
        return;
    }

    m_signal = signal;
    // A live instance is bound to the previous denoiser flavor; drop it so the next Denoise
    // rebuilds against the new one.
    if (m_instance != nullptr)
    {
        Release();
    }
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
    denoiserDesc.denoiser =
        m_signal == Signal::Diffuse ? nrd::Denoiser::RELAX_DIFFUSE : nrd::Denoiser::RELAX_SPECULAR;

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
        std::string(m_signal == Signal::Diffuse ? "NRD RELAX_DIFFUSE" : "NRD RELAX_SPECULAR")
            + " ready (" + std::to_string(resourceWidth) + "x" + std::to_string(resourceHeight)
            + ", " + std::to_string(m_pipelines.size()) + " pipelines)");
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
            texture.dxgiFormat = NrdCommon::NrdFormatToDxgi(textureDesc.format);
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
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_UNORM);
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
    case nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST:
        outResource = resources.radianceHitDist;
        outState = resources.radianceState;
        outDxgiFormat = static_cast<std::uint32_t>(DXGI_FORMAT_R16G16B16A16_FLOAT);
        return true;
    case nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST:
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

    SceneRenderTrace::Scope nrdScope(
        m_signal == Signal::Diffuse ? "dxr-nrd-relax-diffuse" : "dxr-nrd-relax-specular");

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

    // Engine temporalBlend (0..0.99) -> accumulated frames: frames = blend / (1 - blend).
    // 0.95 -> 19, 0.97 -> 32, 0.99 -> 99. RELAX supports up to 255; the old 63 cap put a
    // variance floor (sigma^2/N) high enough to leave residual shimmer on spiky HDR signals.
    const float blend = std::clamp(frame.temporalBlend, 0.0f, 0.99f);
    const auto accumulatedFrames = static_cast<std::uint32_t>(
        std::clamp(blend / std::max(1.0f - blend, 0.01f), 0.0f, 240.0f));

    nrd::RelaxSettings relaxSettings{};
    // Fast history clamps the main history each frame — if it's too short it re-injects
    // noise every frame ("shimmer that never settles"). Keep it >= 4 frames.
    const std::uint32_t fastAccumulatedFrames = std::max(accumulatedFrames / 5u, 4u);
    relaxSettings.specularMaxAccumulatedFrameNum = std::max(accumulatedFrames, 1u);
    relaxSettings.specularMaxFastAccumulatedFrameNum = fastAccumulatedFrames;
    relaxSettings.diffuseMaxAccumulatedFrameNum = std::max(accumulatedFrames, 1u);
    relaxSettings.diffuseMaxFastAccumulatedFrameNum = fastAccumulatedFrames;
    relaxSettings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
    relaxSettings.enableAntiFirefly = frame.antiFirefly;
    relaxSettings.atrousIterationNum =
        static_cast<std::uint32_t>(std::clamp(frame.atrousIterations, 2, 8));
    // Prepass blur exists to spread SPARSE samples (1 spp probabilistic pipelines). Our specular
    // trace provides dense, Karis-weighted, sub-pixel-jittered samples (up to 16 spp), so NRD's
    // 50 px default massively over-blurs — it was the main "smudged mirror" contributor. The
    // diffuse prepass default (30 px) is fine for the broader GI signal, so leave it alone.
    relaxSettings.specularPrepassBlurRadius = 12.0f;
    relaxSettings.diffusePrepassBlurRadius = 30.0f;

    if (m_signal == Signal::Specular)
    {
        // Roughness-aware sharpening. RELAX already scales its A-trous footprint by the per-pixel
        // specular lobe (driven by the roughness guide), but the default lobe fraction + angle
        // slack + edge-stopping relaxation leave a blur FLOOR that softens even near-mirror
        // surfaces — the "reflections are still blurry up close" complaint. Tightening these keeps
        // low-roughness reflections crisp; wide (rough) lobes are largely unaffected because a
        // fraction of a wide angle is still wide. Trade-off: slightly less edge smoothing on rough
        // reflections, covered by temporal accumulation + anti-firefly.
        relaxSettings.lobeAngleFraction = 0.15f;     // default 0.5 — normal-based rejection cone
        relaxSettings.specularLobeAngleSlack = 0.03f; // default 0.15 deg — additive blur floor
        relaxSettings.specularPrepassBlurRadius = 6.0f;
        // Stricter A-trous edge stopping (defaults relax these when reprojection confidence dips,
        // which is exactly when a mirror smears): keep reflected silhouettes sharp.
        relaxSettings.normalEdgeStoppingRelaxation = 0.1f;    // default 0.3
        relaxSettings.luminanceEdgeStoppingRelaxation = 0.2f; // default 0.5
        relaxSettings.roughnessEdgeStoppingRelaxation = 0.4f; // default 1.0
    }

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
    NrdCommon::TransitionTracked(commandList, resources.denoisedOutput, *resources.denoisedState, kAllShaderRead);
    NrdCommon::TransitionTracked(commandList, resources.radianceHitDist, *resources.radianceState, kAllShaderRead);

    nrdScope.Success();
    return true;
}
