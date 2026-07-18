#include "engine/raytracing/DxrRootSignature.h"

#include "engine/rhi/GfxContext.h"
#include "engine/rhi/d3d12/D3D12Throw.h"

#include <d3d12.h>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    ID3D12RootSignature* g_smokeRootSignature = nullptr;
    ID3D12RootSignature* g_smokeLocalRootSignature = nullptr;
    ID3D12RootSignature* g_primaryDebugRootSignature = nullptr;
    ID3D12RootSignature* g_primaryDebugLocalRootSignature = nullptr;
    ID3D12RootSignature* g_reflectionRootSignature = nullptr;
    ID3D12RootSignature* g_reflectionLocalRootSignature = nullptr;
    ID3D12RootSignature* g_pathTracerRootSignature = nullptr;
    ID3D12RootSignature* g_shadowRootSignature = nullptr;
    ID3D12RootSignature* g_shadowLocalRootSignature = nullptr;
    ID3D12RootSignature* g_restirRootSignature = nullptr;
    ID3D12RootSignature* g_restirLocalRootSignature = nullptr;

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

namespace
{
    // Shared hit-shading global root signature layout: CBV b0, per-register SRV tables
    // t0..t(srvCount-1), per-register UAV tables u0..u(uavCount-1), bindless SRV heap (space1),
    // static linear-clamp s0 + linear-wrap s1. Used by reflections/GI (t0-t13, u0-u3) and the
    // path tracer (t0-t14 adds prev-instance transforms, u0-u6 adds RR guide outputs — P4b).
    void SerializeHitShadingGlobalRootSignatureBlob(
        const std::uint32_t srvCount,
        const std::uint32_t uavCount,
        const char* errorContext,
        ComPtr<ID3DBlob>& outBlob)
    {
        std::vector<D3D12_DESCRIPTOR_RANGE1> srvRanges(srvCount);
        for (std::uint32_t registerIndex = 0; registerIndex < srvCount; ++registerIndex)
        {
            srvRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRanges[registerIndex].NumDescriptors = 1;
            srvRanges[registerIndex].BaseShaderRegister = registerIndex;
            srvRanges[registerIndex].RegisterSpace = 0;
            srvRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            srvRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        }

        std::vector<D3D12_DESCRIPTOR_RANGE1> uavRanges(uavCount);
        for (std::uint32_t registerIndex = 0; registerIndex < uavCount; ++registerIndex)
        {
            uavRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            uavRanges[registerIndex].NumDescriptors = 1;
            uavRanges[registerIndex].BaseShaderRegister = registerIndex;
            uavRanges[registerIndex].RegisterSpace = 0;
            uavRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            uavRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
        }

        // Bindless: unbounded SRV array over the whole shader-visible heap (space1), for per-object
        // albedo texture sampling in the closest-hit. g_BindlessTextures[] in hit_shading.hlsli.
        D3D12_DESCRIPTOR_RANGE1 bindlessRange{};
        bindlessRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        bindlessRange.NumDescriptors = UINT_MAX; // unbounded
        bindlessRange.BaseShaderRegister = 0;
        bindlessRange.RegisterSpace = 1;
        bindlessRange.OffsetInDescriptorsFromTableStart = 0;
        bindlessRange.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;

        const std::uint32_t paramCount = 1 + srvCount + uavCount + 1;
        const std::uint32_t bindlessParam = 1 + srvCount + uavCount;
        std::vector<D3D12_ROOT_PARAMETER1> rootParams(paramCount);
        rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[0].Descriptor.ShaderRegister = 0;
        rootParams[0].Descriptor.RegisterSpace = 0;
        rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        for (std::uint32_t srvIndex = 0; srvIndex < srvCount; ++srvIndex)
        {
            rootParams[1 + srvIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParams[1 + srvIndex].DescriptorTable.NumDescriptorRanges = 1;
            rootParams[1 + srvIndex].DescriptorTable.pDescriptorRanges = &srvRanges[srvIndex];
            rootParams[1 + srvIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        for (std::uint32_t uavIndex = 0; uavIndex < uavCount; ++uavIndex)
        {
            rootParams[1 + srvCount + uavIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            rootParams[1 + srvCount + uavIndex].DescriptorTable.NumDescriptorRanges = 1;
            rootParams[1 + srvCount + uavIndex].DescriptorTable.pDescriptorRanges = &uavRanges[uavIndex];
            rootParams[1 + srvCount + uavIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }

        rootParams[bindlessParam].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[bindlessParam].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[bindlessParam].DescriptorTable.pDescriptorRanges = &bindlessRange;
        rootParams[bindlessParam].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC staticSamplers[2]{};
        D3D12_STATIC_SAMPLER_DESC& linearClampSampler = staticSamplers[0];
        linearClampSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linearClampSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linearClampSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linearClampSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linearClampSampler.MaxAnisotropy = 1;
        linearClampSampler.MaxLOD = D3D12_FLOAT32_MAX;
        linearClampSampler.ShaderRegister = 0;
        linearClampSampler.RegisterSpace = 0;
        linearClampSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC& linearWrapSampler = staticSamplers[1];
        linearWrapSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        linearWrapSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        linearWrapSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        linearWrapSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        linearWrapSampler.MaxAnisotropy = 1;
        linearWrapSampler.MaxLOD = D3D12_FLOAT32_MAX;
        linearWrapSampler.ShaderRegister = 1;
        linearWrapSampler.RegisterSpace = 0;
        linearWrapSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
        rootDesc.NumParameters = paramCount;
        rootDesc.pParameters = rootParams.data();
        rootDesc.NumStaticSamplers = 2;
        rootDesc.pStaticSamplers = staticSamplers;
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
            errorContext);
    }
}

// Phase D4/D5 reflections: CBV b0, SRV tables t0-t13, UAV tables u0-u3 (radiance+hitDist,
// viewZ, normal+roughness, motion — NRD guide outputs), static linear-clamp s0.
// See devdoc/dxr/reflections.md for the binding table.
void DxrRootSignature::SerializeReflectionGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    SerializeHitShadingGlobalRootSignatureBlob(
        14, 4, "D3D12SerializeVersionedRootSignature failed for DXR reflections", outBlob);
}

// P4b path tracer: reflection layout plus t14 (prev-instance transforms for object motion),
// t15 (emissive NEE light list), t18-t21 (geometry, aliases, instance lookup), u4-u6 (RR guides),
// u7-u8 (ReSTIR GI + DI reservoirs), u9 direct, u10-u12 P1 surface records, and u13-u18 the
// independent smooth-dielectric transmission RR layer and its guide bundle.
void DxrRootSignature::SerializePathTracerGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    SerializeHitShadingGlobalRootSignatureBlob(
        22, 19, "D3D12SerializeVersionedRootSignature failed for DXR path tracer", outBlob);
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

ID3D12RootSignature* DxrRootSignature::CreatePathTracerGlobalRootSignature()
{
    if (g_pathTracerRootSignature != nullptr)
    {
        g_pathTracerRootSignature->AddRef();
        return g_pathTracerRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreatePathTracerGlobalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializePathTracerGlobalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR path tracer");

    g_pathTracerRootSignature = rootSignature.Detach();
    g_pathTracerRootSignature->AddRef();
    return g_pathTracerRootSignature;
}

void DxrRootSignature::ReleasePathTracerGlobalRootSignature()
{
    if (g_pathTracerRootSignature != nullptr)
    {
        g_pathTracerRootSignature->Release();
        g_pathTracerRootSignature = nullptr;
    }
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

// Phase D8 shadows: CBV b0, SRV tables t0-t4 (TLAS, depth, normal, material0, velocity),
// UAV tables u0-u3 (penumbra, viewZ, normal+roughness, motion — SIGMA guide outputs). No
// bindless or static samplers: the shadow ray does occlusion only (no hit shading).
// See devdoc/dxr/shadows.md.
void DxrRootSignature::SerializeShadowGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    constexpr std::uint32_t kSrvCount = 5;
    constexpr std::uint32_t kUavCount = 4;

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

    D3D12_DESCRIPTOR_RANGE1 uavRanges[kUavCount]{};
    for (std::uint32_t registerIndex = 0; registerIndex < kUavCount; ++registerIndex)
    {
        uavRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[registerIndex].NumDescriptors = 1;
        uavRanges[registerIndex].BaseShaderRegister = registerIndex;
        uavRanges[registerIndex].RegisterSpace = 0;
        uavRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uavRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    }

    constexpr std::uint32_t kParamCount = 1 + kSrvCount + kUavCount;
    D3D12_ROOT_PARAMETER1 rootParams[kParamCount]{};
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

    for (std::uint32_t uavIndex = 0; uavIndex < kUavCount; ++uavIndex)
    {
        rootParams[1 + kSrvCount + uavIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1 + kSrvCount + uavIndex].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1 + kSrvCount + uavIndex].DescriptorTable.pDescriptorRanges = &uavRanges[uavIndex];
        rootParams[1 + kSrvCount + uavIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.NumParameters = kParamCount;
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
        "D3D12SerializeVersionedRootSignature failed for DXR shadows");
}

ID3D12RootSignature* DxrRootSignature::CreateShadowGlobalRootSignature()
{
    if (g_shadowRootSignature != nullptr)
    {
        g_shadowRootSignature->AddRef();
        return g_shadowRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateShadowGlobalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeShadowGlobalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR shadows");

    g_shadowRootSignature = rootSignature.Detach();
    g_shadowRootSignature->AddRef();
    return g_shadowRootSignature;
}

ID3D12RootSignature* DxrRootSignature::CreateShadowLocalRootSignature()
{
    if (g_shadowLocalRootSignature != nullptr)
    {
        g_shadowLocalRootSignature->AddRef();
        return g_shadowLocalRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateShadowLocalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeSmokeLocalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR shadows local root signature");

    g_shadowLocalRootSignature = rootSignature.Detach();
    g_shadowLocalRootSignature->AddRef();
    return g_shadowLocalRootSignature;
}

void DxrRootSignature::ReleaseShadowGlobalRootSignature()
{
    if (g_shadowRootSignature != nullptr)
    {
        g_shadowRootSignature->Release();
        g_shadowRootSignature = nullptr;
    }
}

void DxrRootSignature::ReleaseShadowLocalRootSignature()
{
    if (g_shadowLocalRootSignature != nullptr)
    {
        g_shadowLocalRootSignature->Release();
        g_shadowLocalRootSignature = nullptr;
    }
}

void DxrRootSignature::SerializeRestirGlobalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    // t0-t12: TLAS, P1 history/current, motion/base, current/previous material params,
    // lights, and environment CDF/map.
    constexpr std::uint32_t kSrvCount = 13;
    constexpr std::uint32_t kUavCount = 5;
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

    D3D12_DESCRIPTOR_RANGE1 uavRanges[kUavCount]{};
    for (std::uint32_t registerIndex = 0; registerIndex < kUavCount; ++registerIndex)
    {
        uavRanges[registerIndex].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRanges[registerIndex].NumDescriptors = 1;
        uavRanges[registerIndex].BaseShaderRegister = registerIndex;
        uavRanges[registerIndex].RegisterSpace = 0;
        uavRanges[registerIndex].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        uavRanges[registerIndex].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    }

    D3D12_ROOT_PARAMETER1 rootParams[1 + kSrvCount + kUavCount]{};
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

    for (std::uint32_t uavIndex = 0; uavIndex < kUavCount; ++uavIndex)
    {
        rootParams[1 + kSrvCount + uavIndex].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[1 + kSrvCount + uavIndex].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[1 + kSrvCount + uavIndex].DescriptorTable.pDescriptorRanges = &uavRanges[uavIndex];
        rootParams[1 + kSrvCount + uavIndex].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }

    D3D12_STATIC_SAMPLER_DESC linearClamp{};
    linearClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    linearClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    linearClamp.MaxAnisotropy = 1;
    linearClamp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    linearClamp.MinLOD = 0.0f;
    linearClamp.MaxLOD = D3D12_FLOAT32_MAX;
    linearClamp.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.NumParameters = 1 + kSrvCount + kUavCount;
    rootDesc.pParameters = rootParams;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &linearClamp;
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
        "D3D12SerializeVersionedRootSignature failed for DXR ReSTIR");
}

void DxrRootSignature::SerializeRestirLocalRootSignature(ComPtr<ID3DBlob>& outBlob)
{
    D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc{};
    versionedDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    versionedDesc.Desc_1_1 = rootDesc;

    ComPtr<ID3DBlob> signatureError;
    ThrowIfFailed(
        D3D12SerializeVersionedRootSignature(&versionedDesc, &outBlob, &signatureError),
        "D3D12SerializeVersionedRootSignature failed for DXR ReSTIR local root signature");
}

ID3D12RootSignature* DxrRootSignature::CreateRestirGlobalRootSignature()
{
    if (g_restirRootSignature != nullptr)
    {
        g_restirRootSignature->AddRef();
        return g_restirRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateRestirGlobalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeRestirGlobalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR ReSTIR");

    g_restirRootSignature = rootSignature.Detach();
    g_restirRootSignature->AddRef();
    return g_restirRootSignature;
}

ID3D12RootSignature* DxrRootSignature::CreateRestirLocalRootSignature()
{
    if (g_restirLocalRootSignature != nullptr)
    {
        g_restirLocalRootSignature->AddRef();
        return g_restirLocalRootSignature;
    }

    auto* device = static_cast<ID3D12Device*>(GfxContext::Get().GetDevice());
    if (device == nullptr)
    {
        throw std::runtime_error("CreateRestirLocalRootSignature: GfxContext not initialized");
    }

    ComPtr<ID3DBlob> signatureBlob;
    SerializeRestirLocalRootSignature(signatureBlob);

    ComPtr<ID3D12RootSignature> rootSignature;
    ThrowIfFailed(
        device->CreateRootSignature(
            0,
            signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)),
        "CreateRootSignature failed for DXR ReSTIR local root signature");

    g_restirLocalRootSignature = rootSignature.Detach();
    g_restirLocalRootSignature->AddRef();
    return g_restirLocalRootSignature;
}

void DxrRootSignature::ReleaseRestirGlobalRootSignature()
{
    if (g_restirRootSignature != nullptr)
    {
        g_restirRootSignature->Release();
        g_restirRootSignature = nullptr;
    }
}

void DxrRootSignature::ReleaseRestirLocalRootSignature()
{
    if (g_restirLocalRootSignature != nullptr)
    {
        g_restirLocalRootSignature->Release();
        g_restirLocalRootSignature = nullptr;
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

