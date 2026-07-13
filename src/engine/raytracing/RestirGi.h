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

inline Float3 operator-(const Float3 a, const Float3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline float Dot(const Float3 a, const Float3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline float Length(const Float3 v)
{
    return std::sqrt(Dot(v, v));
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


// Ouyang/RTXDI reconnection mapping from a previous primary receiver to the current receiver.
// Values outside the production support window are rejected, not silently clamped into history.
inline float TemporalJacobian(
    const Float3 secondaryPosition,
    const Float3 secondaryNormal,
    const Float3 previousPrimary,
    const Float3 currentPrimary)
{
    const Float3 toPrevious = previousPrimary - secondaryPosition;
    const Float3 toCurrent = currentPrimary - secondaryPosition;
    const float previousDistance = Length(toPrevious);
    const float currentDistance = Length(toCurrent);
    if (previousDistance <= 1e-4f || currentDistance <= 1e-4f)
    {
        return 0.0f;
    }
    const float previousCos = Dot(secondaryNormal, toPrevious) / previousDistance;
    const float currentCos = Dot(secondaryNormal, toCurrent) / currentDistance;
    if (previousCos <= 1e-4f || currentCos <= 1e-4f)
    {
        return 0.0f;
    }
    const float jacobian = (currentCos / previousCos)
        * ((previousDistance * previousDistance) / (currentDistance * currentDistance));
    return std::isfinite(jacobian) && jacobian >= 1.0f / 16.0f && jacobian <= 4.0f
        ? jacobian
        : 0.0f;
}

inline float FinalizeTemporalBasic(
    const float streamedWeightSum,
    const float selectedTargetAtCurrent,
    const float selectedTargetAtPrevious,
    const float freshM,
    const float previousM,
    const bool selectedPrevious)
{
    const float pi = selectedPrevious ? selectedTargetAtPrevious : selectedTargetAtCurrent;
    const float piSum = selectedTargetAtCurrent * freshM
        + selectedTargetAtPrevious * previousM;
    const float denominator = piSum * selectedTargetAtCurrent;
    return denominator > 0.0f && std::isfinite(denominator) && std::isfinite(pi)
        ? streamedWeightSum * pi / denominator
        : 0.0f;
}
} // namespace restir::gi
