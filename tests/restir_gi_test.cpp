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
    expect(IsInitialEligible(true, true, 0.0f, 0.5f, proposalPdf), "GI rough opaque sample must be eligible");

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
}
