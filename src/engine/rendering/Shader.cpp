#include "engine/rendering/TextureSamplerSettings.h"
#include "engine/rendering/Shader.h"

#include "engine/platform/ExceptionMessage.h"
#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"
#include "engine/rhi/d3d12/HlslCompiler.h"

#include <D3D12MemAlloc.h>
#include <d3d12.h>
#include <dxgiformat.h>

#include <wrl/client.h>

#include <fstream>
#include <algorithm>
#include <glm/gtc/type_ptr.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
    thread_local const Shader* g_activeShader = nullptr;

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

    DXGI_FORMAT RtvFormatForCount(int count)
    {
        (void)count;
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

Shader::Shader(const char* vertexPath, const char* fragmentPath)
{
    try
    {
        BuildFromHlsl(vertexPath, fragmentPath);
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string("Failed to build shader ") + fragmentPath + ": " + SafeExceptionMessage(exception));
    }
    catch (...)
    {
        throw std::runtime_error(std::string("Failed to build shader ") + fragmentPath + ": unknown error");
    }
}

Shader::~Shader()
{
    for (ConstantBuffer& buffer : m_constantBuffers)
    {
        if (buffer.mapped != nullptr && buffer.resource != nullptr)
        {
            static_cast<ID3D12Resource*>(buffer.resource)->Unmap(0, nullptr);
        }

        if (buffer.allocation != nullptr)
        {
            static_cast<D3D12MA::Allocation*>(buffer.allocation)->Release();
        }
    }

    auto releasePipeline = [](void* pipeline) {
        if (pipeline != nullptr)
        {
            static_cast<ID3D12PipelineState*>(pipeline)->Release();
        }
    };

    releasePipeline(m_pipelineState);
    releasePipeline(m_pipelineStateMrt);
    releasePipeline(m_pipelineStateLdr);
    releasePipeline(m_pipelineStateLdrDepthRead);
    releasePipeline(m_pipelineStateDoubleSided);
    releasePipeline(m_pipelineStateMrtDoubleSided);
    releasePipeline(m_pipelineStateLdrDoubleSided);

    if (m_rootSignature != nullptr)
    {
        static_cast<ID3D12RootSignature*>(m_rootSignature)->Release();
    }

    if (m_vertexShader != nullptr)
    {
        static_cast<IUnknown*>(m_vertexShader)->Release();
    }

    if (m_pixelShader != nullptr)
    {
        static_cast<IUnknown*>(m_pixelShader)->Release();
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_constantBuffers(std::move(other.m_constantBuffers)),
      m_uniformLocations(std::move(other.m_uniformLocations)),
      m_rootSignature(other.m_rootSignature),
      m_pipelineState(other.m_pipelineState),
      m_pipelineStateMrt(other.m_pipelineStateMrt),
      m_pipelineStateLdr(other.m_pipelineStateLdr),
      m_pipelineStateLdrDepthRead(other.m_pipelineStateLdrDepthRead),
      m_pipelineStateDoubleSided(other.m_pipelineStateDoubleSided),
      m_pipelineStateMrtDoubleSided(other.m_pipelineStateMrtDoubleSided),
      m_pipelineStateLdrDoubleSided(other.m_pipelineStateLdrDoubleSided),
      m_vertexShader(other.m_vertexShader),
      m_pixelShader(other.m_pixelShader),
      m_textureSlots(std::move(other.m_textureSlots)),
      m_isLinked(other.m_isLinked)
{
    other.m_rootSignature = nullptr;
    other.m_pipelineState = nullptr;
    other.m_pipelineStateMrt = nullptr;
    other.m_pipelineStateLdr = nullptr;
    other.m_pipelineStateLdrDepthRead = nullptr;
    other.m_pipelineStateDoubleSided = nullptr;
    other.m_pipelineStateMrtDoubleSided = nullptr;
    other.m_pipelineStateLdrDoubleSided = nullptr;
    other.m_vertexShader = nullptr;
    other.m_pixelShader = nullptr;
    other.m_isLinked = false;
}

Shader& Shader::operator=(Shader&& other) noexcept
{
    if (this != &other)
    {
        this->~Shader();
        m_constantBuffers = std::move(other.m_constantBuffers);
        m_uniformLocations = std::move(other.m_uniformLocations);
        m_rootSignature = other.m_rootSignature;
        m_pipelineState = other.m_pipelineState;
        m_pipelineStateMrt = other.m_pipelineStateMrt;
        m_pipelineStateLdr = other.m_pipelineStateLdr;
        m_pipelineStateLdrDepthRead = other.m_pipelineStateLdrDepthRead;
        m_pipelineStateDoubleSided = other.m_pipelineStateDoubleSided;
        m_pipelineStateMrtDoubleSided = other.m_pipelineStateMrtDoubleSided;
        m_pipelineStateLdrDoubleSided = other.m_pipelineStateLdrDoubleSided;
        m_vertexShader = other.m_vertexShader;
        m_pixelShader = other.m_pixelShader;
        m_textureSlots = std::move(other.m_textureSlots);
        m_isLinked = other.m_isLinked;
        other.m_rootSignature = nullptr;
        other.m_pipelineState = nullptr;
        other.m_pipelineStateMrt = nullptr;
        other.m_pipelineStateLdr = nullptr;
        other.m_pipelineStateLdrDepthRead = nullptr;
        other.m_pipelineStateDoubleSided = nullptr;
        other.m_pipelineStateMrtDoubleSided = nullptr;
        other.m_pipelineStateLdrDoubleSided = nullptr;
        other.m_vertexShader = nullptr;
        other.m_pixelShader = nullptr;
        other.m_isLinked = false;
    }

    return *this;
}

void Shader::BuildFromHlsl(const std::string& vertexPath, const std::string& fragmentPath)
{
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    D3D12MA::Allocator* allocator = GfxContext::Get().GetMemoryAllocator();

    const std::string vertexSource = ReadTextFile(vertexPath.c_str());
    const std::string fragmentSource = ReadTextFile(fragmentPath.c_str());

    HlslCompileResult vertexCompile{};
    HlslCompileResult pixelCompile{};
    try
    {
        vertexCompile = CompileHlsl(vertexSource, vertexPath, "main", "vs_6_0");
        pixelCompile = CompileHlsl(fragmentSource, fragmentPath, "main", "ps_6_0");
    }
    catch (const std::exception& exception)
    {
        throw std::runtime_error(
            std::string("Shader compile failed for ") + vertexPath + " / " + fragmentPath + ": "
            + SafeExceptionMessage(exception));
    }
    m_vertexShader = vertexCompile.shader.Detach();
    m_pixelShader = pixelCompile.shader.Detach();

    ComPtr<ID3D12RootSignature> rootSignature;

    ComPtr<ID3D12RootSignature> rootSig;
    {
        std::vector<D3D12_ROOT_PARAMETER> rootParams;
        std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
        std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

        D3D12_ROOT_PARAMETER vsCbv{};
        vsCbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        vsCbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        vsCbv.Descriptor.ShaderRegister = 0;
        vsCbv.Descriptor.RegisterSpace = 0;
        rootParams.push_back(vsCbv);

        D3D12_ROOT_PARAMETER psCbv{};
        psCbv.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        psCbv.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        psCbv.Descriptor.ShaderRegister = 0;
        psCbv.Descriptor.RegisterSpace = 0;
        rootParams.push_back(psCbv);

        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 16;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 0;
        ranges.push_back(srvRange);

        D3D12_ROOT_PARAMETER table{};
        table.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        table.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        table.DescriptorTable.NumDescriptorRanges = 1;
        table.DescriptorTable.pDescriptorRanges = &ranges.back();
        rootParams.push_back(table);

        const bool isSkyBackgroundFragment = fragmentPath.find("sky_background") != std::string::npos;
        const bool isScreenCompositeFragment = fragmentPath.find("screen_composite") != std::string::npos;
        const bool isTemporalReprojectFragment = fragmentPath.find("temporal_reproject") != std::string::npos;

        for (UINT registerIndex = 0; registerIndex <= 8; ++registerIndex)
        {
            D3D12_STATIC_SAMPLER_DESC sampler{};
            const bool isEnvironmentEquirectSampler =
                (isSkyBackgroundFragment && registerIndex == 0)
                || (isScreenCompositeFragment && registerIndex == 5);
            if (registerIndex == 8)
            {
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            }
            else if (isTemporalReprojectFragment && registerIndex == 2)
            {
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            }
            else if (isEnvironmentEquirectSampler)
            {
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
                sampler.MaxAnisotropy = 1;
                sampler.MipLODBias = 0.0f;
            }
            else
            {
                const TextureFilterMode filterMode = GfxContext::Get().GetMaterialTextureFilterMode();
                const std::uint32_t anisotropy = GfxContext::Get().GetMaterialTextureAnisotropy();
                if (filterMode != TextureFilterMode::Nearest && anisotropy > 1)
                {
                    sampler.Filter = D3D12_FILTER_ANISOTROPIC;
                    sampler.MaxAnisotropy = anisotropy;
                }
                else
                {
                    sampler.Filter = TextureFilterModeToD3D12Filter(filterMode);
                    sampler.MaxAnisotropy = 1;
                }
                sampler.MipLODBias = GfxContext::Get().GetMaterialTextureMipBias();
            }
            // Material maps tile with UV repeat; shadow + IBL stay clamped.
            const bool wrapMaterialMaps = registerIndex >= 4 && registerIndex <= 7;
            const D3D12_TEXTURE_ADDRESS_MODE addressMode = wrapMaterialMaps
                ? D3D12_TEXTURE_ADDRESS_MODE_WRAP
                : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sampler.AddressU = addressMode;
            sampler.AddressV = addressMode;
            sampler.AddressW = addressMode;
            if (isEnvironmentEquirectSampler)
            {
                sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            }
            sampler.ShaderRegister = registerIndex;
            sampler.RegisterSpace = 0;
            sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            sampler.MaxLOD = D3D12_FLOAT32_MAX;
            staticSamplers.push_back(sampler);
        }

        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = static_cast<UINT>(rootParams.size());
        rootDesc.pParameters = rootParams.data();
        rootDesc.NumStaticSamplers = static_cast<UINT>(staticSamplers.size());
        rootDesc.pStaticSamplers = staticSamplers.empty() ? nullptr : staticSamplers.data();
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ComPtr<ID3DBlob> signatureBlob;
        ComPtr<ID3DBlob> signatureError;
        ThrowIfFailed(
            D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &signatureError),
            "D3D12SerializeRootSignature failed");
        ThrowIfFailed(
            device->CreateRootSignature(
                0,
                signatureBlob->GetBufferPointer(),
                signatureBlob->GetBufferSize(),
                IID_PPV_ARGS(&rootSig)),
            "CreateRootSignature failed");
    }

    m_rootSignature = rootSig.Detach();

    auto createConstantBuffer = [&](const char* name, D3D12_SHADER_VISIBILITY visibility, std::uint32_t sizeBytes) {
        ConstantBuffer buffer{};
        if (sizeBytes == 0)
        {
            sizeBytes = visibility == D3D12_SHADER_VISIBILITY_VERTEX ? 256u : 4096u;
        }

        buffer.size = (sizeBytes + 255u) & ~255u;
        if (buffer.size == 0)
        {
            buffer.size = 256u;
        }
        buffer.staging.assign(buffer.size, 0);
        buffer.rootParameterIndex = visibility == D3D12_SHADER_VISIBILITY_VERTEX ? 0u : 1u;

        D3D12_RESOURCE_DESC resourceDesc{};
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        resourceDesc.Width = buffer.size;
        resourceDesc.Height = 1;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        D3D12MA::ALLOCATION_DESC allocationDesc{};
        allocationDesc.HeapType = D3D12_HEAP_TYPE_UPLOAD;

        ID3D12Resource* resource = nullptr;
        D3D12MA::Allocation* allocation = nullptr;
        if (FAILED(allocator->CreateResource(
                &allocationDesc,
                &resourceDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ,
                nullptr,
                &allocation,
                IID_PPV_ARGS(&resource))))
        {
            throw std::runtime_error(std::string("Failed to create constant buffer for ") + name);
        }

        void* mapped = nullptr;
        if (FAILED(resource->Map(0, nullptr, &mapped)))
        {
            allocation->Release();
            resource->Release();
            throw std::runtime_error(std::string("Failed to map constant buffer for ") + name);
        }

        buffer.resource = resource;
        buffer.allocation = allocation;
        buffer.mapped = mapped;
        m_constantBuffers.push_back(std::move(buffer));
        return m_constantBuffers.size() - 1;
    };

    ComPtr<ID3D12ShaderReflection> vsReflection = vertexCompile.reflection;
    ComPtr<ID3D12ShaderReflection> psReflection = pixelCompile.reflection;

    auto reflectStage = [&](ID3D12ShaderReflection* reflection, const std::uint32_t bufferIndex) {
        D3D12_SHADER_DESC shaderDesc{};
        reflection->GetDesc(&shaderDesc);
        for (UINT cbufferIndex = 0; cbufferIndex < shaderDesc.ConstantBuffers; ++cbufferIndex)
        {
            ID3D12ShaderReflectionConstantBuffer* cbuffer = reflection->GetConstantBufferByIndex(cbufferIndex);
            if (cbuffer == nullptr)
            {
                continue;
            }

            D3D12_SHADER_BUFFER_DESC cbufferDesc{};
            cbuffer->GetDesc(&cbufferDesc);
            for (UINT variableIndex = 0; variableIndex < cbufferDesc.Variables; ++variableIndex)
            {
                ID3D12ShaderReflectionVariable* variable = cbuffer->GetVariableByIndex(variableIndex);
                if (variable == nullptr)
                {
                    continue;
                }

                D3D12_SHADER_VARIABLE_DESC variableDesc{};
                variable->GetDesc(&variableDesc);
                if ((variableDesc.uFlags & D3D_SVF_USED) == 0)
                {
                    continue;
                }

                UniformLocation location{};
                location.bufferIndex = bufferIndex;
                location.offset = static_cast<std::uint32_t>(variableDesc.StartOffset);
                location.size = static_cast<std::uint32_t>(variableDesc.Size);
                m_uniformLocations[variableDesc.Name].push_back(location);
            }
        }
    };

    auto getConstantBufferSize =
        [](ID3D12ShaderReflection* reflection, const char* name, const std::uint32_t fallback) {
            ID3D12ShaderReflectionConstantBuffer* cbuffer = reflection->GetConstantBufferByName(name);
            if (cbuffer == nullptr)
            {
                const char* alternateName =
                    std::strcmp(name, "PerVertex") == 0 ? "PerPixel" : "PerVertex";
                cbuffer = reflection->GetConstantBufferByName(alternateName);
            }

            if (cbuffer == nullptr)
            {
                return fallback;
            }

            D3D12_SHADER_BUFFER_DESC cbufferDesc{};
            cbuffer->GetDesc(&cbufferDesc);
            if (cbufferDesc.Size == 0)
            {
                return fallback;
            }

            return static_cast<std::uint32_t>(cbufferDesc.Size);
        };

    const std::uint32_t vsBufferIndex = createConstantBuffer(
        "PerVertex",
        D3D12_SHADER_VISIBILITY_VERTEX,
        getConstantBufferSize(vsReflection.Get(), "PerVertex", 256));
    const std::uint32_t psBufferIndex = createConstantBuffer(
        "PerPixel",
        D3D12_SHADER_VISIBILITY_PIXEL,
        getConstantBufferSize(psReflection.Get(), "PerPixel", 4096));
    reflectStage(vsReflection.Get(), vsBufferIndex);
    reflectStage(psReflection.Get(), psBufferIndex);

    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = static_cast<ID3D12RootSignature*>(m_rootSignature);
    psoDesc.VS = {static_cast<IDxcBlob*>(m_vertexShader)->GetBufferPointer(),
        static_cast<IDxcBlob*>(m_vertexShader)->GetBufferSize()};
    psoDesc.PS = {static_cast<IDxcBlob*>(m_pixelShader)->GetBufferPointer(),
        static_cast<IDxcBlob*>(m_pixelShader)->GetBufferSize()};
    psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.RasterizerState.DepthClipEnable = TRUE;
    psoDesc.RasterizerState.MultisampleEnable = FALSE;
    psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
    psoDesc.RasterizerState.ForcedSampleCount = 0;
    psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 4;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    psoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    for (UINT targetIndex = 0; targetIndex < 4; ++targetIndex)
    {
        psoDesc.BlendState.RenderTarget[targetIndex].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    psoDesc.SampleDesc.Count = 1;

    D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    D3D12_SHADER_DESC vsDesc{};
    vsReflection->GetDesc(&vsDesc);
    const bool isShadowDepth = vertexPath.find("shadow_depth") != std::string::npos;
    const bool isFullscreen = vertexPath.find("fullscreen") != std::string::npos;
    const bool isIblCubemap = vertexPath.find("ibl_cubemap") != std::string::npos;
    const bool isIblBrdf = vertexPath.find("ibl_brdf") != std::string::npos;
    const bool isGridVertex = vertexPath.find("grid.v") != std::string::npos;
    const bool isGizmoLineVertex = vertexPath.find("gizmo_line.v") != std::string::npos;
    const bool isSkyboxVertex = vertexPath.find("skybox.v") != std::string::npos;
    const bool isSkyBackground = fragmentPath.find("sky_background") != std::string::npos;
    const bool isLinePixel = fragmentPath.find("line.p") != std::string::npos;
    const bool isSelectionMask = vertexPath.find("selection_mask") != std::string::npos;
    const bool isSelectionOutline = vertexPath.find("selection_outline") != std::string::npos;
    const bool isSelectionGlow = fragmentPath.find("selection_glow") != std::string::npos;
    const bool isSelectionSharp = fragmentPath.find("selection_sharp") != std::string::npos;
    const bool isGridComposite = fragmentPath.find("grid_composite") != std::string::npos;
    const bool isDepthBlit = fragmentPath.find("depth_blt") != std::string::npos;
    const bool isMsaaDepthResolve = fragmentPath.find("msaa_depth_resolve") != std::string::npos;

    auto setupAlphaBlend = [](D3D12_RENDER_TARGET_BLEND_DESC& blendDesc) {
        blendDesc.BlendEnable = TRUE;
        blendDesc.LogicOpEnable = FALSE;
        blendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    };

    // Grid alpha-blends into the scene HDR buffer before post-processing.
    auto setupPremultipliedAlphaBlend = [](D3D12_RENDER_TARGET_BLEND_DESC& blendDesc) {
        blendDesc.BlendEnable = TRUE;
        blendDesc.LogicOpEnable = FALSE;
        blendDesc.SrcBlend = D3D12_BLEND_ONE;
        blendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    };

    auto setupAdditiveBlend = [](D3D12_RENDER_TARGET_BLEND_DESC& blendDesc) {
        blendDesc.BlendEnable = TRUE;
        blendDesc.LogicOpEnable = FALSE;
        blendDesc.SrcBlend = D3D12_BLEND_ONE;
        blendDesc.DestBlend = D3D12_BLEND_ONE;
        blendDesc.BlendOp = D3D12_BLEND_OP_ADD;
        blendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.DestBlendAlpha = D3D12_BLEND_ONE;
        blendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    };

    auto setupDepthReadOnly = [](D3D12_DEPTH_STENCIL_DESC& depthDesc) {
        depthDesc.DepthEnable = TRUE;
        depthDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        depthDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        depthDesc.StencilEnable = FALSE;
    };

    auto applyNoDepthPass = [&]() {
        psoDesc.DepthStencilState.DepthEnable = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        // Keep DSVFormat compatible with the viewport depth buffer so OMSetRenderTargets
        // can stay bound while depth testing is disabled.
    };

    auto applySingleRenderTarget = [&](DXGI_FORMAT format) {
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = format;
        for (UINT targetIndex = 1; targetIndex < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++targetIndex)
        {
            psoDesc.RTVFormats[targetIndex] = DXGI_FORMAT_UNKNOWN;
        }
    };

    static D3D12_INPUT_ELEMENT_DESC positionLayout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };

    if (isShadowDepth)
    {
        psoDesc.NumRenderTargets = 0;
        for (UINT targetIndex = 0; targetIndex < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++targetIndex)
        {
            psoDesc.RTVFormats[targetIndex] = DXGI_FORMAT_UNKNOWN;
        }
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        // Store the closest caster surface from the light. Back-face shadow maps hide acne, but
        // they also write the far side of convex casters and open contact gaps at the floor.
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.DepthBias = 0;
        psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = 0.0f;
        static D3D12_INPUT_ELEMENT_DESC shadowLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        psoDesc.InputLayout = {shadowLayout, 2};
    }
    else if (isFullscreen && (isDepthBlit || isMsaaDepthResolve))
    {
        static D3D12_INPUT_ELEMENT_DESC fullscreenLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        psoDesc.InputLayout = {fullscreenLayout, 2};
        psoDesc.NumRenderTargets = 0;
        for (UINT targetIndex = 0; targetIndex < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++targetIndex)
        {
            psoDesc.RTVFormats[targetIndex] = DXGI_FORMAT_UNKNOWN;
        }
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
    }
    else if (isFullscreen || isIblBrdf)
    {
        static D3D12_INPUT_ELEMENT_DESC fullscreenLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        psoDesc.InputLayout = {fullscreenLayout, 2};
        applySingleRenderTarget(isIblBrdf ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT);
        applyNoDepthPass();
        if (isSelectionGlow)
        {
            setupAdditiveBlend(psoDesc.BlendState.RenderTarget[0]);
        }
        else if (isSelectionSharp)
        {
            setupAlphaBlend(psoDesc.BlendState.RenderTarget[0]);
        }
        else if (isGridComposite)
        {
            setupPremultipliedAlphaBlend(psoDesc.BlendState.RenderTarget[0]);
        }
    }
    else if (isSelectionMask)
    {
        psoDesc.InputLayout = {positionLayout, 1};
        applySingleRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT);
        applyNoDepthPass();
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    else if (isSelectionOutline)
    {
        static D3D12_INPUT_ELEMENT_DESC outlineLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        psoDesc.InputLayout = {outlineLayout, 2};
        applySingleRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT);
        applyNoDepthPass();
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    }
    else if (isIblCubemap)
    {
        static D3D12_INPUT_ELEMENT_DESC cubemapLayout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        };
        psoDesc.InputLayout = {cubemapLayout, 1};
        applySingleRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    }
    else if (isGizmoLineVertex && isLinePixel)
    {
        psoDesc.InputLayout = {positionLayout, 1};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        applySingleRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        applyNoDepthPass();
        psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    }
    else if (isGridVertex && isLinePixel)
    {
        psoDesc.InputLayout = {positionLayout, 1};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        applySingleRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT);
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        applyNoDepthPass();
    }
    else if (isGridVertex)
    {
        psoDesc.InputLayout = {positionLayout, 1};
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        setupDepthReadOnly(psoDesc.DepthStencilState);
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        for (UINT targetIndex = 0; targetIndex < 4; ++targetIndex)
        {
            setupAlphaBlend(psoDesc.BlendState.RenderTarget[targetIndex]);
        }
    }
    else if (isSkyboxVertex || isSkyBackground)
    {
        psoDesc.InputLayout = {positionLayout, 1};
        if (isSkyBackground)
        {
            static D3D12_INPUT_ELEMENT_DESC fullscreenLayout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };
            psoDesc.InputLayout = {fullscreenLayout, 2};
        }
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        applyNoDepthPass();
    }
    else
    {
        psoDesc.InputLayout = {inputLayout, 5};
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    }

    const bool isPbr = fragmentPath.find("pbr.p") != std::string::npos;
    const bool isSkyGeometryShader = isSkyboxVertex || isSkyBackground;
    const bool supportsMrt = isPbr || (isGridVertex && !isLinePixel) || isSkyGeometryShader;

    const int geometryMsaaCount = GfxContext::Get().GetActiveMsaaSampleCount();
    const bool useGeometryMsaa =
        geometryMsaaCount > 1 && (isPbr || isSkyGeometryShader);

    const int mrtCount = isPbr ? 7 : 4;
    const bool usesExtendedGbufferMrt = isPbr;

    auto createPipeline = [&](const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc) {
        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT createResult =
            device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(createResult))
        {
            throw std::runtime_error(
                std::string("CreateGraphicsPipelineState failed for ") + fragmentPath + " (HRESULT=0x"
                + std::to_string(static_cast<unsigned long>(createResult)) + ")");
        }
        return pipeline.Detach();
    };

    if (supportsMrt)
    {
        if (useGeometryMsaa)
        {
            psoDesc.SampleDesc.Count = static_cast<UINT>(geometryMsaaCount);
            psoDesc.SampleDesc.Quality = 0;
            psoDesc.RasterizerState.MultisampleEnable = TRUE;
        }

        psoDesc.NumRenderTargets = mrtCount;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[3] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.RTVFormats[4] = usesExtendedGbufferMrt ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_UNKNOWN;
        psoDesc.RTVFormats[5] = usesExtendedGbufferMrt ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_UNKNOWN;
        psoDesc.RTVFormats[6] = usesExtendedGbufferMrt ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_UNKNOWN;
        for (UINT targetIndex = 0; targetIndex < static_cast<UINT>(mrtCount); ++targetIndex)
        {
            psoDesc.BlendState.RenderTarget[targetIndex].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        }

        m_pipelineStateMrt = createPipeline(psoDesc);
        if (isPbr)
        {
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            m_pipelineStateMrtDoubleSided = createPipeline(psoDesc);
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        }

        if (useGeometryMsaa)
        {
            psoDesc.SampleDesc.Count = 1;
            psoDesc.RasterizerState.MultisampleEnable = FALSE;
        }

        applySingleRenderTarget(DXGI_FORMAT_R16G16B16A16_FLOAT);
        if (isGridVertex)
        {
            for (UINT targetIndex = 1; targetIndex < 4; ++targetIndex)
            {
                psoDesc.BlendState.RenderTarget[targetIndex] = {};
                psoDesc.BlendState.RenderTarget[targetIndex].RenderTargetWriteMask = 0;
            }
            setupAlphaBlend(psoDesc.BlendState.RenderTarget[0]);
        }

        m_pipelineState = createPipeline(psoDesc);
        if (isPbr)
        {
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            m_pipelineStateDoubleSided = createPipeline(psoDesc);
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        }

        applySingleRenderTarget(DXGI_FORMAT_R8G8B8A8_UNORM);
        if (isGridVertex)
        {
            for (UINT targetIndex = 1; targetIndex < 4; ++targetIndex)
            {
                psoDesc.BlendState.RenderTarget[targetIndex] = {};
                psoDesc.BlendState.RenderTarget[targetIndex].RenderTargetWriteMask = 0;
            }
            setupAlphaBlend(psoDesc.BlendState.RenderTarget[0]);
        }

        m_pipelineStateLdr = createPipeline(psoDesc);
        if (isPbr)
        {
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            m_pipelineStateLdrDoubleSided = createPipeline(psoDesc);
        }
    }
    else if (isDepthBlit || isMsaaDepthResolve)
    {
        m_pipelineState = createPipeline(psoDesc);
        m_pipelineStateLdr = nullptr;
        m_pipelineStateMrt = nullptr;
    }
    else if (isFullscreen || isIblBrdf)
    {
        m_pipelineState = createPipeline(psoDesc);

        applySingleRenderTarget(DXGI_FORMAT_R8G8B8A8_UNORM);
        if (isSelectionGlow)
        {
            setupAdditiveBlend(psoDesc.BlendState.RenderTarget[0]);
        }
        else if (isSelectionSharp)
        {
            setupAlphaBlend(psoDesc.BlendState.RenderTarget[0]);
        }
        else if (isGridComposite)
        {
            setupPremultipliedAlphaBlend(psoDesc.BlendState.RenderTarget[0]);
        }

        m_pipelineStateLdr = createPipeline(psoDesc);
        m_pipelineStateMrt = nullptr;
    }
    else
    {
        m_pipelineState = createPipeline(psoDesc);
        if (isGizmoLineVertex && isLinePixel)
        {
            applySingleRenderTarget(DXGI_FORMAT_R8G8B8A8_UNORM);
            applyNoDepthPass();
            psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            m_pipelineStateLdr = createPipeline(psoDesc);

            setupDepthReadOnly(psoDesc.DepthStencilState);
            psoDesc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            m_pipelineStateLdrDepthRead = createPipeline(psoDesc);
        }
        else
        {
            m_pipelineStateLdr = nullptr;
        }
        if (isShadowDepth)
        {
            psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            m_pipelineStateDoubleSided = createPipeline(psoDesc);
        }
        m_pipelineStateMrt = nullptr;
    }

    m_textureSlots.assign(16, 0);
    m_isLinked = true;
}

void Shader::WriteUniform(const char* name, const void* data, const std::uint32_t size) const
{
    const auto iterator = m_uniformLocations.find(name);
    if (iterator == m_uniformLocations.end())
    {
        return;
    }

    for (const UniformLocation& location : iterator->second)
    {
        if (location.size == 0 || location.bufferIndex >= m_constantBuffers.size())
        {
            continue;
        }

        ConstantBuffer& buffer =
            const_cast<std::vector<ConstantBuffer>&>(m_constantBuffers)[location.bufferIndex];
        if (location.offset + location.size > buffer.size)
        {
            continue;
        }

        const std::uint32_t copySize = std::min(size, location.size);
        std::memcpy(buffer.staging.data() + location.offset, data, copySize);
    }
}

void Shader::WriteScalarArray(
    const char* name,
    const void* values,
    const std::uint32_t elementSize,
    const int count) const
{
    if (values == nullptr || count <= 0)
    {
        return;
    }

    const auto iterator = m_uniformLocations.find(name);
    if (iterator == m_uniformLocations.end())
    {
        return;
    }

    for (const UniformLocation& location : iterator->second)
    {
        if (location.size == 0 || location.bufferIndex >= m_constantBuffers.size())
        {
            continue;
        }

        ConstantBuffer& buffer =
            const_cast<std::vector<ConstantBuffer>&>(m_constantBuffers)[location.bufferIndex];
        if (location.offset >= buffer.size)
        {
            continue;
        }

        const std::uint32_t tightSize = static_cast<std::uint32_t>(count) * elementSize;
        const std::uint32_t packedSize = static_cast<std::uint32_t>(count) * 16u;
        const std::uint32_t writeSize = std::min(location.size, buffer.size - location.offset);

        if (count > 1 && elementSize < 16u)
        {
            const std::uint32_t copySize = std::min(packedSize, writeSize);
            std::vector<std::uint8_t> packed(copySize, 0);
            const int elementsToPack = static_cast<int>(
                std::min(static_cast<std::uint32_t>(count), copySize / 16u));
            for (int index = 0; index < elementsToPack; ++index)
            {
                std::memcpy(
                    packed.data() + static_cast<std::size_t>(index) * 16u,
                    static_cast<const std::uint8_t*>(values) +
                        static_cast<std::size_t>(index) * elementSize,
                    elementSize);
            }
            std::memcpy(buffer.staging.data() + location.offset, packed.data(), copySize);
        }
        else
        {
            const std::uint32_t copySize = std::min(tightSize, writeSize);
            std::memcpy(buffer.staging.data() + location.offset, values, copySize);
        }
    }
}

void Shader::BindPipeline(
    const bool mrtPass,
    const bool viewportLdr,
    const bool doubleSided,
    const bool depthReadOnly) const
{
    g_activeShader = this;
    auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(GfxContext::Get().GetCommandList());
    void* pipeline = m_pipelineState;
    if (doubleSided)
    {
        if (mrtPass && m_pipelineStateMrtDoubleSided != nullptr)
        {
            pipeline = m_pipelineStateMrtDoubleSided;
        }
        else if (viewportLdr && m_pipelineStateLdrDoubleSided != nullptr)
        {
            pipeline = m_pipelineStateLdrDoubleSided;
        }
        else if (m_pipelineStateDoubleSided != nullptr)
        {
            pipeline = m_pipelineStateDoubleSided;
        }
    }
    else if (mrtPass && m_pipelineStateMrt != nullptr)
    {
        pipeline = m_pipelineStateMrt;
    }
    else if (viewportLdr && depthReadOnly && m_pipelineStateLdrDepthRead != nullptr)
    {
        pipeline = m_pipelineStateLdrDepthRead;
    }
    else if (viewportLdr && m_pipelineStateLdr != nullptr)
    {
        pipeline = m_pipelineStateLdr;
    }
    d3dCommandList->SetPipelineState(static_cast<ID3D12PipelineState*>(pipeline));
    d3dCommandList->SetGraphicsRootSignature(static_cast<ID3D12RootSignature*>(m_rootSignature));
}

void Shader::Use(
    const bool mrtPass,
    const bool viewportLdr,
    const bool doubleSided,
    const bool depthReadOnly) const
{
    std::fill(
        const_cast<std::vector<std::uintptr_t>&>(m_textureSlots).begin(),
        const_cast<std::vector<std::uintptr_t>&>(m_textureSlots).end(),
        0);
    BindPipeline(mrtPass, viewportLdr, doubleSided, depthReadOnly);
}

void Shader::UseOnCommandList(void* commandList) const
{
    auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(commandList);
    d3dCommandList->SetPipelineState(static_cast<ID3D12PipelineState*>(m_pipelineState));
    d3dCommandList->SetGraphicsRootSignature(static_cast<ID3D12RootSignature*>(m_rootSignature));
}

void Shader::FlushUniforms() const
{
    FlushUniformsOnCommandList(GfxContext::Get().GetCommandList());
}

void Shader::FlushUniformsOnCommandList(void* commandList) const
{
    auto* d3dCommandList = static_cast<ID3D12GraphicsCommandList*>(commandList);
    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());

    for (std::size_t bufferIndex = 0; bufferIndex < m_constantBuffers.size(); ++bufferIndex)
    {
        const ConstantBuffer& buffer = m_constantBuffers[bufferIndex];
        if (buffer.resource == nullptr || buffer.staging.empty())
        {
            continue;
        }

        const GfxContext::TransientUploadAllocation upload =
            GfxContext::Get().AllocateTransientUpload(buffer.staging.data(), buffer.size);
        if (upload.gpuAddress == 0)
        {
            continue;
        }

        d3dCommandList->SetGraphicsRootConstantBufferView(
            static_cast<UINT>(buffer.rootParameterIndex),
            upload.gpuAddress);
    }

    const std::uint32_t drawSrvTableStart = GfxContext::Get().AllocateDrawSrvTable();
    if (drawSrvTableStart == UINT32_MAX)
    {
        return;
    }

    auto* srvHeap = static_cast<ID3D12DescriptorHeap*>(GfxContext::Get().GetSrvHeap());
    const std::uint32_t descriptorSize = GfxContext::Get().GetSrvDescriptorSize();
    D3D12_CPU_DESCRIPTOR_HANDLE cpuBase = srvHeap->GetCPUDescriptorHandleForHeapStart();
    cpuBase.ptr += static_cast<SIZE_T>(drawSrvTableStart) * descriptorSize;

    for (std::uint32_t unit = 0; unit < m_textureSlots.size(); ++unit)
    {
        if (m_textureSlots[unit] == 0)
        {
            continue;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE src{};
        src.ptr = static_cast<SIZE_T>(m_textureSlots[unit]);
        D3D12_CPU_DESCRIPTOR_HANDLE dst = cpuBase;
        dst.ptr += static_cast<SIZE_T>(unit) * descriptorSize;
        device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    ID3D12DescriptorHeap* heaps[] = {srvHeap};
    d3dCommandList->SetDescriptorHeaps(1, heaps);

    D3D12_GPU_DESCRIPTOR_HANDLE gpuTable = srvHeap->GetGPUDescriptorHandleForHeapStart();
    gpuTable.ptr += static_cast<UINT64>(drawSrvTableStart) * descriptorSize;
    d3dCommandList->SetGraphicsRootDescriptorTable(2, gpuTable);
}

void Shader::BindTextureSlot(const int unit, const std::uintptr_t srvCpuHandle) const
{
    if (unit < 0 || static_cast<std::size_t>(unit) >= m_textureSlots.size())
    {
        return;
    }

    const_cast<std::vector<std::uintptr_t>&>(m_textureSlots)[static_cast<std::size_t>(unit)] = srvCpuHandle;
}

void Shader::SetFloat(const char* name, const float value) const
{
    WriteUniform(name, &value, sizeof(float));
}

void Shader::SetInt(const char* name, const int value) const
{
    WriteUniform(name, &value, sizeof(int));
}

void Shader::SetIntArray(const char* name, const int* values, const int count) const
{
    WriteScalarArray(name, values, sizeof(int), count);
}

void Shader::SetFloatArray(const char* name, const float* values, const int count) const
{
    WriteScalarArray(name, values, sizeof(float), count);
}

void Shader::SetMat4(const char* name, const glm::mat4& value) const
{
    WriteUniform(name, glm::value_ptr(value), sizeof(glm::mat4));
}

void Shader::SetMat4Array(const char* name, const glm::mat4* values, const int count) const
{
    WriteUniform(name, values, static_cast<std::uint32_t>(count) * sizeof(glm::mat4));
}

void Shader::SetVec2(const char* name, const glm::vec2& value) const
{
    WriteUniform(name, &value, sizeof(glm::vec2));
}

void Shader::SetVec3(const char* name, const glm::vec3& value) const
{
    WriteUniform(name, &value, sizeof(glm::vec3));
}

void Shader::SetVec3Array(const char* name, const glm::vec3* values, const int count) const
{
    WriteScalarArray(name, values, sizeof(glm::vec3), count);
}

void Shader::SetVec4Array(const char* name, const glm::vec4* values, const int count) const
{
    WriteUniform(name, values, static_cast<std::uint32_t>(count) * sizeof(glm::vec4));
}

unsigned int Shader::GetProgramId() const
{
    return static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(m_pipelineState));
}

bool Shader::IsLinked() const
{
    return m_isLinked;
}

bool Shader::HasUniform(const char* name) const
{
    return name != nullptr && m_uniformLocations.find(name) != m_uniformLocations.end();
}

ID3D12RootSignature* Shader::GetRootSignature() const
{
    return static_cast<ID3D12RootSignature*>(m_rootSignature);
}

ID3D12PipelineState* Shader::GetPipelineState() const
{
    return static_cast<ID3D12PipelineState*>(m_pipelineState);
}

const Shader* Shader::GetActiveShader()
{
    return g_activeShader;
}
