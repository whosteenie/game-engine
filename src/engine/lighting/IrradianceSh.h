#pragma once

#include <array>
#include <glm/glm.hpp>
#include <vector>

// L2 (9 coefficient) spherical harmonics irradiance for diffuse IBL.
struct IrradianceSh9
{
    std::array<glm::vec4, 9> coefficients{};
};

IrradianceSh9 ProjectIrradianceSh9FromEquirect(
    const std::vector<float>& rgbaRadiance,
    int width,
    int height,
    int sampleCount = 8192);
