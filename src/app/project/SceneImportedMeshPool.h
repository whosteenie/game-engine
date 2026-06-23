#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

class Mesh;

struct ImportMeshKey
{
    std::string assetPath;
    int nodeIndex = -1;

    bool operator==(const ImportMeshKey& other) const
    {
        return nodeIndex == other.nodeIndex && assetPath == other.assetPath;
    }
};

struct ImportMeshKeyHash
{
    std::size_t operator()(const ImportMeshKey& key) const
    {
        return std::hash<std::string>{}(key.assetPath)
            ^ (static_cast<std::size_t>(key.nodeIndex) << 1);
    }
};

using ImportedMeshReusePool =
    std::unordered_map<ImportMeshKey, std::unique_ptr<Mesh>, ImportMeshKeyHash>;
