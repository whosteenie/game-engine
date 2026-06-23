#include "engine/ShaderCache.h"

#include "engine/Shader.h"

#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex g_shaderCacheMutex;
    std::unordered_map<std::string, std::weak_ptr<Shader>> g_shaderCache;
}

std::string ShaderCache::MakeKey(const char* vertexPath, const char* fragmentPath)
{
    return std::string(vertexPath) + "|" + fragmentPath;
}

std::shared_ptr<Shader> ShaderCache::Load(const char* vertexPath, const char* fragmentPath)
{
    const std::string cacheKey = MakeKey(vertexPath, fragmentPath);

    std::lock_guard<std::mutex> lock(g_shaderCacheMutex);

    const auto existing = g_shaderCache.find(cacheKey);
    if (existing != g_shaderCache.end())
    {
        if (std::shared_ptr<Shader> shader = existing->second.lock())
        {
            return shader;
        }
    }

    std::shared_ptr<Shader> shader = std::make_shared<Shader>(vertexPath, fragmentPath);
    g_shaderCache[cacheKey] = shader;
    return shader;
}
