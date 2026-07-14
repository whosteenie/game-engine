#include "engine/rhi/d3d12/HlslCompiler.h"

#include "engine/platform/EngineLog.h"
#include "engine/rhi/d3d12/D3D12Throw.h"

#include <windows.h>

#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace
{
    std::wstring Utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
        if (wideLength <= 0)
        {
            return {};
        }

        std::wstring wide(static_cast<std::size_t>(wideLength - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), wideLength);
        return wide;
    }

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

    class IncludeHandler final : public IDxcIncludeHandler
    {
    public:
        explicit IncludeHandler(IDxcUtils* utils) : m_utils(utils) {}

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override
        {
            if (object == nullptr)
            {
                return E_POINTER;
            }

            if (IsEqualIID(riid, __uuidof(IUnknown)) || IsEqualIID(riid, __uuidof(IDxcIncludeHandler)))
            {
                *object = this;
                AddRef();
                return S_OK;
            }

            *object = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refCount; }

        ULONG STDMETHODCALLTYPE Release() override
        {
            const ULONG refCount = --m_refCount;
            if (refCount == 0)
            {
                delete this;
            }

            return refCount;
        }

        HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR filename, IDxcBlob** includeSource) override
        {
            if (includeSource == nullptr || m_utils == nullptr || filename == nullptr)
            {
                return E_INVALIDARG;
            }

            *includeSource = nullptr;

            const int narrowLength =
                WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
            if (narrowLength <= 0)
            {
                return E_FAIL;
            }

            std::string path(static_cast<std::size_t>(narrowLength - 1), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                filename,
                -1,
                path.data(),
                narrowLength,
                nullptr,
                nullptr);

            if (path.find('/') == std::string::npos && path.find('\\') == std::string::npos)
            {
                path = std::string("assets/shaders/") + path;
            }

            try
            {
                const std::string content = ReadTextFile(path.c_str());
                ComPtr<IDxcBlobEncoding> blobEncoding;
                const HRESULT hr = m_utils->CreateBlob(
                    content.data(),
                    static_cast<UINT32>(content.size()),
                    DXC_CP_UTF8,
                    &blobEncoding);
                if (FAILED(hr))
                {
                    return hr;
                }

                return blobEncoding.CopyTo(includeSource);
            }
            catch (...)
            {
                return E_FAIL;
            }
        }

    private:
        IDxcUtils* m_utils = nullptr;
        ULONG m_refCount = 1;
    };
}

namespace
{
    bool IsDxbcContainer(const void* data, const std::size_t size)
    {
        if (data == nullptr || size < 4)
        {
            return false;
        }

        const auto* bytes = static_cast<const unsigned char*>(data);
        return bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C';
    }

    void LogDxilHeaderFourCC(const char* label, const void* data, const std::size_t size)
    {
        if (data == nullptr || size < 4)
        {
            EngineLog::Info("hlsl", std::string(label) + ": missing or too small");
            return;
        }

        const auto* bytes = static_cast<const unsigned char*>(data);
        char header[16];
        std::snprintf(
            header,
            sizeof(header),
            "%02X%02X%02X%02X",
            bytes[0],
            bytes[1],
            bytes[2],
            bytes[3]);

        EngineLog::Info(
            "hlsl",
            std::string(label) + ": bytes=" + std::to_string(size) + " header=0x" + header);
    }

    [[noreturn]] void ThrowShaderCompileError(const std::string& message)
    {
        EngineLog::Warn("hlsl", message);
        throw std::runtime_error(message);
    }

    void AppendUtf8Blob(std::string& message, IDxcBlobUtf8* errors)
    {
        if (errors == nullptr)
        {
            return;
        }

        const char* errorText = errors->GetStringPointer();
        const std::size_t errorLength = static_cast<std::size_t>(errors->GetStringLength());
        if (errorText == nullptr || errorLength == 0)
        {
            return;
        }

        for (std::size_t index = 0; index < errorLength; ++index)
        {
            const unsigned char byte = static_cast<unsigned char>(errorText[index]);
            if (byte == '\0')
            {
                break;
            }

            if (byte == '\n' || byte == '\r' || byte == '\t' || (byte >= 32 && byte < 127))
            {
                message.push_back(static_cast<char>(byte));
            }
        }
    }
}

HlslCompileResult CompileHlsl(
    const std::string& source,
    const std::string& sourcePath,
    const char* entry,
    const char* targetProfile)
{
    ComPtr<IDxcUtils> utils;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcCreateInstance(DxcUtils)");

    ComPtr<IDxcCompiler3> compiler;
    ThrowIfFailed(
        DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
        "DxcCreateInstance(DxcCompiler)");

    ComPtr<IDxcBlobEncoding> sourceBlob;
    ThrowIfFailed(
        utils->CreateBlob(source.c_str(), source.size(), DXC_CP_UTF8, &sourceBlob),
        "CreateBlob(shader source)");

    const std::wstring wideEntry = Utf8ToWide(entry);
    const std::wstring wideTarget = Utf8ToWide(targetProfile);
    const std::wstring wideSourcePath = Utf8ToWide(sourcePath);

    const wchar_t* compilerArgs[] = {
        wideSourcePath.c_str(),
        L"-E",
        wideEntry.c_str(),
        L"-T",
        wideTarget.c_str(),
        L"-Zi",
    };

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    IncludeHandler includeHandler(utils.Get());
    includeHandler.AddRef();

    ComPtr<IDxcResult> compileResult;
    const HRESULT compileHr = compiler->Compile(
        &sourceBuffer,
        compilerArgs,
        static_cast<UINT32>(sizeof(compilerArgs) / sizeof(compilerArgs[0])),
        &includeHandler,
        IID_PPV_ARGS(&compileResult));
    includeHandler.Release();

    if (FAILED(compileHr))
    {
        ThrowShaderCompileError(
            std::string("DXC Compile call failed for ") + sourcePath + " (" + targetProfile + ")");
    }

    HRESULT status = S_OK;
    compileResult->GetStatus(&status);

    ComPtr<IDxcBlobUtf8> errors;
    compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (FAILED(status))
    {
        std::string message = "Shader compile failed: " + sourcePath + " (" + targetProfile + ")";
        if (errors != nullptr && errors->GetStringLength() > 0)
        {
            message.append("\n");
            AppendUtf8Blob(message, errors.Get());
        }
        ThrowShaderCompileError(message);
    }

    if (errors != nullptr && errors->GetStringLength() > 0)
    {
        OutputDebugStringA(errors->GetStringPointer());
    }

    HlslCompileResult result{};
    HRESULT objectHr = compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result.shader), nullptr);
    if (FAILED(objectHr))
    {
        ThrowShaderCompileError(
            std::string("DXC_OUT_OBJECT failed for ") + sourcePath + " (" + targetProfile + ") (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(objectHr)) + ")");
    }

    ComPtr<IDxcBlob> reflectionBlob;
    const HRESULT reflectionHr =
        compileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(&reflectionBlob), nullptr);
    if (FAILED(reflectionHr) || reflectionBlob == nullptr)
    {
        ThrowShaderCompileError(
            std::string("Shader reflection missing for ") + sourcePath + " (" + targetProfile + ") (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(reflectionHr)) + ")");
    }

    DxcBuffer reflectionData{};
    reflectionData.Ptr = reflectionBlob->GetBufferPointer();
    reflectionData.Size = static_cast<UINT32>(reflectionBlob->GetBufferSize());
    reflectionData.Encoding = DXC_CP_ACP;
    const HRESULT createReflectionHr =
        utils->CreateReflection(&reflectionData, IID_PPV_ARGS(&result.reflection));
    if (FAILED(createReflectionHr))
    {
        ThrowShaderCompileError(
            std::string("CreateReflection failed for ") + sourcePath + " (" + targetProfile + ") (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(createReflectionHr)) + ")");
    }

    return result;
}

DxilLibraryBytecode PrepareDxilLibraryBytecode(ComPtr<IDxcBlob> dxcOutput)
{
    if (dxcOutput == nullptr)
    {
        throw std::runtime_error("PrepareDxilLibraryBytecode: DXC output blob missing");
    }

    DxilLibraryBytecode result{};
    result.containerByteCount = dxcOutput->GetBufferSize();

    const void* containerData = dxcOutput->GetBufferPointer();
    if (!IsDxbcContainer(containerData, result.containerByteCount))
    {
        result.bytecode = dxcOutput;
        result.extractedFromDxbcContainer = false;
        LogDxilHeaderFourCC("dxil-library prepare: using raw blob", containerData, result.containerByteCount);
        return result;
    }

    ComPtr<IDxcUtils> utils;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcCreateInstance(DxcUtils)");

    DxcBuffer containerBuffer{};
    containerBuffer.Ptr = containerData;
    containerBuffer.Size = static_cast<UINT32>(result.containerByteCount);
    containerBuffer.Encoding = DXC_CP_ACP;

    void* partData = nullptr;
    UINT32 partSize = 0;
    const HRESULT extractHr =
        utils->GetDxilContainerPart(&containerBuffer, DXC_PART_DXIL, &partData, &partSize);
    if (FAILED(extractHr) || partData == nullptr || partSize == 0)
    {
        throw std::runtime_error(
            std::string("GetDxilContainerPart(DXC_PART_DXIL) failed (HRESULT=0x")
            + std::to_string(static_cast<unsigned long>(extractHr)) + ")");
    }

    ComPtr<IDxcBlobEncoding> dxilBlob;
    ThrowIfFailed(
        utils->CreateBlob(partData, partSize, DXC_CP_ACP, &dxilBlob),
        "CreateBlob(DXIL part)");

    result.bytecode = dxilBlob;
    result.extractedFromDxbcContainer = true;

    LogDxilHeaderFourCC("dxil-library prepare: container", containerData, result.containerByteCount);
    LogDxilHeaderFourCC("dxil-library prepare: extracted dxil", partData, partSize);
    EngineLog::Info(
        "hlsl",
        std::string("dxil-library prepare: extracted=")
            + (result.extractedFromDxbcContainer ? "yes" : "no") + " containerBytes="
            + std::to_string(result.containerByteCount) + " dxilBytes=" + std::to_string(partSize));

    return result;
}

HlslCompileResult CompileHlslLibrary(
    const std::string& source,
    const std::string& sourcePath,
    const HlslLibraryCompileOptions& options)
{
    ComPtr<IDxcUtils> utils;
    ThrowIfFailed(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcCreateInstance(DxcUtils)");

    ComPtr<IDxcCompiler3> compiler;
    ThrowIfFailed(
        DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)),
        "DxcCreateInstance(DxcCompiler)");

    ComPtr<IDxcBlobEncoding> sourceBlob;
    ThrowIfFailed(
        utils->CreateBlob(source.c_str(), source.size(), DXC_CP_UTF8, &sourceBlob),
        "CreateBlob(shader source)");

    const std::wstring wideSourcePath = Utf8ToWide(sourcePath);
    const std::wstring wideTargetProfile = Utf8ToWide(
        options.targetProfile != nullptr && options.targetProfile[0] != '\0' ? options.targetProfile : "lib_6_3");

    std::vector<const wchar_t*> compilerArgs = {
        wideSourcePath.c_str(),
        L"-T",
        wideTargetProfile.c_str(),
        L"-Zi",
    };
    std::vector<std::wstring> wideDefines;
    wideDefines.reserve(options.defines.size());
    for (const std::string& define : options.defines)
    {
        if (!define.empty())
        {
            wideDefines.push_back(Utf8ToWide(define));
        }
    }
    for (const std::wstring& define : wideDefines)
    {
        compilerArgs.push_back(L"-D");
        compilerArgs.push_back(define.c_str());
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_UTF8;

    IncludeHandler includeHandler(utils.Get());
    includeHandler.AddRef();

    ComPtr<IDxcResult> compileResult;
    const HRESULT compileHr = compiler->Compile(
        &sourceBuffer,
        compilerArgs.data(),
        static_cast<UINT32>(compilerArgs.size()),
        &includeHandler,
        IID_PPV_ARGS(&compileResult));
    includeHandler.Release();

    if (FAILED(compileHr))
    {
        ThrowShaderCompileError(
            std::string("DXC Compile call failed for library ") + sourcePath + " ("
            + (options.targetProfile != nullptr ? options.targetProfile : "lib_6_3") + ")");
    }

    HRESULT status = S_OK;
    compileResult->GetStatus(&status);

    ComPtr<IDxcBlobUtf8> errors;
    compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (FAILED(status))
    {
        std::string message = "Shader library compile failed: " + sourcePath + " ("
            + (options.targetProfile != nullptr ? options.targetProfile : "lib_6_3") + ")";
        if (errors != nullptr && errors->GetStringLength() > 0)
        {
            message.append("\n");
            AppendUtf8Blob(message, errors.Get());
        }
        ThrowShaderCompileError(message);
    }

    if (errors != nullptr && errors->GetStringLength() > 0)
    {
        OutputDebugStringA(errors->GetStringPointer());
    }

    HlslCompileResult result{};
    const HRESULT objectHr = compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&result.shader), nullptr);
    if (FAILED(objectHr))
    {
        ThrowShaderCompileError(
            std::string("DXC_OUT_OBJECT failed for library ") + sourcePath + " (HRESULT=0x"
            + std::to_string(static_cast<unsigned long>(objectHr)) + ")");
    }

    return result;
}
