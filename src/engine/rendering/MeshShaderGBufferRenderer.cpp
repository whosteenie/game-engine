#include "engine/rendering/MeshShaderGBufferRenderer.h"

#include "engine/camera/Camera.h"
#include "engine/platform/ExceptionMessage.h"
#include "engine/rendering/Constants.h"
#include "engine/rendering/Mesh.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <d3d12.h>

#include <wrl/client.h>

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

    struct MeshShaderFrameConstants
    {
        glm::mat4 model{1.0f};
        glm::mat4 prevModel{1.0f};
        glm::mat4 view{1.0f};
        glm::mat4 prevView{1.0f};
        glm::mat4 projection{1.0f};
        glm::mat4 unjitteredProjection{1.0f};
        glm::mat4 prevUnjitteredProjection{1.0f};
        glm::vec4 albedoRoughness{1.0f, 1.0f, 1.0f, 0.5f};
        glm::vec4 metallicHistoryStride{0.0f, 1.0f, 14.0f, 0.0f};
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
    if (m_rootSignature != nullptr)
    {
        static_cast<ID3D12RootSignature*>(m_rootSignature)->Release();
    }
}

void MeshShaderGBufferRenderer::CreateRootSignature()
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());

    D3D12_ROOT_PARAMETER rootParams[5]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rootParams[0].Descriptor.ShaderRegister = 0;

    for (UINT rootIndex = 1; rootIndex < 5; ++rootIndex)
    {
        rootParams[rootIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[rootIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_MESH;
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

    const std::string meshSource = ReadTextFile(EngineConstants::SceneGBufferMeshShader);
    const std::string pixelSource = ReadTextFile(EngineConstants::SceneGBufferMeshFragmentShader);
    const HlslCompileResult meshCompile =
        CompileHlsl(meshSource, EngineConstants::SceneGBufferMeshShader, "main", "ms_6_5");
    const HlslCompileResult pixelCompile =
        CompileHlsl(pixelSource, EngineConstants::SceneGBufferMeshFragmentShader, "main", "ps_6_0");

    MeshPipelineStream stream{};
    stream.rootSignature.data = static_cast<ID3D12RootSignature*>(m_rootSignature);
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
    stream.rasterizer.data.CullMode = D3D12_CULL_MODE_BACK;
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

    D3D12_PIPELINE_STATE_STREAM_DESC streamDesc{};
    streamDesc.SizeInBytes = sizeof(stream);
    streamDesc.pPipelineStateSubobjectStream = &stream;

    ComPtr<ID3D12PipelineState> pipelineState;
    ThrowIfFailed(
        device2->CreatePipelineState(&streamDesc, IID_PPV_ARGS(&pipelineState)),
        "CreatePipelineState(mesh shader G-buffer) failed");
    m_pipelineState = pipelineState.Detach();
}

std::uint32_t MeshShaderGBufferRenderer::DispatchMesh(
    const Mesh& mesh,
    const Camera& camera,
    const glm::mat4& model,
    const glm::mat4& prevModel,
    const glm::mat4& prevView,
    const glm::mat4& prevUnjitteredProjection,
    const bool temporalHistoryValid,
    const glm::vec3& albedo,
    const float roughness,
    const float metallic) const
{
    if (!m_supported || mesh.GetMeshletCount() == 0)
    {
        return 0;
    }

    mesh.EnsureGpuResources();
    if (!mesh.GetVertexShaderResourceBuffer().IsValid()
        || !mesh.GetMeshletBuffer().IsValid()
        || !mesh.GetMeshletVertexBuffer().IsValid()
        || !mesh.GetMeshletTriangleBuffer().IsValid())
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
    constants.model = model;
    constants.prevModel = prevModel;
    constants.view = camera.GetViewMatrix();
    constants.prevView = prevView;
    constants.projection = camera.GetProjectionMatrix();
    constants.unjitteredProjection = camera.GetUnjitteredProjectionMatrix();
    constants.prevUnjitteredProjection = prevUnjitteredProjection;
    constants.albedoRoughness = glm::vec4(albedo, roughness);
    constants.metallicHistoryStride = glm::vec4(
        metallic,
        temporalHistoryValid ? 1.0f : 0.0f,
        static_cast<float>(mesh.GetFloatsPerVertex()),
        0.0f);

    const GfxContext::TransientUploadAllocation upload =
        GfxContext::Get().AllocateTransientUpload(&constants, sizeof(constants));
    if (upload.gpuAddress == 0)
    {
        return 0;
    }

    commandList6->SetPipelineState(static_cast<ID3D12PipelineState*>(m_pipelineState));
    commandList6->SetGraphicsRootSignature(static_cast<ID3D12RootSignature*>(m_rootSignature));
    commandList6->SetGraphicsRootConstantBufferView(0, upload.gpuAddress);
    commandList6->SetGraphicsRootShaderResourceView(
        1,
        mesh.GetVertexShaderResourceBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(
        2,
        mesh.GetMeshletBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(
        3,
        mesh.GetMeshletVertexBuffer().GetGpuVirtualAddress());
    commandList6->SetGraphicsRootShaderResourceView(
        4,
        mesh.GetMeshletTriangleBuffer().GetGpuVirtualAddress());
    const std::uint32_t dispatchGroupCount = mesh.GetMeshletCount();
    commandList6->DispatchMesh(dispatchGroupCount, 1, 1);
    return dispatchGroupCount;
}
