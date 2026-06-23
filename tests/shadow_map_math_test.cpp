#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <vector>
#include <cstdlib>
#include <iostream>

#include "engine/ShadowMapMath.h"

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

    const std::vector<float> cascadeSplits = ComputeCascadeSplitDistances(3, 0.1f, 100.0f);
    ExpectTrue(cascadeSplits.size() == 4U, "Three cascades should produce four split boundaries");
    ExpectNear(cascadeSplits.front(), 0.1f, 1e-4f, "First cascade split should start at the near plane");
    ExpectNear(cascadeSplits.back(), 100.0f, 1e-4f, "Last cascade split should end at the far plane");
    ExpectTrue(
        cascadeSplits[1] > cascadeSplits[0] && cascadeSplits[2] > cascadeSplits[1],
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

    if (gFailures == 0)
    {
        std::cout << "All shadow map math tests passed.\n";
        return EXIT_SUCCESS;
    }

    std::cerr << gFailures << " test(s) failed.\n";
    return EXIT_FAILURE;
}
