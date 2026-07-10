#include "engine/scene/RotationUtils.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace
{
    int gFailures = 0;

    void ExpectNear(const float actual, const float expected, const float tolerance, const char* message)
    {
        if (std::abs(actual - expected) > tolerance)
        {
            std::cerr << "FAIL: " << message << " (actual=" << actual << " expected=" << expected << ")\n";
            ++gFailures;
        }
    }

    void ExpectVec3Near(const glm::vec3& actual, const glm::vec3& expected, float tolerance, const char* message)
    {
        ExpectNear(actual.x, expected.x, tolerance, message);
        ExpectNear(actual.y, expected.y, tolerance, message);
        ExpectNear(actual.z, expected.z, tolerance, message);
    }

    void RunCameraAlignBasisTests()
    {
        const glm::vec3 eye(3.0f, 5.0f, 7.0f);
        const glm::vec3 target(0.0f, 1.0f, 0.0f);
        const glm::mat4 viewMatrix = glm::lookAtLH(eye, target, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::mat4 inverseViewMatrix = glm::inverse(viewMatrix);
        const glm::vec3 expectedForward = glm::normalize(target - eye);
        const glm::vec3 expectedUp = glm::normalize(glm::vec3(inverseViewMatrix[1]));

        const glm::mat4 worldMatrix =
            RotationUtils::BuildCameraObjectWorldMatrixFromEditorViewInverse(inverseViewMatrix);

        glm::vec3 position{};
        glm::vec3 forward{};
        glm::vec3 up{};
        RotationUtils::ExtractCameraBasis(worldMatrix, position, forward, up);

        ExpectVec3Near(position, eye, 1e-4f, "Aligned camera position should match editor eye position");
        ExpectVec3Near(forward, expectedForward, 1e-4f, "Aligned camera forward should match editor view direction");
        ExpectVec3Near(up, expectedUp, 1e-4f, "Aligned camera up should match editor up");
    }
}

void RunRotationUtilsTests(int& failures)
{
    RunCameraAlignBasisTests();
    failures += gFailures;
}
