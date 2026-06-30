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
