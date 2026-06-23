#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <iostream>

#include "engine/lighting/ShadowMapMath.h"

namespace
{
    int gFailures = 0;

    void ExpectTrue(const bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << "\n";
            ++gFailures;
        }
    }

    void ExpectNear(const float actual, const float expected, const float tolerance, const char* message)
    {
        if (std::abs(actual - expected) > tolerance)
        {
            std::cerr << "FAIL: " << message << " (actual=" << actual << " expected=" << expected << ")\n";
            ++gFailures;
        }
    }
}

int main()
{
    const glm::vec3 boundsMin(-6.0f, -0.01f, -6.0f);
    const glm::vec3 boundsMax(6.0f, 3.0f, 6.0f);
    const glm::vec3 lightDirection(0.3f, -0.8f, 0.2f);

    const ShadowLightSpaceSetup setup = BuildShadowLightSpace(
        lightDirection,
        boundsMin,
        boundsMax,
        4096);

    ExpectTrue(setup.orthoWidth > 0.0f, "Ortho width should be positive");
    ExpectTrue(setup.orthoHeight > 0.0f, "Ortho height should be positive");
    ExpectTrue(setup.texelWorldSizeX > 0.0f, "Texel width should be positive");
    ExpectTrue(setup.texelWorldSizeY > 0.0f, "Texel height should be positive");

    const float texelSpan = std::max(setup.texelWorldSizeX, setup.texelWorldSizeY);
    const float facingLightBias = ComputeShadowBias(1.0f, texelSpan);
    const float grazingLightBias = ComputeShadowBias(0.0f, texelSpan);
    ExpectTrue(
        grazingLightBias > facingLightBias,
        "Shadow bias should increase as the surface turns away from the light");

    const glm::vec3 cubeCenter(0.0f, 1.5f, 0.0f);
    const glm::vec3 cubeShadowNdc = WorldToShadowNdc(setup.lightSpaceMatrix, cubeCenter);
    ExpectTrue(cubeShadowNdc.x >= 0.0f && cubeShadowNdc.x <= 1.0f, "Cube center UV.x should be inside shadow map");
    ExpectTrue(cubeShadowNdc.y >= 0.0f && cubeShadowNdc.y <= 1.0f, "Cube center UV.y should be inside shadow map");
    ExpectTrue(cubeShadowNdc.z >= 0.0f && cubeShadowNdc.z <= 1.0f, "Cube center depth should be inside shadow map");

    const float snapMagnitude = glm::length(setup.snapOffsetNdc);
    ExpectTrue(
        snapMagnitude < (2.0f / 4096.0f),
        "Texel snap offset should be smaller than one shadow texel in NDC");

    const std::vector<float> cascadeSplits = ComputeCascadeSplitDistances(4, 0.1f, 100.0f);
    ExpectTrue(cascadeSplits.size() == 5U, "Four cascades should produce five split boundaries");
    ExpectNear(cascadeSplits.front(), 0.1f, 1e-4f, "First cascade split should start at the near plane");
    ExpectNear(cascadeSplits.back(), 100.0f, 1e-4f, "Last cascade split should end at the far plane");
    ExpectTrue(
        cascadeSplits[1] > cascadeSplits[0] && cascadeSplits[2] > cascadeSplits[1] &&
            cascadeSplits[3] > cascadeSplits[2],
        "Cascade split distances should be monotonically increasing");

    const glm::mat4 inverseView = glm::inverse(glm::lookAt(
        glm::vec3(0.0f, 2.0f, 6.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f)));
    const std::array<glm::vec3, 8> nearFrustumCorners = ComputeCascadeFrustumCorners(
        inverseView,
        16.0f / 9.0f,
        45.0f,
        cascadeSplits[0],
        cascadeSplits[1]);
    const std::array<glm::vec3, 8> farFrustumCorners = ComputeCascadeFrustumCorners(
        inverseView,
        16.0f / 9.0f,
        45.0f,
        cascadeSplits[2],
        cascadeSplits[3]);

    const ShadowLightSpaceSetup nearCascadeSetup = BuildShadowLightSpace(
        lightDirection,
        ComputeBoundsMin(nearFrustumCorners),
        ComputeBoundsMax(nearFrustumCorners),
        4096);
    const ShadowLightSpaceSetup farCascadeSetup = BuildShadowLightSpace(
        lightDirection,
        ComputeBoundsMin(farFrustumCorners),
        ComputeBoundsMax(farFrustumCorners),
        4096);

    ExpectTrue(
        nearCascadeSetup.texelWorldSizeX < farCascadeSetup.texelWorldSizeX,
        "Near cascade texels should be smaller than the farthest cascade");

    const glm::vec3 casterBoundsMin(-20.0f, 0.0f, -20.0f);
    const glm::vec3 casterBoundsMax(20.0f, 10.0f, 20.0f);
    const ShadowLightSpaceSetup frustumOnlyXySetup = BuildShadowLightSpaceForFrustumCorners(
        lightDirection,
        nearFrustumCorners,
        4096,
        0.03f,
        0.12f,
        &casterBoundsMin,
        &casterBoundsMax,
        true);
    const ShadowLightSpaceSetup frustumPlusCasterXySetup = BuildShadowLightSpaceForFrustumCorners(
        lightDirection,
        nearFrustumCorners,
        4096,
        0.03f,
        0.12f,
        &casterBoundsMin,
        &casterBoundsMax,
        false);

    ExpectTrue(
        frustumOnlyXySetup.orthoWidth <= frustumPlusCasterXySetup.orthoWidth + 1e-3f,
        "Frustum-only XY fit should not enlarge ortho width versus frustum + caster fit");
    ExpectTrue(
        frustumOnlyXySetup.texelWorldSizeX <= frustumPlusCasterXySetup.texelWorldSizeX + 1e-6f,
        "Frustum-only XY fit should produce equal or smaller texels");

    const glm::vec3 floorPoint(0.0f, 0.0f, 0.0f);
    float stableHalfExtent = 0.0f;
    glm::vec2 stableCenterLight(0.0f);
    float stableOrthoNear = 0.0f;
    float stableOrthoFar = 0.0f;

    const glm::mat4 nearCameraView = glm::lookAt(
        glm::vec3(0.0f, 2.0f, 6.0f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 dollyCameraView = glm::lookAt(
        glm::vec3(0.0f, 2.0f, 4.5f),
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(0.0f, 1.0f, 0.0f));

    const std::array<glm::vec3, 8> dollyFrustumCorners = ComputeCascadeFrustumCorners(
        glm::inverse(dollyCameraView),
        16.0f / 9.0f,
        45.0f,
        cascadeSplits[0],
        cascadeSplits[1]);

    const ShadowLightSpaceSetup stablePassSetup = BuildShadowLightSpaceForFrustumCorners(
        lightDirection,
        nearFrustumCorners,
        4096,
        0.03f,
        0.12f,
        &casterBoundsMin,
        &casterBoundsMax,
        true,
        nullptr,
        &stableHalfExtent,
        &stableCenterLight,
        &stableOrthoNear,
        &stableOrthoFar,
        true);
    const ShadowLightSpaceSetup stableRetainedSetup = BuildShadowLightSpaceForFrustumCorners(
        lightDirection,
        nearFrustumCorners,
        4096,
        0.03f,
        0.12f,
        &casterBoundsMin,
        &casterBoundsMax,
        true,
        nullptr,
        &stableHalfExtent,
        &stableCenterLight,
        &stableOrthoNear,
        &stableOrthoFar,
        false);

    const glm::vec3 floorShadowNdcStart = WorldToShadowNdc(stablePassSetup.lightSpaceMatrix, floorPoint);
    const glm::vec3 floorShadowNdcEnd = WorldToShadowNdc(stableRetainedSetup.lightSpaceMatrix, floorPoint);
    ExpectNear(
        floorShadowNdcStart.x,
        floorShadowNdcEnd.x,
        1e-5f,
        "Floor shadow UV.x should stay stable when the frustum is unchanged");
    ExpectNear(
        floorShadowNdcStart.y,
        floorShadowNdcEnd.y,
        1e-5f,
        "Floor shadow UV.y should stay stable when the frustum is unchanged");

    const ShadowLightSpaceSetup dollyFrustumSetup = BuildShadowLightSpaceForFrustumCorners(
        lightDirection,
        dollyFrustumCorners,
        4096,
        0.03f,
        0.12f,
        &casterBoundsMin,
        &casterBoundsMax,
        true,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        true);
    ExpectNear(
        stablePassSetup.lightView[0][0],
        dollyFrustumSetup.lightView[0][0],
        1e-5f,
        "Light view should not change when the camera dollies");
    ExpectNear(
        stablePassSetup.lightView[3][0],
        dollyFrustumSetup.lightView[3][0],
        1e-5f,
        "Light view translation should stay anchored to world origin");

    glm::vec3 intersectionMin;
    glm::vec3 intersectionMax;
    ExpectTrue(
        ComputeBoundsIntersection(
            glm::vec3(-2.0f, 0.0f, -2.0f),
            glm::vec3(2.0f, 4.0f, 2.0f),
            glm::vec3(0.0f, 0.0f, 0.0f),
            glm::vec3(6.0f, 2.0f, 6.0f),
            intersectionMin,
            intersectionMax),
        "Overlapping bounds should intersect");
    ExpectNear(intersectionMin.x, 0.0f, 1e-4f, "Intersection min X");
    ExpectNear(intersectionMax.y, 2.0f, 1e-4f, "Intersection max Y");

    if (gFailures == 0)
    {
        std::cout << "All shadow map math tests passed.\n";
        return EXIT_SUCCESS;
    }

    std::cerr << gFailures << " test(s) failed.\n";
    return EXIT_FAILURE;
}
