#include "engine/rendering/passes/MeshShaderShadowRenderer.h"

#include "engine/platform/system/ExceptionMessage.h"
#include "engine/rendering/core/Constants.h"
#include "engine/rendering/resources/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <d3d12.h>

#include <wrl/client.h>

#include <climits>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
    // Amplification-shader group size; must match SCENE_SHADOW_AS_GROUP_SIZE in the shaders.
    constexpr std::uint32_t kShadowAsGroupSize = 32;

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

    struct MeshShaderShadowConstants
    {
        glm::mat4 lightSpaceMatrix{1.0f};
        glm::vec4 lightDirectionBias{0.0f, 1.0f, 0.0f, 0.0f}; // xyz = dir, w = normal-offset bias
        glm::vec4 cullParams{0.0f, 14.0f, 0.0f, 0.0f};        // x = meshlet count, y = float stride
        glm::vec4 frustumPlanes[6]{};
    };

    template<typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
    struct alignas(void*) PsoStreamSubobject
    {
        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
        T data{};
    };

    struct MeshShadowPipelineStream
    {
        PsoStreamSubobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> rootSignature;
        PsoStreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS> amplificationShader;
        PsoStreamSubobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> meshShader;
        PsoStreamSubobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
        PsoStreamSubobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> rasterizer;
        PsoStreamSubobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depthStencil;
        PsoStreamSubobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> sampleMask;
        PsoStreamSubobject<D3D12_PRIMITIVE_TOPOLOGY_TYPE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY> topology;
        PsoStreamSubobject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsvFormat;
        PsoStreamSubobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sampleDesc;
    };
}

MeshShaderShadowRenderer::MeshShaderShadowRenderer()
{
    if (!GfxContext::Get().IsMeshShaderSupported())
    {
        return;
    }

    try
    {
        CreateRootSignature();
        CreatePipelineStates();
        m_supported = true;
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string("Failed to create mesh shader shadow renderer: ")
            + SafeExceptionMessage(exception));
    }
}

MeshShaderShadowRenderer::~MeshShaderShadowRenderer()
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

void MeshShaderShadowRenderer::CreateRootSignature()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());

    // b0 = frame CBV; t0..t5 = vertex floats, meshlets, meshlet verts, meshlet tris, instances,
    // instance ids. Visibility ALL because the amplification shader also reads meshlets/instances.
    D3D12_ROOT_PARAMETER rootParams[7]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[0].Descriptor.ShaderRegister = 0;

    for (UINT rootIndex = 1; rootIndex < 7; ++rootIndex)
    {
        rootParams[rootIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[rootIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[rootIndex].Descriptor.ShaderRegister = rootIndex - 1;
        rootParams[rootIndex].Descriptor.RegisterSpace = 0;
    }

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
    rootDesc.pParameters = rootParams;
    rootDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

    ComPtr<ID3DBlob> signatureBlob;
    ComPtr<ID3DBlob> signatureError;
    ThrowIfFailed(
        D3D12SerializeRootSignature(
            &rootDesc,
            D3D_ROOT_SIGNATURE_VERSION_1,
            &signatureBlob,
            &signatureError),
        "D3D12SerializeRootSignature(mesh shader shadows) failed");

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature(mesh shader shadows) failed");
    m_rootSignature = rootSignature.Detach();
}

void MeshShaderShadowRenderer::CreatePipelineStates()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    ComPtr<ID3D12Device2> device2;
    ThrowIfFailed(device->QueryInterface(IID_PPV_ARGS(&device2)), "QueryInterface(ID3D12Device2) failed");

    const std::string amplificationSource =
        ReadTextFile(EngineConstants::SceneShadowAmplificationShader);
    const std::string meshSource = ReadTextFile(EngineConstants::SceneShadowMeshShader);
    const HlslCompileResult amplificationCompile = CompileHlsl(
        amplificationSource, EngineConstants::SceneShadowAmplificationShader, "main", "as_6_5");
    const HlslCompileResult meshCompile =
        CompileHlsl(meshSource, EngineConstants::SceneShadowMeshShader, "main", "ms_6_5");

    auto buildStream = [&](const D3D12_CULL_MODE cullMode) {
        MeshShadowPipelineStream stream{};
        stream.rootSignature.data = static_cast<ID3D12RootSignature*>(m_rootSignature);
        stream.amplificationShader.data = {
            amplificationCompile.shader->GetBufferPointer(),
            amplificationCompile.shader->GetBufferSize()};
        stream.meshShader.data = {
            meshCompile.shader->GetBufferPointer(),
            meshCompile.shader->GetBufferSize()};

        stream.blend.data.AlphaToCoverageEnable = FALSE;
        stream.blend.data.IndependentBlendEnable = FALSE;
        for (D3D12_RENDER_TARGET_BLEND_DESC& target : stream.blend.data.RenderTarget)
        {
            target.RenderTargetWriteMask = 0;
        }

        stream.rasterizer.data.FillMode = D3D12_FILL_MODE_SOLID;
        stream.rasterizer.data.CullMode = cullMode;
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
        stream.dsvFormat.data = DXGI_FORMAT_D32_FLOAT;
        stream.sampleDesc.data.Count = 1;
        stream.sampleDesc.data.Quality = 0;
        return stream;
    };

    auto createPipeline = [&](const MeshShadowPipelineStream& stream) {
        D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
        streamDesc.SizeInBytes = sizeof(stream);
        streamDesc.pPipelineStateSubobjectStream =
            const_cast<MeshShadowPipelineStream*>(&stream);

        ComPtr<ID3D12PipelineState> pipelineState;
        ThrowIfFailed(
            device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState)),
            "CreatePipelineState(mesh shader shadows) failed");
        return pipelineState.Detach();
    };

    const MeshShadowPipelineStream singleSided = buildStream(D3D12_CULL_MODE_BACK);
    const MeshShadowPipelineStream doubleSided = buildStream(D3D12_CULL_MODE_NONE);
    m_pipelineState = createPipeline(singleSided);
    m_pipelineStateDoubleSided = createPipeline(doubleSided);
}

std::uint32_t MeshShaderShadowRenderer::DispatchMeshAssetBatch(
    const Batch& batch,
    const SceneTables& sceneTables,
    const glm::mat4& lightSpaceMatrix,
    const std::array<glm::vec4, 6>& cascadeFrustumPlanes,
    const glm::vec3& lightDirectionTowardSource,
    const float casterDepthBias) const
{
    const Mesh* mesh = batch.mesh;
    if (!m_supported || mesh == nullptr || mesh->GetMeshletCount() == 0)
    {
        return 0;
    }
    if (batch.instanceIds.empty() || sceneTables.instanceTableGpuAddress == 0)
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

    MeshShaderShadowConstants constants{};
    constants.lightSpaceMatrix = lightSpaceMatrix;
    constants.lightDirectionBias = glm::vec4(lightDirectionTowardSource, casterDepthBias);
    constants.cullParams = glm::vec4(
        static_cast<float>(mesh->GetMeshletCount()),
        static_cast<float>(mesh->GetFloatsPerVertex()),
        0.0f,
        0.0f);
    for (std::size_t planeIndex = 0; planeIndex < cascadeFrustumPlanes.size(); ++planeIndex)
    {
        constants.frustumPlanes[planeIndex] = cascadeFrustumPlanes[planeIndex];
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

    ID3D12PipelineState* pipeline = static_cast<ID3D12PipelineState*>(
        batch.doubleSided && m_pipelineStateDoubleSided != nullptr
            ? m_pipelineStateDoubleSided
            : m_pipelineState);
    if (pipeline == nullptr)
    {
        return 0;
    }

    commandList6->SetPipelineState(pipeline);
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
    commandList6->SetGraphicsRootShaderResourceView(6, instanceIdUpload.gpuAddress);

    const std::uint32_t meshletCount = mesh->GetMeshletCount();
    const std::uint32_t instanceCount = static_cast<std::uint32_t>(batch.instanceIds.size());
    const std::uint32_t amplificationGroupsPerInstance =
        (meshletCount + kShadowAsGroupSize - 1) / kShadowAsGroupSize;
    commandList6->DispatchMesh(amplificationGroupsPerInstance, instanceCount, 1);
    return meshletCount * instanceCount;
}
