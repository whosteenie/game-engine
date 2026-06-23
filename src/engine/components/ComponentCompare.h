#pragma once

#include <cmath>

namespace ComponentCompare
{
    constexpr float kEpsilon = 1e-4f;

    inline bool FloatsEqual(float left, float right)
    {
        return std::fabs(left - right) <= kEpsilon;
    }
}
