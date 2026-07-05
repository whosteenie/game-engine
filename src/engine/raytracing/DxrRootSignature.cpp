#include "engine/raytracing/DxrRootSignature.h"

#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"

#include <d3d12.h>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace
{
    ID3D12RootSignature* g_smokeRootSignature = nullptr;
    ID3D12RootSignature* g_smokeLocalRootSignature = nullptr;
    ID3D12RootSignature* g_primaryDebugRootSignature = nullptr;
    ID3D12RootSignature* g_primaryDebugLocalRootSignature = nullptr;
    ID3D12RootSignature* g_reflectionRootSignature = nullptr;
    ID3D12RootSignature* g_reflectionLocalRootSignature = nullptr;

    void SerializeSmokeGlobalRootSignatureBlob(ComPtr<ID3DBlob>& outBlob)
    {
        D3D12_DESCRIPTOR_RANGE1 srvRange{};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.RegisterSpace = 0;
        srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        srvRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

        D3D12_DESCRIPTOR_RANGE1 uavRange{};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 1;
        uavRange.BaseShaderRegister = 0;
        uavRange.RegisterSpace = 0;
        uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

        D3D12_ROOT_PARAMETER1 rootParams[3]{};
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1].DescriptorTable.pDescriptorRanges = &srvRange;
        rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[2].DescriptorTable.pDescriptorRanges = &uavRange;
        rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
        rootDesc.NumParameters = 3;
        rootDesc.pParameters = rootParams;
        rootDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
        versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versionedDesc.Desc_1_1 = rootDesc;

        ComPtr<ID3DBlob> signatureError;
        ThrowIfFailed(
            D3D12SerializeVersionedRootSignature(&versionedDesc, &outBlob, &signatureError),
            "D3D12SerializeVersionedRootSignature failed for DXR smoke");
    }

    void SerializeSmokeLocalRootSignatureBlob(ComPtr<ID3DBlob>& outBlob)
    {
        D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
        versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versionedDesc.Desc_1_1 = rootDesc;

        ComPtr<ID3DBlob> signatureError;
        ThrowIfFailed(
            D3D12SerializeVersionedRootSignature(&versionedDesc, &outBlob, &signatureError),
            "D3D12SerializeVersionedRootSignature failed for DXR smoke local root signature");
    }
}

void DxrRootSignature::SerializeSmokeGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    SerializeSmokeGlobalRootSignatureBlob(outBlob);
}

void DxrRootSignature::SerializeSmokeLocalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    SerializeSmokeLocalRootSignatureBlob(outBlob);
}

ID3D12RootSignature* DxrRootSignature::CreateSmokeGlobalRootSignature()
{
    if (g_smokeRootSignature != nullptr)
    {
        g_smokeRootSignature->AddRef();
        return g_smokeRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateSmokeGlobalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeSmokeGlobalRootSignatureBlob(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR smoke");

    g_smokeRootSignature = rootSignature.Detach();
    g_smokeRootSignature->AddRef();
    return g_smokeRootSignature;
}

ID3D12RootSignature* DxrRootSignature::CreateSmokeLocalRootSignature()
{
    if (g_smokeLocalRootSignature != nullptr)
    {
        g_smokeLocalRootSignature->AddRef();
        return g_smokeLocalRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateSmokeLocalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeSmokeLocalRootSignatureBlob(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR smoke local root signature");

    g_smokeLocalRootSignature = rootSignature.Detach();
    g_smokeLocalRootSignature->AddRef();
    return g_smokeLocalRootSignature;
}

void DxrRootSignature::ReleaseSmokeGlobalRootSignature()
{
    if (g_smokeRootSignature != nullptr)
    {
        g_smokeRootSignature->Release();
        g_smokeRootSignature = nullptr;
    }
}

void DxrRootSignature::ReleaseSmokeLocalRootSignature()
{
    if (g_smokeLocalRootSignature != nullptr)
    {
        g_smokeLocalRootSignature->Release();
        g_smokeLocalRootSignature = nullptr;
    }
}

void DxrRootSignature::SerializePrimaryDebugGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    D3D12_DESCRIPTOR_RANGE1 srvRanges[5]{};
    for (std::uint32_t registerIndex = 0; registerIndex < 5; ++registerIndex)
    {
        srvRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[registerIndex].NumDescriptors = 1;
        srvRanges[registerIndex].BaseShaderRegister = registerIndex;
        srvRanges[registerIndex].RegisterSpace = 0;
        srvRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        srvRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    }

    D3D12_DESCRIPTOR_RANGE1 uavRanges[2]{};
    for (std::uint32_t registerIndex = 0; registerIndex < 2; ++registerIndex)
    {
        uavRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[registerIndex].NumDescriptors = 1;
        uavRanges[registerIndex].BaseShaderRegister = registerIndex;
        uavRanges[registerIndex].RegisterSpace = 0;
        uavRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uavRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    }

    D3D12_ROOT_PARAMETER1 rootParams[8]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    for (std::uint32_t srvIndex = 0; srvIndex < 5; ++srvIndex)
    {
        rootParams[1 + srvIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1 + srvIndex].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1 + srvIndex].DescriptorTable.pDescriptorRanges = &srvRanges[srvIndex];
        rootParams[1 + srvIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    for (std::uint32_t uavIndex = 0; uavIndex < 2; ++uavIndex)
    {
        rootParams[6 + uavIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[6 + uavIndex].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[6 + uavIndex].DescriptorTable.pDescriptorRanges = &uavRanges[uavIndex];
        rootParams[6 + uavIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.NumParameters = 8;
    rootDesc.pParameters = rootParams;
    rootDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1 = rootDesc;

    ComPtr<ID3DBlob> signatureError;
    ThrowIfFailed(
        D3D12SerializeVersionedRootSignature(&versionedDesc, &outBlob, &signatureError),
        "D3D12SerializeVersionedRootSignature failed for DXR primary debug");
}

void DxrRootSignature::SerializePrimaryDebugLocalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    SerializeSmokeLocalRootSignatureBlob(outBlob);
}

ID3D12RootSignature* DxrRootSignature::CreatePrimaryDebugGlobalRootSignature()
{
    if (g_primaryDebugRootSignature != nullptr)
    {
        g_primaryDebugRootSignature->AddRef();
        return g_primaryDebugRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreatePrimaryDebugGlobalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializePrimaryDebugGlobalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR primary debug");

    g_primaryDebugRootSignature = rootSignature.Detach();
    g_primaryDebugRootSignature->AddRef();
    return g_primaryDebugRootSignature;
}

ID3D12RootSignature* DxrRootSignature::CreatePrimaryDebugLocalRootSignature()
{
    if (g_primaryDebugLocalRootSignature != nullptr)
    {
        g_primaryDebugLocalRootSignature->AddRef();
        return g_primaryDebugLocalRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreatePrimaryDebugLocalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializePrimaryDebugLocalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR primary debug local root signature");

    g_primaryDebugLocalRootSignature = rootSignature.Detach();
    g_primaryDebugLocalRootSignature->AddRef();
    return g_primaryDebugLocalRootSignature;
}

// Phase D4 reflections: CBV b0, SRV tables t0-t10, UAV table u0, static linear-clamp s0.
// See devdoc/dxr-reflections.md for the binding table.
void DxrRootSignature::SerializeReflectionGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    constexpr std::uint32_t kSrvCount = 11;

    D3D12_DESCRIPTOR_RANGE1 srvRanges[kSrvCount]{};
    for (std::uint32_t registerIndex = 0; registerIndex < kSrvCount; ++registerIndex)
    {
        srvRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[registerIndex].NumDescriptors = 1;
        srvRanges[registerIndex].BaseShaderRegister = registerIndex;
        srvRanges[registerIndex].RegisterSpace = 0;
        srvRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        srvRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    }

    D3D12_DESCRIPTOR_RANGE1 uavRange{};
    uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    uavRange.NumDescriptors = 1;
    uavRange.BaseShaderRegister = 0;
    uavRange.RegisterSpace = 0;
    uavRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    uavRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;

    D3D12_ROOT_PARAMETER1 rootParams[1 + kSrvCount + 1]{};
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    for (std::uint32_t srvIndex = 0; srvIndex < kSrvCount; ++srvIndex)
    {
        rootParams[1 + srvIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1 + srvIndex].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1 + srvIndex].DescriptorTable.pDescriptorRanges = &srvRanges[srvIndex];
        rootParams[1 + srvIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    rootParams[1 + kSrvCount].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[1 + kSrvCount].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[1 + kSrvCount].DescriptorTable.pDescriptorRanges = &uavRange;
    rootParams[1 + kSrvCount].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC linearClampSampler{};
    linearClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClampSampler.MaxAnisotropy = 1;
    linearClampSampler.MaxLOD = D3D12_FLOAT32_MAX;
    linearClampSampler.ShaderRegister = 0;
    linearClampSampler.RegisterSpace = 0;
    linearClampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.NumParameters = 1 + kSrvCount + 1;
    rootDesc.pParameters = rootParams;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &linearClampSampler;
    rootDesc.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1 = rootDesc;

    ComPtr<ID3DBlob> signatureError;
    ThrowIfFailed(
        D3D12SerializeVersionedRootSignature(&versionedDesc, &outBlob, &signatureError),
        "D3D12SerializeVersionedRootSignature failed for DXR reflections");
}

ID3D12RootSignature* DxrRootSignature::CreateReflectionGlobalRootSignature()
{
    if (g_reflectionRootSignature != nullptr)
    {
        g_reflectionRootSignature->AddRef();
        return g_reflectionRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateReflectionGlobalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeReflectionGlobalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR reflections");

    g_reflectionRootSignature = rootSignature.Detach();
    g_reflectionRootSignature->AddRef();
    return g_reflectionRootSignature;
}

ID3D12RootSignature* DxrRootSignature::CreateReflectionLocalRootSignature()
{
    if (g_reflectionLocalRootSignature != nullptr)
    {
        g_reflectionLocalRootSignature->AddRef();
        return g_reflectionLocalRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateReflectionLocalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeSmokeLocalRootSignatureBlob(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR reflections local root signature");

    g_reflectionLocalRootSignature = rootSignature.Detach();
    g_reflectionLocalRootSignature->AddRef();
    return g_reflectionLocalRootSignature;
}

void DxrRootSignature::ReleaseReflectionGlobalRootSignature()
{
    if (g_reflectionRootSignature != nullptr)
    {
        g_reflectionRootSignature->Release();
        g_reflectionRootSignature = nullptr;
    }
}

void DxrRootSignature::ReleaseReflectionLocalRootSignature()
{
    if (g_reflectionLocalRootSignature != nullptr)
    {
        g_reflectionLocalRootSignature->Release();
        g_reflectionLocalRootSignature = nullptr;
    }
}

void DxrRootSignature::ReleasePrimaryDebugGlobalRootSignature()
{
    if (g_primaryDebugRootSignature != nullptr)
    {
        g_primaryDebugRootSignature->Release();
        g_primaryDebugRootSignature = nullptr;
    }
}

void DxrRootSignature::ReleasePrimaryDebugLocalRootSignature()
{
    if (g_primaryDebugLocalRootSignature != nullptr)
    {
        g_primaryDebugLocalRootSignature->Release();
        g_primaryDebugLocalRootSignature = nullptr;
    }
}

