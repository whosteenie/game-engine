#include "engine/raytracing/DxrInstanceTransform.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
    void ExpectNear(const float actual, const float expected, const float tolerance, const char* message, int& failures)
    {
        if (std::abs(actual - expected) > tolerance)
        {
            std::cerr << "FAIL: " << message << " (actual=" << actual << " expected=" << expected << ")\n";
            ++failures;
        }
    }

    void ExpectTransformEquals(
        const float actual[12],
        const std::array<float, 12>& expected,
        const char* message,
        int& failures)
    {
        for (int index = 0; index < 12; ++index)
        {
            if (std::abs(actual[index] - expected[index]) > 1e-5f)
            {
                std::cerr << "FAIL: " << message << " at index " << index << " (actual=" << actual[index]
                          << " expected=" << expected[index] << ")\n";
                ++failures;
            }
        }
    }
}

void RunDxrAccelerationStructureTests(int& failures)
{
    {
        float transform[12]{};
        WriteD3D12InstanceTransform(glm::mat4(1.0f), transform);
        const std::array<float, 12> expected = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f};
        ExpectTransformEquals(transform, expected, "DxrInstanceTransform_GlmToD3D12 identity", failures);
    }

    {
        const glm::mat4 translation = glm::translate(glm::mat4(1.0f), glm::vec3(4.0f, 5.0f, 6.0f));
        float transform[12]{};
        WriteD3D12InstanceTransform(translation, transform);
        const std::array<float, 12> expected = {
            1.0f, 0.0f, 0.0f, 4.0f,
            0.0f, 1.0f, 0.0f, 5.0f,
            0.0f, 0.0f, 1.0f, 6.0f};
        ExpectTransformEquals(transform, expected, "DxrInstanceTransform_GlmToD3D12 translation", failures);
    }

    {
        const glm::mat4 rotation =
            glm::rotate(glm::mat4(1.0f), glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
        float transform[12]{};
        WriteD3D12InstanceTransform(rotation, transform);
        const std::array<float, 12> expected = {
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, 0.0f};
        ExpectTransformEquals(transform, expected, "DxrInstanceTransform_GlmToD3D12 rotation Y90", failures);
    }

    {
        const std::vector<std::uint32_t> indexCounts = {12, 36, 6};
        const std::uint64_t total = SumUniqueMeshTriangleCounts(indexCounts);
        if (total != 18u)
        {
            std::cerr << "FAIL: DxrDiagnostics_TriangleSum (actual=" << total << " expected=18)\n";
            ++failures;
        }
    }
}
