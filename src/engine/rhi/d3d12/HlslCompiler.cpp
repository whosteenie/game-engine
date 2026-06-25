#include "engine/rhi/d3d12/HlslCompiler.h"

#include "engine/platform/EngineLog.h"
#include "engine/rhi/d3d12/D3D12Throw.h"

#include <windows.h>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

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
