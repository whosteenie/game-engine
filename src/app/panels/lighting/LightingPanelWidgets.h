#pragma once

#include "engine/rendering/core/RenderDebug.h"

class SceneRenderer;
class ScreenSpaceEffects;

namespace LightingPanelWidgets
{
    const char* RenderDebugModeHelpText(RenderDebugMode mode);
    bool IsDebugViewModeAvailable(
        RenderDebugMode mode,
        const SceneRenderer& renderer,
        const ScreenSpaceEffects& screenSpaceEffects);
    const char* DebugViewModeUnavailableReason(
        RenderDebugMode mode,
        const SceneRenderer& renderer,
        const ScreenSpaceEffects& screenSpaceEffects);
    bool DrawDebugViewPicker(ScreenSpaceEffects& screenSpaceEffects, SceneRenderer& renderer);
}
