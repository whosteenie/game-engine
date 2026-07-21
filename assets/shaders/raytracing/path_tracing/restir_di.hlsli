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

static const uint kRestirDiSampleInvalid = 0u;
static const uint kRestirDiSampleEmissive = 1u;
static const uint kRestirDiSampleEnvironment = 2u;
static const uint kRestirDiTemporalMCap = 20u;
static const uint kRestirDiTemporalAgeCap = 30u;
// RTXDI's production defaults for spatial neighbor validation. Spatial neighbors are different
// pixels in the current frame, so they must not use the stricter same-pixel temporal reprojection
// thresholds (which turn ordinary perspective depth gradients into moving rejection contours).
static const float kRestirDiSpatialDepthThreshold = 0.1;
static const float kRestirDiSpatialNormalThreshold = 0.5;
// Smooth metals retain the temporally resampled estimate: sparse spatial light selection produces
// a less reconstructable signal than the input on this lobe class.
static const float kRestirDiSpatialMetalRoughnessCutoff = 0.2;
// Replayable light identity for P3. Emissive: index0=light, index1=triangle, uv=barycentric
// randoms. Environment: uv=equirect coordinates. No receiver-side contribution is stored here.
struct RestirDiLightSample
{
    uint sampleType;
    uint index0;
    uint index1;
    uint _pad0;
    float2 uv;
    float2 _pad1;
};

struct RestirDiTemporalReservoir
{
    RestirDiLightSample sample;
    float wSum;
    float targetPdf;
    float W;
    uint M;
    uint age;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

struct RestirDiReservoirSet
{
    RestirDiTemporalReservoir emissive;
    RestirDiTemporalReservoir environment;
};

RestirDiLightSample RestirDiInvalidLightSample()
{
    RestirDiLightSample s;
    s.sampleType = kRestirDiSampleInvalid;
    s.index0 = 0u;
    s.index1 = 0u;
    s._pad0 = 0u;
    s.uv = 0.0.xx;
    s._pad1 = 0.0.xx;
    return s;
}

RestirDiTemporalReservoir RestirDiTemporalInit()
{
    RestirDiTemporalReservoir r;
    r.sample = RestirDiInvalidLightSample();
    r.wSum = 0.0;
    r.targetPdf = 0.0;
    r.W = 0.0;
    r.M = 0u;
    r.age = 0u;
    r._pad0 = r._pad1 = r._pad2 = 0u;
    return r;
}

void RestirDiTemporalUpdate(
    inout RestirDiTemporalReservoir r,
    RestirDiLightSample sample,
    float targetPdf,
    float proposalPdf,
    float xi)
{
    r.M += 1u;
    if (targetPdf <= 0.0 || proposalPdf <= 0.0)
    {
        return;
    }
    const float weight = targetPdf / proposalPdf;
    r.wSum += weight;
    if (xi * r.wSum < weight)
    {
        r.sample = sample;
        r.targetPdf = targetPdf;
    }
}

void RestirDiTemporalFinalize(inout RestirDiTemporalReservoir r)
{
    r.W = r.targetPdf > 0.0
        ? r.wSum / (max(float(r.M), 1.0) * r.targetPdf)
        : 0.0;
}

// Reservoir-of-reservoir combine. `targetAtReceiver` is the selected source sample reevaluated in
// the current domain. Source M is confidence, not one candidate, and is preserved through the cap.
bool RestirDiTemporalCombine(
    inout RestirDiTemporalReservoir dst,
    RestirDiTemporalReservoir src,
    float targetAtReceiver,
    float xi)
{
    const uint sourceM = min(src.M, kRestirDiTemporalMCap);
    if (sourceM == 0u)
    {
        return false;
    }

    // Combining reservoirs represents combining their complete candidate streams. Preserve M even
    // when this source's selected sample has zero target at the destination receiver; omitting
    // those zero-weight candidates biases every surviving sample high. This matches
    // RTXDI_InternalSimpleResample, which increments M independently of the RIS weight.
    dst.M += sourceM;
    if (!isfinite(src.W) || !isfinite(targetAtReceiver)
        || src.W <= 0.0 || targetAtReceiver <= 0.0)
    {
        return false;
    }
    const float weight = targetAtReceiver * src.W * float(sourceM);
    if (!isfinite(weight) || weight <= 0.0)
    {
        return false;
    }
    dst.wSum += weight;
    if (xi * dst.wSum < weight)
    {
        dst.sample = src.sample;
        dst.targetPdf = targetAtReceiver;
        dst.age = src.age;
        return true;
    }
    return false;
}

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
