// Validates ExtractFrustumPlanesZO (engine/rendering/core/FrustumCull.h) against the engine's actual
// projection convention (glm::perspectiveLH_ZO), and the sphere-vs-frustum test mirrored from
// meshlet_cull.hlsli. This is the mathematically risky half of C5.5 meshlet culling; the AS/MS
// plumbing is mechanical, but a wrong plane sign or depth convention here would silently cull
// visible geometry.

#include "engine/rendering/core/FrustumCull.h"
#include "test_expect.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>

namespace
{
    // CPU mirror of meshlet_cull.hlsli::SphereInsideFrustum.
    bool SphereInsideFrustum(const std::array<glm::vec4, 6>& planes, const glm::vec3& center, float radius)
    {
        for (const glm::vec4& plane : planes)
        {
            if (glm::dot(glm::vec3(plane), center) + plane.w < -radius)
            {
                return false;
            }
        }
        return true;
    }

    // Left-handed zero-to-one projection matching Camera::GetProjectionMatrix. Identity view => the
    // camera sits at the origin looking down +Z.
    glm::mat4 TestClip(float fovDegrees, float aspect, float nearPlane, float farPlane)
    {
        return glm::perspectiveLH_ZO(glm::radians(fovDegrees), aspect, nearPlane, farPlane)
            * glm::mat4(1.0f);
    }
}

void RunFrustumCullTests()
{
    const float fov = 60.0f;
    const float aspect = 16.0f / 9.0f;
    const float nearPlane = 0.1f;
    const float farPlane = 100.0f;
    const std::array<glm::vec4, 6> planes = ExtractFrustumPlanesZO(TestClip(fov, aspect, nearPlane, farPlane));

    // A point straight ahead at mid-depth is inside every plane.
    test::ExpectTrue(
        SphereInsideFrustum(planes, glm::vec3(0.0f, 0.0f, 50.0f), 0.0f),
        "On-axis mid-depth point is inside the frustum");

    // Behind the camera (-Z in this LH setup) fails the near plane.
    test::ExpectTrue(
        !SphereInsideFrustum(planes, glm::vec3(0.0f, 0.0f, -5.0f), 0.0f),
        "Point behind the camera is culled (near plane)");

    // Beyond the far plane is culled.
    test::ExpectTrue(
        !SphereInsideFrustum(planes, glm::vec3(0.0f, 0.0f, farPlane + 10.0f), 0.0f),
        "Point beyond the far plane is culled");

    // Far off to the side at mid-depth is culled by a side plane. Half-width at depth d is
    // tan(fov/2) * d * aspect horizontally.
    const float depth = 50.0f;
    const float halfWidth = std::tan(glm::radians(fov) * 0.5f) * depth * aspect;
    test::ExpectTrue(
        SphereInsideFrustum(planes, glm::vec3(halfWidth * 0.5f, 0.0f, depth), 0.0f),
        "Point within the horizontal half-width is inside");
    test::ExpectTrue(
        !SphereInsideFrustum(planes, glm::vec3(halfWidth * 2.0f, 0.0f, depth), 0.0f),
        "Point well beyond the horizontal half-width is culled");

    // Conservative: a sphere whose CENTER is outside but whose radius reaches the frustum is kept.
    test::ExpectTrue(
        SphereInsideFrustum(planes, glm::vec3(halfWidth * 2.0f, 0.0f, depth), halfWidth * 2.0f),
        "A large sphere straddling the side plane is conservatively kept");

    // A sphere fully outside (center far out, small radius) is still culled.
    test::ExpectTrue(
        !SphereInsideFrustum(planes, glm::vec3(halfWidth * 4.0f, 0.0f, depth), 1.0f),
        "A small sphere fully outside the side plane is culled");

    // Planes are normalized (unit normals) so the signed distance is metric.
    for (const glm::vec4& plane : planes)
    {
        const float normalLength = glm::length(glm::vec3(plane));
        test::ExpectNear(normalLength, 1.0f, 1e-4f, "Frustum plane normal is unit length");
    }
}
