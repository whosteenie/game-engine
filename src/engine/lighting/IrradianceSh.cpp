#include "engine/lighting/IrradianceSh.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace
{
    constexpr float kPi = glm::pi<float>();

    float RadicalInverseVdC(std::uint32_t bits)
    {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    glm::vec2 Hammersley(int sampleIndex, int sampleCount)
    {
        return glm::vec2(
            static_cast<float>(sampleIndex) / static_cast<float>(sampleCount),
            RadicalInverseVdC(static_cast<std::uint32_t>(sampleIndex)));
    }

    glm::vec3 UniformSphereSample(const glm::vec2& xi)
    {
        const float phi = glm::two_pi<float>() * xi.x;
        const float cosTheta = 1.0f - 2.0f * xi.y;
        const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
        return glm::vec3(
            std::cos(phi) * sinTheta,
            cosTheta,
            std::sin(phi) * sinTheta);
    }

    glm::vec2 DirectionToEquirectUv(const glm::vec3& direction)
    {
        // Matches ibl_equirect_to_cubemap.ps.hlsl (HDR loaded with stbi flip on).
        glm::vec2 uv(
            std::atan2(direction.z, direction.x),
            std::asin(std::clamp(direction.y, -1.0f, 1.0f)));
        uv.x = uv.x * (1.0f / glm::two_pi<float>()) + 0.5f;
        uv.y = uv.y * (1.0f / kPi) + 0.5f;
        return uv;
    }

    glm::vec3 SampleEquirect(
        const std::vector<float>& rgbaRadiance,
        const int width,
        const int height,
        const glm::vec2& uv)
    {
        if (width <= 0 || height <= 0 || rgbaRadiance.empty())
        {
            return glm::vec3(0.0f);
        }

        const float u = std::clamp(uv.x, 0.0f, 1.0f);
        const float v = std::clamp(uv.y, 0.0f, 1.0f);
        const float x = u * static_cast<float>(width - 1);
        const float y = v * static_cast<float>(height - 1);

        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(x0 + 1, width - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float tx = x - static_cast<float>(x0);
        const float ty = y - static_cast<float>(y0);

        const auto sampleTexel = [&](const int texelX, const int texelY) {
            const std::size_t pixelIndex =
                static_cast<std::size_t>(texelY) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(texelX);
            const std::size_t channelIndex = pixelIndex * 4;
            return glm::vec3(
                rgbaRadiance[channelIndex + 0],
                rgbaRadiance[channelIndex + 1],
                rgbaRadiance[channelIndex + 2]);
        };

        const glm::vec3 c00 = sampleTexel(x0, y0);
        const glm::vec3 c10 = sampleTexel(x1, y0);
        const glm::vec3 c01 = sampleTexel(x0, y1);
        const glm::vec3 c11 = sampleTexel(x1, y1);
        const glm::vec3 c0 = glm::mix(c00, c10, tx);
        const glm::vec3 c1 = glm::mix(c01, c11, tx);
        return glm::mix(c0, c1, ty);
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

    void ApplyCosineLobeConvolution(std::array<glm::vec3, 9>& radianceSh)
    {
        constexpr float kBand0 = kPi;
        constexpr float kBand1 = 2.0f * kPi / 3.0f;
        constexpr float kBand2 = kPi / 4.0f;

        radianceSh[0] *= kBand0;
        for (int coefficientIndex = 1; coefficientIndex <= 3; ++coefficientIndex)
        {
            radianceSh[static_cast<std::size_t>(coefficientIndex)] *= kBand1;
        }
        for (int coefficientIndex = 4; coefficientIndex <= 8; ++coefficientIndex)
        {
            radianceSh[static_cast<std::size_t>(coefficientIndex)] *= kBand2;
        }
    }
}

IrradianceSh9 ProjectIrradianceSh9FromEquirect(
    const std::vector<float>& rgbaRadiance,
    const int width,
    const int height,
    const int sampleCount)
{
    IrradianceSh9 result;
    if (rgbaRadiance.empty() || width <= 0 || height <= 0 || sampleCount <= 0)
    {
        return result;
    }

    std::array<glm::vec3, 9> radianceSh{};
    const float sampleWeight = (4.0f * kPi) / static_cast<float>(sampleCount);

    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        const glm::vec3 direction = UniformSphereSample(Hammersley(sampleIndex, sampleCount));
        const glm::vec3 radiance = SampleEquirect(
            rgbaRadiance,
            width,
            height,
            DirectionToEquirectUv(direction));

        float basis[9];
        EvaluateShBasis(direction, basis);
        for (int coefficientIndex = 0; coefficientIndex < 9; ++coefficientIndex)
        {
            radianceSh[static_cast<std::size_t>(coefficientIndex)] +=
                radiance * basis[coefficientIndex] * sampleWeight;
        }
    }

    ApplyCosineLobeConvolution(radianceSh);

    for (int coefficientIndex = 0; coefficientIndex < 9; ++coefficientIndex)
    {
        const glm::vec3 coefficient = radianceSh[static_cast<std::size_t>(coefficientIndex)];
        result.coefficients[static_cast<std::size_t>(coefficientIndex)] =
            glm::vec4(coefficient, 0.0f);
    }

    return result;
}
