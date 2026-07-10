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

    // Reconnection Jacobian: identical endpoints → |J| = 1; half distance at receiver → 4×.
    {
        const float xs[3] = {0.0f, 0.0f, 0.0f};
        const float ns[3] = {0.0f, 0.0f, 1.0f};
        const float posQ[3] = {0.0f, 0.0f, 2.0f};
        const float posRSame[3] = {0.0f, 0.0f, 2.0f};
        const float posRCloser[3] = {0.0f, 0.0f, 1.0f};

        auto jacobian = [](const float* x, const float* n, const float* q, const float* r) {
            const float toQ[3] = {q[0] - x[0], q[1] - x[1], q[2] - x[2]};
            const float toR[3] = {r[0] - x[0], r[1] - x[1], r[2] - x[2]};
            const float distQ = std::sqrt(toQ[0] * toQ[0] + toQ[1] * toQ[1] + toQ[2] * toQ[2]);
            const float distR = std::sqrt(toR[0] * toR[0] + toR[1] * toR[1] + toR[2] * toR[2]);
            const float cosQ = (toQ[0] * n[0] + toQ[1] * n[1] + toQ[2] * n[2]) / distQ;
            const float cosR = (toR[0] * n[0] + toR[1] * n[1] + toR[2] * n[2]) / distR;
            return (cosR / cosQ) * ((distQ * distQ) / (distR * distR));
        };

        const float jSame = jacobian(xs, ns, posQ, posRSame);
        const float jCloser = jacobian(xs, ns, posQ, posRCloser);
        if (std::fabs(jSame - 1.0f) > 1.0e-4f)
        {
            std::cerr << "FAIL: identical reconnection Jacobian expected 1, got " << jSame << "\n";
            ++failures;
        }
        if (std::fabs(jCloser - 4.0f) > 1.0e-4f)
        {
            std::cerr << "FAIL: half-distance Jacobian expected 4, got " << jCloser << "\n";
            ++failures;
        }
    }
}
