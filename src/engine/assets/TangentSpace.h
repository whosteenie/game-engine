#pragma once

#include <glm/glm.hpp>

#include <vector>

namespace TangentSpace
{
    // glTF / MikkTSpace tangent frame prepared for pbr.ps.hlsl:
    // bitangent = cross(normal, tangent.xyz) * tangent.w
    glm::vec4 FinalizeImportTangent(const glm::vec4& tangent);

    void GenerateMikkTSpaceTangents(
        std::vector<float>& vertices,
        const std::vector<unsigned int>& indices);
}
