#include "engine/lighting/IrradianceSh.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

namespace
{
    void ExpectNear(
        int& failures,
        const float actual,
        const float expected,
        const float tolerance,
        const char* message)
    {
        if (std::abs(actual - expected) > tolerance)
        {
            std::cerr << "FAIL: " << message << " (actual=" << actual << " expected=" << expected << ")\n";
            ++failures;
        }
    }

    void EvaluateShBasis(const glm::vec3& direction, float basisOut[9])
    {
        const float x = direction.x;
        const float y = direction.y;
        const float z = direction.z;

        basisOut[0] = 0.282095f;
        basisOut[1] = 0.488603f * y;
        basisOut[2] = 0.488603f * z;
        basisOut[3] = 0.488603f * x;
        basisOut[4] = 1.092548f * x * y;
        basisOut[5] = 1.092548f * y * z;
        basisOut[6] = 0.315392f * (3.0f * z * z - 1.0f);
        basisOut[7] = 1.092548f * z * x;
        basisOut[8] = 0.546274f * (x * x - y * y);
    }

    glm::vec3 EvaluateDiffuseIrradianceSh(
        const IrradianceSh9& irradianceSh,
        const glm::vec3& normal)
    {
        const glm::vec3 n = glm::normalize(normal);
        float basis[9];
        EvaluateShBasis(n, basis);

        glm::vec3 irradiance(0.0f);
        for (int coefficientIndex = 0; coefficientIndex < 9; ++coefficientIndex)
        {
            const glm::vec3 coefficient = glm::vec3(
                irradianceSh.coefficients[static_cast<std::size_t>(coefficientIndex)]);
            irradiance += coefficient * basis[coefficientIndex];
        }

        return glm::max(irradiance, glm::vec3(0.0f));
    }
}

void RunIrradianceShTests(int& failures)
{
    constexpr int width = 4;
    constexpr int height = 2;
    std::vector<float> rgba(static_cast<std::size_t>(width * height * 4), 1.0f);

    const IrradianceSh9 irradianceSh = ProjectIrradianceSh9FromEquirect(rgba, width, height);
    const float expectedIrradiance = glm::pi<float>();
    constexpr float tolerance = 0.1f;

    const std::array<glm::vec3, 4> testNormals = {
        glm::vec3(0.0f, 1.0f, 0.0f),
        glm::vec3(1.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f),
        glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)),
    };

    for (std::size_t normalIndex = 0; normalIndex < testNormals.size(); ++normalIndex)
    {
        const glm::vec3 irradiance = EvaluateDiffuseIrradianceSh(irradianceSh, testNormals[normalIndex]);
        ExpectNear(failures, irradiance.r, expectedIrradiance, tolerance, "Uniform white SH9 irradiance R");
        ExpectNear(failures, irradiance.g, expectedIrradiance, tolerance, "Uniform white SH9 irradiance G");
        ExpectNear(failures, irradiance.b, expectedIrradiance, tolerance, "Uniform white SH9 irradiance B");
    }
}
