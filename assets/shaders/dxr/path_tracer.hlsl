// DXR path tracer — Phase P2 core integrator (devdoc/dxr/path-tracing.md).
//
// Megakernel: the raygen owns the bounce loop (throughput + NEE + BRDF sampling + Russian roulette).
// Closest-hit only extracts surface data; shadow and bounce traces originate from raygen so
// MaxTraceRecursionDepth = 1 suffices. P1 direct-only shading is subsumed by the loop.

#include "hit_shading.hlsli"
#include "restir_pack.hlsli"

RWTexture2D<float4> g_Output : register(u0);   // rgb = HDR radiance, a = specular hit-distance guide (RR4)
RWTexture2D<float> g_DepthOutput : register(u1); // hyperbolic depth [0,1] at primary hit (DLSS)
RWTexture2D<uint2> g_Metadata : register(u2);  // (instanceId+1, primitiveIndex)
RWTexture2D<float4> g_MotionOutput : register(u3); // NDC motion (curr - prev) at primary hit, matches RT4
// P4b bounce-0 RR material guides (devdoc/dxr/pt/full-rr-guides.md): the same hit that seeds the
// integrator produces every DLSS-RR guide, so color and guides agree at the sub-pixel level.
RWTexture2D<float4> g_DiffuseAlbedoGuide : register(u4);   // RGBA8: albedo·(1−metallic)
RWTexture2D<float4> g_SpecularAlbedoGuide : register(u5);  // RGBA8: EnvBRDFApprox2(F0, roughness², NoV)
RWTexture2D<float4> g_NormalRoughnessGuide : register(u6); // RGBA16F: world normal xyz + roughness w
// G5/R1: first-indirect-vertex sample + M=1 reservoir (passthrough until R2 reuse).
RWStructuredBuffer<RestirInitialSample> g_InitialSample : register(u7);
RWStructuredBuffer<RestirReservoir> g_ReservoirCurrent : register(u8);
// R2: bounce-0 direct only — temporal shades g_Output = direct + Y·W (never subtract packed Y).
RWTexture2D<float4> g_DirectOutput : register(u9);

// P4b: previous-frame object-to-world rows per compact TLAS InstanceID.
// Explicit rows (row_i = column-major glm m[col][i]) — see DxrPrevInstanceTransformEntry.
struct PrevInstanceTransform
{
    float4 row0;
    float4 row1;
    float4 row2;
};
StructuredBuffer<PrevInstanceTransform> g_PrevInstanceTransforms : register(t14);

// F2/F2b emissive NEE: one entry per emissive instance (S5 step 14 / A1).
struct EmissiveLightEntry
{
    float3 emissive;
    float pickWeight;
    uint instanceId;
    uint triangleOffset;
    uint triangleCount;
    float surfaceArea;
};
StructuredBuffer<EmissiveLightEntry> g_EmissiveLights : register(t15);

struct EmissiveTriangleEntry
{
    float3 v0;
    float pickWeight;
    float3 v1;
    float triangleArea;
    float3 v2;
    float _pad0;
    float3 faceNormal;
    uint primitiveIndex;
};
StructuredBuffer<EmissiveTriangleEntry> g_EmissiveTriangles : register(t18);

#include "pt_env_light.hlsli"

// The shared cbuffer field g_SamplesPerPixel carries the PT max-bounce count (reused verbatim; the
// reflection/GI passes read it as an actual sample count). Alias it for readable PT intent.
#define g_MaxBounces g_SamplesPerPixel

// Path-tracer-only packing in reflection cbuffer fields this pass does not otherwise use.
#define kPtFireflyClampEnabled (g_AoRayCount != 0u)
#define kPtRussianRouletteEnabled (g_HasGiTrace != 0u)
#define kPtCenterPrimaryRays (g_RoughnessCutoff > 0.5)
#define g_PtAmbientStrength g_GiStrength
// Host packs a 0..8 integer as a float (DxrPathTracerDispatch clamps it). saturate() would collapse
// every non-zero setting to a single ray — clamp to the real [0,8] range instead.
#define g_PtAmbientAoRayCount uint(round(clamp(_PadUnjitteredViewProj.x, 0.0, 8.0)))
#define kPtHasInstanceMotion (_PadUnjitteredViewProj.y > 0.5)
// Ray-cone pixel spread angle (radians/pixel ≈ 2·tan(fovY/2)/renderHeight) for albedo texture LOD.
// Mip-0 sampling flickers at texel frequency under DLSS jitter; with P4b the albedo GUIDE comes
// from the PT, and RR remodulates its output with that guide, imprinting the flicker on screen.
#define g_PtPixelSpreadAngle max(_PadUnjitteredViewProj.z, 1e-6)
// Matches lit.vs uTemporalHistoryValid: when false, prevClip = currClip (zero motion).
#define kPtMotionHistoryValid (_PadUnjitteredViewProj.w > 0.5)
// Radiance-term isolation for black-edge debugging (RenderDebugMode PtIsolate*). Host packs modes
// 0..9 as a float; saturate() would collapse every mode >= 2 to DirectSun, making most isolate
// views unreachable — clamp to the real [0,9] range instead.
#define g_PtDebugIsolateMode uint(round(clamp(_PadPtEmissiveNee, 0.0, 9.0)))

// Soft sun / ambient AO sample counts (RNG comes from PathRng — no salt blocks, G3).
static const uint kPtSoftSunSampleCount = 4u;
// G7/P5: deep-bounce sun NEE keeps 1 cone sample — RR absorbs the extra variance.
static const uint kPtSoftSunSampleCountDeep = 1u;

static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const uint kPayloadFlagVisibility = 2u;
// G7/P2: request bits OR'd into payload.hit before TraceRay (cleared/replaced on miss/hit).
static const uint kPayloadReqShadingData = 4u; // normal-map, bary, triangleLod
static const uint kPayloadReqPrimarySurface = 8u; // motion + depth (primary only)
static const uint kPayloadHitBackFace = 16u; // packed into hit on closest-hit (G7/P1)
static const uint kRussianRouletteStartBounce = 3u;
static const float kRussianRouletteMaxProb = 0.95;
// Below this roughness, specular uses a delta mirror bounce instead of VNDF (alpha floor ~0.032
// otherwise reads as frosted even at roughness 0).
static const float kPtDeltaSpecularRoughness = 0.03;
// Sentinel pdf for delta events (camera ray, perfect mirror, dielectric interface) so MIS gives
// weight ≈ 1 when no BSDF-sampling partner exists (PBRT 14.3.x).
static const float kDeltaScatterPdf = 1.0e10;

struct Payload
{
    float3 normal;         // interpolated vertex normal (world); use for shadow/AO bias
    float3 shadingNormal;  // normal-map perturbed (world); from closest-hit only
    float hitDistance;
    uint instanceId;
    uint primitiveIndex;
    // Pre-TraceRay: request bits. Post-hit: bit0=1, optional kPayloadHitBackFace.
    uint hit;
    float2 barycentrics;
    // Ray-cone LOD constant for the hit triangle's albedo texture: 0.5·log2(texelArea/worldArea)
    // (RTG ch. 20). Computed in closest-hit (needs ObjectToWorld); 0 for untextured materials.
    float triangleLod;
    // Primary-surface motion/depth from closest-hit: raster-style clip-space interpolation of the
    // hit triangle's vertices (lit.vs currClip/prevClip → pbr.ps ComputeMotionNdc). Per-hit world
    // reprojection shimmers under DLSS-RR because perspective needs clip-space interpolation.
    float2 primaryMotionNdc;
    float primaryDepth;
};

float2 PixelToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float2 ComputeMotionNdc(float4 currClip, float4 prevClip)
{
    const float2 currNdc = currClip.xy / currClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    return currNdc - prevNdc;
}

float3 PrevWorldFromObject(uint instanceId, float3 objectPos)
{
    const PrevInstanceTransform prev = g_PrevInstanceTransforms[instanceId];
    const float4 p = float4(objectPos, 1.0);
    return float3(dot(prev.row0, p), dot(prev.row1, p), dot(prev.row2, p));
}

// DLSS-RR specular albedo guide (vendored ProgrammingGuideDLSS_RR.md §4.2.1, [Ray Tracing Gems
// ch. 32]): preintegrated environment BRDF — NOT raw F0. Canonical call passes alpha = roughness².
// Raw F0 underestimates rough-metal reflectance (missing the scale/bias lift), which over-amplifies
// RR's demodulated specular at metal silhouettes (the 2026-07-11 magenta-fringe episode).
float3 EnvBRDFApprox2(float3 specularColor, float alpha, float NoV)
{
    NoV = abs(NoV);
    float4 X;
    X.x = 1.0;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;
    float4 Y;
    Y.x = 1.0;
    Y.y = alpha;
    Y.z = alpha * alpha;
    Y.w = alpha * Y.z;
    const float2x2 M1 = float2x2(0.99044, -1.28514, 1.29678, -0.755907);
    const float3x3 M2 = float3x3(1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627);
    const float2x2 M3 = float2x2(0.0365463, 3.32707, 9.0632, -9.04756);
    const float3x3 M4 = float3x3(1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123);
    float bias = dot(mul(M1, X.xy), Y.xy) * rcp(dot(mul(M2, X.xyw), Y.xyw));
    float scale = dot(mul(M3, X.xy), Y.xy) * rcp(dot(mul(M4, X.xzw), Y.xyw));
    bias *= saturate(specularColor.g * 50.0); // NVIDIA hack for specular reflectance of 0
    return mad(specularColor, max(0.0, scale), max(0.0, bias));
}

void ResetPayload(inout Payload payload)
{
    payload.normal = float3(0.0, 0.0, 1.0);
    payload.shadingNormal = float3(0.0, 0.0, 1.0);
    payload.hitDistance = 0.0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hit = 0;
    payload.barycentrics = 0.0.xx;
    payload.triangleLod = 0.0;
    payload.primaryMotionNdc = 0.0.xx;
    payload.primaryDepth = 1.0;
}

bool PayloadIsHit(Payload payload)
{
    return (payload.hit & 1u) != 0u;
}

bool PayloadHitBackFace(Payload payload)
{
    return (payload.hit & kPayloadHitBackFace) != 0u;
}

// Matches lit.vs + pbr.ps: interpolate unjittered curr/prev clip per vertex for MVs; DLSS depth
// uses jittered clip z/w at the same hit (HW-depth convention, motionVectorsJittered = false).
void ComputeVertexInterpolatedPrimarySurface(
    out float2 motionNdc,
    out float primaryDepth,
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics)
{
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];
    const float w = 1.0 - barycentrics.x - barycentrics.y;
    const float3 bary = float3(w, barycentrics.x, barycentrics.y);

    const float3x4 objectToWorld = ObjectToWorld3x4();
    float4 currClipUnj = 0.0.xxxx;
    float4 prevClipUnj = 0.0.xxxx;
    float4 currClipJit = 0.0.xxxx;

    const uint indices[3] = { i0, i1, i2 };
    [unroll]
    for (uint vi = 0u; vi < 3u; ++vi)
    {
        const float3 objectPos = LoadObjectPosition(geo, indices[vi]);
        const float3 currWorld = mul(objectToWorld, float4(objectPos, 1.0)).xyz;
        float3 prevWorld = currWorld;
        if (kPtHasInstanceMotion)
        {
            prevWorld = PrevWorldFromObject(instanceId, objectPos);
        }

        currClipUnj += mul(g_UnjitteredViewProj, float4(currWorld, 1.0)) * bary[vi];
        prevClipUnj += mul(g_PrevViewProj, float4(prevWorld, 1.0)) * bary[vi];
        currClipJit += mul(g_ViewProj, float4(currWorld, 1.0)) * bary[vi];
    }

    if (!kPtMotionHistoryValid)
    {
        prevClipUnj = currClipUnj;
    }

    motionNdc = ComputeMotionNdc(currClipUnj, prevClipUnj);
    primaryDepth = saturate(currClipJit.z / max(currClipJit.w, 1e-6));
}

// GGX VNDF half-vector sampling (Heitz 2018) — same as reflections.hlsl.
float3 SampleGgxVndfHalfVector(float3 normal, float3 viewWorld, float roughness, float2 xi)
{
    const float alpha = max(roughness * roughness, 1e-3);

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);

    const float3 viewTangent = float3(
        dot(viewWorld, tangent),
        dot(viewWorld, bitangent),
        dot(viewWorld, normal));

    const float3 stretchedView =
        normalize(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));

    const float lenSq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    const float3 basis1 = lenSq > 1e-7
        ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lenSq)
        : float3(1.0, 0.0, 0.0);
    const float3 basis2 = cross(stretchedView, basis1);

    const float r = sqrt(xi.x);
    const float phi = 2.0 * kPi * xi.y;
    const float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    const float s = 0.5 * (1.0 + stretchedView.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;

    const float3 halfStretched = t1 * basis1 + t2 * basis2
        + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * stretchedView;

    const float3 halfTangent = normalize(float3(
        alpha * halfStretched.x,
        alpha * halfStretched.y,
        max(halfStretched.z, 1e-6)));

    return normalize(
        tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

#include "pt_dielectric.hlsli"

float TraceVisibility(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    Payload probe;
    ResetPayload(probe);
    probe.hit = kPayloadFlagVisibility;

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(g_SceneTlas, occlusionFlags, 0xFF, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
}

// Sun NEE visibility: opaque scenes use cheap any-hit; scenes with dielectrics keep
// TraceTransmissiveVisibility so sunlight still transmits through windows onto floors.
float TraceSunNeeVisibility(float3 origin, float3 direction, float tMax)
{
    return (g_SceneHasTransmission > 0.5)
        ? TraceTransmissiveVisibility(origin, direction, tMax)
        : TraceVisibility(origin, direction, tMax);
}

// Soft sun visibility: cone-jittered shadow rays matching shadows.hlsl / reflections.hlsl.
float TraceSoftSunVisibility(
    float3 origin,
    float3 shadingNormal,
    inout PathRng rng,
    uint sampleCount)
{
    const float3 sunDir = normalize(g_SunDirection);
    if (dot(shadingNormal, sunDir) <= 0.0)
    {
        return 0.0;
    }

    const uint count = max(sampleCount, 1u);
    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(sunDir, tangent, bitangent);

    float visSum = 0.0;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < count; ++sampleIndex)
    {
        const float4 xi = PathRngNext4(rng);
        const float diskRadius = sqrt(xi.x);
        const float diskPhi = 2.0 * kPi * xi.y;
        const float2 disk = float2(diskRadius * cos(diskPhi), diskRadius * sin(diskPhi));
        const float3 rayDir = normalize(
            sunDir + (tangent * disk.x + bitangent * disk.y) * g_SunAngularTanRadius);
        visSum += TraceSunNeeVisibility(origin, rayDir, g_MaxTraceDistance);
    }

    return visSum / float(count);
}

// Ray-cone filtered albedo (RTG ch. 20): lod = triangleLod + log2(coneWidth) − log2(n·v).
// Matches the raster path's mip-filtered footprint closely enough that the albedo value is stable
// under sub-pixel jitter — required for the P4b diffuse-albedo GUIDE, which RR remodulates with.
float3 SampleSurfaceAlbedo(uint instanceId, uint primitiveIndex, float2 barycentrics, float albedoLod)
{
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const MaterialEntry material = LoadMaterialForInstance(instanceId);

    float3 albedo = material.albedo;
    if (material.albedoTexIndex != 0xFFFFFFFFu && material.albedoUvOffsetFloats != 0xFFFFFFFFu)
    {
        const float2 hitUv =
            ComputeHitUv(geo, primitiveIndex, material.albedoUvOffsetFloats, barycentrics);
        albedo *= SampleBindlessTextureRgb(material.albedoTexIndex, hitUv, albedoLod);
    }
    return albedo;
}

float ComputeAlbedoLod(Payload payload, float coneWidth, float3 rayDirection)
{
    const float nDotD = max(saturate(dot(payload.normal, -rayDirection)), 0.05);
    return payload.triangleLod + log2(max(coneWidth, 1e-6)) - log2(nDotD);
}

// P4b RR material guides — encoding must match full-rr-guides.md / rr_guides.ps.hlsl modes 0–2.
// NVIDIA-canonical guide semantics (vendored ProgrammingGuideDLSS_RR.md §4.2.1), settled by the
// 2026-07-11 guide 2x2 (rr-gi-diagnosis.md §E3): a metal diffuse lean toward neutral 0.5 makes RR
// attribute the metal's noisy specular GI to its diffuse accumulation channel -> intense boil on
// metals; black metal diffuse + RAW-F0 spec under-attributes rough-metal reflectance -> magenta
// silhouette fringe. Canonical black metal diffuse + EnvBRDFApprox2 spec is the remaining quadrant.
void ComputePtPrimaryRrMaterialGuides(
    float3 albedo,
    float3 hitNormal,
    float roughness,
    float metallic,
    float transmission,
    float indexOfRefraction,
    float3 viewDir,
    out float3 diffuseGuide,
    out float3 specGuide,
    out float3 guideNormal,
    out float guideRoughness)
{
    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float dielectricWeight = DielectricWeight(transmission, metallic);
    const float nDotV = saturate(dot(hitNormal, viewDir));

    diffuseGuide = albedo * (1.0 - metallic) * (1.0 - dielectricWeight);

    const float dielectricSpec = FresnelDielectric(
        nDotV,
        1.0 / max(indexOfRefraction, 1.0));
    const float3 opaqueSpecGuide = max(
        EnvBRDFApprox2(f0, roughness * roughness, nDotV),
        0.04.xxx);
    specGuide = lerp(
        opaqueSpecGuide,
        float3(dielectricSpec, dielectricSpec, dielectricSpec),
        dielectricWeight);

    guideNormal = normalize(hitNormal);
    guideRoughness = roughness;
}

float ComputeTransmissionGuideAlbedoLod(TransmissionGuideHit guide, float coneWidth)
{
    const float nDotD = max(saturate(dot(guide.normal, -guide.refractDir)), 0.05);
    return guide.triangleLod + log2(max(coneWidth, 1e-6)) - log2(nDotD);
}

float2 ComputeSkyAnchorMotion(float3 anchorDirection)
{
    const float3 skyAnchor = g_CameraPos + anchorDirection * (g_MaxTraceDistance * 0.5);
    float4 currClipUnj = mul(g_UnjitteredViewProj, float4(skyAnchor, 1.0));
    float4 prevClipUnj = mul(g_PrevViewProj, float4(skyAnchor, 1.0));
    if (!kPtMotionHistoryValid)
    {
        prevClipUnj = currClipUnj;
    }
    return ComputeMotionNdc(currClipUnj, prevClipUnj);
}

// Dual-frame refracted motion: replay prev-camera primary → glass → refract → background, then
// project both world hits (Omniverse translucent virtual motion). Fixes rotation smear through panes.
float2 ComputeTransmissionVirtualMotion(
    uint2 pixel,
    float3 glassHitPos,
    float3 primaryRayDir,
    TransmissionGuideHit currGuide,
    float originBias)
{
    if (!kPtMotionHistoryValid)
    {
        return currGuide.valid ? currGuide.motion : 0.0.xx;
    }

    if (!currGuide.valid)
    {
        return ComputeSkyAnchorMotion(currGuide.refractDir);
    }

    // True background hit position (the guide traced the full enter+exit path); reconstructing it as
    // a single straight segment from the glass hit is wrong once the path bends through a solid.
    const float3 worldCurr = currGuide.backgroundWorldPos;

    const float2 clipXY = PixelToClipXY((float2(pixel) + 0.5) / float2(g_OutputSize));
    const float4 prevFarH = mul(g_PrevInvViewProj, float4(clipXY, 1.0, 1.0));
    const float3 prevRayDir = normalize(prevFarH.xyz / prevFarH.w - g_PrevCameraPos);

    RayDesc prevPrimaryRay;
    prevPrimaryRay.Origin = g_PrevCameraPos;
    prevPrimaryRay.Direction = prevRayDir;
    prevPrimaryRay.TMin = 0.001;
    prevPrimaryRay.TMax = g_MaxTraceDistance;

    Payload prevPrimaryPayload;
    ResetPayload(prevPrimaryPayload);
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, prevPrimaryRay, prevPrimaryPayload);
    if (prevPrimaryPayload.hit == 0)
    {
        return ComputeSkyAnchorMotion(currGuide.refractDir);
    }

    const float3 prevGlassHitPos = g_PrevCameraPos + prevRayDir * prevPrimaryPayload.hitDistance;
    const MaterialEntry prevGlassMat = LoadMaterialForInstance(prevPrimaryPayload.instanceId);
    const float prevDielectricWeight =
        DielectricWeight(prevGlassMat.transmission, prevGlassMat.metallic);
    if (prevDielectricWeight < 0.01)
    {
        return currGuide.motion;
    }

    const float prevNdotV = saturate(dot(prevPrimaryPayload.normal, -prevRayDir));
    const float prevOriginBias =
        max(prevPrimaryPayload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - prevNdotV));
    const bool prevThinPane = prevGlassMat.thinWalled > 0.5;

    const TransmissionGuideHit prevGuide = TraceTransmissionGuide(
        prevGlassHitPos,
        prevPrimaryPayload.normal,
        prevRayDir,
        prevGlassMat.indexOfRefraction,
        prevThinPane,
        prevThinPane ? max(prevOriginBias, kThinShellMinExitBias) : prevOriginBias,
        prevPrimaryPayload.instanceId);

    if (!prevGuide.valid)
    {
        return ComputeSkyAnchorMotion(prevGuide.refractDir);
    }

    const float3 worldPrev = prevGuide.backgroundWorldPos;

    const float4 currClipUnj = mul(g_UnjitteredViewProj, float4(worldCurr, 1.0));
    const float4 prevClipUnj = mul(g_PrevViewProj, float4(worldPrev, 1.0));
    return ComputeMotionNdc(currClipUnj, prevClipUnj);
}

float TracePrimaryAmbientOcclusion(inout PathRng rng, float3 origin, float3 normal, uint rayCount)
{
    if (rayCount == 0u)
    {
        return 1.0;
    }

    const float aoRadius = max(g_MaxTraceDistance * 0.05, 0.5);
    float aoSum = 0.0;
    [loop]
    for (uint aoIndex = 0u; aoIndex < rayCount; ++aoIndex)
    {
        const float2 aoXi = PathRngNext2(rng);
        const float3 aoDir = CosineSampleHemisphere(normal, aoXi);
        aoSum += TraceVisibility(origin, aoDir, aoRadius);
    }

    return aoSum / float(rayCount);
}

float3 EvaluateRealTimeDiffuseAmbient(
    float3 diffuseAlbedo,
    float3 shadingNormal,
    float aoVisibility)
{
    // Environment IS NEE replaces the SH ambient floor when the CDF is bound (F2).
    if (g_EnvLightImportanceCount > 0u)
    {
        return 0.0.xxx;
    }

    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    if (diffuseWeight <= 0.02)
    {
        return 0.0.xxx;
    }

    const float3 irradiance = EvaluateDiffuseIrradianceSh(shadingNormal);
    return diffuseAlbedo * irradiance / kPi
        * aoVisibility
        * g_EnvironmentIntensity
        * g_PtAmbientStrength;
}

float3 EvaluateRealTimeEmitterDisplayAmbient(
    float3 diffuseAlbedo,
    float3 shadingNormal)
{
    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    if (diffuseWeight <= 0.02)
    {
        return 0.0.xxx;
    }

    // Camera-visible emissive meshes should not become flat full-bright cards in real-time view.
    // Use the emissive material for transport, but keep the primary surface display shaded without
    // spending AO or secondary rays on the emitter terminal.
    const float3 irradiance = EvaluateDiffuseIrradianceSh(shadingNormal);
    return diffuseAlbedo * irradiance / kPi
        * g_EnvironmentIntensity
        * g_PtAmbientStrength;
}

float TracePrimarySunVisibility(
    inout PathRng rng,
    float3 hitNormal,
    float3 shadowOrigin,
    uint softSunSampleCount)
{
    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    if (ndotl <= 0.0)
    {
        return 0.0;
    }

    return (g_SunAngularTanRadius > 1e-6)
        ? TraceSoftSunVisibility(shadowOrigin, hitNormal, rng, softSunSampleCount)
        : TraceSunNeeVisibility(shadowOrigin, sunL, g_MaxTraceDistance);
}

float BalanceHeuristic(float pdfA, float pdfB)
{
    const float denom = pdfA + pdfB;
    return denom > 1e-6 ? pdfA / denom : 1.0;
}

// SSR injects previous-frame bloom into reflected scene color (ssr_scene_color.ps.hlsl). PT traces
// emissive directly, so approximate display bloom on emissive sources for mirror/reflection paths.
float3 EmissiveWithBloomHalo(float3 emissive)
{
    if (g_PtBloomHaloIntensity <= 0.0)
    {
        return emissive;
    }

    const float lum = max(emissive.r, max(emissive.g, emissive.b));
    const float knee = max(lum - 1.0, 0.0);
    const float halo = knee * knee * g_PtBloomHaloIntensity * 2.0;
    return emissive + emissive * (halo / max(lum, 1e-4));
}

float EmissiveLightPickPdf(uint instanceId)
{
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return 0.0;
    }

    [loop]
    for (uint lightIndex = 0u; lightIndex < g_EmissiveLightCount; ++lightIndex)
    {
        if (g_EmissiveLights[lightIndex].instanceId == instanceId)
        {
            return g_EmissiveLights[lightIndex].pickWeight / g_EmissiveLightPickWeightSum;
        }
    }
    return 0.0;
}

// Look up an emitter's NEE selection pdf AND its total mesh surface area by instance id, so a
// BSDF-sampled ray that lands on an emitter can compute the competing NEE density for MIS.
void EmissiveLightLookup(uint instanceId, out float pickPdf, out float surfaceArea)
{
    pickPdf = 0.0;
    surfaceArea = 0.0;
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return;
    }

    [loop]
    for (uint lightIndex = 0u; lightIndex < g_EmissiveLightCount; ++lightIndex)
    {
        if (g_EmissiveLights[lightIndex].instanceId == instanceId)
        {
            pickPdf = g_EmissiveLights[lightIndex].pickWeight / g_EmissiveLightPickWeightSum;
            surfaceArea = g_EmissiveLights[lightIndex].surfaceArea;
            return;
        }
    }
}

// Solid-angle density of the emissive-NEE strategy for a light seen at `dist` with emitter-facing
// cosine `cosThetaEmitter`: pickPdf * (1/area) * dist^2 / cos. Instance-level pickPdf + area are
// used for the BSDF-hit MIS partner (triangle NEE uses per-triangle area on the light side).
float EmissiveNeePdfSolidAngle(float pickPdf, float surfaceArea, float dist2, float cosThetaEmitter)
{
    if (pickPdf <= 0.0 || surfaceArea <= 1e-8 || cosThetaEmitter <= 1e-6)
    {
        return 0.0;
    }
    return pickPdf * (1.0 / surfaceArea) * dist2 / cosThetaEmitter;
}

// Uniform sample on a triangle; pdfArea is 1 / triangle area.
void SampleUniformPointOnTriangle(
    float3 v0,
    float3 v1,
    float3 v2,
    float triangleArea,
    float2 xi,
    out float3 surfacePoint,
    out float pdfArea)
{
    if (triangleArea <= 1e-8)
    {
        surfacePoint = (v0 + v1 + v2) / 3.0;
        pdfArea = 1.0;
        return;
    }

    const float sqrtXi0 = sqrt(saturate(xi.x));
    const float u = 1.0 - sqrtXi0;
    const float v = xi.y * sqrtXi0;
    const float w = 1.0 - u - v;
    surfacePoint = u * v0 + v * v1 + w * v2;
    pdfArea = 1.0 / triangleArea;
}

// GGX/Trowbridge-Reitz + height-correlated Smith helpers for the opaque BRDF estimator.
// alpha MUST match SampleGgxVndfHalfVector's internal floor (1e-3) so the evaluated pdf corresponds
// to the sampling distribution (a mismatch biases the estimator).
float GgxD(float NoH, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * d * d, 1e-9);
}

float SmithG1(float NoX, float alpha)
{
    const float a2 = alpha * alpha;
    return 2.0 * NoX / max(NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX), 1e-9);
}

float SmithG2HeightCorrelated(float NoV, float NoL, float alpha)
{
    const float a2 = alpha * alpha;
    const float lambdaV = NoL * sqrt(a2 + (1.0 - a2) * NoV * NoV);
    const float lambdaL = NoV * sqrt(a2 + (1.0 - a2) * NoL * NoL);
    return 2.0 * NoV * NoL / max(lambdaV + lambdaL, 1e-9);
}

// Plain Schlick Fresnel (no roughness ceiling — that raster-IBL hack, R4, does not belong in the PT
// energy split). f0 = 0.04 dielectric / albedo metal.
float3 FresnelSchlick(float cosTheta, float3 f0)
{
    const float m = saturate(1.0 - cosTheta);
    const float m2 = m * m;
    return f0 + (1.0.xxx - f0) * (m2 * m2 * m);
}

float OpaqueBsdfLobeSelectionProbFromNoV(float NoV, float3 f0, float3 albedo, float metallic)
{
    const float3 fresnelNoV = FresnelSchlick(NoV, f0);
    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));
    const float specLum = Luminance(fresnelNoV);
    const float diffLum = Luminance(baseDiffuse);
    float pSpec = specLum / max(specLum + diffLum, 1e-4);
    pSpec = lerp(pSpec, 1.0, saturate(metallic));
    return clamp(pSpec, 0.1, 0.9);
}

// Reject only when the light lies behind the tangent plane. Do not early-out on grazing view
// (NoV → 0): raster uses a denominator floor (pbr.ps.hlsl CalcCookTorranceContribution) so
// silhouettes stay lit when the sun is visible — a hard NoV cutoff drew black void rims.
float3 EvaluateOpaqueBsdf(
    float3 hitNormal,
    float3 viewDir,
    float3 wi,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic)
{
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3);
    const float NoV = saturate(dot(hitNormal, viewDir));
    const float NoL = saturate(dot(hitNormal, wi));
    if (NoL <= 0.0)
    {
        return 0.0.xxx;
    }

    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));
    const float3 fresnelNoV = FresnelSchlick(NoV, f0);
    const float3 h = normalize(viewDir + wi);
    const float NoH = saturate(dot(hitNormal, h));
    const float VoH = saturate(dot(viewDir, h));

    const float d = GgxD(NoH, alpha);
    const float g2 = SmithG2HeightCorrelated(NoV, NoL, alpha);
    const float3 fresnel = FresnelSchlick(VoH, f0);

    const float3 specCos = d * g2 * fresnel / max(4.0 * NoV, 1e-4);
    const float3 diffCos = baseDiffuse * (1.0.xxx - fresnelNoV) * (NoL / kPi);
    return specCos + diffCos;
}

// Mixture directional pdf for sampling direction wi (matches SampleOpaqueInterface's pdfMix).
float OpaqueBsdfPdf(
    float3 hitNormal,
    float3 viewDir,
    float3 wi,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic)
{
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3);
    const float NoV = saturate(dot(hitNormal, viewDir));
    const float NoL = saturate(dot(hitNormal, wi));
    if (NoL <= 0.0)
    {
        return 0.0;
    }

    const float pSpec = OpaqueBsdfLobeSelectionProbFromNoV(NoV, f0, albedo, metallic);
    const float3 h = normalize(viewDir + wi);
    const float NoH = saturate(dot(hitNormal, h));

    const float d = GgxD(NoH, alpha);
    const float g1 = SmithG1(NoV, alpha);
    const float pdfSpec = g1 * d / max(4.0 * NoV, 1e-4);
    const float pdfDiff = NoL / kPi;
    return pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff;
}

// Sun NEE — full opaque BSDF toward the sun. Delta light: no MIS partner, weight 1.
// Gate on the SHADING normal (like env/emissive NEE): with normal maps, bump walls stay lit beyond
// the geometric terminator, and a geometric-normal gate chops them with a hard edge exactly at the
// sun-visibility boundary. The traced shadow ray (from the geometric-offset origin) still provides
// real occlusion, so light naturally falls off past the terminator instead of stepping to black.
float3 EvaluateDirectSun(
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 shadingNormal,
    float sunVis)
{
    const float3 sunL = normalize(g_SunDirection);
    if (dot(shadingNormal, sunL) <= 0.0)
    {
        return 0.0.xxx;
    }

    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    const float3 bsdf = EvaluateOpaqueBsdf(shadingNormal, viewDir, sunL, f0, albedo, roughness, metallic);
    return bsdf * sunRadiance * sunVis;
}

// One emissive mesh-light sample per bounce (triangle surface + transmissive shadow ray), full-BSDF,
// MIS-weighted against BSDF sampling with the true mixture pdf (both measured in solid angle).
float3 EvaluateDirectEmissive(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin)
{
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return 0.0.xxx;
    }

    const float4 xiPick = PathRngNext4(rng);
    const float4 xiSurface = PathRngNext4(rng);

    float pick = xiPick.x * g_EmissiveLightPickWeightSum;
    uint lightIndex = g_EmissiveLightCount - 1u;
    [loop]
    for (uint i = 0u; i < g_EmissiveLightCount; ++i)
    {
        pick -= g_EmissiveLights[i].pickWeight;
        if (pick <= 0.0)
        {
            lightIndex = i;
            break;
        }
    }

    const EmissiveLightEntry light = g_EmissiveLights[lightIndex];
    if (light.triangleCount == 0u)
    {
        return 0.0.xxx;
    }

    float triPick = xiPick.y * light.pickWeight;
    uint triangleIndex = light.triangleOffset + light.triangleCount - 1u;
    [loop]
    for (uint triLocal = 0u; triLocal < light.triangleCount; ++triLocal)
    {
        const EmissiveTriangleEntry tri = g_EmissiveTriangles[light.triangleOffset + triLocal];
        triPick -= tri.pickWeight;
        if (triPick <= 0.0)
        {
            triangleIndex = light.triangleOffset + triLocal;
            break;
        }
    }

    const EmissiveTriangleEntry emitterTri = g_EmissiveTriangles[triangleIndex];

    float3 lightPoint;
    float pdfArea;
    SampleUniformPointOnTriangle(
        emitterTri.v0,
        emitterTri.v1,
        emitterTri.v2,
        emitterTri.triangleArea,
        xiSurface.xy,
        lightPoint,
        pdfArea);

    float3 toLight = lightPoint - shadowOrigin;
    const float dist2 = max(dot(toLight, toLight), 1e-8);
    const float dist = sqrt(dist2);
    const float3 wi = toLight / dist;

    const float cosThetaReceiver = saturate(dot(hitNormal, wi));
    if (cosThetaReceiver <= 0.0)
    {
        return 0.0.xxx;
    }

    const float cosThetaEmitter = saturate(dot(emitterTri.faceNormal, -wi));
    if (cosThetaEmitter <= 0.0)
    {
        return 0.0.xxx;
    }

    // Opaque any-hit: glass blocks emissive NEE like a wall. Sun keeps TraceSunNeeVisibility for
    // window→floor lighting; emissive-through-glass via NEE is rarely noticeable and was the
    // glass+emissive perf cliff (a0cc7f8). BSDF paths through glass still find emitters.
    const float visibility = TraceVisibility(shadowOrigin, wi, dist - 0.001);
    if (visibility <= 0.0)
    {
        return 0.0.xxx;
    }

    const float pickPdf = emitterTri.pickWeight / g_EmissiveLightPickWeightSum;
    const float pdfSolidAngle =
        EmissiveNeePdfSolidAngle(pickPdf, emitterTri.triangleArea, dist2, cosThetaEmitter);
    const float pdfBsdf = OpaqueBsdfPdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float misWeight = BalanceHeuristic(pdfSolidAngle, pdfBsdf);

    // EvaluateOpaqueBsdf already returns f * cosThetaReceiver, so the geometry term must carry ONLY
    // the emitter cosine and 1/dist² — multiplying by cosThetaReceiver again dims area lights by an
    // extra NoL and desyncs this NEE estimate from the BSDF-hit side of the MIS pair (C3).
    const float3 bsdf = EvaluateOpaqueBsdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float geometryTerm = cosThetaEmitter / dist2;

    return bsdf * EmissiveWithBloomHalo(light.emissive) * geometryTerm * visibility * misWeight
        / max(pickPdf * pdfArea, 1e-8);
}

// Infinite environment light NEE — luminance-importance sampled HDR equirect, MIS vs BSDF.
float3 EvaluateDirectEnvironment(
    inout PathRng rng,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float3 hitNormal,
    float3 shadowOrigin)
{
    if (g_EnvLightImportanceCount == 0u)
    {
        return 0.0.xxx;
    }

    const float4 xi = PathRngNext4(rng);
    float3 wi;
    float pdfEnv;
    if (!SampleEnvLightDirection(xi, wi, pdfEnv))
    {
        return 0.0.xxx;
    }

    if (dot(hitNormal, wi) <= 0.0)
    {
        return 0.0.xxx;
    }

    // Same opaque-fast policy as emissive NEE (see EvaluateDirectEmissive).
    const float visibility = TraceVisibility(shadowOrigin, wi, g_MaxTraceDistance);
    if (visibility <= 0.0)
    {
        return 0.0.xxx;
    }

    const float pdfBsdf = OpaqueBsdfPdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float misWeight = BalanceHeuristic(pdfEnv, pdfBsdf);
    const float3 bsdf = EvaluateOpaqueBsdf(hitNormal, viewDir, wi, f0, albedo, roughness, metallic);
    const float3 radiance = EnvNeeRadiance(wi);
    return bsdf * radiance * visibility * misWeight / max(pdfEnv, 1e-8);
}

// Opaque BRDF bounce: stochastic diffuse/GGX-specular lobe pick, weighted by the FULL BRDF over the
// mixture pdf — the one-sample MIS estimator (Veach 1997 §9.2.4). Specular carries the correct VNDF
// estimator weight F(VoH)*G2/G1 (Heitz 2018), NOT constant f0; diffuse is Fresnel-attenuated by
// (1-F) so diffuse+specular conserve energy (white furnace). scatterPdf returns the mixture
// directional pdf for NEE MIS. No forced-lobe branches — the divisor is always the true selection
// probability, so no lobe is over/under-weighted (B3).
void SampleOpaqueInterface(
    float3 hitNormal,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float2 xi2d,
    float lobeXi,
    out float3 nextDir,
    out bool isSpecular,
    out float scatterPdf,
    inout float3 throughput)
{
    const float ggxRoughness = min(max(roughness, 1e-4), 0.99);
    const float alpha = max(ggxRoughness * ggxRoughness, 1e-3); // matches SampleGgxVndfHalfVector
    const float NoV = saturate(dot(hitNormal, viewDir));

    const float3 baseDiffuse = albedo * (1.0 - saturate(metallic));

    // View-side Fresnel — reused for the lobe-selection heuristic AND the energy-conserving diffuse
    // weight below. Using F(NoV) (evaluated once) rather than the per-microfacet F(VoH) is what keeps
    // diffuse+specular from gaining energy at grazing angles (verified by the white-furnace test).
    const float3 fresnelNoV = FresnelSchlick(NoV, f0);

    // Lobe selection probability: an importance-sampling heuristic — any value in (0,1) is unbiased
    // (it only affects variance). Balance specular vs diffuse reflectance, bias toward specular for
    // metals, and keep BOTH lobes samplable so the mixture pdf stays valid.
    const float specLum = Luminance(fresnelNoV);
    const float diffLum = Luminance(baseDiffuse);
    float pSpec = specLum / max(specLum + diffLum, 1e-4);
    pSpec = lerp(pSpec, 1.0, saturate(metallic));
    pSpec = clamp(pSpec, 0.1, 0.9);

    const bool sampledSpecular = (lobeXi < pSpec);
    float3 l;
    if (sampledSpecular && roughness <= kPtDeltaSpecularRoughness)
    {
        // Delta mirror / near-mirror: bypass VNDF (its 1e-3 alpha floor reads as frosted).
        l = normalize(reflect(-viewDir, hitNormal));
        isSpecular = true;
        nextDir = l;

        const float NoL = dot(hitNormal, l);
        if (NoL <= 0.0)
        {
            scatterPdf = 1.0;
            throughput = 0.0.xxx;
            return;
        }

        throughput *= fresnelNoV / max(pSpec, 1e-6);
        scatterPdf = kDeltaScatterPdf;
        return;
    }

    if (sampledSpecular)
    {
        const float3 h = SampleGgxVndfHalfVector(hitNormal, viewDir, ggxRoughness, xi2d);
        l = reflect(-viewDir, h);
    }
    else
    {
        l = CosineSampleHemisphere(hitNormal, xi2d);
    }

    isSpecular = sampledSpecular;
    nextDir = l;

    const float NoL = dot(hitNormal, l);
    if (NoL <= 0.0)
    {
        // Sampled below the horizon: terminate this path sample (unbiased).
        scatterPdf = 1.0;
        throughput = 0.0.xxx;
        return;
    }

    const float3 h = normalize(viewDir + l);
    const float NoH = saturate(dot(hitNormal, h));
    const float VoH = saturate(dot(viewDir, h));

    const float d = GgxD(NoH, alpha);
    const float g1 = SmithG1(NoV, alpha);
    const float g2 = SmithG2HeightCorrelated(NoV, NoL, alpha);
    const float3 fresnel = FresnelSchlick(VoH, f0);

    // BRDF * cos(theta_l). Specular uses per-microfacet F(VoH); diffuse uses view-side F(NoV) so the
    // two lobes conserve energy (a per-microfacet (1-F(VoH)) diffuse gains ~9% at grazing).
    const float3 specCos = d * g2 * fresnel / max(4.0 * NoV, 1e-4);              // f_spec * NoL
    const float3 diffCos = baseDiffuse * (1.0.xxx - fresnelNoV) * (NoL / kPi);   // f_diff * NoL, (1-F(NoV)) split

    // Mixture directional pdf: VNDF for specular (= G1*D/(4*NoV)), cosine for diffuse.
    const float pdfSpec = g1 * d / max(4.0 * NoV, 1e-4);
    const float pdfDiff = NoL / kPi;
    const float pdfMix = pSpec * pdfSpec + (1.0 - pSpec) * pdfDiff;

    throughput *= (specCos + diffCos) / max(pdfMix, 1e-9);
    scatterPdf = pdfMix;
}

// Unified material bounce: dielectric Fresnel/Snell or opaque GGX.
//
// G2: Fresnel at a dielectric interface is always stochastic (reflect XOR refract) — never the old
// real-time dual inject. That is what ReSTIR needs for sample~pdf on glass.
//
// Glass vs opaque mixture (dielectricWeight = transmission·(1−metallic)):
//   Reference — Bernoulli pick with 1/p (unbiased; fireflies average out under accumulation).
//   Real-time — always follow the glass lobe when dw > 0 and scale throughput by dw (p=1 for that
//   choice). Opaque residual stays on NEE/ambient via opaqueWeight. This avoids the 1/dw mid-t
//   blowout at 1 spp; the path is still a single dielectric sample with a well-defined pdf.
bool SampleMaterialBounce(
    inout PathRng rng,
    float3 hitNormal,
    float3 rayDir,
    float3 viewDir,
    float3 f0,
    float3 albedo,
    float roughness,
    float metallic,
    float transmission,
    float ior,
    bool thinWalled,
    bool pathInMedium,
    out float3 nextDir,
    out bool isSpecular,
    out bool outPathInMedium,
    out float scatterPdf,
    inout float3 throughput)
{
    const float dielectricWeight = DielectricWeight(transmission, metallic);
    const float4 xi = PathRngNext4(rng);

    outPathInMedium = pathInMedium;
    isSpecular = false;
    scatterPdf = 1.0;

    const bool useGlassPath = dielectricWeight > 0.0
        && (kPtCenterPrimaryRays || xi.w < dielectricWeight);

    if (useGlassPath)
    {
        if (kPtCenterPrimaryRays)
        {
            throughput *= dielectricWeight;
        }
        else
        {
            throughput /= max(dielectricWeight, 1e-6);
        }
        SampleDielectricInterface(
            hitNormal,
            rayDir,
            roughness,
            ior,
            thinWalled,
            pathInMedium,
            xi.z,
            xi.xy,
            nextDir,
            outPathInMedium,
            scatterPdf);
        isSpecular = true;
        return true;
    }

    if (dielectricWeight > 0.0)
    {
        throughput /= max(1.0 - dielectricWeight, 1e-6);
    }

    SampleOpaqueInterface(
        hitNormal,
        viewDir,
        f0,
        albedo,
        roughness,
        metallic,
        xi.xy,
        xi.z,
        nextDir,
        isSpecular,
        scatterPdf,
        throughput);
    return isSpecular;
}

float3 SelectPtDebugRadiance(
    uint isolateMode,
    bool primaryHit,
    float3 radiance,
    float3 radiancePreClamp,
    float3 termDirectSun,
    float3 termDirectEmissive,
    float3 termSurfaceEmissive,
    float3 termAmbient,
    float3 termIndirect,
    float primaryAoVis,
    float primarySunVis,
    float specHitDistGuide)
{
    if (isolateMode == 0u)
    {
        return radiance;
    }

    // Sky pixels: only show the environment for views that include miss/sky transport.
    // Surface-only terms use black sky so they do not look like the full composite.
    if (!primaryHit)
    {
        if (isolateMode == 7u || isolateMode == 8u)
        {
            return radiancePreClamp;
        }
        return 0.0.xxx;
    }

    if (isolateMode == 1u)
    {
        return termDirectSun;
    }
    if (isolateMode == 2u)
    {
        return termDirectEmissive * float3(1.0, 0.65, 1.0);
    }
    if (isolateMode == 3u)
    {
        return termSurfaceEmissive;
    }
    if (isolateMode == 4u)
    {
        return termAmbient * float3(0.65, 0.8, 1.0);
    }
    if (isolateMode == 5u)
    {
        const float v = saturate(primaryAoVis * 4.0);
        return float3(v * 0.2, v, v * 0.25);
    }
    if (isolateMode == 6u)
    {
        const float v = saturate(primarySunVis * 4.0);
        return float3(v, v * 0.85, v * 0.15);
    }
    if (isolateMode == 7u)
    {
        // G5: structural indirect = throughputAfterFirstScatter * Lo_tail (not residual subtract).
        return termIndirect;
    }
    if (isolateMode == 8u)
    {
        return radiancePreClamp;
    }
    if (isolateMode == 9u)
    {
        const float normalizedDist = saturate(specHitDistGuide / max(g_MaxTraceDistance, 1e-4));
        const float v = 1.0 - normalizedDist;
        return float3(v, v * 0.35, 0.0);
    }

    return radiancePreClamp;
}

[shader("raygeneration")]
void PathTracerRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const uint pixelIndex = pixel.y * g_OutputSize.x + pixel.x;

    PathRng rng = InitPathRng(pixel, g_FrameIndex);
    const uint pathSeed = rng.seed;
    const float4 primaryXi = PathRngNext4(rng);
    const float2 primaryOffset = kPtCenterPrimaryRays ? float2(0.5, 0.5) : primaryXi.zw;
    const float2 texCoord = (float2(pixel) + primaryOffset) / float2(g_OutputSize);
    const float2 clipXY = PixelToClipXY(texCoord);

    const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
    const float3 farWorld = farH.xyz / farH.w;
    const float3 cameraRayDir = normalize(farWorld - g_CameraPos);

    // Match the host clamp (DxrSettings 1..16) and the reflection/GI passes; previously capped at 8,
    // so slider values 9..16 silently did nothing.
    const uint maxBounces = clamp(g_MaxBounces, 1u, 16u);

    // G5: bounce-0 direct vs Lo_tail (bounce≥1) with local throughput reset after first scatter.
    float3 directRadiance = 0.0.xxx;
    float3 loTail = 0.0.xxx;
    float3 throughput = 1.0.xxx;
    float3 throughputAfterFirstScatter = 1.0.xxx;
    bool inTail = false;
    bool haveInitialSample = false;
    float3 sampleXs = 0.0.xxx;
    float3 sampleNs = float3(0.0, 1.0, 0.0);
    float samplePdf = 1.0;
    uint sampleFlags = 0u;
    float primaryRoughness = 1.0;
    float primaryDielectricWeight = 0.0;

    RayDesc ray;
    ray.Origin = g_CameraPos;
    ray.Direction = cameraRayDir;
    ray.TMin = 0.001;
    ray.TMax = g_MaxTraceDistance;

    uint primaryInstanceId = 0u;
    uint primaryPrimitiveIndex = 0u;
    bool primaryHit = false;
    float primaryDepth = 1.0;
    float2 primaryMotion = 0.0.xx;
    float specHitDistGuide = g_MaxTraceDistance;
    // Ray-cone width for albedo texture LOD, grown by pixel spread × distance along the path.
    float pathConeWidth = 0.0;
    // Roughness of the surface that launched the current ray — drives env mip on miss, matching
    // reflections.hlsl (payload.surfaceRoughness). Previously bounce>=1 always used 1.0, which
    // made mirror sky reflections black while primary camera misses (roughness 0) still worked.
    float missEnvRoughness = 0.0;
    // Real-time only: primary-hit AO-gated SH ambient (devdoc/dxr/pt/crevice-darkening.md v2) covers
    // the diffuse sky floor, so a DIFFUSE bounce that escapes must NOT also add the environment.
    // SPECULAR bounces still add the env on miss (background + reflections). Reference traces the sky
    // purely and always adds it on miss.
    bool addEnvOnMiss = true;
    float lastScatterPdf = kDeltaScatterPdf;
    bool pathInMedium = false;
    float3 mediumTint = 1.0.xxx;

    float3 termDirectSun = 0.0.xxx;
    float3 termDirectEmissive = 0.0.xxx;
    float3 termSurfaceEmissive = 0.0.xxx;
    float3 termAmbient = 0.0.xxx;
    float primaryAoVis = 1.0;
    float primarySunVis = 0.0;

    [loop]
    for (uint bounce = 0u; bounce <= maxBounces; ++bounce)
    {
        Payload payload;
        ResetPayload(payload);
        // G7/P2: primary needs motion/depth; every shading bounce needs LOD/bary/normal-map.
        payload.hit = kPayloadReqShadingData;
        if (bounce == 0u)
        {
            payload.hit |= kPayloadReqPrimarySurface;
        }
        TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);

        if (payload.hit == 0)
        {
            if (bounce == 0u)
            {
                // Sky pixel: camera-only reprojection (raster sky keeps MV=0; PT supplies finite anchor).
                const float3 skyAnchor = g_CameraPos + ray.Direction * (g_MaxTraceDistance * 0.5);
                float4 currClipUnj = mul(g_UnjitteredViewProj, float4(skyAnchor, 1.0));
                float4 prevClipUnj = mul(g_PrevViewProj, float4(skyAnchor, 1.0));
                if (!kPtMotionHistoryValid)
                {
                    prevClipUnj = currClipUnj;
                }
                primaryMotion = ComputeMotionNdc(currClipUnj, prevClipUnj);
                primaryDepth = 1.0;

                // Sky RR guides: (0.5, 0.5, 0.5) albedo per the DLSS-RR Integration Guide §3.4.2
                // (white/black diffuse and zero specular are known-bad — devdoc/dxr/pt/sky-motion.md).
                g_DiffuseAlbedoGuide[pixel] = float4(0.5, 0.5, 0.5, 1.0);
                g_SpecularAlbedoGuide[pixel] = float4(0.5, 0.5, 0.5, 1.0);
                g_NormalRoughnessGuide[pixel] = float4(0.0, 0.0, 1.0, 1.0);
            }

            float3 missRadiance = 0.0.xxx;
            if (g_EnvLightImportanceCount > 0u)
            {
                // Env NEE active: the miss-side env is the BSDF-sampling half of the env-lighting MIS
                // pair (or the direct sky for a camera/delta ray, where lastScatterPdf =
                // kDeltaScatterPdf → weight ≈ 1). Sample the SHARP equirect — the same radiance the
                // NEE side sees — so both strategies estimate one integrand (also removes the R1
                // prefilter double-blur), and MIS-weight against the env NEE density. Previously the
                // miss added the FULL env: specular double-counted in both modes, diffuse
                // double-counted in reference and under-counted in real-time (C2).
                const float envMisPdf = EnvNeePdfForDirection(ray.Direction);
                const float envMisWeight =
                    envMisPdf > 0.0 ? BalanceHeuristic(lastScatterPdf, envMisPdf) : 1.0;
                missRadiance = SampleEnvEquirectRadiance(ray.Direction) * envMisWeight;
            }
            else if (addEnvOnMiss)
            {
                missRadiance = SampleEnvironment(ray.Direction, missEnvRoughness);
            }

            const float3 missContrib = throughput * missRadiance;
            if (inTail)
            {
                loTail += missContrib;
                if (!haveInitialSample)
                {
                    sampleFlags |= kRestirSampleNoReuse;
                }
            }
            else
            {
                directRadiance += missContrib;
            }
            break;
        }

        pathConeWidth += g_PtPixelSpreadAngle * payload.hitDistance;

        const MaterialEntry material = LoadMaterialForInstance(payload.instanceId);
        const float3 hitNormalGeom = payload.normal;
        const float3 hitNormal = payload.shadingNormal;
        const float albedoLod = ComputeAlbedoLod(payload, pathConeWidth, ray.Direction);
        float3 albedo;
        float surfaceRoughness;
        float surfaceMetallic;
        float3 surfaceEmissiveColor;
        ResolveSurfaceMaterialScalars(
            payload.instanceId,
            payload.primitiveIndex,
            payload.barycentrics,
            albedoLod,
            albedo,
            surfaceRoughness,
            surfaceMetallic,
            surfaceEmissiveColor);
        const float3 viewDir = -ray.Direction;
        const float3 hitPos = ray.Origin + ray.Direction * payload.hitDistance;
        const float3 shadowOrigin = hitPos + hitNormalGeom * max(payload.hitDistance * 0.001, 0.002);

        if (pathInMedium && bounce > 0u)
        {
            throughput *= BeerLambertMediumAttenuation(mediumTint, payload.hitDistance);
        }

        // First indirect vertex (xs): record once when the post-primary scatter lands.
        if (inTail && !haveInitialSample)
        {
            sampleXs = hitPos;
            sampleNs = hitNormal;
            samplePdf = lastScatterPdf;
            haveInitialSample = true;
        }

        const float3 f0 = lerp(0.04.xxx, albedo, surfaceMetallic);
        const float dielectricWeight =
            DielectricWeight(material.transmission, surfaceMetallic);
        const float opaqueWeight = 1.0 - dielectricWeight;
        const float3 specularEnergy =
            FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(surfaceRoughness, 0.55));
        const float3 diffuseAlbedo =
            albedo * (1.0.xxx - specularEnergy) * (1.0 - surfaceMetallic) * (1.0 - dielectricWeight);

        const float emissiveLuminance =
            max(surfaceEmissiveColor.r, max(surfaceEmissiveColor.g, surfaceEmissiveColor.b));
        // Real-time: treat emitters as path terminals. Staring at a bright cube was paying full
        // soft-sun + emissive + env NEE from the light surface and continuing high-throughput
        // bounces — view-dependent ~2× PT cost with no glass. Reference keeps full paths.
        const bool terminalEmissiveHit = kPtCenterPrimaryRays && emissiveLuminance > 1e-4;
        const bool realTimePrimaryEmitterDisplay = terminalEmissiveHit && bounce == 0u;
        if (emissiveLuminance > 1e-4 && !realTimePrimaryEmitterDisplay)
        {
            float emitterPickPdf;
            float emitterArea;
            EmissiveLightLookup(payload.instanceId, emitterPickPdf, emitterArea);
            // Triangle NEE only samples the emitter's winding front face, so a BSDF ray landing on the
            // BACK face has no NEE partner and must take full weight — otherwise emissive panel back
            // faces render MIS-dimmed (C6). Emission itself stays two-sided.
            const float cosEmitter = saturate(dot(hitNormal, viewDir));
            const float pdfNee = (!PayloadHitBackFace(payload))
                ? EmissiveNeePdfSolidAngle(
                    emitterPickPdf, emitterArea, payload.hitDistance * payload.hitDistance, cosEmitter)
                : 0.0;
            const float misHit = pdfNee > 0.0 ? BalanceHeuristic(lastScatterPdf, pdfNee) : 1.0;
            const float3 surfaceEmissive = EmissiveWithBloomHalo(surfaceEmissiveColor) * misHit;
            const float3 emissiveContrib = throughput * surfaceEmissive;
            if (inTail)
            {
                loTail += emissiveContrib;
            }
            else
            {
                directRadiance += emissiveContrib;
                termSurfaceEmissive += emissiveContrib;
            }
        }

        // G7/P5: full soft-sun cone at the primary; 1 sample on the Lo_tail (and deeper).
        // Skip NEE on pure dielectrics. Real-time emissive terminals still get sun shading on the
        // emitter surface (faces read as lit), but skip self-emissive/env NEE and further bounces.
        float3 sunPathContrib = 0.0.xxx;
        float3 emissiveNeeContrib = 0.0.xxx;
        float3 envNeeContrib = 0.0.xxx;
        float sunVis = 0.0;
        if (opaqueWeight > 0.0)
        {
            if (!terminalEmissiveHit || bounce == 0u)
            {
                // Emitter terminals: 1 hard/soft sample is enough for face shading; full cone is for
                // ordinary surfaces. Avoids the stare-at-cube soft-sun tax.
                const uint softSunSamples = terminalEmissiveHit
                    ? 1u
                    : ((bounce == 0u) ? kPtSoftSunSampleCount : kPtSoftSunSampleCountDeep);
                sunVis = TracePrimarySunVisibility(rng, hitNormal, shadowOrigin, softSunSamples);
                const float3 sunContrib = opaqueWeight
                    * EvaluateDirectSun(
                        viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                        hitNormal, sunVis);
                sunPathContrib = throughput * sunContrib;
            }

            if (!terminalEmissiveHit)
            {
                // Mesh-light NEE must run in the GI tail too. Otherwise Cornell-box indirect
                // lighting relies on random BSDF rays hitting the ceiling light, which is extreme
                // variance and presents as frame-to-frame boiling before ReSTIR even runs.
                const float3 emissiveNee = opaqueWeight
                    * EvaluateDirectEmissive(
                        rng, viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                        hitNormal, shadowOrigin);
                emissiveNeeContrib = throughput * emissiveNee;

                const float3 envNee = opaqueWeight
                    * EvaluateDirectEnvironment(
                        rng, viewDir, f0, albedo, surfaceRoughness, surfaceMetallic,
                        hitNormal, shadowOrigin);
                envNeeContrib = throughput * envNee;
            }
        }
        if (inTail)
        {
            loTail += sunPathContrib;
            loTail += emissiveNeeContrib;
            loTail += envNeeContrib;
        }
        else
        {
            directRadiance += sunPathContrib;
            termDirectSun += sunPathContrib;
            directRadiance += emissiveNeeContrib;
            termDirectEmissive += emissiveNeeContrib;
            // Bounce-0 env NEE is part of direct (no dedicated term AOV historically).
            directRadiance += envNeeContrib;
        }

        // Real-time v2: primary-hit AO-gated SH ambient (devdoc/dxr/pt/crevice-darkening.md). Fills
        // crevices without the v1 washout from unoccluded per-bounce SH. Reference omits this.
        // Emissive terminals get ray-free ambient display; AO on a light surface is wasted TraceRays.
        if (kPtCenterPrimaryRays && bounce == 0u && !terminalEmissiveHit)
        {
            primaryAoVis =
                TracePrimaryAmbientOcclusion(rng, shadowOrigin, hitNormalGeom, g_PtAmbientAoRayCount);
            // Same soft-sun sample as the radiance path (G3: do not re-draw — AOV must match).
            primarySunVis = sunVis;
            const float3 ambientContrib =
                EvaluateRealTimeDiffuseAmbient(diffuseAlbedo, hitNormal, primaryAoVis);
            const float3 ambientPathContrib = throughput * ambientContrib;
            directRadiance += ambientPathContrib;
            termAmbient += ambientPathContrib;
        }
        else if (realTimePrimaryEmitterDisplay)
        {
            primaryAoVis = 1.0;
            primarySunVis = sunVis;
            const float3 ambientContrib =
                EvaluateRealTimeEmitterDisplayAmbient(diffuseAlbedo, hitNormal);
            const float3 ambientPathContrib = throughput * ambientContrib;
            directRadiance += ambientPathContrib;
            termAmbient += ambientPathContrib;

            const float3 visibleEmissive = EmissiveWithBloomHalo(surfaceEmissiveColor);
            const float3 visibleEmissiveContrib = throughput * visibleEmissive;
            directRadiance += visibleEmissiveContrib;
            termSurfaceEmissive += visibleEmissiveContrib;
        }

        if (bounce == 0u)
        {
            primaryHit = true;
            primaryInstanceId = payload.instanceId;
            primaryPrimitiveIndex = payload.primitiveIndex;
            primaryMotion = payload.primaryMotionNdc;
            primaryRoughness = surfaceRoughness;
            primaryDielectricWeight = dielectricWeight;
            const float nDotVPrimary = saturate(dot(hitNormalGeom, viewDir));

            float3 diffuseGuide;
            float3 specGuide;
            float3 guideNormal;
            float guideRoughness;
            ComputePtPrimaryRrMaterialGuides(
                albedo,
                hitNormal,
                surfaceRoughness,
                surfaceMetallic,
                material.transmission,
                material.indexOfRefraction,
                viewDir,
                diffuseGuide,
                specGuide,
                guideNormal,
                guideRoughness);

            // Glass: PSR-style transmission guides — depth, motion, and material guides describe the
            // refracted background surface, not the glass polygon (devdoc/dxr/pt/transmission-rr-guides.md).
            if (dielectricWeight > 0.0)
            {
                specHitDistGuide = g_MaxTraceDistance;

                const float guideOriginBias =
                    max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotVPrimary));
                const bool thinPane = material.thinWalled > 0.5;
                const float shellBias = thinPane ? max(guideOriginBias, kThinShellMinExitBias) : guideOriginBias;
                const float transmitFresnel = FresnelDielectric(
                    nDotVPrimary,
                    1.0 / max(material.indexOfRefraction, 1.0));
                const float transmitWeight = saturate(1.0 - transmitFresnel);

                const TransmissionGuideHit txGuide = TraceTransmissionGuide(
                    hitPos,
                    hitNormalGeom,
                    ray.Direction,
                    material.indexOfRefraction,
                    thinPane,
                    shellBias,
                    payload.instanceId);

                const float2 virtualMotion = ComputeTransmissionVirtualMotion(
                    pixel, hitPos, ray.Direction, txGuide, shellBias);

                if (txGuide.valid)
                {
                    primaryDepth = txGuide.depth;
                    primaryMotion = lerp(payload.primaryMotionNdc, virtualMotion, transmitWeight);

                    float3 bgDiffuse;
                    float3 bgSpec;
                    float3 bgNormal;
                    float bgRoughness;
                    const MaterialEntry bgMaterial = LoadMaterialForInstance(txGuide.instanceId);
                    float3 bgAlbedo;
                    float bgMetallic;
                    float3 bgEmissive;
                    ResolveSurfaceMaterialScalars(
                        txGuide.instanceId,
                        txGuide.primitiveIndex,
                        txGuide.barycentrics,
                        ComputeTransmissionGuideAlbedoLod(txGuide, pathConeWidth),
                        bgAlbedo,
                        bgRoughness,
                        bgMetallic,
                        bgEmissive);
                    const float3 bgViewDir = -txGuide.refractDir;
                    ComputePtPrimaryRrMaterialGuides(
                        bgAlbedo,
                        txGuide.shadingNormal,
                        bgRoughness,
                        bgMetallic,
                        bgMaterial.transmission,
                        bgMaterial.indexOfRefraction,
                        bgViewDir,
                        bgDiffuse,
                        bgSpec,
                        bgNormal,
                        bgRoughness);

                    diffuseGuide = lerp(diffuseGuide, bgDiffuse, transmitWeight);
                    specGuide = lerp(specGuide, bgSpec, transmitWeight);
                    guideNormal = normalize(lerp(guideNormal, bgNormal, transmitWeight));
                    guideRoughness = lerp(guideRoughness, bgRoughness, transmitWeight);
                }
                else
                {
                    primaryDepth = 1.0;
                    primaryMotion = lerp(payload.primaryMotionNdc, virtualMotion, transmitWeight);
                    const float3 skyGuide = float3(0.5, 0.5, 0.5);
                    diffuseGuide = lerp(diffuseGuide, skyGuide, transmitWeight);
                    specGuide = lerp(specGuide, skyGuide, transmitWeight);
                }
            }
            else
            {
                primaryDepth = payload.primaryDepth;
            }

            g_DiffuseAlbedoGuide[pixel] = float4(diffuseGuide, 1.0);
            g_SpecularAlbedoGuide[pixel] = float4(specGuide, 1.0);
            g_NormalRoughnessGuide[pixel] = float4(guideNormal, guideRoughness);

            // Stable RR4 spec hit-distance guide (devdoc/dxr/pt/rr4-spec-hitdist.md): trace ONE
            // DETERMINISTIC mirror ray from the primary hit (no RNG) so DLSS-RR can reproject
            // reflections at their virtual depth without wobble. Reflective surfaces only; rougher /
            // diffuse / miss report g_MaxTraceDistance ("no specular reprojection"). Independent of
            // the stochastic radiance bounce chosen below.
            // Scale out as dielectric weight rises — glass refraction must not use a mirror guide.
            const float mirrorGuideWeight =
                (1.0 - dielectricWeight)
                * (1.0 - smoothstep(0.45, 0.65, surfaceRoughness));
            if (mirrorGuideWeight > 0.0)
            {
                // Match the grazing-aware bounce offset — shadowOrigin self-intersects at silhouettes
                // and reports ~0 hit distance, which RR reads as black sliding rims (F8).
                const float guideOriginBias =
                    max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotVPrimary));
                RayDesc guideRay;
                guideRay.Origin = hitPos + hitNormal * guideOriginBias;
                guideRay.Direction = normalize(reflect(ray.Direction, hitNormal));
                guideRay.TMin = 0.001;
                guideRay.TMax = g_MaxTraceDistance;

                Payload guidePayload;
                ResetPayload(guidePayload);
                TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, guideRay, guidePayload);
                if (guidePayload.hit != 0)
                {
                    const float finiteGuide = max(guidePayload.hitDistance, 0.05);
                    specHitDistGuide = lerp(g_MaxTraceDistance, finiteGuide, mirrorGuideWeight);
                }
                else
                {
                    specHitDistGuide = g_MaxTraceDistance;
                }
            }
        }

        if (terminalEmissiveHit)
        {
            break;
        }

        if (bounce >= maxBounces)
        {
            // Terminal specular tail: the mirror-direction environment, energy-weighted (a
            // hall-of-mirrors fade instead of black). The DIFFUSE tail is covered by primary-hit SH
            // in real-time, or added here in reference mode. Only fires for paths that did not escape.
            const float terminalNdotV = saturate(dot(hitNormal, viewDir));
            const float3 reflectTail =
                SampleEnvironment(reflect(-viewDir, hitNormal), TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight))
                * EnvBrdfApprox(f0, surfaceRoughness, terminalNdotV);
            const float3 transmitTail = SampleEnvironment(
                ray.Direction,
                TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight));
            float3 terminal = lerp(reflectTail, transmitTail, dielectricWeight);
            if (!kPtCenterPrimaryRays && g_EnvLightImportanceCount == 0u)
            {
                terminal += diffuseAlbedo * EvaluateDiffuseIrradianceSh(hitNormal) / kPi
                    * g_EnvironmentIntensity;
            }
            const float3 terminalContrib = throughput * terminal;
            if (inTail)
            {
                loTail += terminalContrib;
            }
            else
            {
                directRadiance += terminalContrib;
            }
            break;
        }

        float3 nextDir;
        bool isSpecular = false;
        float scatterPdf = 1.0;
        const bool pathInMediumBefore = pathInMedium;
        SampleMaterialBounce(
            rng,
            hitNormal,
            ray.Direction,
            viewDir,
            f0,
            albedo,
            surfaceRoughness,
            surfaceMetallic,
            material.transmission,
            material.indexOfRefraction,
            material.thinWalled > 0.5,
            pathInMedium,
            nextDir,
            isSpecular,
            pathInMedium,
            scatterPdf,
            throughput);
        if (pathInMedium && !pathInMediumBefore && material.thinWalled < 0.5)
        {
            mediumTint = albedo;
        }
        else if (!pathInMedium && pathInMediumBefore)
        {
            mediumTint = 1.0.xxx;
        }

        lastScatterPdf = scatterPdf;

        // After the primary scatter: freeze connection throughput and start Lo_tail at xs with
        // local throughput = 1 (ReSTIR GI / G5). Shade reconnects as t1 * Lo_tail.
        if (bounce == 0u)
        {
            throughputAfterFirstScatter = throughput;
            throughput = 1.0.xxx;
            inTail = true;
            if (primaryDielectricWeight > 0.01
                || primaryRoughness <= kPtDeltaSpecularRoughness
                || scatterPdf >= kDeltaScatterPdf * 0.5)
            {
                sampleFlags |= kRestirSampleNoReuse;
            }
        }

        // Real-time: specular bounces add env on miss; diffuse bounces use primary-hit SH only.
        // Reference: always add the true sky. Transmitted rays always see through on miss.
        addEnvOnMiss = kPtCenterPrimaryRays ? (isSpecular || pathInMedium) : true;

        missEnvRoughness = TransmissionMissEnvRoughness(surfaceRoughness, dielectricWeight);

        if (kPtRussianRouletteEnabled && bounce >= kRussianRouletteStartBounce)
        {
            // Measure full-path throughput for the kill probability (parity with pre-G5), but only
            // divide the local Lo_tail throughput — shade still multiplies by t1.
            const float3 rrMeasure =
                inTail ? (throughputAfterFirstScatter * throughput) : throughput;
            const float rrProb = min(
                max(rrMeasure.r, max(rrMeasure.g, rrMeasure.b)),
                kRussianRouletteMaxProb);
            const float rrXi = PathRngNext(rng);
            if (rrProb <= 1e-4 || rrXi > rrProb)
            {
                break;
            }
            throughput /= rrProb;
        }

        const float nDotV = saturate(dot(hitNormal, viewDir));
        const float originBias = max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV));
        const bool thinPane = material.thinWalled > 0.5 && dielectricWeight > 0.0;
        ray.Origin = hitPos + nextDir * originBias;
        if (thinPane)
        {
            // Thin slab is zero-thickness; escape any physical shell (scaled-cube panes) before continuing.
            ray.Origin = hitPos + nextDir * max(originBias, kThinShellMinExitBias);
        }
        else if (dielectricWeight > 0.0 && !pathInMediumBefore && pathInMedium)
        {
            // Entering a solid volume: push past the entry interface so the refracted segment does not
            // immediately self-hit the same shell triangle (reads as frosted/milky glass).
            ray.Origin = hitPos + nextDir * max(originBias, 0.02);
        }
        else if (dielectricWeight > 0.0 && pathInMediumBefore && !pathInMedium)
        {
            ray.Origin = hitPos + nextDir * max(originBias, 0.02);
        }
        ray.Direction = nextDir;
    }

    // G6: clamp Lo_tail before reservoir write; composite safety clamp remains below.
    float3 loTailForStore = loTail;
    if (kPtFireflyClampEnabled && g_PtDebugIsolateMode != 8u)
    {
        loTailForStore = ClampRadiance(loTail);
    }

    const float3 termIndirect = throughputAfterFirstScatter * loTail;
    const float3 radiancePreClamp = directRadiance + termIndirect;
    float3 radiance = directRadiance + throughputAfterFirstScatter * loTailForStore;
    if (kPtFireflyClampEnabled && g_PtDebugIsolateMode != 8u)
    {
        radiance = ClampRadiance(radiance);
    }

    // R1/R2: store Y = t1·Lo_tail (indirect contribution). Temporal shades direct + Y·W.
    if (!haveInitialSample)
    {
        sampleFlags |= kRestirSampleNoReuse;
    }
    const float3 indirectContrib = throughputAfterFirstScatter * loTailForStore;
    const RestirInitialSample initialSample = RestirMakeInitialSample(
        sampleXs,
        sampleNs,
        indirectContrib,
        samplePdf,
        pathSeed,
        sampleFlags);
    g_InitialSample[pixelIndex] = initialSample;
    g_ReservoirCurrent[pixelIndex] = RestirMakePassthroughReservoir(initialSample);

    const float3 displayRadiance = SelectPtDebugRadiance(
        g_PtDebugIsolateMode,
        primaryHit,
        radiance,
        radiancePreClamp,
        termDirectSun,
        termDirectEmissive,
        termSurfaceEmissive,
        termAmbient,
        termIndirect,
        primaryAoVis,
        primarySunVis,
        specHitDistGuide);

    // Unclamped bounce-0 direct for ReSTIR shade; g_Output keeps full M=1 / isolate AOVs.
    g_DirectOutput[pixel] = float4(directRadiance, 0.0);
    g_Output[pixel] = float4(displayRadiance, specHitDistGuide);
    g_DepthOutput[pixel] = primaryDepth;
    g_MotionOutput[pixel] = float4(primaryMotion, 0.0, 1.0);
    g_Metadata[pixel] = primaryHit
        ? uint2(primaryInstanceId + 1u, primaryPrimitiveIndex)
        : uint2(0, 0);
}

[shader("miss")]
void PathTracerMiss(inout Payload payload)
{
    if (payload.hit == kPayloadFlagVisibility)
    {
        payload.hit = 0;
        return;
    }

    payload.hit = 0;
    payload.hitDistance = g_MaxTraceDistance;
}

// Per-triangle ray-cone LOD constant (RTG ch. 20): 0.5·log2(albedo texel area / world area).
// Needs ObjectToWorld3x4, so it must run in the closest-hit; the raygen adds the per-path terms.
float ComputeTriangleAlbedoLodConstant(uint instanceId, uint primitiveIndex)
{
    const MaterialEntry material = LoadMaterialForInstance(instanceId);
    if (material.albedoTexIndex == 0xFFFFFFFFu || material.albedoUvOffsetFloats == 0xFFFFFFFFu)
    {
        return 0.0;
    }

    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const uint indexBase = geo.indexUintOffset + primitiveIndex * 3u;
    const uint i0 = g_SceneIndices[indexBase + 0];
    const uint i1 = g_SceneIndices[indexBase + 1];
    const uint i2 = g_SceneIndices[indexBase + 2];

    const float3x4 objectToWorld = ObjectToWorld3x4();
    const float3 w0 = mul(objectToWorld, float4(LoadObjectPosition(geo, i0), 1.0)).xyz;
    const float3 w1 = mul(objectToWorld, float4(LoadObjectPosition(geo, i1), 1.0)).xyz;
    const float3 w2 = mul(objectToWorld, float4(LoadObjectPosition(geo, i2), 1.0)).xyz;
    const float worldArea = 0.5 * length(cross(w1 - w0, w2 - w0));

    const float2 uv0 = LoadObjectUv(geo, i0, material.albedoUvOffsetFloats);
    const float2 uv1 = LoadObjectUv(geo, i1, material.albedoUvOffsetFloats);
    const float2 uv2 = LoadObjectUv(geo, i2, material.albedoUvOffsetFloats);
    const float2 e1 = uv1 - uv0;
    const float2 e2 = uv2 - uv0;
    const float uvArea = 0.5 * abs(e1.x * e2.y - e1.y * e2.x);

    uint texWidth;
    uint texHeight;
    g_BindlessTextures[NonUniformResourceIndex(material.albedoTexIndex)]
        .GetDimensions(texWidth, texHeight);
    const float texelArea = uvArea * float(texWidth) * float(texHeight);

    if (worldArea <= 1e-12 || texelArea <= 1e-12)
    {
        return 0.0;
    }

    return 0.5 * log2(texelArea / worldArea);
}

[shader("closesthit")]
void PathTracerClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    if (payload.hit == kPayloadFlagVisibility)
    {
        return;
    }

    const uint request = payload.hit;
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];

    const float3 rayDir = WorldRayDirection();
    const float hitT = RayTCurrent();

    const bool hitBackFace = HitKind() == kHitKindTriangleBackFace;
    float3 hitNormal = ComputeWorldShadingNormal(geo, primitiveIndex, attribs.barycentrics);
    if (hitBackFace)
    {
        hitNormal = -hitNormal;
    }
    if (dot(hitNormal, rayDir) > 0.0)
    {
        hitNormal = -hitNormal;
    }

    payload.hit = 1u | (hitBackFace ? kPayloadHitBackFace : 0u);
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;
    payload.hitDistance = hitT;
    payload.normal = hitNormal;

    // G7/P2: skip primary-only / shading work unless the raygen asked for it. Shadow and
    // transmission-visibility segments only need geometric normal + instance + distance.
    if ((request & kPayloadReqShadingData) != 0u)
    {
        payload.shadingNormal = ApplyWorldNormalMap(
            instanceId, primitiveIndex, attribs.barycentrics, hitNormal, -rayDir, 0.0);
        payload.barycentrics = attribs.barycentrics;
        payload.triangleLod = ComputeTriangleAlbedoLodConstant(instanceId, primitiveIndex);
    }
    else
    {
        payload.shadingNormal = hitNormal;
        payload.barycentrics = 0.0.xx;
        payload.triangleLod = 0.0;
    }

    if ((request & kPayloadReqPrimarySurface) != 0u)
    {
        ComputeVertexInterpolatedPrimarySurface(
            payload.primaryMotionNdc,
            payload.primaryDepth,
            instanceId,
            primitiveIndex,
            attribs.barycentrics);
    }
    else
    {
        payload.primaryMotionNdc = 0.0.xx;
        payload.primaryDepth = 1.0;
    }
}
