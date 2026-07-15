#include "engine/raytracing/DxrShaderCache.h"

#include "engine/platform/EngineLog.h"
#include "engine/rhi/GfxContext.h"
#include "engine/raytracing/DxrTrace.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace
{
    std::mutex g_dxrShaderCacheMutex;
    // Pipelines retain the state object, not the source DXIL blob. A weak cache therefore expires
    // immediately after state-object creation and cannot service a later warm-up request. Keep
    // the small set of compiled libraries alive until RenderingPipelineCache::Clear().
    std::unordered_map<std::string, std::shared_ptr<DxrCompiledLibrary>> g_dxrShaderCache;

    void AppendUniqueDefine(std::vector<std::string>& defines, const char* define)
    {
        if (std::find(defines.begin(), defines.end(), define) == defines.end())
        {
            defines.emplace_back(define);
        }
    }

    std::string MakeCacheKey(
        const char* libraryPath,
        const std::size_t sourceHash,
        const DxrShaderLibraryCompileOptions& options)
    {
        std::string key = std::string(libraryPath) + "#" + std::to_string(sourceHash)
            + "#profile=" + options.targetProfile + "#diagnostic="
            + (options.diagnosticPermutation ? "1" : "0") + "#ser="
            + (options.serPermutation ? "1" : "0") + "#inline-visibility="
            + (options.inlineVisibilityPermutation ? "1" : "0");
        for (const std::string& define : options.featureDefines)
        {
            key += "#define=" + define;
        }
        return key;
    }
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
    return Load(libraryPath, MakeActiveDeviceCompileOptions(diagnosticPermutation));
}

DxrShaderLibraryCompileOptions DxrShaderCache::MakeActiveDeviceCompileOptions(
    const bool diagnosticPermutation)
{
    DxrShaderLibraryCompileOptions options{};
    options.diagnosticPermutation = diagnosticPermutation;

    const GfxContext& gfx = GfxContext::Get();
    if (gfx.IsInitialized())
    {
        options.targetProfile = gfx.GetPreferredDxrLibraryProfile();
    }

    AppendUniqueDefine(
        options.featureDefines,
        options.targetProfile != "lib_6_3" ? "DXR_MODERN_LIBRARY=1" : "DXR_MODERN_LIBRARY=0");
    // PF6 is correctness-preserving and only selected when the device supports inline DXR.
    // Unsupported hardware keeps the exact legacy TraceRay visibility path.
    options.inlineVisibilityPermutation = gfx.IsInitialized() && gfx.IsInlineRaytracingSupported();
    AppendUniqueDefine(
        options.featureDefines,
        options.inlineVisibilityPermutation
            ? "DXR_INLINE_VISIBILITY_PERMUTATION=1"
            : "DXR_INLINE_VISIBILITY_PERMUTATION=0");
    if (diagnosticPermutation)
    {
        AppendUniqueDefine(options.featureDefines, "PT_DIAGNOSTIC_PERMUTATION=1");
    }
    return options;
}

std::shared_ptr<DxrCompiledLibrary> DxrShaderCache::Load(
    const char* const libraryPath,
    const DxrShaderLibraryCompileOptions& requestedOptions)
{
    const std::string source = ReadShaderSource(libraryPath);
    const std::size_t sourceHash = std::hash<std::string>{}(source);
    DxrShaderLibraryCompileOptions options = requestedOptions;
    if (options.targetProfile.empty())
    {
        options.targetProfile = "lib_6_3";
    }
    AppendUniqueDefine(
        options.featureDefines,
        options.targetProfile != "lib_6_3" ? "DXR_MODERN_LIBRARY=1" : "DXR_MODERN_LIBRARY=0");
    AppendUniqueDefine(
        options.featureDefines,
        options.serPermutation ? "DXR_SER_PERMUTATION=1" : "DXR_SER_PERMUTATION=0");
    AppendUniqueDefine(
        options.featureDefines,
        options.inlineVisibilityPermutation
            ? "DXR_INLINE_VISIBILITY_PERMUTATION=1"
            : "DXR_INLINE_VISIBILITY_PERMUTATION=0");
    if (options.diagnosticPermutation)
    {
        AppendUniqueDefine(options.featureDefines, "PT_DIAGNOSTIC_PERMUTATION=1");
    }
    const std::string cacheKey = MakeCacheKey(libraryPath, sourceHash, options);

    {
        std::lock_guard<std::mutex> lock(g_dxrShaderCacheMutex);
        const auto existing = g_dxrShaderCache.find(cacheKey);
        if (existing != g_dxrShaderCache.end())
        {
            return existing->second;
        }
    }

    // Do not hold the cache lock while DXC works. Distinct libraries/permutations can compile in
    // parallel during project warm-up; if two callers race for the same key, the first completed
    // result wins and the other temporary blob is released.
    DxrBreadcrumb(std::string("shader compile begin: ") + libraryPath);
    HlslLibraryCompileOptions compileOptions{};
    compileOptions.targetProfile = options.targetProfile.c_str();
    compileOptions.defines = options.featureDefines;
    const HlslCompileResult compileResult = CompileHlslLibrary(source, libraryPath, compileOptions);
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
            + (options.diagnosticPermutation ? " [diagnostic]" : " [production]") + " profile="
            + options.targetProfile + " ser=" + (options.serPermutation ? "on" : "off")
            + " inlineVisibility=" + (options.inlineVisibilityPermutation ? "on" : "off")
            + " containerBytes="
            + std::to_string(preparedBytecode.containerByteCount) + " dxilBytes="
            + std::to_string(library->dxilBytecode->GetBufferSize()) + " extracted="
            + (library->extractedFromDxbcContainer ? "yes" : "no"));
    {
        std::lock_guard<std::mutex> lock(g_dxrShaderCacheMutex);
        const auto existing = g_dxrShaderCache.find(cacheKey);
        if (existing != g_dxrShaderCache.end())
        {
            return existing->second;
        }

        g_dxrShaderCache.emplace(cacheKey, library);
    }
    return library;
}

void DxrShaderCache::Clear()
{
    std::lock_guard<std::mutex> lock(g_dxrShaderCacheMutex);
    g_dxrShaderCache.clear();
}
