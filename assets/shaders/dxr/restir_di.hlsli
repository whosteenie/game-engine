// ReSTIR DI initial sampling, M=1 (restir-production-roadmap.md P2).
//
// Pure resampling math — no scene globals. The caller evaluates, for each direct-light candidate
// (emissive triangle or environment direction), the UNSHADOWED contribution
//     f = BSDF * radiance * geometry * misWeight        (solid-angle measure, visibility EXCLUDED)
// and the proposal pdf p(x) it was drawn from (same solid-angle measure), then streams them here
// via weighted reservoir sampling. After finalize, the caller traces ONE visibility ray for the
// selected winner and shades f_winner * V * W.
//
// Expected-value parity gate (P2): the RIS target is p̂(x) = luminance(f(x)) and candidates use the
// same proposal p the existing NEE uses, so at M=1 the UCW is W = wSum/(1·p̂) = (p̂/p)/p̂ = 1/p and
// the shade is f·V/p — identical to a plain one-sample light-NEE estimator. M>1 keeps that
// expectation while lowering variance (one shadow ray amortized over several light proposals).
// Visibility is kept OUT of the target (validated once on the winner) per the roadmap.
//
// Mirrored byte-for-byte in src/engine/raytracing/RestirDi.h (CPU tests: tests/restir_di_test.cpp).

#ifndef RESTIR_DI_HLSLI
#define RESTIR_DI_HLSLI

struct RestirDiReservoir
{
    float3 contribution; // unshadowed f of the selected candidate (BSDF·radiance·geometry·MIS)
    float3 direction;    // wi toward the selected light sample (final visibility ray direction)
    float distance;      // visibility-ray tMax to the selected sample
    float targetPdf;     // p̂ of the selected candidate = luminance(contribution)
    float wSum;          // running sum of RIS resampling weights
    uint M;              // number of candidates streamed (ALL proposal draws, incl. zero-target)
    float W;             // unbiased contribution weight (UCW), set by RestirDiFinalize
};

float RestirDiTargetLuminance(float3 f)
{
    return max(0.2126 * f.r + 0.7152 * f.g + 0.0722 * f.b, 0.0);
}

RestirDiReservoir RestirDiInit()
{
    RestirDiReservoir r;
    r.contribution = 0.0.xxx;
    r.direction = float3(0.0, 0.0, 1.0);
    r.distance = 0.0;
    r.targetPdf = 0.0;
    r.wSum = 0.0;
    r.M = 0u;
    r.W = 0.0;
    return r;
}

// Stream one light candidate. `contribution` = unshadowed f; `proposalPdf` = p(x) in the same
// (solid-angle) measure; xi in [0,1) drives WRS selection. Every candidate counts toward M — a
// zero-target draw is still a proposal sample and must appear in the 1/M normalization for the
// estimator to stay unbiased.
void RestirDiUpdate(
    inout RestirDiReservoir r,
    float3 contribution,
    float3 direction,
    float distance,
    float proposalPdf,
    float xi)
{
    r.M += 1u;

    const float targetPdf = RestirDiTargetLuminance(contribution);
    if (targetPdf <= 0.0 || proposalPdf <= 0.0)
    {
        return;
    }

    const float weight = targetPdf / proposalPdf;
    r.wSum += weight;
    if (r.wSum > 0.0 && xi * r.wSum < weight)
    {
        r.contribution = contribution;
        r.direction = direction;
        r.distance = distance;
        r.targetPdf = targetPdf;
    }
}

void RestirDiFinalize(inout RestirDiReservoir r)
{
    const float m = max(float(r.M), 1.0);
    r.W = (r.targetPdf > 0.0) ? r.wSum / (m * r.targetPdf) : 0.0;
}

// Final DI radiance for the selected sample after its visibility ray (visibility in [0,1]).
float3 RestirDiShade(RestirDiReservoir r, float visibility)
{
    const float w = (r.W == r.W && r.W > 0.0) ? r.W : 0.0;
    return r.contribution * visibility * w;
}

#endif // RESTIR_DI_HLSLI
