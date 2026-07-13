#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// CPU mirror of assets/shaders/dxr/restir_di.hlsli (ReSTIR DI initial sampling, roadmap P2).
// Keep the resampling math in lockstep with the HLSL — the expected-value parity, WRS
// proportionality, and unbiasedness gates are proven against this mirror in tests/restir_di_test.cpp.

namespace restir_di
{
    struct Reservoir
    {
        float contribution[3] = {0.0f, 0.0f, 0.0f};
        float direction[3] = {0.0f, 0.0f, 1.0f};
        float distance = 0.0f;
        float targetPdf = 0.0f;
        float wSum = 0.0f;
        std::uint32_t M = 0;
        float W = 0.0f;
    };

    inline float TargetLuminance(const float f[3])
    {
        return std::max(0.2126f * f[0] + 0.7152f * f[1] + 0.0722f * f[2], 0.0f);
    }

    inline Reservoir Init()
    {
        return Reservoir{};
    }

    // Stream one light candidate. contribution = unshadowed f; proposalPdf = p(x) (solid angle);
    // xi in [0,1) drives WRS selection. Every candidate counts toward M.
    inline void Update(
        Reservoir& r,
        const float contribution[3],
        const float direction[3],
        const float distance,
        const float proposalPdf,
        const float xi)
    {
        r.M += 1u;

        const float targetPdf = TargetLuminance(contribution);
        if (targetPdf <= 0.0f || proposalPdf <= 0.0f)
        {
            return;
        }

        const float weight = targetPdf / proposalPdf;
        r.wSum += weight;
        if (r.wSum > 0.0f && xi * r.wSum < weight)
        {
            r.contribution[0] = contribution[0];
            r.contribution[1] = contribution[1];
            r.contribution[2] = contribution[2];
            r.direction[0] = direction[0];
            r.direction[1] = direction[1];
            r.direction[2] = direction[2];
            r.distance = distance;
            r.targetPdf = targetPdf;
        }
    }

    inline void Finalize(Reservoir& r)
    {
        const float m = std::max(static_cast<float>(r.M), 1.0f);
        r.W = (r.targetPdf > 0.0f) ? r.wSum / (m * r.targetPdf) : 0.0f;
    }

    // Final DI radiance for the selected sample after its visibility ray (visibility in [0,1]).
    inline void Shade(const Reservoir& r, const float visibility, float outRadiance[3])
    {
        const float w = (r.W == r.W && r.W > 0.0f) ? r.W : 0.0f;
        outRadiance[0] = r.contribution[0] * visibility * w;
        outRadiance[1] = r.contribution[1] * visibility * w;
        outRadiance[2] = r.contribution[2] * visibility * w;
    }

    inline void CombineTemporal(
        Reservoir& destination,
        const Reservoir& source,
        const float targetAtCurrentReceiver,
        const float xi,
        const std::uint32_t mCap = 20u)
    {
        const std::uint32_t sourceM = std::min(source.M, mCap);
        if (sourceM == 0u)
        {
            return;
        }
        destination.M += sourceM;
        if (!std::isfinite(source.W) || !std::isfinite(targetAtCurrentReceiver)
            || source.W <= 0.0f || targetAtCurrentReceiver <= 0.0f)
        {
            return;
        }
        const float weight = targetAtCurrentReceiver * source.W * static_cast<float>(sourceM);
        if (!std::isfinite(weight) || weight <= 0.0f)
        {
            return;
        }
        destination.wSum += weight;
        if (xi * destination.wSum < weight)
        {
            destination = Reservoir{
                {source.contribution[0], source.contribution[1], source.contribution[2]},
                {source.direction[0], source.direction[1], source.direction[2]},
                source.distance,
                targetAtCurrentReceiver,
                destination.wSum,
                destination.M,
                0.0f};
        }
    }

    inline void CapAndFinalizeTemporal(Reservoir& reservoir, const std::uint32_t mCap = 20u)
    {
        if (reservoir.M > mCap)
        {
            reservoir.wSum *= static_cast<float>(mCap) / static_cast<float>(reservoir.M);
            reservoir.M = mCap;
        }
        Finalize(reservoir);
    }

    // RTXDI BASIC source-mixture correction. The selected sample is evaluated at the current and
    // previous receivers; selectedFromPrevious identifies the source reservoir that selected it.
    inline void FinalizeTemporalBasic(
        Reservoir& reservoir,
        const float currentTarget,
        const float previousTarget,
        const float currentM,
        const float previousM,
        const bool selectedFromPrevious)
    {
        const float pi = selectedFromPrevious ? previousTarget : currentTarget;
        const float piSum = currentTarget * currentM + previousTarget * previousM;
        const float denominator = reservoir.targetPdf * piSum;
        reservoir.W = denominator > 0.0f && std::isfinite(denominator) && std::isfinite(pi)
            ? reservoir.wSum * pi / denominator
            : 0.0f;
    }

    inline void FinalizeSpatialBasic(
        Reservoir& reservoir,
        const float selectedSourceTarget,
        const float sourceTargetTimesMSum)
    {
        const float denominator = reservoir.targetPdf * sourceTargetTimesMSum;
        reservoir.W = denominator > 0.0f && std::isfinite(denominator)
            && std::isfinite(selectedSourceTarget)
            ? reservoir.wSum * selectedSourceTarget / denominator
            : 0.0f;
    }
} // namespace restir_di
