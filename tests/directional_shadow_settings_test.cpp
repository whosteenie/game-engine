#include "engine/lighting/DirectionalShadowSettings.h"

#include <nlohmann/json.hpp>

void RunDirectionalShadowSettingsTests(int& failures)
{
    auto expectTrue = [&failures](const bool condition, const char* message) {
        if (!condition)
        {
            ++failures;
        }
    };

    DirectionalShadowSettings settings;
    settings.SetFilterMode(DirectionalShadowFilterMode::PCSS);
    settings.SetShadowMapResolution(2048);
    settings.SetShadowBlurEnabled(true);
    settings.SetShadowBlurRadius(3.5f);

    DirectionalShadowSettings roundTrip;
    roundTrip.ApplyFromJson(settings.ToJson());
    expectTrue(
        roundTrip.GetFilterMode() == settings.GetFilterMode(),
        "shadow json round-trip filter mode");
    expectTrue(
        roundTrip.GetShadowMapResolution() == settings.GetShadowMapResolution(),
        "shadow json round-trip resolution");
    expectTrue(
        roundTrip.GetShadowBlurEnabled() == settings.GetShadowBlurEnabled(),
        "shadow json round-trip blur enabled");
    expectTrue(
        roundTrip.GetShadowBlurRadius() == settings.GetShadowBlurRadius(),
        "shadow json round-trip blur radius");
}
