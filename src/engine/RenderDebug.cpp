#include "engine/RenderDebug.h"

const char* RenderDebugModeLabel(RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::None:
        return "None (final image)";
    case RenderDebugMode::ShadowFactor:
        return "Shadow factor (PBR)";
    case RenderDebugMode::DirectLighting:
        return "Direct lighting (PBR)";
    case RenderDebugMode::AmbientIbl:
        return "Ambient / IBL (PBR)";
    case RenderDebugMode::LightSpaceUv:
        return "Light-space UV (PBR)";
    case RenderDebugMode::LightSpaceDepth:
        return "Light-space depth (PBR)";
    case RenderDebugMode::Ssao:
        return "SSAO buffer";
    case RenderDebugMode::ContactShadows:
        return "Contact shadows buffer";
    case RenderDebugMode::CompositeOcclusion:
        return "Composite occlusion (SSAO + contact)";
    default:
        return "Unknown";
    }
}
