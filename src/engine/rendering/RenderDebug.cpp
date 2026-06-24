#include "engine/rendering/RenderDebug.h"

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
    case RenderDebugMode::CascadeIndex:
        return "Cascade index (PBR)";
    case RenderDebugMode::GeometricNormal:
        return "Geometric normal (PBR)";
    case RenderDebugMode::TangentHandedness:
        return "Tangent handedness (PBR)";
    case RenderDebugMode::ViewDepth:
        return "View depth (PBR)";
    case RenderDebugMode::CascadeBlendFactor:
        return "Cascade blend factor (PBR)";
    case RenderDebugMode::DiffuseIbl:
        return "Diffuse IBL (geom normal)";
    case RenderDebugMode::SpecularIbl:
        return "Specular IBL (PBR)";
    case RenderDebugMode::DirectDiffuseGeom:
        return "Direct diffuse (geom N·L)";
    case RenderDebugMode::ShadedNormal:
        return "Shaded normal (PBR)";
    case RenderDebugMode::Ssao:
        return "SSAO buffer";
    case RenderDebugMode::CompositeOcclusion:
        return "Composite occlusion (SSAO)";
    default:
        return "Unknown";
    }
}
