#include "engine/rendering/DxrSettings.h"

void RunDxrSettingsTests(int& failures)
{
    auto expectTrue = [&failures](const bool condition, const char* message) {
        if (!condition)
        {
            ++failures;
        }
    };

    DxrSettings settings;
    settings.SetEnabled(true);
    settings.SetReflectionsEnabled(true);
    settings.ClampToHardwareCapabilities(false);
    expectTrue(!settings.IsEnabled(), "enabled forced off when RT unsupported");
    expectTrue(!settings.IsReflectionsEnabled(), "reflections forced off when RT unsupported");

    settings.SetEnabled(true);
    settings.SetReflectionsSamplesPerPixel(32);
    settings.SetMaxTraceDistance(1000.0f);
    settings.SetTemporalBlend(2.0f);
    settings.ClampToHardwareCapabilities(true);
    expectTrue(settings.IsEnabled(), "enabled preserved when RT supported");
    expectTrue(settings.GetReflectionsSamplesPerPixel() == 16, "samples clamped to 16");
    expectTrue(settings.GetMaxTraceDistance() == 500.0f, "max trace distance clamped to 500");
    expectTrue(settings.GetTemporalBlend() == 0.99f, "temporal blend clamped to 0.99");
}
