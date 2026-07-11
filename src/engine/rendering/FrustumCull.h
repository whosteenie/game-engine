#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cmath>

// Extract the 6 world-space frustum planes from a clip matrix (projection * view). Assumes the engine
// convention: glm column-major matrices and D3D zero-to-one clip depth (glm::perspectiveLH_ZO — see
// Camera::GetProjectionMatrix). Each returned plane is (xyz = inward normal, w = offset), normalized,
// so a point p is inside the frustum when dot(plane.xyz, p) + plane.w >= 0. Matches
// meshlet_cull.hlsli::SphereInsideFrustum. Gribb & Hartmann, "Fast Extraction of Viewing Frustum
// Planes from the WorldViewProjection Matrix".
inline std::array<glm::vec4, 6> ExtractFrustumPlanesZO(const glm::mat4& clip)
{
    // Math rows of the column-major matrix: element(row r, col c) == clip[c][r].
    const glm::vec4 row0(clip[0][0], clip[1][0], clip[2][0], clip[3][0]);
    const glm::vec4 row1(clip[0][1], clip[1][1], clip[2][1], clip[3][1]);
    const glm::vec4 row2(clip[0][2], clip[1][2], clip[2][2], clip[3][2]);
    const glm::vec4 row3(clip[0][3], clip[1][3], clip[2][3], clip[3][3]);

    std::array<glm::vec4, 6> planes = {
        row3 + row0, // left   (x_clip >= -w)
        row3 - row0, // right  (x_clip <=  w)
        row3 + row1, // bottom (y_clip >= -w)
        row3 - row1, // top    (y_clip <=  w)
        row2,        // near   (z_clip >=  0, zero-to-one depth)
        row3 - row2, // far    (z_clip <=  w)
    };

    for (glm::vec4& plane : planes)
    {
        const float length = std::sqrt(plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
        if (length > 1e-8f)
        {
            plane /= length;
        }
    }
    return planes;
}
