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
    case RenderDebugMode::SsgiTraceHitMask:
        return "SSGI trace hit mask";
    case RenderDebugMode::SsgiTraceHitDistance:
        return "SSGI trace confidence";
    case RenderDebugMode::SsgiFinalContribution:
        return "SSGI final contribution";
    case RenderDebugMode::SsrSceneColor:
        return "SSR scene color";
    case RenderDebugMode::SsrSceneValidity:
        return "SSR scene validity";
    case RenderDebugMode::SsrTraceRaw:
        return "SSR trace raw";
    case RenderDebugMode::SsrTraceConfidence:
        return "SSR trace confidence";
    case RenderDebugMode::SsrDenoiseSpatial:
        return "SSR denoise spatial";
    case RenderDebugMode::SsrDenoiseTemporal:
        return "SSR denoise temporal";
    case RenderDebugMode::SsrDenoiseFinal:
        return "SSR denoise final";
    case RenderDebugMode::SsrSvgfVariance:
        return "SSR SVGF variance";
    case RenderDebugMode::SsrUpscaled:
        return "SSR upscaled";
    case RenderDebugMode::SsrSpecReplacement:
        return "SSR spec replacement";
    case RenderDebugMode::RtDispatchSmoke:
        return "RT dispatch smoke";
    case RenderDebugMode::RtPrimaryHit:
        return "RT primary hit";
    case RenderDebugMode::RtPrimaryDepth:
        return "RT primary depth";
    case RenderDebugMode::RtPrimaryNormal:
        return "RT primary normal";
    case RenderDebugMode::RtReflectionRaw:
        return "RT reflection raw";
    case RenderDebugMode::RtReflectionConfidence:
        return "RT reflection hit distance";
    case RenderDebugMode::RtReflectionDenoised:
        return "RT reflection denoised";
    case RenderDebugMode::RtSpecReplacement:
        return "RT spec replacement";
    case RenderDebugMode::RtShadowRaw:
        return "RT shadow raw";
    case RenderDebugMode::RtShadowDenoised:
        return "RT shadow denoised";
    case RenderDebugMode::RtGiRaw:
        return "RT GI raw";
    case RenderDebugMode::RtGiDenoised:
        return "RT GI denoised";
    case RenderDebugMode::RtGiInject:
        return "RT GI inject";
    case RenderDebugMode::RrDiffuseAlbedo:
        return "RR guide: diffuse albedo";
    case RenderDebugMode::RrSpecularAlbedo:
        return "RR guide: specular albedo";
    case RenderDebugMode::RrNormalRoughness:
        return "RR guide: normal-roughness";
    case RenderDebugMode::PtIsolateDirectSun:
        return "PT isolate: direct sun";
    case RenderDebugMode::PtIsolateDirectEmissive:
        return "PT isolate: emissive NEE";
    case RenderDebugMode::PtIsolateSurfaceEmissive:
        return "PT isolate: surface emissive";
    case RenderDebugMode::PtIsolateAmbient:
        return "PT isolate: SH ambient";
    case RenderDebugMode::PtIsolateAoVisibility:
        return "PT isolate: AO visibility";
    case RenderDebugMode::PtIsolateSunVisibility:
        return "PT isolate: sun visibility";
    case RenderDebugMode::PtIsolateIndirect:
        return "PT isolate: indirect only";
    case RenderDebugMode::PtIsolatePreClamp:
        return "PT isolate: pre-clamp radiance";
    case RenderDebugMode::PtIsolateSpecHitDist:
        return "PT isolate: spec hit distance";
    case RenderDebugMode::PtRestirLinearDepth:
        return "PT ReSTIR surface: linear depth";
    case RenderDebugMode::PtRestirGeometricNormal:
        return "PT ReSTIR surface: geometric normal";
    case RenderDebugMode::PtRestirMaterialId:
        return "PT ReSTIR surface: material ID";
    case RenderDebugMode::PtRestirLobeClass:
        return "PT ReSTIR surface: lobe class";
    case RenderDebugMode::PtRestirReservoirM: return "PT ReSTIR DI: reservoir M";
    case RenderDebugMode::PtRestirReservoirAge: return "PT ReSTIR DI: reservoir age";
    case RenderDebugMode::PtRestirChosenSource: return "PT ReSTIR DI: chosen source";
    case RenderDebugMode::PtRestirTemporalRejection: return "PT ReSTIR DI: temporal rejection";
    case RenderDebugMode::PtRestirSpatialSource: return "PT ReSTIR DI: spatial source";
    case RenderDebugMode::PtRestirSpatialRejection: return "PT ReSTIR DI: spatial rejection";
    case RenderDebugMode::PtRestirGiReservoirM: return "PT ReSTIR GI: reservoir M";
    case RenderDebugMode::PtRestirGiReservoirAge: return "PT ReSTIR GI: reservoir age";
    case RenderDebugMode::PtRestirGiChosenSource: return "PT ReSTIR GI: chosen source";
    case RenderDebugMode::PtRestirGiTemporalRejection: return "PT ReSTIR GI: temporal rejection";
    case RenderDebugMode::PtRestirGiUcw: return "PT ReSTIR GI: UCW / weight";
    case RenderDebugMode::PtRestirGiContribution: return "PT ReSTIR GI: contribution";
    case RenderDebugMode::PtRestirGiReuseMinusFresh: return "PT ReSTIR GI: reuse - fresh (bias)";
    case RenderDebugMode::PtRestirGiReusedRadiance: return "PT ReSTIR GI: reused radiance";
    case RenderDebugMode::PtEnvDiProbeSampling: return "PT perf probe: env DI sampling";
    case RenderDebugMode::PtEnvDiProbeBsdfMis: return "PT perf probe: env DI sampling + BSDF/MIS";
    case RenderDebugMode::PtEnvDiProbeCandidate: return "PT perf probe: env DI candidate (no reservoir)";
    case RenderDebugMode::PtEnvDiProbeRadiance: return "PT perf probe: env DI radiance (no metadata/reservoir)";
    case RenderDebugMode::PtEnvDiProbeMetadata: return "PT perf probe: env DI metadata (no radiance/reservoir)";
    case RenderDebugMode::PtTemporalRelativeSigma:
        return "PT temporal: relative sigma";
    case RenderDebugMode::PtTemporalFrameDelta:
        return "PT temporal: frame delta";
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
    case RenderDebugMode::SsgiTraceHitMask:
    case RenderDebugMode::SsgiTraceHitDistance:
    case RenderDebugMode::SsgiFinalContribution:
        return true;
    default:
        return false;
    }
}

bool IsSsrDebugMode(const RenderDebugMode mode)
{
    return IsSsrSceneDebugMode(mode) || IsSsrTraceDebugMode(mode) || IsSsrDenoiseDebugMode(mode)
        || IsSsrCompositeDebugMode(mode);
}

bool IsSsrSceneDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsrSceneColor:
    case RenderDebugMode::SsrSceneValidity:
        return true;
    default:
        return false;
    }
}

bool IsSsrTraceDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsrTraceRaw:
    case RenderDebugMode::SsrTraceConfidence:
        return true;
    default:
        return false;
    }
}

bool IsSsrDenoiseDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::SsrDenoiseSpatial:
    case RenderDebugMode::SsrDenoiseTemporal:
    case RenderDebugMode::SsrDenoiseFinal:
    case RenderDebugMode::SsrSvgfVariance:
    case RenderDebugMode::SsrUpscaled:
        return true;
    default:
        return false;
    }
}

bool IsSsrCompositeDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::SsrSpecReplacement;
}

bool IsDxrDebugMode(const RenderDebugMode mode)
{
    return mode == RenderDebugMode::RtDispatchSmoke || IsRtPrimaryDebugMode(mode)
        || IsRtReflectionDebugMode(mode) || IsRtShadowDebugMode(mode) || IsRtGiDebugMode(mode);
}

bool IsRtGiDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtGiRaw:
    case RenderDebugMode::RtGiDenoised:
    case RenderDebugMode::RtGiInject:
        return true;
    default:
        return false;
    }
}

bool IsRrGuideDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RrDiffuseAlbedo:
    case RenderDebugMode::RrSpecularAlbedo:
    case RenderDebugMode::RrNormalRoughness:
        return true;
    default:
        return false;
    }
}

bool IsPtIsolateDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtIsolateDirectSun:
    case RenderDebugMode::PtIsolateDirectEmissive:
    case RenderDebugMode::PtIsolateSurfaceEmissive:
    case RenderDebugMode::PtIsolateAmbient:
    case RenderDebugMode::PtIsolateAoVisibility:
    case RenderDebugMode::PtIsolateSunVisibility:
    case RenderDebugMode::PtIsolateIndirect:
    case RenderDebugMode::PtIsolatePreClamp:
    case RenderDebugMode::PtIsolateSpecHitDist:
    case RenderDebugMode::PtRestirLinearDepth:
    case RenderDebugMode::PtRestirGeometricNormal:
    case RenderDebugMode::PtRestirMaterialId:
    case RenderDebugMode::PtRestirLobeClass:
    case RenderDebugMode::PtRestirReservoirM:
    case RenderDebugMode::PtRestirReservoirAge:
    case RenderDebugMode::PtRestirChosenSource:
    case RenderDebugMode::PtRestirTemporalRejection:
    case RenderDebugMode::PtRestirSpatialSource:
    case RenderDebugMode::PtRestirSpatialRejection:
    case RenderDebugMode::PtRestirGiReservoirM:
    case RenderDebugMode::PtRestirGiReservoirAge:
    case RenderDebugMode::PtRestirGiChosenSource:
    case RenderDebugMode::PtRestirGiTemporalRejection:
    case RenderDebugMode::PtRestirGiUcw:
    case RenderDebugMode::PtRestirGiContribution:
    case RenderDebugMode::PtRestirGiReuseMinusFresh:
    case RenderDebugMode::PtRestirGiReusedRadiance:
    case RenderDebugMode::PtEnvDiProbeSampling:
    case RenderDebugMode::PtEnvDiProbeBsdfMis:
    case RenderDebugMode::PtEnvDiProbeCandidate:
    case RenderDebugMode::PtEnvDiProbeRadiance:
    case RenderDebugMode::PtEnvDiProbeMetadata:
        return true;
    default:
        return false;
    }
}

bool IsPtTemporalStatsDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtTemporalRelativeSigma:
    case RenderDebugMode::PtTemporalFrameDelta:
        return true;
    default:
        return false;
    }
}

int PtDebugIsolateModeFromRenderDebug(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::PtIsolateDirectSun: return 1;
    case RenderDebugMode::PtIsolateDirectEmissive: return 2;
    case RenderDebugMode::PtIsolateSurfaceEmissive: return 3;
    case RenderDebugMode::PtIsolateAmbient: return 4;
    case RenderDebugMode::PtIsolateAoVisibility: return 5;
    case RenderDebugMode::PtIsolateSunVisibility: return 6;
    case RenderDebugMode::PtIsolateIndirect: return 7;
    case RenderDebugMode::PtIsolatePreClamp: return 8;
    case RenderDebugMode::PtIsolateSpecHitDist: return 9;
    case RenderDebugMode::PtRestirLinearDepth: return 10;
    case RenderDebugMode::PtRestirGeometricNormal: return 11;
    case RenderDebugMode::PtRestirMaterialId: return 12;
    case RenderDebugMode::PtRestirLobeClass: return 13;
    case RenderDebugMode::PtRestirReservoirM: return 14;
    case RenderDebugMode::PtRestirReservoirAge: return 15;
    case RenderDebugMode::PtRestirChosenSource: return 16;
    case RenderDebugMode::PtRestirTemporalRejection: return 17;
    case RenderDebugMode::PtRestirSpatialSource: return 18;
    case RenderDebugMode::PtRestirSpatialRejection: return 19;
    case RenderDebugMode::PtRestirGiReservoirM: return 20;
    case RenderDebugMode::PtRestirGiReservoirAge: return 21;
    case RenderDebugMode::PtRestirGiChosenSource: return 22;
    case RenderDebugMode::PtRestirGiTemporalRejection: return 23;
    case RenderDebugMode::PtRestirGiUcw: return 24;
    case RenderDebugMode::PtRestirGiContribution: return 25;
    case RenderDebugMode::PtRestirGiReuseMinusFresh: return 26;
    case RenderDebugMode::PtRestirGiReusedRadiance: return 27;
    case RenderDebugMode::PtEnvDiProbeSampling: return 28;
    case RenderDebugMode::PtEnvDiProbeBsdfMis: return 29;
    case RenderDebugMode::PtEnvDiProbeCandidate: return 30;
    case RenderDebugMode::PtEnvDiProbeRadiance: return 31;
    case RenderDebugMode::PtEnvDiProbeMetadata: return 32;
    default: return 0;
    }
}

RenderDebugMode RenderDebugModeFromPtDebugIsolateMode(const int mode)
{
    switch (mode)
    {
    case 28: return RenderDebugMode::PtEnvDiProbeSampling;
    case 29: return RenderDebugMode::PtEnvDiProbeBsdfMis;
    case 30: return RenderDebugMode::PtEnvDiProbeCandidate;
    case 31: return RenderDebugMode::PtEnvDiProbeRadiance;
    case 32: return RenderDebugMode::PtEnvDiProbeMetadata;
    default: return RenderDebugMode::None;
    }
}

bool IsRtShadowDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtShadowRaw:
    case RenderDebugMode::RtShadowDenoised:
        return true;
    default:
        return false;
    }
}

bool IsRtPrimaryDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtPrimaryHit:
    case RenderDebugMode::RtPrimaryDepth:
    case RenderDebugMode::RtPrimaryNormal:
        return true;
    default:
        return false;
    }
}

bool IsRtReflectionDebugMode(const RenderDebugMode mode)
{
    switch (mode)
    {
    case RenderDebugMode::RtReflectionRaw:
    case RenderDebugMode::RtReflectionConfidence:
    case RenderDebugMode::RtReflectionDenoised:
    case RenderDebugMode::RtSpecReplacement:
        return true;
    default:
        return false;
    }
}
