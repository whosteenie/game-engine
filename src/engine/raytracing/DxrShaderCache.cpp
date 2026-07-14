#include "engine/raytracing/DxrShaderCache.h"

#include "engine/platform/EngineLog.h"
#include "engine/raytracing/DxrTrace.h"

#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
    std::mutex g_dxrShaderCacheMutex;
    std::unordered_map<std::string, std::weak_ptr<DxrCompiledLibrary>> g_dxrShaderCache;
}

std::string DxrShaderCache::ReadShaderSource(const char* libraryPath)
{
    std::ifstream file(libraryPath);
    if (!file.is_open())
    {
        throw std::runtime_error(std::string("Failed to open DXR shader library: ") + libraryPath);
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::shared_ptr<DxrCompiledLibrary> DxrShaderCache::Load(
    const char* const libraryPath,
    const bool diagnosticPermutation)
{
    const std::string source = ReadShaderSource(libraryPath);
    const std::size_t sourceHash = std::hash<std::string>{}(source);
    const char* const define = diagnosticPermutation ? "PT_DIAGNOSTIC_PERMUTATION=1" : nullptr;
    const std::string cacheKey = std::string(libraryPath) + "#" + std::to_string(sourceHash)
        + (diagnosticPermutation ? "#diagnostic" : "#production");

    std::lock_guard<std::mutex> lock(g_dxrShaderCacheMutex);

    const auto existing = g_dxrShaderCache.find(cacheKey);
    if (existing != g_dxrShaderCache.end())
    {
        if (std::shared_ptr<DxrCompiledLibrary> library = existing->second.lock())
        {
            return library;
        }
    }

    DxrBreadcrumb(std::string("shader compile begin: ") + libraryPath);
    const HlslCompileResult compileResult = CompileHlslLibrary(source, libraryPath, define);
    DxrBreadcrumb(std::string("shader compile ok: ") + libraryPath);
    if (compileResult.shader == nullptr)
    {
        throw std::runtime_error(std::string("DXR library compile produced no bytecode: ") + libraryPath);
    }

    const DxilLibraryBytecode preparedBytecode = PrepareDxilLibraryBytecode(compileResult.shader);

    auto library = std::make_shared<DxrCompiledLibrary>();
    library->containerBytecode = compileResult.shader;
    library->dxilBytecode = preparedBytecode.bytecode;
    library->extractedFromDxbcContainer = preparedBytecode.extractedFromDxbcContainer;
    library->sourcePath = libraryPath;

    EngineLog::Info(
        "dxr-shader",
        std::string("Compiled DXR library: ") + libraryPath
            + (diagnosticPermutation ? " [diagnostic]" : " [production]") + " containerBytes="
            + std::to_string(preparedBytecode.containerByteCount) + " dxilBytes="
            + std::to_string(library->dxilBytecode->GetBufferSize()) + " extracted="
            + (library->extractedFromDxbcContainer ? "yes" : "no"));
    g_dxrShaderCache[cacheKey] = library;
    return library;
}

void DxrShaderCache::Clear()
{
    std::lock_guard<std::mutex> lock(g_dxrShaderCacheMutex);
    g_dxrShaderCache.clear();
}
