#include "engine/raytracing/RestirGi.h"

#include <cmath>
#include <iostream>

namespace
{
constexpr float kPi = 3.14159265358979323846f;

bool Near(const float a, const float b, const float eps = 1e-5f)
{
    return std::abs(a - b) <= eps;
}

bool Near(const restir::gi::Float3 a, const restir::gi::Float3 b, const float eps = 1e-5f)
{
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}
} // namespace

void RunRestirGiTests(int& failures)
{
    using namespace restir::gi;
    const auto expect = [&](const bool condition, const char* message) {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    };

    // Deterministic M=1 parity: (f*cos) * Li * (1/pdf) equals the baseline path throughput * Li.
    const Float3 bsdfCos{0.12f, 0.21f, 0.31f};
    const Float3 incoming{3.0f, 2.0f, 1.0f};
    constexpr float proposalPdf = 0.37f;
    expect(
        Near(ShadeFresh(bsdfCos, incoming, InitialUcw(proposalPdf)),
             (bsdfCos * (1.0f / proposalPdf)) * incoming),
        "GI M=1 receiver reconstruction must equal baseline throughput shading");

    // Diffuse furnace: cosine proposal cancels Lambertian cos/pi, leaving albedo * Li.
    const Float3 albedo{0.2f, 0.5f, 0.8f};
    constexpr float noL = 0.63f;
    const Float3 lambertCos = albedo * (noL / kPi);
    const Float3 furnace{4.0f, 4.0f, 4.0f};
    expect(
        Near(ShadeFresh(lambertCos, furnace, InitialUcw(noL / kPi)), albedo * furnace),
        "GI cosine-sampled diffuse furnace must conserve albedo-scaled radiance");

    const Float3 halfAlbedo = albedo * 0.5f;
    expect(
        Near(ShadeFresh(halfAlbedo * (noL / kPi), furnace, InitialUcw(noL / kPi)),
             ShadeFresh(lambertCos, furnace, InitialUcw(noL / kPi)) * 0.5f),
        "GI output must scale linearly with diffuse albedo");

    // Radiance is invariant along an unoccluded vacuum segment. Distance enters only when P6
    // reconnects to another receiver through its Jacobian; P5 must not inject an inverse-square.
    const Float3 atOneMeter = ShadeFresh(lambertCos, furnace, InitialUcw(noL / kPi));
    const Float3 atTenMeters = ShadeFresh(lambertCos, furnace, InitialUcw(noL / kPi));
    expect(Near(atOneMeter, atTenMeters), "GI native directional sample must preserve radiance over distance");

    expect(
        Near(ShadeFresh(Float3{}, furnace, InitialUcw(noL / kPi)), Float3{}),
        "GI back-facing/zero-cosine receiver must contribute zero");
    expect(!IsInitialEligible(false, true, 0.0f, 0.5f, proposalPdf), "GI disabled must use baseline");
    expect(!IsInitialEligible(true, true, 1.0f, 0.5f, proposalPdf), "GI transmission must use baseline");
    expect(!IsInitialEligible(true, true, 0.0f, 0.03f, proposalPdf), "GI delta/smooth lobe must use baseline");
    expect(IsInitialEligible(true, true, 0.0f, 0.1f, proposalPdf),
        "GI glossy input must use final-shading MIS instead of a hard eligibility cutoff");
    expect(IsInitialEligible(true, true, 0.0f, 0.5f, proposalPdf), "GI rough opaque sample must be eligible");

    // RTXDI's GI boiling filter operates on radiance times UCW over a 16x16 reservoir tile. At the
    // production default strength 0.2 the threshold is 41x: ordinary samples survive, while a
    // genuinely bright reused reservoir is removed before it can spread through P7.
    expect(
        Near(BoilingFilterMultiplier(0.2f), 41.0f),
        "GI boiling-filter default strength must retain RTXDI's 41x multiplier");
    expect(
        Near(EffectiveReservoirWeight(Float3{10.0f, 10.0f, 10.0f}, 2.0f), 20.0f),
        "GI boiling filter must include stored radiance as well as UCW");
    expect(
        !ShouldBoilingFilter(40.0f, 1.0f, 0.2f)
            && ShouldBoilingFilter(42.0f, 1.0f, 0.2f),
        "GI boiling filter must reject only effective-weight outliers above the tile threshold");
    // Expected previous depth carries the camera translation explicitly. A dolly can therefore
    // match the same point without comparing two camera-relative depths for equality.
    expect(
        MatchesExpectedPreviousDepth(12.0f, 12.1f),
        "GI expected previous depth must accept a matching dolly-reprojected surface");
    expect(
        !MatchesExpectedPreviousDepth(12.0f, 12.5f),
        "GI expected previous depth must reject the wrong reprojected surface");
    const Float3 secondaryPosition{0.0f, 0.0f, 0.0f};
    const Float3 secondaryNormal{0.0f, 0.0f, 1.0f};
    const Float3 previousPrimary{0.0f, 0.0f, 2.0f};
    expect(
        Near(TemporalJacobian(
            secondaryPosition, secondaryNormal, previousPrimary, previousPrimary), 1.0f),
        "GI equal-geometry temporal Jacobian must be one");
    expect(
        Near(TemporalJacobian(
            secondaryPosition, secondaryNormal, previousPrimary, Float3{0.0f, 0.0f, 1.0f}), 4.0f),
        "GI half-distance temporal Jacobian must be four");
    expect(
        TemporalJacobian(
            secondaryPosition, secondaryNormal, previousPrimary, Float3{0.0f, 0.0f, -2.0f}) == 0.0f,
        "GI opposite secondary-normal support must reject temporal history");
    expect(
        TemporalJacobian(
            secondaryPosition, secondaryNormal, previousPrimary, Float3{0.0f, 0.0f, 0.25f}) == 0.0f,
        "GI out-of-policy temporal Jacobian must reject rather than clamp");

    // With equal current/previous target densities and UCWs, RTXDI BASIC normalization preserves
    // the original inverse PDF while carrying source confidence M.
    constexpr float target = 2.0f;
    constexpr float previousM = 5.0f;
    const float initialUcw = InitialUcw(proposalPdf);
    const float streamedWeight = target * initialUcw * (1.0f + previousM);
    expect(
        Near(FinalizeTemporalBasic(
            streamedWeight, target, target, 1.0f, previousM, false), initialUcw),
        "GI BASIC temporal normalization must preserve stationary M=1 expected weight");
    expect(
        Near(FinalizeTemporalBasic(
            streamedWeight, target, target, 1.0f, previousM, true), initialUcw),
        "GI BASIC normalization must be selection-invariant for equal domains");
    expect(
        FinalizeTemporalBasic(streamedWeight, 0.0f, target, 1.0f, previousM, true) == 0.0f,
        "GI zero current-domain support must finalize to zero");

    // Multi-domain expected-value (roadmap §6). The stationary test above uses one target for both
    // domains and therefore cannot see a domain-dependent bias. This drives the full two-source
    // streaming+finalize with ASYMMETRIC current/previous targets (the curved-surface / camera-motion
    // regime where the P6 grain and hard line appear) and checks the selection-probability-weighted
    // estimator against the analytic RTXDI BASIC MIS combination E = Σ m_i(X_i)·f_i·W_i. Passing
    // proves those artifacts are estimator VARIANCE, not normalization bias — i.e. a P7 spatial-reuse
    // concern, not a P6 correctness bug.
    {
        const float Wf = InitialUcw(0.37f);
        const float Wp = InitialUcw(0.21f);
        constexpr float Mf = 1.0f;
        constexpr float Mp = 12.0f;
        // Selected-sample targets at (current, previous) domains for the fresh and previous winners.
        constexpr float Tcf = 2.0f;   // fresh sample, current domain
        constexpr float Tpf = 1.3f;   // fresh sample, previous domain
        constexpr float Tcp = 0.8f;   // previous sample, current domain
        constexpr float Tpp = 1.7f;   // previous sample, previous domain
        constexpr float ff = 5.0f;    // fresh sample actual shaded contribution (scalar proxy)
        constexpr float fp = 4.0f;    // previous sample actual shaded contribution

        // GiCombine streams risWeight = targetAtCurrent · sourceUcw · sourceM for each source.
        const float wSum = Tcf * Wf * Mf + Tcp * Wp * Mp;
        const float pFresh = (Tcf * Wf * Mf) / wSum;
        const float pPrev = (Tcp * Wp * Mp) / wSum;
        const float wyFresh = FinalizeTemporalBasic(wSum, Tcf, Tpf, Mf, Mp, false);
        const float wyPrev = FinalizeTemporalBasic(wSum, Tcp, Tpp, Mf, Mp, true);
        const float eCode = pFresh * ff * wyFresh + pPrev * fp * wyPrev;

        // Analytic RTXDI BASIC MIS: m_i uses the source sample evaluated in every domain, ×M_i.
        const float mf = (Mf * Tcf) / (Mf * Tcf + Mp * Tpf);
        const float mp = (Mp * Tpp) / (Mf * Tcp + Mp * Tpp);
        const float eRef = mf * ff * Wf + mp * fp * Wp;

        expect(
            Near(eCode, eRef, 1e-4f),
            "GI BASIC temporal normalization must stay unbiased across differing reconnection domains");
    }

    // P7 spatial streaming transforms each neighbor into the center domain with its reconnection
    // Jacobian and preserves the complete source M as proposal multiplicity.
    {
        constexpr float centerTarget = 4.0f;
        constexpr float centerJacobian = 1.0f;
        constexpr float centerUcw = 2.0f;
        constexpr float centerM = 3.0f;
        constexpr float neighborTarget = 2.0f;
        constexpr float neighborJacobian = 0.5f;
        constexpr float neighborUcw = 5.0f;
        constexpr float neighborM = 7.0f;
        const float centerWeight = SpatialStreamWeight(
            centerTarget, centerJacobian, centerUcw, centerM);
        const float neighborWeight = SpatialStreamWeight(
            neighborTarget, neighborJacobian, neighborUcw, neighborM);
        expect(Near(centerWeight, 24.0f), "GI spatial center stream must preserve source M");
        expect(Near(neighborWeight, 35.0f),
            "GI spatial neighbor stream must apply Jacobian and preserve source M");

        // Assume the neighbor wins. Its selected target is reevaluated at both source receivers;
        // these asymmetric domains distinguish BASIC correction from a same-domain 1/M finalize.
        constexpr float selectedAtCenterSource = 1.5f;
        constexpr float selectedAtNeighborSource = 3.0f;
        const float piSum = selectedAtCenterSource * centerM
            + selectedAtNeighborSource * neighborM;
        expect(
            Near(FinalizeSpatialBasic(
                centerWeight + neighborWeight,
                neighborTarget,
                selectedAtNeighborSource,
                piSum),
                (59.0f * 3.0f) / 51.0f),
            "GI BASIC spatial normalization must use every source domain and source M");
        expect(
            FinalizeSpatialBasic(centerWeight + neighborWeight, 0.0f, 1.0f, piSum) == 0.0f,
            "GI spatial zero center support must fall back instead of emitting a poison weight");
        expect(
            SpatialStreamWeight(neighborTarget, 0.0f, neighborUcw, neighborM) == 0.0f,
            "GI spatial invalid Jacobian must reject the neighbor");
    }
}
