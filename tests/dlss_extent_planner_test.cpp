#include "engine/rhi/DlssContext.h"
#include "engine/rendering/ScreenSpaceEffects.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>

namespace
{
struct FakeSdk
{
    std::uint32_t calls = 0;
    bool forceFailure = false;
    bool invalidRange = false;
};

void Expect(const bool condition, int& failures)
{
    if (!condition)
    {
        ++failures;
    }
}

std::uint32_t QualityDivisor(const DlssQuality quality)
{
    switch (quality)
    {
    case DlssQuality::DLAA: return 1;
    case DlssQuality::Quality: return 2;
    case DlssQuality::Balanced: return 2;
    case DlssQuality::Performance: return 2;
    case DlssQuality::UltraPerformance: return 3;
    }
    return 1;
}

bool QueryFakeSdk(
    void* const userData,
    const DlssExtentRecommendationKey& key,
    DlssExtentRecommendation& recommendation,
    std::string& failureReason)
{
    auto& sdk = *static_cast<FakeSdk*>(userData);
    ++sdk.calls;
    if (sdk.forceFailure)
    {
        failureReason = "forced-test-failure";
        return false;
    }

    const std::uint32_t divisor = QualityDivisor(key.quality);
    const DlssExtent recommended{
        key.outputExtent.width / divisor,
        key.outputExtent.height / divisor};
    recommendation = {
        recommended,
        {recommended.width / 2u, recommended.height / 2u},
        key.outputExtent};
    if (sdk.invalidRange)
    {
        recommendation.minimum.width = recommendation.recommended.width + 1u;
    }
    return true;
}

DlssExtentRecommendationKey MakeKey(
    const std::uint32_t viewportId,
    const DlssExtent outputExtent,
    const DlssReconstructionFeature feature,
    const DlssQuality quality)
{
    DlssExtentRecommendationKey key{};
    key.viewportId = viewportId;
    key.outputExtent = outputExtent;
    key.feature = feature;
    key.quality = quality;
    return key;
}
} // namespace

void RunDlssExtentPlannerTests(int& failures)
{
    constexpr std::array<DlssQuality, 5> qualities = {
        DlssQuality::DLAA,
        DlssQuality::Quality,
        DlssQuality::Balanced,
        DlssQuality::Performance,
        DlssQuality::UltraPerformance};
    constexpr std::array<DlssReconstructionFeature, 2> features = {
        DlssReconstructionFeature::SuperResolution,
        DlssReconstructionFeature::RayReconstruction};
    constexpr std::array<DlssExtent, 2> sceneGameExtents = {
        DlssExtent{1280u, 720u},
        DlssExtent{1920u, 1080u}};

    DlssExtentRecommendationCache cache;
    FakeSdk sdk{};

    // S2-P2 is shadow-only: the pre-existing active allocation factors remain unchanged.
    Expect(DlssPresetRenderScale(DlssPreset::Quality) == 0.667f, failures);
    Expect(DlssPresetRenderScale(DlssPreset::Balanced) == 0.58f, failures);
    Expect(DlssPresetRenderScale(DlssPreset::Performance) == 0.5f, failures);
    Expect(DlssPresetRenderScale(DlssPreset::UltraPerformance) == 0.333f, failures);
    Expect(
        std::lround(1920.0f * DlssPresetRenderScale(DlssPreset::Quality)) == 1281,
        failures);

    // Every exposed DLSS/RR x DLAA/SR tuple at two independent Scene/Game viewport sizes.
    for (std::uint32_t viewport = 0; viewport < sceneGameExtents.size(); ++viewport)
    {
        for (const DlssReconstructionFeature feature : features)
        {
            for (const DlssQuality quality : qualities)
            {
                const DlssExtentRecommendationKey key =
                    MakeKey(viewport, sceneGameExtents[viewport], feature, quality);
                const DlssExtentPlanLookup lookup = cache.Plan(key, &QueryFakeSdk, &sdk);
                Expect(!lookup.cacheHit, failures);
                Expect(lookup.plan.IsValid() && lookup.plan.IsSdkRecommendation(), failures);
                Expect(lookup.plan.key == key, failures);
                Expect(
                    lookup.plan.rrNoArbitraryDrs
                        == (feature == DlssReconstructionFeature::RayReconstruction),
                    failures);
                if (quality == DlssQuality::DLAA)
                {
                    Expect(lookup.plan.extent.recommended == key.outputExtent, failures);
                }
            }
        }
    }
    Expect(sdk.calls == 20u && cache.Size() == 20u, failures);

    // Repeating the complete matrix is cache-only.
    for (std::uint32_t viewport = 0; viewport < sceneGameExtents.size(); ++viewport)
    {
        for (const DlssReconstructionFeature feature : features)
        {
            for (const DlssQuality quality : qualities)
            {
                const auto lookup = cache.Plan(
                    MakeKey(viewport, sceneGameExtents[viewport], feature, quality),
                    &QueryFakeSdk,
                    &sdk);
                Expect(lookup.cacheHit, failures);
            }
        }
    }
    Expect(sdk.calls == 20u, failures);

    // Every declared key field produces a distinct recommendation/cache entry.
    DlssExtentRecommendationCache invalidationCache;
    FakeSdk invalidationSdk{};
    const DlssExtentRecommendationKey base = MakeKey(
        7u,
        {1600u, 900u},
        DlssReconstructionFeature::SuperResolution,
        DlssQuality::Quality);
    std::array<DlssExtentRecommendationKey, 6> variants = {base, base, base, base, base, base};
    variants[1].viewportId = 8u;
    variants[2].outputExtent.width = 1601u;
    variants[3].outputExtent.height = 901u;
    variants[4].feature = DlssReconstructionFeature::RayReconstruction;
    variants[5].quality = DlssQuality::Balanced;
    for (const auto& key : variants)
    {
        Expect(
            !invalidationCache.Plan(key, &QueryFakeSdk, &invalidationSdk).cacheHit,
            failures);
    }
    Expect(invalidationSdk.calls == variants.size() && invalidationCache.Size() == variants.size(), failures);
    Expect(invalidationCache.Plan(base, &QueryFakeSdk, &invalidationSdk).cacheHit, failures);
    invalidationCache.Erase(base);
    Expect(!invalidationCache.Plan(base, &QueryFakeSdk, &invalidationSdk).cacheHit, failures);
    invalidationCache.Clear();
    Expect(invalidationCache.Size() == 0u, failures);
    Expect(!invalidationCache.Plan(base, &QueryFakeSdk, &invalidationSdk).cacheHit, failures);

    // Forced failure is explicit. RR uses native fallback only, preserving no-arbitrary-DRS.
    DlssExtentRecommendationCache failureCache;
    FakeSdk failureSdk{};
    failureSdk.forceFailure = true;
    const DlssExtentRecommendationKey rrFailureKey = MakeKey(
        3u,
        {2560u, 1440u},
        DlssReconstructionFeature::RayReconstruction,
        DlssQuality::UltraPerformance);
    const DlssExtentPlanLookup rrFailure =
        failureCache.Plan(rrFailureKey, &QueryFakeSdk, &failureSdk);
    Expect(!rrFailure.plan.IsSdkRecommendation(), failures);
    Expect(rrFailure.plan.fallbackReason == "forced-test-failure", failures);
    Expect(rrFailure.plan.extent.recommended == rrFailureKey.outputExtent, failures);
    Expect(rrFailure.plan.rrNoArbitraryDrs, failures);

    // Ordinary SR fallback is visibly labelled fallback rather than an SDK recommendation.
    const DlssExtentRecommendationKey srFailureKey = MakeKey(
        4u,
        {1920u, 1080u},
        DlssReconstructionFeature::SuperResolution,
        DlssQuality::Performance);
    const DlssExtentPlanLookup srFailure =
        failureCache.Plan(srFailureKey, &QueryFakeSdk, &failureSdk);
    Expect(!srFailure.plan.IsSdkRecommendation(), failures);
    Expect(srFailure.plan.extent.recommended == DlssExtent{960u, 540u}, failures);

    // Contradictory SDK ranges are rejected and surfaced as explicit fallback.
    DlssExtentRecommendationCache invalidRangeCache;
    FakeSdk invalidRangeSdk{};
    invalidRangeSdk.invalidRange = true;
    const DlssExtentPlanLookup invalidRange =
        invalidRangeCache.Plan(base, &QueryFakeSdk, &invalidRangeSdk);
    Expect(!invalidRange.plan.IsSdkRecommendation(), failures);
    Expect(
        invalidRange.plan.fallbackReason == "sdk-recommendation-outside-returned-range",
        failures);
}
