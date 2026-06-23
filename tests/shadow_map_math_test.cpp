#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
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

    if (gFailures == 0)
    {
        std::cout << "All shadow map math tests passed.\n";
        return EXIT_SUCCESS;
    }

    std::cerr << gFailures << " test(s) failed.\n";
    return EXIT_FAILURE;
}
