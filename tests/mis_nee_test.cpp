// CPU mirror of S3 MIS helpers in path_tracer.hlsl (BalanceHeuristic, EmissiveNeePdfSolidAngle,
// kDeltaScatterPdf weight-1 emission). Guards devdoc/dxr/pt/pt-audit.md steps 8–9: emitter-hit MIS
// uses consistent solid-angle measures and camera/delta paths must not halve visible emitters (B2).

#include "test_expect.h"

#include <cmath>

namespace
{
    const float kDeltaScatterPdf = 1.0e10f;

    float BalanceHeuristic(float pdfA, float pdfB)
    {
        const float denom = pdfA + pdfB;
        return denom > 1e-6f ? pdfA / denom : 1.0f;
    }

    float EmissiveNeePdfSolidAngle(float pickPdf, float surfaceArea, float dist2, float cosThetaEmitter)
    {
        if (pickPdf <= 0.0f || surfaceArea <= 1e-8f || cosThetaEmitter <= 1e-6f)
        {
            return 0.0f;
        }
        return pickPdf * (1.0f / surfaceArea) * dist2 / cosThetaEmitter;
    }
}

void RunMisNeeTests()
{
    test::ExpectNear(BalanceHeuristic(2.0f, 2.0f), 0.5f, 1e-6f, "BalanceHeuristic symmetric");
    test::ExpectNear(BalanceHeuristic(0.0f, 0.0f), 1.0f, 1e-6f, "BalanceHeuristic zero denom -> 1");

    const float pickPdf = 0.25f;
    const float area = 4.0f;
    const float dist2 = 9.0f;
    const float cosEmitter = 0.8f;
    const float pdfSolid =
        EmissiveNeePdfSolidAngle(pickPdf, area, dist2, cosEmitter);
    test::ExpectNear(pdfSolid, pickPdf * (1.0f / area) * dist2 / cosEmitter, 1e-6f,
        "EmissiveNeePdfSolidAngle area->solid angle");

    test::ExpectNear(EmissiveNeePdfSolidAngle(0.0f, area, dist2, cosEmitter), 0.0f, 1e-6f,
        "EmissiveNeePdfSolidAngle rejects zero pickPdf");
    test::ExpectNear(EmissiveNeePdfSolidAngle(pickPdf, area, dist2, 0.0f), 0.0f, 1e-6f,
        "EmissiveNeePdfSolidAngle rejects grazing emitter");

    // Primary / delta bounce: lastScatterPdf sentinel dominates finite NEE pdf -> weight ~1 (not 0.5).
    const float misPrimary = BalanceHeuristic(kDeltaScatterPdf, pdfSolid);
    test::ExpectNear(misPrimary, 1.0f, 1e-4f, "Primary emitter MIS weight ~1 with kDeltaScatterPdf");

    // Pre-S3 bug: mixing discrete pickPdf with BSDF solid-angle pdf halved emission.
    const float misBroken = BalanceHeuristic(pickPdf, pdfSolid);
    test::ExpectTrue(misBroken < 0.6f,
        "Discrete pickPdf vs solid-angle pdf would halve visible emitters (B2 regression guard)");
}
