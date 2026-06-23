#pragma once

enum class RenderDebugMode
{
    None = 0,
    ShadowFactor,
    DirectLighting,
    AmbientIbl,
    LightSpaceUv,
    LightSpaceDepth,
    Ssao,
    ContactShadows,
    CompositeOcclusion,
};

const char* RenderDebugModeLabel(RenderDebugMode mode);
