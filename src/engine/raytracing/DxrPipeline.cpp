#include "engine/raytracing/DxrPipeline.h"

#include "engine/platform/EngineLog.h"
#include "engine/raytracing/DxrContext.h"
#include "engine/raytracing/DxrRootSignature.h"
#include "engine/raytracing/DxrShaderCache.h"
#include "engine/raytracing/DxrTrace.h"
#include "engine/rendering/Constants.h"
#include "engine/rhi/GfxContext.h"

#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace
{
    std::string WideToUtf8(const wchar_t* wideText)
    {
        if (wideText == nullptr || wideText[0] == L'\0')
        {
            return {};
        }

        const int narrowLength = WideCharToMultiByte(CP_UTF8, 0, wideText, -1, nullptr, 0, nullptr, nullptr);
        if (narrowLength <= 0)
        {
            return {};
        }

        std::string narrow(static_cast<std::size_t>(narrowLength - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wideText, -1, narrow.data(), narrowLength, nullptr, nullptr);
        return narrow;
    }

    const char* SubobjectTypeName(const D3D12_STATE_SUBOBJECT_TYPE type)
    {
        switch (type)
        {
        case D3D12_STATE_SUBOBJECT_TYPE_STATE_OBJECT_CONFIG:
            return "STATE_OBJECT_CONFIG";
        case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            return "GLOBAL_ROOT_SIGNATURE";
        case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            return "LOCAL_ROOT_SIGNATURE";
        case D3D12_STATE_SUBOBJECT_TYPE_NODE_MASK:
            return "NODE_MASK";
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
            return "DXIL_LIBRARY";
        case D3D12_STATE_SUBOBJECT_TYPE_EXISTING_COLLECTION:
            return "EXISTING_COLLECTION";
        case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
            return "SUBOBJECT_TO_EXPORTS_ASSOCIATION";
        case D3D12_STATE_SUBOBJECT_TYPE_DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
            return "DXIL_SUBOBJECT_TO_EXPORTS_ASSOCIATION";
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
            return "RAYTRACING_SHADER_CONFIG";
        case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
            return "RAYTRACING_PIPELINE_CONFIG";
        case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
            return "HIT_GROUP";
        default:
            return "UNKNOWN";
        }
    }

    std::size_t FindSubobjectIndex(
        const std::vector<D3D12_STATE_SUBOBJECT>& subobjects,
        const D3D12_STATE_SUBOBJECT* target)
    {
        for (std::size_t index = 0; index < subobjects.size(); ++index)
        {
            if (&subobjects[index] == target)
            {
                return index;
            }
        }

        return static_cast<std::size_t>(-1);
    }

    struct SmokeRtpsoSubobjects
    {
        D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{};
        D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{};
        D3D12_DXIL_LIBRARY_DESC dxilLibraryDesc{};
        D3D12_HIT_GROUP_DESC hitGroupDesc{};
        D3D12_GLOBAL_ROOT_SIGNATURE globalRootSignatureDesc{};
        D3D12_LOCAL_ROOT_SIGNATURE localRootSignatureDesc{};
        std::vector<D3D12_EXPORT_DESC> exports;
        std::vector<LPCWSTR> exportNames;
        std::vector<LPCWSTR> allRtpsoExportNames;
        std::vector<LPCWSTR> hitGroupExportNames;
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION shaderConfigAssociation{};
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION pipelineConfigAssociation{};
        D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION localRootSignatureAssociation{};
        std::vector<D3D12_STATE_SUBOBJECT> subobjects;
    };

    void AppendSubobject(
        std::vector<D3D12_STATE_SUBOBJECT>& subobjects,
        const D3D12_STATE_SUBOBJECT_TYPE type,
        const void* description)
    {
        D3D12_STATE_SUBOBJECT subobject{};
        subobject.Type = type;
        subobject.pDesc = description;
        subobjects.push_back(subobject);
    }

    void LogDxilBytecodeSummary(const D3D12_SHADER_BYTECODE& dxilBytecode)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(dxilBytecode.pShaderBytecode);
        const std::size_t byteCount = static_cast<std::size_t>(dxilBytecode.BytecodeLength);
        if (bytes == nullptr || byteCount < 4)
        {
            DxrBreadcrumb("pipeline dxil summary: bytecode missing or too small");
            return;
        }

        char header[16];
        std::snprintf(
            header,
            sizeof(header),
            "%02X%02X%02X%02X",
            bytes[0],
            bytes[1],
            bytes[2],
            bytes[3]);

        DxrBreadcrumb(
            std::string("pipeline dxil summary: bytes=") + std::to_string(byteCount) + " header=0x"
            + header);
    }

    void LogCreateStateObjectEnvironment(ID3D12Device5* device5, const D3D12_SHADER_BYTECODE& dxilBytecode)
    {
        char deviceText[32];
        std::snprintf(deviceText, sizeof(deviceText), "%p", static_cast<void*>(device5));

        const bool dxilDllPresent = std::filesystem::exists("dxil.dll");
        const bool dxcompilerDllPresent = std::filesystem::exists("dxcompiler.dll");

        DxrBreadcrumb(
            std::string("pipeline env: device5=") + deviceText + " tier="
            + std::to_string(GfxContext::Get().GetRaytracingTier()) + " dxilBytes="
            + std::to_string(dxilBytecode.BytecodeLength) + " dxil.dll="
            + (dxilDllPresent ? "yes" : "NO") + " dxcompiler.dll="
            + (dxcompilerDllPresent ? "yes" : "NO"));

        EngineLog::Info(
            "dxr-pipeline",
            std::string("CreateStateObject environment: device5=") + deviceText + " tier="
                + std::to_string(GfxContext::Get().GetRaytracingTier()) + " dxilBytes="
                + std::to_string(dxilBytecode.BytecodeLength));
    }

    void LogSubobjectArray(const char* label, const SmokeRtpsoSubobjects& owner)
    {
        std::string summary = std::string("pipeline args ") + label + ": count="
            + std::to_string(owner.subobjects.size());

        for (std::size_t index = 0; index < owner.subobjects.size(); ++index)
        {
            const D3D12_STATE_SUBOBJECT& subobject = owner.subobjects[index];
            summary += " [";
            summary += std::to_string(index);
            summary += "=";
            summary += SubobjectTypeName(subobject.Type);

            switch (subobject.Type)
            {
            case D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE:
            {
                const auto* globalDesc =
                    static_cast<const D3D12_GLOBAL_ROOT_SIGNATURE*>(subobject.pDesc);
                summary += " ptr=";
                summary += (globalDesc != nullptr && globalDesc->pGlobalRootSignature != nullptr) ? "set"
                                                                                                 : "null";
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE:
            {
                const auto* localDesc = static_cast<const D3D12_LOCAL_ROOT_SIGNATURE*>(subobject.pDesc);
                summary += " ptr=";
                summary += (localDesc != nullptr && localDesc->pLocalRootSignature != nullptr) ? "set"
                                                                                               : "null";
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY:
            {
                const auto* libraryDesc = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(subobject.pDesc);
                summary += " exports=";
                summary += std::to_string(libraryDesc->NumExports);
                summary += " bytecodeBytes=";
                summary += std::to_string(libraryDesc->DXILLibrary.BytecodeLength);
                for (UINT exportIndex = 0; exportIndex < libraryDesc->NumExports; ++exportIndex)
                {
                    summary += " ";
                    summary += WideToUtf8(libraryDesc->pExports[exportIndex].Name);
                }
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG:
            {
                const auto* config = static_cast<const D3D12_RAYTRACING_SHADER_CONFIG*>(subobject.pDesc);
                summary += " payload=";
                summary += std::to_string(config->MaxPayloadSizeInBytes);
                summary += " attr=";
                summary += std::to_string(config->MaxAttributeSizeInBytes);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG:
            {
                const auto* config = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(subobject.pDesc);
                summary += " depth=";
                summary += std::to_string(config->MaxTraceRecursionDepth);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP:
            {
                const auto* hitGroup = static_cast<const D3D12_HIT_GROUP_DESC*>(subobject.pDesc);
                summary += " export=";
                summary += WideToUtf8(hitGroup->HitGroupExport);
                summary += " closestHit=";
                summary += WideToUtf8(hitGroup->ClosestHitShaderImport);
                break;
            }
            case D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION:
            {
                const auto* association =
                    static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(subobject.pDesc);
                const std::size_t targetIndex =
                    FindSubobjectIndex(owner.subobjects, association->pSubobjectToAssociate);
                summary += " targetIndex=";
                summary += targetIndex == static_cast<std::size_t>(-1) ? "INVALID" : std::to_string(targetIndex);
                summary += " exports=";
                summary += std::to_string(association->NumExports);
                for (UINT exportIndex = 0; exportIndex < association->NumExports; ++exportIndex)
                {
                    summary += " ";
                    summary += WideToUtf8(association->pExports[exportIndex]);
                }
                break;
            }
            default:
                break;
            }

            summary += "]";
        }

        DxrBreadcrumb(summary);
        EngineLog::Info("dxr-pipeline", summary);
    }

    bool BuildSmokeRtpsoSubobjects(
        SmokeRtpsoSubobjects& out,
        const D3D12_SHADER_BYTECODE& dxilBytecode,
        ID3D12RootSignature* globalRootSignature,
        ID3D12RootSignature* localRootSignature,
        std::string& outError)
    {
        outError.clear();
        out = SmokeRtpsoSubobjects{};

        if (globalRootSignature == nullptr || localRootSignature == nullptr)
        {
            outError = "DXR smoke root signatures missing";
            return false;
        }

        out.shaderConfig.MaxPayloadSizeInBytes = 16;
        out.shaderConfig.MaxAttributeSizeInBytes = 8;
        out.pipelineConfig.MaxTraceRecursionDepth = 1u;

        out.exportNames = {L"SmokeRayGen", L"SmokeMiss", L"SmokeHit"};
        out.allRtpsoExportNames = {L"SmokeRayGen", L"SmokeMiss", L"SmokeHit", L"SmokeHitGroup"};
        out.hitGroupExportNames = {L"SmokeHitGroup"};

        out.exports.resize(out.exportNames.size());
        for (std::size_t exportIndex = 0; exportIndex < out.exportNames.size(); ++exportIndex)
        {
            out.exports[exportIndex].Name = out.exportNames[exportIndex];
            out.exports[exportIndex].ExportToRename = nullptr;
            out.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
        }

        out.dxilLibraryDesc.DXILLibrary = dxilBytecode;
        out.dxilLibraryDesc.NumExports = static_cast<UINT>(out.exports.size());
        out.dxilLibraryDesc.pExports = out.exports.data();

        out.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        out.hitGroupDesc.HitGroupExport = L"SmokeHitGroup";
        out.hitGroupDesc.ClosestHitShaderImport = L"SmokeHit";

        out.subobjects.clear();
        out.subobjects.reserve(10);

        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
            &out.dxilLibraryDesc);

        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
            &out.hitGroupDesc);

        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
            &out.shaderConfig);
        const std::size_t shaderConfigIndex = out.subobjects.size() - 1;

        out.shaderConfigAssociation.pSubobjectToAssociate = &out.subobjects[shaderConfigIndex];
        out.shaderConfigAssociation.NumExports = static_cast<UINT>(out.allRtpsoExportNames.size());
        out.shaderConfigAssociation.pExports = out.allRtpsoExportNames.data();
        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
            &out.shaderConfigAssociation);

        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
            &out.pipelineConfig);
        const std::size_t pipelineConfigIndex = out.subobjects.size() - 1;

        out.pipelineConfigAssociation.pSubobjectToAssociate = &out.subobjects[pipelineConfigIndex];
        out.pipelineConfigAssociation.NumExports = static_cast<UINT>(out.allRtpsoExportNames.size());
        out.pipelineConfigAssociation.pExports = out.allRtpsoExportNames.data();
        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
            &out.pipelineConfigAssociation);

        out.globalRootSignatureDesc.pGlobalRootSignature = globalRootSignature;
        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
            &out.globalRootSignatureDesc);

        out.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
            &out.localRootSignatureDesc);
        const std::size_t localRootSignatureIndex = out.subobjects.size() - 1;

        out.localRootSignatureAssociation.pSubobjectToAssociate =
            &out.subobjects[localRootSignatureIndex];
        out.localRootSignatureAssociation.NumExports = 1;
        out.localRootSignatureAssociation.pExports = out.hitGroupExportNames.data();
        AppendSubobject(
            out.subobjects,
            D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
            &out.localRootSignatureAssociation);

        return true;
    }

    bool CreateStateObjectFromSubobjects(
        ID3D12Device5* device5,
        const SmokeRtpsoSubobjects& subobjects,
        const char* label,
        ComPtr<ID3D12StateObject>& outStateObject,
        std::string& outError)
    {
        LogSubobjectArray(label, subobjects);

        D3D12_STATE_OBJECT_DESC stateObjectDesc{};
        stateObjectDesc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
        stateObjectDesc.NumSubobjects = static_cast<UINT>(subobjects.subobjects.size());
        stateObjectDesc.pSubobjects = subobjects.subobjects.data();

        DxrBreadcrumb(std::string("pipeline CreateStateObject enter (") + label + ")");

        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(device5->QueryInterface(IID_PPV_ARGS(&infoQueue))))
        {
            infoQueue->ClearStoredMessages();
        }

        const HRESULT createHr =
            device5->CreateStateObject(&stateObjectDesc, IID_PPV_ARGS(&outStateObject));
        if (FAILED(createHr))
        {
            char hrText[16];
            std::snprintf(hrText, sizeof(hrText), "0x%08X", static_cast<unsigned>(createHr));
            outError = std::string("CreateStateObject failed (") + label + ", HRESULT=" + hrText + ")";
            EngineLog::Error("dxr-pipeline", outError);
            DxrBreadcrumb(std::string("pipeline CreateStateObject failed (") + label + ") " + hrText);
            GfxContext::Get().LogD3D12InfoQueueMessages(label);
            return false;
        }

        DxrBreadcrumb(std::string("pipeline CreateStateObject ok (") + label + ")");
        return true;
    }
}

void DxrPipeline::Release()
{
    if (m_stateObjectProperties != nullptr)
    {
        m_stateObjectProperties->Release();
        m_stateObjectProperties = nullptr;
    }

    if (m_stateObject != nullptr)
    {
        m_stateObject->Release();
        m_stateObject = nullptr;
    }

    if (m_globalRootSignature != nullptr)
    {
        m_globalRootSignature->Release();
        m_globalRootSignature = nullptr;
    }
}

DxrPipeline::~DxrPipeline()
{
    Release();
}

bool DxrPipeline::CreateSmokePipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreateSmokePipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        m_globalRootSignature = DxrRootSignature::CreateSmokeGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreateSmokeLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    DxrBreadcrumb("pipeline shader load begin");
    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrSmokeLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: shader load");
        return false;
    }
    DxrBreadcrumb("pipeline shader load ok");

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR smoke library bytecode missing";
        return false;
    }

    // CreateStateObject requires the signed DXBC container from DXC, not the extracted DXIL part
    // (debug layer: "Hash check failed for DXILibrary" on raw extracted bytecode).
    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    LogCreateStateObjectEnvironment(device5, dxilBytecode);
    LogDxilBytecodeSummary(dxilBytecode);
    if (library->extractedFromDxbcContainer && library->containerBytecode != nullptr)
    {
        DxrBreadcrumb(
            std::string("pipeline dxil source: signed DXBC container for RTPSO (containerBytes=")
            + std::to_string(library->containerBytecode->GetBufferSize()) + " extractedDxilBytes="
            + std::to_string(library->dxilBytecode->GetBufferSize()) + ")");
    }
    else
    {
        DxrBreadcrumb("pipeline dxil source: using raw DXC blob (no DXBC container)");
    }

    DxrBreadcrumb("pipeline CreateStateObject begin");
    SmokeRtpsoSubobjects subobjects{};
    if (!BuildSmokeRtpsoSubobjects(
            subobjects,
            dxilBytecode,
            m_globalRootSignature,
            localRootSignature,
            outError))
    {
        DxrBreadcrumb("pipeline failed: build subobjects");
        return false;
    }

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "smoke-full", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR smoke RTPSO");
    DxrBreadcrumb("pipeline CreateSmokePipeline ok");
    return true;
}

bool DxrPipeline::CreatePrimaryDebugPipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreatePrimaryDebugPipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        m_globalRootSignature = DxrRootSignature::CreatePrimaryDebugGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: primary debug root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreatePrimaryDebugLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: primary debug local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrPrimaryDebugLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: primary debug shader load");
        return false;
    }

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR primary debug library bytecode missing";
        return false;
    }

    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    SmokeRtpsoSubobjects subobjects{};
    subobjects.shaderConfig.MaxPayloadSizeInBytes = 32;
    subobjects.shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects.pipelineConfig.MaxTraceRecursionDepth = 1u;

    subobjects.exportNames = {L"PrimaryRayGen", L"PrimaryMiss", L"PrimaryClosestHit"};
    subobjects.allRtpsoExportNames = {
        L"PrimaryRayGen", L"PrimaryMiss", L"PrimaryClosestHit", L"PrimaryHitGroup"};
    subobjects.hitGroupExportNames = {L"PrimaryHitGroup"};

    subobjects.exports.resize(subobjects.exportNames.size());
    for (std::size_t exportIndex = 0; exportIndex < subobjects.exportNames.size(); ++exportIndex)
    {
        subobjects.exports[exportIndex].Name = subobjects.exportNames[exportIndex];
        subobjects.exports[exportIndex].ExportToRename = nullptr;
        subobjects.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    subobjects.dxilLibraryDesc.DXILLibrary = dxilBytecode;
    subobjects.dxilLibraryDesc.NumExports = static_cast<UINT>(subobjects.exports.size());
    subobjects.dxilLibraryDesc.pExports = subobjects.exports.data();

    subobjects.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    subobjects.hitGroupDesc.HitGroupExport = L"PrimaryHitGroup";
    subobjects.hitGroupDesc.ClosestHitShaderImport = L"PrimaryClosestHit";

    subobjects.subobjects.clear();
    subobjects.subobjects.reserve(10);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
        &subobjects.dxilLibraryDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
        &subobjects.hitGroupDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
        &subobjects.shaderConfig);
    const std::size_t shaderConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.shaderConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[shaderConfigIndex];
    subobjects.shaderConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.shaderConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.shaderConfigAssociation);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &subobjects.pipelineConfig);
    const std::size_t pipelineConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.pipelineConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[pipelineConfigIndex];
    subobjects.pipelineConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.pipelineConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.pipelineConfigAssociation);

    subobjects.globalRootSignatureDesc.pGlobalRootSignature = m_globalRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
        &subobjects.globalRootSignatureDesc);

    subobjects.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
        &subobjects.localRootSignatureDesc);

    subobjects.localRootSignatureAssociation.pSubobjectToAssociate =
        &subobjects.subobjects[subobjects.subobjects.size() - 1];
    subobjects.localRootSignatureAssociation.NumExports =
        static_cast<UINT>(subobjects.hitGroupExportNames.size());
    subobjects.localRootSignatureAssociation.pExports = subobjects.hitGroupExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.localRootSignatureAssociation);

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "primary-debug-full", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR primary debug RTPSO");
    DxrBreadcrumb("pipeline CreatePrimaryDebugPipeline ok");
    return true;
}

// Phase P0/P1 path-tracer RTPSO (devdoc/dxr/path-tracing.md). P4b: uses the path-tracer global
// root signature (reflection layout + t14 prev-instance transforms + u4-u6 RR guide outputs).
bool DxrPipeline::CreatePathTracerPipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreatePathTracerPipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        // G5/R2 widened PT UAVs (ReSTIR + direct); drop any stale singleton from an older layout.
        DxrRootSignature::ReleasePathTracerGlobalRootSignature();
        m_globalRootSignature = DxrRootSignature::CreatePathTracerGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: path tracer root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreateReflectionLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: path tracer local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrPathTracerLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: path tracer shader load");
        return false;
    }

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR path tracer library bytecode missing";
        return false;
    }

    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    SmokeRtpsoSubobjects subobjects{};
    // Must be >= the path_tracer.hlsl Payload struct size. P6 carries the previous-frame
    // primary depth for GI history validation, bringing the payload to 17 DWORDs (68 B).
    // Keep this declaration in sync with the HLSL struct or CreateStateObject will fail.
    subobjects.shaderConfig.MaxPayloadSizeInBytes = 68;
    subobjects.shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects.pipelineConfig.MaxTraceRecursionDepth = 1u;

    subobjects.exportNames = {L"PathTracerRayGen", L"PathTracerMiss", L"PathTracerClosestHit"};
    subobjects.allRtpsoExportNames = {
        L"PathTracerRayGen", L"PathTracerMiss", L"PathTracerClosestHit", L"PathTracerHitGroup"};
    subobjects.hitGroupExportNames = {L"PathTracerHitGroup"};

    subobjects.exports.resize(subobjects.exportNames.size());
    for (std::size_t exportIndex = 0; exportIndex < subobjects.exportNames.size(); ++exportIndex)
    {
        subobjects.exports[exportIndex].Name = subobjects.exportNames[exportIndex];
        subobjects.exports[exportIndex].ExportToRename = nullptr;
        subobjects.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    subobjects.dxilLibraryDesc.DXILLibrary = dxilBytecode;
    subobjects.dxilLibraryDesc.NumExports = static_cast<UINT>(subobjects.exports.size());
    subobjects.dxilLibraryDesc.pExports = subobjects.exports.data();

    subobjects.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    subobjects.hitGroupDesc.HitGroupExport = L"PathTracerHitGroup";
    subobjects.hitGroupDesc.ClosestHitShaderImport = L"PathTracerClosestHit";

    subobjects.subobjects.clear();
    subobjects.subobjects.reserve(10);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
        &subobjects.dxilLibraryDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
        &subobjects.hitGroupDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
        &subobjects.shaderConfig);
    const std::size_t shaderConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.shaderConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[shaderConfigIndex];
    subobjects.shaderConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.shaderConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.shaderConfigAssociation);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &subobjects.pipelineConfig);
    const std::size_t pipelineConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.pipelineConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[pipelineConfigIndex];
    subobjects.pipelineConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.pipelineConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.pipelineConfigAssociation);

    subobjects.globalRootSignatureDesc.pGlobalRootSignature = m_globalRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
        &subobjects.globalRootSignatureDesc);

    subobjects.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
        &subobjects.localRootSignatureDesc);

    subobjects.localRootSignatureAssociation.pSubobjectToAssociate =
        &subobjects.subobjects[subobjects.subobjects.size() - 1];
    subobjects.localRootSignatureAssociation.NumExports =
        static_cast<UINT>(subobjects.hitGroupExportNames.size());
    subobjects.localRootSignatureAssociation.pExports = subobjects.hitGroupExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.localRootSignatureAssociation);

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "path-tracer-full", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR path tracer RTPSO");
    DxrBreadcrumb("pipeline CreatePathTracerPipeline ok");
    return true;
}

// Phase D4 reflections RTPSO (see devdoc/dxr/reflections.md). Structure mirrors the
// primary-debug pipeline; only the library, exports, and root signatures differ.
bool DxrPipeline::CreateReflectionsPipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreateReflectionsPipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        m_globalRootSignature = DxrRootSignature::CreateReflectionGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: reflection root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreateReflectionLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: reflection local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrReflectionsLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: reflection shader load");
        return false;
    }

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR reflections library bytecode missing";
        return false;
    }

    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    SmokeRtpsoSubobjects subobjects{};
    subobjects.shaderConfig.MaxPayloadSizeInBytes = 32;
    subobjects.shaderConfig.MaxAttributeSizeInBytes = 8;
    // Depth 2: closest-hit traces occlusion rays (soft sun cone + AO + spec occlusion) and an
    // optional one-bounce diffuse GI trace (world-space, not screen-space reprojection).
    subobjects.pipelineConfig.MaxTraceRecursionDepth = 2u;

    subobjects.exportNames = {L"ReflectionRayGen", L"ReflectionMiss", L"ReflectionClosestHit"};
    subobjects.allRtpsoExportNames = {
        L"ReflectionRayGen", L"ReflectionMiss", L"ReflectionClosestHit", L"ReflectionHitGroup"};
    subobjects.hitGroupExportNames = {L"ReflectionHitGroup"};

    subobjects.exports.resize(subobjects.exportNames.size());
    for (std::size_t exportIndex = 0; exportIndex < subobjects.exportNames.size(); ++exportIndex)
    {
        subobjects.exports[exportIndex].Name = subobjects.exportNames[exportIndex];
        subobjects.exports[exportIndex].ExportToRename = nullptr;
        subobjects.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    subobjects.dxilLibraryDesc.DXILLibrary = dxilBytecode;
    subobjects.dxilLibraryDesc.NumExports = static_cast<UINT>(subobjects.exports.size());
    subobjects.dxilLibraryDesc.pExports = subobjects.exports.data();

    subobjects.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    subobjects.hitGroupDesc.HitGroupExport = L"ReflectionHitGroup";
    subobjects.hitGroupDesc.ClosestHitShaderImport = L"ReflectionClosestHit";

    subobjects.subobjects.clear();
    subobjects.subobjects.reserve(10);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
        &subobjects.dxilLibraryDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
        &subobjects.hitGroupDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
        &subobjects.shaderConfig);
    const std::size_t shaderConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.shaderConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[shaderConfigIndex];
    subobjects.shaderConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.shaderConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.shaderConfigAssociation);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &subobjects.pipelineConfig);
    const std::size_t pipelineConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.pipelineConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[pipelineConfigIndex];
    subobjects.pipelineConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.pipelineConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.pipelineConfigAssociation);

    subobjects.globalRootSignatureDesc.pGlobalRootSignature = m_globalRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
        &subobjects.globalRootSignatureDesc);

    subobjects.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
        &subobjects.localRootSignatureDesc);

    subobjects.localRootSignatureAssociation.pSubobjectToAssociate =
        &subobjects.subobjects[subobjects.subobjects.size() - 1];
    subobjects.localRootSignatureAssociation.NumExports =
        static_cast<UINT>(subobjects.hitGroupExportNames.size());
    subobjects.localRootSignatureAssociation.pExports = subobjects.hitGroupExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.localRootSignatureAssociation);

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "reflections-full", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR reflections RTPSO");
    DxrBreadcrumb("pipeline CreateReflectionsPipeline ok");
    return true;
}

// Phase D8 shadows RTPSO (see devdoc/dxr/shadows.md). Copy of CreateReflectionsPipeline;
// only the library, exports, root signatures, and payload size (16 bytes) differ.
bool DxrPipeline::CreateShadowsPipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreateShadowsPipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        m_globalRootSignature = DxrRootSignature::CreateShadowGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: shadow root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreateShadowLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: shadow local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrShadowsLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: shadow shader load");
        return false;
    }

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR shadows library bytecode missing";
        return false;
    }

    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    SmokeRtpsoSubobjects subobjects{};
    subobjects.shaderConfig.MaxPayloadSizeInBytes = 16;
    subobjects.shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects.pipelineConfig.MaxTraceRecursionDepth = 1u;

    subobjects.exportNames = {L"ShadowRayGen", L"ShadowMiss", L"ShadowClosestHit"};
    subobjects.allRtpsoExportNames = {
        L"ShadowRayGen", L"ShadowMiss", L"ShadowClosestHit", L"ShadowHitGroup"};
    subobjects.hitGroupExportNames = {L"ShadowHitGroup"};

    subobjects.exports.resize(subobjects.exportNames.size());
    for (std::size_t exportIndex = 0; exportIndex < subobjects.exportNames.size(); ++exportIndex)
    {
        subobjects.exports[exportIndex].Name = subobjects.exportNames[exportIndex];
        subobjects.exports[exportIndex].ExportToRename = nullptr;
        subobjects.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    subobjects.dxilLibraryDesc.DXILLibrary = dxilBytecode;
    subobjects.dxilLibraryDesc.NumExports = static_cast<UINT>(subobjects.exports.size());
    subobjects.dxilLibraryDesc.pExports = subobjects.exports.data();

    subobjects.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    subobjects.hitGroupDesc.HitGroupExport = L"ShadowHitGroup";
    subobjects.hitGroupDesc.ClosestHitShaderImport = L"ShadowClosestHit";

    subobjects.subobjects.clear();
    subobjects.subobjects.reserve(10);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
        &subobjects.dxilLibraryDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
        &subobjects.hitGroupDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
        &subobjects.shaderConfig);
    const std::size_t shaderConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.shaderConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[shaderConfigIndex];
    subobjects.shaderConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.shaderConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.shaderConfigAssociation);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &subobjects.pipelineConfig);
    const std::size_t pipelineConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.pipelineConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[pipelineConfigIndex];
    subobjects.pipelineConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.pipelineConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.pipelineConfigAssociation);

    subobjects.globalRootSignatureDesc.pGlobalRootSignature = m_globalRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
        &subobjects.globalRootSignatureDesc);

    subobjects.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
        &subobjects.localRootSignatureDesc);

    subobjects.localRootSignatureAssociation.pSubobjectToAssociate =
        &subobjects.subobjects[subobjects.subobjects.size() - 1];
    subobjects.localRootSignatureAssociation.NumExports =
        static_cast<UINT>(subobjects.hitGroupExportNames.size());
    subobjects.localRootSignatureAssociation.pExports = subobjects.hitGroupExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.localRootSignatureAssociation);

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "shadows-full", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR shadows RTPSO");
    DxrBreadcrumb("pipeline CreateShadowsPipeline ok");
    return true;
}

// Phase D9 diffuse-GI RTPSO (see devdoc/dxr/diffuse-gi.md). Reuses the REFLECTION global + local
// root signatures wholesale (same SRV inputs; GI just doesn't reference the GGX-specific ones);
// only the DXIL library and exports differ. Payload size 32 (matches the reflections config).
bool DxrPipeline::CreateGiPipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreateGiPipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        m_globalRootSignature = DxrRootSignature::CreateReflectionGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: gi root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreateReflectionLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: gi local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrGiLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: gi shader load");
        return false;
    }

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR diffuse GI library bytecode missing";
        return false;
    }

    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    SmokeRtpsoSubobjects subobjects{};
    subobjects.shaderConfig.MaxPayloadSizeInBytes = 32;
    subobjects.shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects.pipelineConfig.MaxTraceRecursionDepth = 1u;

    subobjects.exportNames = {L"GiRayGen", L"GiMiss", L"GiClosestHit"};
    subobjects.allRtpsoExportNames = {L"GiRayGen", L"GiMiss", L"GiClosestHit", L"GiHitGroup"};
    subobjects.hitGroupExportNames = {L"GiHitGroup"};

    subobjects.exports.resize(subobjects.exportNames.size());
    for (std::size_t exportIndex = 0; exportIndex < subobjects.exportNames.size(); ++exportIndex)
    {
        subobjects.exports[exportIndex].Name = subobjects.exportNames[exportIndex];
        subobjects.exports[exportIndex].ExportToRename = nullptr;
        subobjects.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    subobjects.dxilLibraryDesc.DXILLibrary = dxilBytecode;
    subobjects.dxilLibraryDesc.NumExports = static_cast<UINT>(subobjects.exports.size());
    subobjects.dxilLibraryDesc.pExports = subobjects.exports.data();

    subobjects.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    subobjects.hitGroupDesc.HitGroupExport = L"GiHitGroup";
    subobjects.hitGroupDesc.ClosestHitShaderImport = L"GiClosestHit";

    subobjects.subobjects.clear();
    subobjects.subobjects.reserve(10);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
        &subobjects.dxilLibraryDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
        &subobjects.hitGroupDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
        &subobjects.shaderConfig);
    const std::size_t shaderConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.shaderConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[shaderConfigIndex];
    subobjects.shaderConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.shaderConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.shaderConfigAssociation);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &subobjects.pipelineConfig);
    const std::size_t pipelineConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.pipelineConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[pipelineConfigIndex];
    subobjects.pipelineConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.pipelineConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.pipelineConfigAssociation);

    subobjects.globalRootSignatureDesc.pGlobalRootSignature = m_globalRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
        &subobjects.globalRootSignatureDesc);

    subobjects.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
        &subobjects.localRootSignatureDesc);

    subobjects.localRootSignatureAssociation.pSubobjectToAssociate =
        &subobjects.subobjects[subobjects.subobjects.size() - 1];
    subobjects.localRootSignatureAssociation.NumExports =
        static_cast<UINT>(subobjects.hitGroupExportNames.size());
    subobjects.localRootSignatureAssociation.pExports = subobjects.hitGroupExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.localRootSignatureAssociation);

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "diffuse-gi-full", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR diffuse GI RTPSO");
    DxrBreadcrumb("pipeline CreateGiPipeline ok");
    return true;
}

bool DxrPipeline::CreateRestirPipeline(std::string& outError)
{
    outError.clear();
    Release();

    DxrBreadcrumb("pipeline CreateRestirPipeline begin");
    ID3D12Device5* device5 = DxrContext::Get().GetDevice5();
    if (device5 == nullptr)
    {
        outError = "ID3D12Device5 unavailable";
        return false;
    }

    try
    {
        m_globalRootSignature = nullptr;
        DxrRootSignature::ReleaseRestirGlobalRootSignature();
        m_globalRootSignature = DxrRootSignature::CreateRestirGlobalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: restir root signature");
        return false;
    }

    ID3D12RootSignature* localRootSignature = nullptr;
    try
    {
        localRootSignature = DxrRootSignature::CreateRestirLocalRootSignature();
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: restir local root signature");
        return false;
    }

    struct LocalRootSignatureScope
    {
        ID3D12RootSignature* signature = nullptr;
        ~LocalRootSignatureScope()
        {
            if (signature != nullptr)
            {
                signature->Release();
            }
        }
    } localRootSignatureScope{localRootSignature};

    std::shared_ptr<DxrCompiledLibrary> library;
    try
    {
        library = DxrShaderCache::Load(EngineConstants::DxrRestirLibraryShader);
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        DxrBreadcrumb("pipeline failed: restir shader load");
        return false;
    }

    if (library == nullptr || library->dxilBytecode == nullptr)
    {
        outError = "DXR ReSTIR library bytecode missing";
        return false;
    }

    D3D12_SHADER_BYTECODE dxilBytecode{};
    if (library->containerBytecode != nullptr)
    {
        dxilBytecode.pShaderBytecode = library->containerBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->containerBytecode->GetBufferSize();
    }
    else
    {
        dxilBytecode.pShaderBytecode = library->dxilBytecode->GetBufferPointer();
        dxilBytecode.BytecodeLength = library->dxilBytecode->GetBufferSize();
    }

    SmokeRtpsoSubobjects subobjects{};
    subobjects.shaderConfig.MaxPayloadSizeInBytes = 16;
    subobjects.shaderConfig.MaxAttributeSizeInBytes = 8;
    subobjects.pipelineConfig.MaxTraceRecursionDepth = 1u;

    subobjects.exportNames = {
        L"RestirTemporalRayGen",
        L"RestirSpatialRayGen",
        L"RestirMiss",
        L"RestirClosestHit"};
    subobjects.allRtpsoExportNames = {
        L"RestirTemporalRayGen",
        L"RestirSpatialRayGen",
        L"RestirMiss",
        L"RestirClosestHit",
        L"RestirHitGroup"};
    subobjects.hitGroupExportNames = {L"RestirHitGroup"};

    subobjects.exports.resize(subobjects.exportNames.size());
    for (std::size_t exportIndex = 0; exportIndex < subobjects.exportNames.size(); ++exportIndex)
    {
        subobjects.exports[exportIndex].Name = subobjects.exportNames[exportIndex];
        subobjects.exports[exportIndex].ExportToRename = nullptr;
        subobjects.exports[exportIndex].Flags = D3D12_EXPORT_FLAG_NONE;
    }

    subobjects.dxilLibraryDesc.DXILLibrary = dxilBytecode;
    subobjects.dxilLibraryDesc.NumExports = static_cast<UINT>(subobjects.exports.size());
    subobjects.dxilLibraryDesc.pExports = subobjects.exports.data();

    subobjects.hitGroupDesc.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
    subobjects.hitGroupDesc.HitGroupExport = L"RestirHitGroup";
    subobjects.hitGroupDesc.ClosestHitShaderImport = L"RestirClosestHit";

    subobjects.subobjects.clear();
    subobjects.subobjects.reserve(10);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,
        &subobjects.dxilLibraryDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,
        &subobjects.hitGroupDesc);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,
        &subobjects.shaderConfig);
    const std::size_t shaderConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.shaderConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[shaderConfigIndex];
    subobjects.shaderConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.shaderConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.shaderConfigAssociation);

    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,
        &subobjects.pipelineConfig);
    const std::size_t pipelineConfigIndex = subobjects.subobjects.size() - 1;

    subobjects.pipelineConfigAssociation.pSubobjectToAssociate = &subobjects.subobjects[pipelineConfigIndex];
    subobjects.pipelineConfigAssociation.NumExports = static_cast<UINT>(subobjects.allRtpsoExportNames.size());
    subobjects.pipelineConfigAssociation.pExports = subobjects.allRtpsoExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.pipelineConfigAssociation);

    subobjects.globalRootSignatureDesc.pGlobalRootSignature = m_globalRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,
        &subobjects.globalRootSignatureDesc);

    subobjects.localRootSignatureDesc.pLocalRootSignature = localRootSignature;
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE,
        &subobjects.localRootSignatureDesc);

    subobjects.localRootSignatureAssociation.pSubobjectToAssociate =
        &subobjects.subobjects[subobjects.subobjects.size() - 1];
    subobjects.localRootSignatureAssociation.NumExports =
        static_cast<UINT>(subobjects.hitGroupExportNames.size());
    subobjects.localRootSignatureAssociation.pExports = subobjects.hitGroupExportNames.data();
    AppendSubobject(
        subobjects.subobjects,
        D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION,
        &subobjects.localRootSignatureAssociation);

    ComPtr<ID3D12StateObject> stateObject;
    if (!CreateStateObjectFromSubobjects(device5, subobjects, "restir-stubs", stateObject, outError))
    {
        return false;
    }

    ID3D12StateObjectProperties* stateObjectProperties = nullptr;
    if (FAILED(stateObject->QueryInterface(IID_PPV_ARGS(&stateObjectProperties))))
    {
        outError = "QueryInterface(ID3D12StateObjectProperties) failed";
        return false;
    }

    m_stateObject = stateObject.Detach();
    m_stateObjectProperties = stateObjectProperties;

    EngineLog::Info("dxr-pipeline", "Created DXR ReSTIR stub RTPSO");
    DxrBreadcrumb("pipeline CreateRestirPipeline ok");
    return true;
}
