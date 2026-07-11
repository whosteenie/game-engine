#include "engine/rendering/MeshShaderGBufferRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/lighting/CascadedShadowMap.h"
#include "engine/lighting/DirectionalShadowSettings.h"
#include "engine/lighting/IBL.h"
#include "engine/lighting/IrradianceSh.h"
#include "engine/lighting/Light.h"
#include "engine/lighting/SceneLighting.h"
#include "engine/lighting/ShadowMapMath.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/FrustumCull.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <glm/gtc/type_ptr.hpp>

#include <d3d12.h>

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <climits>
#include <iterator>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
    std::string ReadTextFile(const char* filepath)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
        {
            throw std::runtime_error(std::string("Failed to open shader file: ") + filepath);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    // Amplification-shader group size; must match SCENE_GBUFFER_AS_GROUP_SIZE in the shaders.
    constexpr std::uint32_t kMeshShaderAsGroupSize = 32;

    // Convert a shader-visible SRV heap CPU handle (as exposed by IBL) back to its heap index, so it
    // can be bound as a descriptor table via GetSrvHeapGpuHandle. Mirrors
    // DxrDispatchContext::DepthSrvIndexFromCpuHandle.
    std::uint32_t SrvIndexFromCpuHandle(std::uintptr_t srvCpuHandle)
    {
        if (srvCpuHandle == 0)
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
        if (descriptorSize == 0)
        {
            return UINT32_MAX;
        }
        return static_cast<std::uint32_t>((srvCpuHandle - heapStart.ptr) / descriptorSize);
    }

    struct MeshShaderFrameConstants
    {
        glm::mat4 view{1.0f};
        glm::mat4 prevView{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 unjitteredProjection{1.0f};
        glm::mat4 prevUnjitteredProjection{1.0f};
        glm::vec4 historyStrideMeshId{1.0f, 14.0f, 0.0f, 0.0f};
        glm::vec4 cullParams{0.0f, 0.0f, 0.0f, 0.0f}; // x = meshlet count
        glm::vec4 frustumPlanes[6]{};
    };

    template<typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
    struct alignas(void*) PsoStreamSubobject
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
        T data{};
    };

    struct MeshPipelineStream
    {
        PsoStreamSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rootSignature;
        PsoStreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> amplificationShader;
        PsoStreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> meshShader;
        PsoStreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> pixelShader;
        PsoStreamSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
        PsoStreamSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
        PsoStreamSubobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depthStencil;
        PsoStreamSubobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sampleMask;
        PsoStreamSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
        PsoStreamSubobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rtvFormats;
        PsoStreamSubobject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsvFormat;
        PsoStreamSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sampleDesc;
    };
}

MeshShaderGBufferRenderer::MeshShaderGBufferRenderer()
{
    if (!GfxContext::Get().IsMeshShaderSupported())
    {
        return;
    }

    try
    {
        CreateRootSignature();
        CreatePipelineState();
        m_supported = true;
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string("Failed to create mesh shader G-buffer renderer: ")
            + SafeExceptionMessage(exception));
    }
}

MeshShaderGBufferRenderer::~MeshShaderGBufferRenderer()
{
    if (m_pipelineState != nullptr)
    {
        static_cast<ID3D12PipelineState*>(m_pipelineState)->Release();
    }
    if (m_pipelineStateDoubleSided != nullptr)
    {
        static_cast<ID3D12PipelineState*>(m_pipelineStateDoubleSided)->Release();
    }
    if (m_rootSignature != nullptr)
    {
        static_cast<ID3D12RootSignature*>(m_rootSignature)->Release();
    }
}

void MeshShaderGBufferRenderer::CreateRootSignature()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());

    // 0: frame CBV (b0)  1-7: buffer SRVs (t0-t6)  8: bindless textures (space1)
    // 9: lighting CBV (b1)  10-12: shadow(t7)/prefilter(t8)/brdf-LUT(t9) SRV tables
    D3D12_ROOT_PARAMETER rootParams[13]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[0].Descriptor.ShaderRegister = 0;

    for (UINT rootIndex = 1; rootIndex < 8; ++rootIndex)
    {
        rootParams[rootIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[rootIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[rootIndex].Descriptor.ShaderRegister = rootIndex - 1;
        rootParams[rootIndex].Descriptor.RegisterSpace = 0;
    }

    D3D12_DESCRIPTOR_RANGE bindlessRange{};
    bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    bindlessRange.NumDescriptors = UINT_MAX;
    bindlessRange.BaseShaderRegister = 0;
    bindlessRange.RegisterSpace = 1;
    bindlessRange.OffsetInDescriptorsFromTableStart = 0;

    rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[8].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[8].DescriptorTable.pDescriptorRanges = &bindlessRange;

    rootParams[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    rootParams[9].Descriptor.ShaderRegister = 1;
    rootParams[9].Descriptor.RegisterSpace = 0;

    // One single-descriptor SRV range per lighting texture (they live at arbitrary, non-contiguous
    // heap indices, so each is bound as its own table pointing at the resource's slot).
    D3D12_DESCRIPTOR_RANGE lightingSrvRanges[3]{};
    for (UINT lightingIndex = 0; lightingIndex < 3; ++lightingIndex)
    {
        lightingSrvRanges[lightingIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        lightingSrvRanges[lightingIndex].NumDescriptors = 1;
        lightingSrvRanges[lightingIndex].BaseShaderRegister = 7 + lightingIndex; // t7, t8, t9
        lightingSrvRanges[lightingIndex].RegisterSpace = 0;
        lightingSrvRanges[lightingIndex].OffsetInDescriptorsFromTableStart = 0;

        const UINT rootIndex = 10 + lightingIndex;
        rootParams[rootIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[rootIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rootParams[rootIndex].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[rootIndex].DescriptorTable.pDescriptorRanges = &lightingSrvRanges[lightingIndex];
    }

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    D3D12_STATIC_SAMPLER_DESC& linearWrapSampler = samplers[0];
    linearWrapSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearWrapSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrapSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrapSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    linearWrapSampler.MipLODBias = 0.0f;
    linearWrapSampler.MaxAnisotropy = 8;
    linearWrapSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    linearWrapSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    linearWrapSampler.MinLOD = 0.0f;
    linearWrapSampler.MaxLOD = D3D12_FLOAT32_MAX;
    linearWrapSampler.ShaderRegister = 0;
    linearWrapSampler.RegisterSpace = 0;
    linearWrapSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1: linear clamp for IBL prefilter cube + BRDF LUT (LUT edges must clamp).
    D3D12_STATIC_SAMPLER_DESC& linearClampSampler = samplers[1];
    linearClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampSampler.MipLODBias = 0.0f;
    linearClampSampler.MaxAnisotropy = 1;
    linearClampSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    linearClampSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    linearClampSampler.MinLOD = 0.0f;
    linearClampSampler.MaxLOD = D3D12_FLOAT32_MAX;
    linearClampSampler.ShaderRegister = 1;
    linearClampSampler.RegisterSpace = 0;
    linearClampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
    rootDesc.pParameters = rootParams;
    rootDesc.NumStaticSamplers = static_cast<UINT>(std::size(samplers));
    rootDesc.pStaticSamplers = samplers;
    rootDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> signatureError;
    ThrowIfFailed(
        D3D12SerializeRootSignature(
            &rootDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &signatureBlob,
            &signatureError),
        "D3D12SerializeRootSignature(mesh shader G-buffer) failed");

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature(mesh shader G-buffer) failed");
    m_rootSignature = rootSignature.Detach();
}

void MeshShaderGBufferRenderer::CreatePipelineState()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    ComPtr<ID3D12Device2> device2;
    ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&device2)), "QueryInterface(ID3D12Device2) failed");

    const std::string amplificationSource =
        ReadTextFile(EngineConstants::SceneGBufferAmplificationShader);
    const std::string meshSource = ReadTextFile(EngineConstants::SceneGBufferMeshShader);
    const std::string pixelSource = ReadTextFile(EngineConstants::SceneGBufferMeshFragmentShader);
    const HlslCompileResult amplificationCompile = CompileHlsl(
        amplificationSource, EngineConstants::SceneGBufferAmplificationShader, "main", "as_6_5");
    const HlslCompileResult meshCompile =
        CompileHlsl(meshSource, EngineConstants::SceneGBufferMeshShader, "main", "ms_6_5");
    const HlslCompileResult pixelCompile =
        CompileHlsl(pixelSource, EngineConstants::SceneGBufferMeshFragmentShader, "main", "ps_6_0");

    MeshPipelineStream stream{};
    stream.rootSignature.data = static_cast<ID3D12RootSignature*>(m_rootSignature);
    stream.amplificationShader.data = {
        amplificationCompile.shader->GetBufferPointer(),
        amplificationCompile.shader->GetBufferSize()};
    stream.meshShader.data = {
        meshCompile.shader->GetBufferPointer(),
        meshCompile.shader->GetBufferSize()};
    stream.pixelShader.data = {
        pixelCompile.shader->GetBufferPointer(),
        pixelCompile.shader->GetBufferSize()};

    stream.blend.data.AlphaToCoverageEnable = FALSE;
    stream.blend.data.IndependentBlendEnable = FALSE;
    for (D3D12_RENDER_TARGET_BLEND_DESC& target : stream.blend.data.RenderTarget)
    {
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }

    stream.rasterizer.data.FillMode = D3D12_FILL_MODE_SOLID;
    stream.rasterizer.data.FrontCounterClockwise = TRUE;
    stream.rasterizer.data.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    stream.rasterizer.data.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    stream.rasterizer.data.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    stream.rasterizer.data.DepthClipEnable = TRUE;
    stream.rasterizer.data.MultisampleEnable = FALSE;
    stream.rasterizer.data.AntialiasedLineEnable = FALSE;
    stream.rasterizer.data.ForcedSampleCount = 0;
    stream.rasterizer.data.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    stream.depthStencil.data.DepthEnable = TRUE;
    stream.depthStencil.data.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    stream.depthStencil.data.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    stream.depthStencil.data.StencilEnable = FALSE;

    stream.sampleMask.data = UINT_MAX;
    stream.topology.data = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    stream.rtvFormats.data.NumRenderTargets = 7;
    stream.rtvFormats.data.RTFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    stream.rtvFormats.data.RTFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    stream.rtvFormats.data.RTFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    stream.rtvFormats.data.RTFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    stream.rtvFormats.data.RTFormats[4] = DXGI_FORMAT_R16G16_FLOAT;
    stream.rtvFormats.data.RTFormats[5] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    stream.rtvFormats.data.RTFormats[6] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    stream.dsvFormat.data = DXGI_FORMAT_D24_UNORM_S8_UINT;
    stream.sampleDesc.data.Count = 1;
    stream.sampleDesc.data.Quality = 0;

    // Two cull-mode variants: CULL_BACK for single-sided materials, CULL_NONE for double-sided
    // (glTF `doubleSided`). Without the double-sided variant, front faces of models authored for
    // two-sided rendering get culled and you see through to the interior (mirrors the classic path's
    // double-sided PSO and the mesh-shader shadow renderer).
    auto createPipeline = [&](D3D12_CULL_MODE cullMode) {
        stream.rasterizer.data.CullMode = cullMode;
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes = sizeof(stream);
        streamDesc.pPipelineStateSubobjectStream = &stream;

        ComPtr<ID3D12PipelineState> pipelineState;
        ThrowIfFailed(
            device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState)),
            "CreatePipelineState(mesh shader G-buffer) failed");
        return pipelineState.Detach();
    };

    m_pipelineState = createPipeline(D3D12_CULL_MODE_BACK);
    m_pipelineStateDoubleSided = createPipeline(D3D12_CULL_MODE_NONE);
}

MeshShaderGBufferRenderer::MeshLightingBindings MeshShaderGBufferRenderer::BuildLightingBindings(
    const Camera& camera,
    const SceneLighting& lighting,
    const CascadedShadowMap* shadowMap,
    const IBL& ibl,
    const DirectionalShadowSettings& shadowSettings) const
{
    MeshLightingBindings bindings;
    MeshLightingConstants& constants = bindings.constants;

    constants.viewPos = glm::vec4(camera.GetPosition(), 0.0f);

    const std::vector<Light>& lights = lighting.GetLights();
    const int lightCount = std::min(static_cast<int>(lights.size()), 8);
    const bool hasRenderedShadow = shadowMap != nullptr && shadowMap->HasRenderedDepth();
    constants.lightMeta = glm::ivec4(
        lightCount,
        lighting.GetShadowLightIndex(),
        hasRenderedShadow ? 1 : 0,
        static_cast<int>(shadowSettings.GetFilterMode()));

    for (int i = 0; i < lightCount; ++i)
    {
        const Light& light = lights[static_cast<std::size_t>(i)];
        MeshLightData& dst = constants.lights[i];
        dst.typeAndFlags = glm::ivec4(static_cast<int>(light.GetType()), 0, 0, 0);
        dst.position = glm::vec4(light.GetPosition(), 0.0f);
        dst.direction = glm::vec4(glm::normalize(light.GetDirection()), 0.0f);
        dst.color = glm::vec4(light.GetColor(), 0.0f);
        dst.params0 = glm::vec4(
            light.GetIntensity(),
            light.GetConstantAttenuation(),
            light.GetLinearAttenuation(),
            light.GetQuadraticAttenuation());
        dst.params1 = glm::vec4(
            light.GetRange(),
            light.GetInnerCutoffCos(),
            light.GetOuterCutoffCos(),
            0.0f);
    }

    if (shadowMap != nullptr)
    {
        const int activeCascadeCount = shadowMap->GetActiveCascadeCount();
        const std::array<glm::mat4, CascadedShadowMap::MaxCascades>& lightSpaceMatrices =
            shadowMap->GetLightSpaceMatrices();
        const std::array<float, CascadedShadowMap::MaxCascades>& cascadeEndSplits =
            shadowMap->GetCascadeEndSplits();
        const std::array<ShadowLightSpaceSetup, CascadedShadowMap::MaxCascades>& cascadeSetups =
            shadowMap->GetCascadeSetups();

        for (int cascadeIndex = 0; cascadeIndex < CascadedShadowMap::MaxCascades; ++cascadeIndex)
        {
            constants.lightSpaceMatrices[cascadeIndex] = lightSpaceMatrices[cascadeIndex];
            constants.cascadeEndSplits[cascadeIndex] = cascadeEndSplits[cascadeIndex];
            const ShadowLightSpaceSetup& setup = cascadeSetups[static_cast<std::size_t>(cascadeIndex)];
            constants.cascadeTexelWorldSizes[cascadeIndex] =
                std::max(setup.texelWorldSizeX, setup.texelWorldSizeY);
            constants.cascadeClipDepthMin[cascadeIndex] = setup.clipDepthContentMin;
            constants.cascadeClipDepthMax[cascadeIndex] = setup.clipDepthContentMax;
        }

        constants.shadowParams0 = glm::vec4(
            shadowSettings.GetCascadeBlendRatio(),
            static_cast<float>(activeCascadeCount),
            camera.GetNearPlane(),
            static_cast<float>(shadowMap->GetResolution()));
        constants.shadowParams1 = glm::vec4(
            static_cast<float>(shadowSettings.GetPcfKernelRadius()),
            shadowSettings.GetUsePoissonPcf() ? 1.0f : 0.0f,
            shadowSettings.GetMinPenumbraTexels(),
            static_cast<float>(shadowSettings.GetPcssBlockerRadius()));
        constants.shadowParams2 = glm::vec4(
            shadowSettings.GetPcssLightAngularSize(),
            shadowSettings.GetPcssMinPenumbraTexels(),
            shadowSettings.GetPcssMaxPenumbraTexels(),
            shadowSettings.GetWorldBiasScale());
        constants.shadowParams3 = glm::vec4(shadowSettings.GetDepthBiasScale(), 0.0f, 0.0f, 0.0f);

        bindings.shadowSrvIndex = shadowMap->GetDepthSrvIndex();
    }

    constants.iblParams = glm::vec4(
        ibl.GetMaxReflectionLod(),
        ibl.GetEnvironmentIntensity(),
        ibl.GetReflectionsReplaceSpecIbl() ? 1.0f : 0.0f,
        ibl.GetGiReplacesDiffuseIbl() ? 1.0f : 0.0f);

    const IrradianceSh9& sh = ibl.GetIrradianceSh9();
    for (std::size_t coeff = 0; coeff < sh.coefficients.size() && coeff < 9; ++coeff)
    {
        constants.irradianceSh[coeff] = sh.coefficients[coeff];
    }

    bindings.prefilterSrvIndex = SrvIndexFromCpuHandle(ibl.GetPrefilterMapSrvCpuHandle());
    bindings.brdfSrvIndex = SrvIndexFromCpuHandle(ibl.GetBrdfLutSrvCpuHandle());

    bindings.valid =
        bindings.shadowSrvIndex != UINT32_MAX
        && bindings.prefilterSrvIndex != UINT32_MAX
        && bindings.brdfSrvIndex != UINT32_MAX;
    return bindings;
}

std::uint32_t MeshShaderGBufferRenderer::DispatchMeshAssetBatch(
    const Batch& batch,
    const SceneTables& sceneTables,
    const MeshLightingBindings& lightingBindings,
    const Camera& camera,
    const glm::mat4& prevView,
    const glm::mat4& prevUnjitteredProjection,
    const bool temporalHistoryValid) const
{
    if (!lightingBindings.valid)
    {
        return 0;
    }
    const Mesh* mesh = batch.mesh;
    if (!m_supported || mesh == nullptr || mesh->GetMeshletCount() == 0)
    {
        return 0;
    }
    if (batch.instanceIds.empty()
        || sceneTables.instanceTableGpuAddress == 0
        || sceneTables.materialTableGpuAddress == 0)
    {
        return 0;
    }

    mesh->EnsureGpuResources();
    if (!mesh->GetVertexShaderResourceBuffer().IsValid()
        || !mesh->GetMeshletBuffer().IsValid()
        || !mesh->GetMeshletVertexBuffer().IsValid()
        || !mesh->GetMeshletTriangleBuffer().IsValid())
    {
        return 0;
    }

    auto* baseCommandList =
        static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    ComPtr<ID3D12GraphicsCommandList6> commandList6;
    if (FAILED(baseCommandList->QueryInterface(IID_PPV_ARGS(&commandList6))) || commandList6 == nullptr)
    {
        return 0;
    }

    MeshShaderFrameConstants constants{};
    constants.view = camera.GetViewMatrix();
    constants.prevView = prevView;
    constants.projection = camera.GetProjectionMatrix();
    constants.unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
    constants.prevUnjitteredProjection = prevUnjitteredProjection;
    constants.historyStrideMeshId = glm::vec4(
        temporalHistoryValid ? 1.0f : 0.0f,
        static_cast<float>(mesh->GetFloatsPerVertex()),
        static_cast<float>(batch.meshId),
        0.0f);
    constants.cullParams =
        glm::vec4(static_cast<float>(mesh->GetMeshletCount()), 0.0f, 0.0f, 0.0f);
    // Cull against the exact frustum being rasterized (jittered projection); conservative meshlet
    // spheres make the sub-pixel jitter irrelevant, so nothing that draws is ever culled.
    const std::array<glm::vec4, 6> frustumPlanes =
        ExtractFrustumPlanesZO(camera.GetProjectionMatrix() * camera.GetViewMatrix());
    for (std::size_t planeIndex = 0; planeIndex < frustumPlanes.size(); ++planeIndex)
    {
        constants.frustumPlanes[planeIndex] = frustumPlanes[planeIndex];
    }

    const GfxContext::TransientUploadAllocation upload =
        GfxContext::Get().AllocateTransientUpload(&constants, sizeof(constants));
    if (upload.gpuAddress == 0)
    {
        return 0;
    }
    const GfxContext::TransientUploadAllocation instanceIdUpload =
        GfxContext::Get().AllocateTransientUpload(
            batch.instanceIds.data(),
            static_cast<std::uint32_t>(batch.instanceIds.size() * sizeof(std::uint32_t)));
    if (instanceIdUpload.gpuAddress == 0)
    {
        return 0;
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    if (srvHeap == nullptr)
    {
        return 0;
    }
    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    commandList6->SetDescriptorHeaps(1, heaps);

    void* selectedPipeline =
        batch.doubleSided && m_pipelineStateDoubleSided != nullptr
            ? m_pipelineStateDoubleSided
            : m_pipelineState;
    commandList6->SetPipelineState(static_cast<ID3D12PipelineState*>(selectedPipeline));
    commandList6->SetGraphicsRootSignature(static_cast<ID3D12RootSignature*>(m_rootSignature));
    commandList6->SetGraphicsRootConstantBufferView(0, upload.gpuAddress);
    commandList6->SetGraphicsRootShaderResourceView(
        1,
        mesh->GetVertexShaderResourceBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(
        2,
        mesh->GetMeshletBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(
        3,
        mesh->GetMeshletVertexBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(
        4,
        mesh->GetMeshletTriangleBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(5, sceneTables.instanceTableGpuAddress);
    commandList6->SetGraphicsRootShaderResourceView(6, sceneTables.materialTableGpuAddress);
    commandList6->SetGraphicsRootShaderResourceView(7, instanceIdUpload.gpuAddress);

    D3D12_GPU_DESCRIPTOR_HANDLE bindlessTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
    commandList6->SetGraphicsRootDescriptorTable(8, bindlessTable);

    const GfxContext::TransientUploadAllocation lightingUpload =
        GfxContext::Get().AllocateTransientUpload(
            &lightingBindings.constants, sizeof(lightingBindings.constants));
    if (lightingUpload.gpuAddress == 0)
    {
        return 0;
    }
    commandList6->SetGraphicsRootConstantBufferView(9, lightingUpload.gpuAddress);

    const std::uint32_t lightingSrvIndices[3] = {
        lightingBindings.shadowSrvIndex,
        lightingBindings.prefilterSrvIndex,
        lightingBindings.brdfSrvIndex};
    for (UINT lightingSlot = 0; lightingSlot < 3; ++lightingSlot)
    {
        D3D12_GPU_DESCRIPTOR_HANDLE handle{};
        handle.ptr = reinterpret_cast<UINT64>(
            GfxContext::Get().GetSrvHeapGpuHandle(lightingSrvIndices[lightingSlot]));
        commandList6->SetGraphicsRootDescriptorTable(10 + lightingSlot, handle);
    }

    const std::uint32_t meshletCount = mesh->GetMeshletCount();
    const std::uint32_t instanceCount = static_cast<std::uint32_t>(batch.instanceIds.size());
    // The amplification shader frustum-culls; dispatch one AS group per (meshlet chunk, instance).
    const std::uint32_t amplificationGroupsPerInstance =
        (meshletCount + kMeshShaderAsGroupSize - 1) / kMeshShaderAsGroupSize;
    commandList6->DispatchMesh(amplificationGroupsPerInstance, instanceCount, 1);
    return meshletCount * instanceCount;
}
