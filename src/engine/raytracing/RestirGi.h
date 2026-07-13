#pragma once

#include <algorithm>
#include <cmath>

namespace restir::gi
{
struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Float3 operator*(const Float3 a, const Float3 b)
{
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}

inline Float3 operator*(const Float3 a, const float b)
{
    return {a.x * b, a.y * b, a.z * b};
}

// The P5 reservoir stores raw secondary radiance. Its M=1 UCW is explicitly inverse primary
// directional proposal density; the PDF is not embedded in radiance and is not stored ambiguously.
inline float InitialUcw(const float proposalPdf)
{
    return proposalPdf > 0.0f && std::isfinite(proposalPdf) ? 1.0f / proposalPdf : 0.0f;
}

inline Float3 ShadeFresh(
    const Float3 bsdfTimesCos,
    const Float3 incomingRadiance,
    const float ucw,
    const float visibility = 1.0f)
{
    return bsdfTimesCos * incomingRadiance * (std::max(ucw, 0.0f) * std::clamp(visibility, 0.0f, 1.0f));
}

inline bool IsInitialEligible(
    const bool enabled,
    const bool hasSecondary,
    const float transmissionWeight,
    const float roughness,
    const float proposalPdf)
{
    return enabled && hasSecondary && transmissionWeight <= 0.01f && roughness >= 0.2f
        && proposalPdf > 0.0f && proposalPdf < 1.0e9f && std::isfinite(proposalPdf);
}
} // namespace restir::gi
