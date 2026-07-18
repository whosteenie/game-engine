#pragma once

#include "engine/rendering/post/ScreenSpaceEffects.h"

#include <nlohmann/json_fwd.hpp>

class ScreenSpaceEffects;

namespace ScreenSpaceEffectsSettings
{
    struct LoadedAntiAliasingSettings
    {
        AntiAliasingMode antiAliasingMode = AntiAliasingMode::None;
        int msaaSampleCount = 1;
    };

    nlohmann::json ToJson(const ScreenSpaceEffects& effects);
    void ApplyFromJson(ScreenSpaceEffects& effects, const nlohmann::json& value);

    LoadedAntiAliasingSettings NormalizeAntiAliasingSettings(
        AntiAliasingMode antiAliasingMode,
        int msaaSampleCount);
    LoadedAntiAliasingSettings ResolveLoadedAntiAliasingSettings(const nlohmann::json& rendererValue);
    LoadedAntiAliasingSettings ResolveAntiAliasingDelta(
        const nlohmann::json& effectsValue,
        AntiAliasingMode currentMode,
        int currentMsaaSampleCount);
}
