#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

// Writes glm world matrix as D3D12 row-major 3x4 instance transform.
inline void WriteD3D12InstanceTransform(const glm::mat4& worldMatrix, float outTransform[12])
{
    for (int row = 0; row < 3; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            outTransform[row * 4 + col] = worldMatrix[col][row];
        }
    }
}

// Sum of triangle counts for unique meshes (indexCount / 3 per mesh).
inline std::uint64_t SumUniqueMeshTriangleCounts(const std::vector<std::uint32_t>& indexCounts)
{
    std::uint64_t total = 0;
    for (const std::uint32_t indexCount : indexCounts)
    {
        total += static_cast<std::uint64_t>(indexCount) / 3u;
    }

    return total;
}
