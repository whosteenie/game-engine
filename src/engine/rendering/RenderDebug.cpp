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
        return "Diffuse IBL (shaded normal)";
    case RenderDebugMode::SpecularIbl:
        return "Specular IBL (PBR)";
    case RenderDebugMode::DirectDiffuseGeom:
        return "Direct diffuse (geom N·L)";
    case RenderDebugMode::ShadedNormal:
        return "Shaded normal (PBR)";
    case RenderDebugMode::ShadowFactorUnbiased:
        return "Shadow factor (unbiased)";
    case RenderDebugMode::ShadowMapStoredDepth:
        return "Shadow map stored depth";
    case RenderDebugMode::ShadowDepthSeparation:
        return "Shadow depth separation";
    case RenderDebugMode::Ssao:
        return "SSAO buffer";
    case RenderDebugMode::GtaoRaw:
        return "GTAO raw";
    case RenderDebugMode::GtaoFiltered:
        return "GTAO filtered";
    case RenderDebugMode::CompositeOcclusion:
        return "Composite occlusion (AO)";
    case RenderDebugMode::GeomSunFacing:
        return "Geom sun facing (N·L)";
    case RenderDebugMode::ShadowCompareDepth:
        return "Shadow compare depth";
    case RenderDebugMode::ShadowBlockedCenter:
        return "Shadow blocked (center texel)";
    case RenderDebugMode::MotionVectors:
        return "Motion vectors";
    case RenderDebugMode::GBufferAlbedo:
        return "G-buffer albedo";
    case RenderDebugMode::GBufferRoughness:
        return "G-buffer roughness";
    case RenderDebugMode::GBufferMetallic:
        return "G-buffer metallic";
    case RenderDebugMode::GBufferEmissive:
        return "G-buffer emissive";
    case RenderDebugMode::RadianceBuffer:
        return "Radiance buffer";
    case RenderDebugMode::RadianceValidity:
        return "Radiance validity";
    case RenderDebugMode::RadianceTemporal:
        return "Radiance temporal";
    case RenderDebugMode::GiDisocclusion:
        return "GI disocclusion";
    case RenderDebugMode::RadianceTemporalDelta:
        return "Radiance temporal delta";
    case RenderDebugMode::SsgiTraceRaw:
        return "SSGI trace raw";
    case RenderDebugMode::SsgiDenoiseSpatial:
        return "SSGI denoise spatial";
    case RenderDebugMode::SsgiDenoiseTemporal:
        return "SSGI denoise temporal";
    case RenderDebugMode::SsgiDenoiseFinal:
        return "SSGI denoise final";
    case RenderDebugMode::SsgiInject:
        return "SSGI inject";
    default:
        return "Unknown";
    }
}

bool IsPbrMaterialDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::ShadowFactor:
    case RenderDebugMode::DirectLighting:
    case RenderDebugMode::AmbientIbl:
    case RenderDebugMode::LightSpaceUv:
    case RenderDebugMode::LightSpaceDepth:
    case RenderDebugMode::CascadeIndex:
    case RenderDebugMode::GeometricNormal:
    case RenderDebugMode::TangentHandedness:
    case RenderDebugMode::ViewDepth:
    case RenderDebugMode::CascadeBlendFactor:
    case RenderDebugMode::DiffuseIbl:
    case RenderDebugMode::SpecularIbl:
    case RenderDebugMode::DirectDiffuseGeom:
    case RenderDebugMode::ShadedNormal:
    case RenderDebugMode::ShadowFactorUnbiased:
    case RenderDebugMode::ShadowMapStoredDepth:
    case RenderDebugMode::ShadowDepthSeparation:
    case RenderDebugMode::GeomSunFacing:
    case RenderDebugMode::ShadowCompareDepth:
    case RenderDebugMode::ShadowBlockedCenter:
        return true;
    default:
        return false;
    }
}

bool IsGBufferDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::GBufferAlbedo:
    case RenderDebugMode::GBufferRoughness:
    case RenderDebugMode::GBufferMetallic:
    case RenderDebugMode::GBufferEmissive:
        return true;
    default:
        return false;
    }
}

bool IsRadianceDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RadianceBuffer:
    case RenderDebugMode::RadianceValidity:
        return true;
    default:
        return false;
    }
}

bool IsGiTemporalDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RadianceTemporal:
    case RenderDebugMode::GiDisocclusion:
    case RenderDebugMode::RadianceTemporalDelta:
        return true;
    default:
        return false;
    }
}

bool IsSsgiDenoiseDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsgiTraceRaw:
    case RenderDebugMode::SsgiDenoiseSpatial:
    case RenderDebugMode::SsgiDenoiseTemporal:
    case RenderDebugMode::SsgiDenoiseFinal:
        return true;
    case RenderDebugMode::SsgiInject:
        return true;
    default:
        return false;
    }
}
