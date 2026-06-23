#pragma once

enum class RenderDebugMode
{
    None = 0,
    ShadowFactor,
    DirectLighting,
    AmbientIbl,
    LightSpaceUv,
    LightSpaceDepth,
    CascadeIndex,
    Ssao,
    CompositeOcclusion,
};

const char* RenderDebugModeLabel(RenderDebugMode mode);
