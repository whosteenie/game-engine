#include "engine/raytracing/RestirTypes.h"

#include <cmath>
#include <iostream>

namespace
{
// Mirrors RestirFinalizeW / RestirUpdate / RestirMergeReservoir (restir_pack.hlsli).
void FinalizeW(RestirReservoir& reservoir, const float pHat)
{
    const float m = std::max(static_cast<float>(reservoir.M), 1.0f);
    reservoir.W = reservoir.wSum / (m * pHat);
}

void Update(RestirReservoir& reservoir, const float weight)
{
    if (weight <= 0.0f)
    {
        return;
    }
    reservoir.wSum += weight;
    reservoir.M += 1u;
}

void Merge(RestirReservoir& dst, const RestirReservoir& src, const float misWeight, const float pHat)
{
    if (src.M == 0u || misWeight <= 0.0f)
    {
        return;
    }
    const float weight = misWeight * src.W * pHat * static_cast<float>(src.M);
    if (weight <= 0.0f)
    {
        return;
    }
    dst.wSum += weight;
    dst.M += src.M;
}
} // namespace

void RunRestirWrsTests(int& failures)
{
    // Identical contribution reservoirs merged with mis=1 must keep W ≈ 1 (passthrough energy).
    {
        const float pHat = 2.5f;
        RestirReservoir merged{};
        Update(merged, pHat);

        RestirReservoir prev{};
        prev.wSum = pHat * 20.0f;
        prev.W = 1.0f;
        prev.M = 20u;

        Merge(merged, prev, 1.0f, pHat);
        FinalizeW(merged, pHat);

        if (std::fabs(merged.W - 1.0f) > 1.0e-4f || merged.M != 21u)
        {
            std::cerr << "FAIL: confidence WRS merge expected W=1 M=21, got W=" << merged.W
                      << " M=" << merged.M << "\n";
            ++failures;
        }
    }

    // Broken temporal MIS (mis=M_i/ΣM then ×M in Merge) drives W to 0.5 at M_prev=1 —
    // kept as a regression fixture so we do not reintroduce it.
    {
        const float pHat = 1.0f;
        const float mPrev = 1.0f;
        const float misNew = 1.0f / (1.0f + mPrev);
        const float misPrev = mPrev / (1.0f + mPrev);

        RestirReservoir merged{};
        Update(merged, misNew * pHat);
        RestirReservoir prev{};
        prev.W = 1.0f;
        prev.M = 1u;
        Merge(merged, prev, misPrev, pHat);
        FinalizeW(merged, pHat);

        if (std::fabs(merged.W - 0.5f) > 1.0e-4f)
        {
            std::cerr << "FAIL: expected broken-MIS regression fixture W=0.5, got " << merged.W
                      << "\n";
            ++failures;
        }
    }
}
