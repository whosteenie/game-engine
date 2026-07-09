// DXR path tracer — Phase P2 core integrator (devdoc/dxr/path-tracing.md).
//
// Megakernel: the raygen owns the bounce loop (throughput + NEE + BRDF sampling + Russian roulette).
// Closest-hit only extracts surface data; shadow and bounce traces originate from raygen so
// MaxTraceRecursionDepth = 1 suffices. P1 direct-only shading is subsumed by the loop.

#include "hit_shading.hlsli"

RWTexture2D<float4> g_Output : register(u0);   // rgb = HDR radiance, a = specular hit-distance guide (RR4)
RWTexture2D<float> g_DepthOutput : register(u1); // hyperbolic depth [0,1] at primary hit (DLSS)
RWTexture2D<uint2> g_Metadata : register(u2);  // (instanceId+1, primitiveIndex)
RWTexture2D<float4> g_MotionOutput : register(u3); // NDC motion (curr - prev) at primary hit, matches RT4
// P4b bounce-0 RR material guides (devdoc/dxr/pt/full-rr-guides.md): the same hit that seeds the
// integrator produces every DLSS-RR guide, so color and guides agree at the sub-pixel level.
RWTexture2D<float4> g_DiffuseAlbedoGuide : register(u4);   // RGBA8: albedo·(1−metallic)
RWTexture2D<float4> g_SpecularAlbedoGuide : register(u5);  // RGBA8: EnvBRDFApprox2(F0, roughness², NoV)
RWTexture2D<float4> g_NormalRoughnessGuide : register(u6); // RGBA16F: world normal xyz + roughness w

// P4b: previous-frame object-to-world rows per instance (indexed by InstanceID == object index).
// Explicit rows (row_i = column-major glm m[col][i]) — see DxrPrevInstanceTransformEntry.
struct PrevInstanceTransform
{
    float4 row0;
    float4 row1;
    float4 row2;
};
StructuredBuffer<PrevInstanceTransform> g_PrevInstanceTransforms : register(t14);

// F2/F2b emissive NEE: compact list of emissive instances (world-space AABB area lights).
struct EmissiveLightEntry
{
    float3 boundsMin;
    float pickWeight;
    float3 boundsMax;
    float surfaceArea;
    float3 emissive;
    uint instanceId;
};
StructuredBuffer<EmissiveLightEntry> g_EmissiveLights : register(t15);

// Path-tracer-only packing in reflection cbuffer fields this pass does not otherwise use.
#define kPtFireflyClampEnabled (g_AoRayCount != 0u)
#define kPtRussianRouletteEnabled (g_HasGiTrace != 0u)
#define kPtCenterPrimaryRays (g_RoughnessCutoff > 0.5)
#define g_PtAmbientStrength g_GiStrength
#define g_PtAmbientAoRayCount uint(round(saturate(_PadUnjitteredViewProj.x)))
#define kPtHasInstanceMotion (_PadUnjitteredViewProj.y > 0.5)
// Ray-cone pixel spread angle (radians/pixel ≈ 2·tan(fovY/2)/renderHeight) for albedo texture LOD.
// Mip-0 sampling flickers at texel frequency under DLSS jitter; with P4b the albedo GUIDE comes
// from the PT, and RR remodulates its output with that guide, imprinting the flicker on screen.
#define g_PtPixelSpreadAngle max(_PadUnjitteredViewProj.z, 1e-6)
// Matches lit.vs uTemporalHistoryValid: when false, prevClip = currClip (zero motion).
#define kPtMotionHistoryValid (_PadUnjitteredViewProj.w > 0.5)
// Radiance-term isolation for black-edge debugging (RenderDebugMode PtIsolate*).
#define g_PtDebugIsolateMode uint(round(saturate(_PadPtEmissiveNee)))

static const uint kPtAmbientAoRngSalt = 128u;
static const uint kPtSoftSunRngSalt = 32u;
static const uint kPtEmissiveNeeSalt = 96u;
static const uint kPtSoftSunSampleCount = 4u;

static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const uint kPayloadFlagVisibility = 2u;
static const uint kRussianRouletteStartBounce = 3u;
static const float kRussianRouletteMaxProb = 0.95;

struct Payload
{
    float3 normal;
    float hitDistance;
    uint instanceId;
    uint primitiveIndex;
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

// DLSS-RR specular albedo guide (Integration Guide appendix, [Ray Tracing Gems ch. 32]):
// preintegrated environment BRDF — NOT raw F0. alpha = roughness².
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
    payload.hitDistance = 0.0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hit = 0;
    payload.barycentrics = 0.0.xx;
    payload.triangleLod = 0.0;
    payload.primaryMotionNdc = 0.0.xx;
    payload.primaryDepth = 1.0;
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

// Soft sun visibility: cone-jittered shadow rays matching shadows.hlsl / reflections.hlsl.
float TraceSoftSunVisibility(float3 origin, float3 shadingNormal, uint2 rngPixel, uint salt)
{
    const float3 sunDir = normalize(g_SunDirection);
    if (dot(shadingNormal, sunDir) <= 0.0)
    {
        return 0.0;
    }

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(sunDir, tangent, bitangent);

    float visSum = 0.0;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < kPtSoftSunSampleCount; ++sampleIndex)
    {
        const float4 xi = RandomXi4(rngPixel, g_FrameIndex, salt + sampleIndex);
        const float diskRadius = sqrt(xi.x);
        const float diskPhi = 2.0 * kPi * xi.y;
        const float2 disk = float2(diskRadius * cos(diskPhi), diskRadius * sin(diskPhi));
        const float3 rayDir = normalize(
            sunDir + (tangent * disk.x + bitangent * disk.y) * g_SunAngularTanRadius);
        visSum += TraceVisibility(origin, rayDir, g_MaxTraceDistance);
    }

    return visSum / float(kPtSoftSunSampleCount);
}

// Ray-cone filtered albedo (RTG ch. 20): lod = triangleLod + log2(coneWidth) − log2(n·v).
// Matches the raster path's mip-filtered footprint closely enough that the albedo value is stable
// under sub-pixel jitter — required for the P4b diffuse-albedo GUIDE, which RR remodulates with.
float3 SampleSurfaceAlbedo(uint instanceId, uint primitiveIndex, float2 barycentrics, float albedoLod)
{
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];
    const MaterialEntry material = g_Materials[instanceId];

    float3 albedo = material.albedo;
    if (material.albedoTexIndex != 0xFFFFFFFFu && material.albedoUvOffsetFloats != 0xFFFFFFFFu)
    {
        const float2 hitUv =
            ComputeHitUv(geo, primitiveIndex, material.albedoUvOffsetFloats, barycentrics);
        const float3 texel =
            g_BindlessTextures[NonUniformResourceIndex(material.albedoTexIndex)]
                .SampleLevel(g_LinearWrapSampler, hitUv, max(albedoLod, 0.0)).rgb;
        albedo *= texel;
    }
    return albedo;
}

float ComputeAlbedoLod(Payload payload, float coneWidth, float3 rayDirection)
{
    const float nDotD = max(saturate(dot(payload.normal, -rayDirection)), 0.05);
    return payload.triangleLod + log2(max(coneWidth, 1e-6)) - log2(nDotD);
}

float TracePrimaryAmbientOcclusion(uint2 pixel, float3 origin, float3 normal, uint rayCount)
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
        const float2 aoXi = RandomXi4(pixel, g_FrameIndex, aoIndex + kPtAmbientAoRngSalt).xy;
        const float3 aoDir = CosineSampleHemisphere(normal, aoXi);
        aoSum += TraceVisibility(origin, aoDir, aoRadius);
    }

    return aoSum / float(rayCount);
}

float3 EvaluateRealTimeDiffuseAmbient(
    uint2 pixel,
    float3 diffuseAlbedo,
    float3 hitNormal,
    float3 shadowOrigin)
{
    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    if (diffuseWeight <= 0.02)
    {
        return 0.0.xxx;
    }

    const float aoVisibility =
        TracePrimaryAmbientOcclusion(pixel, shadowOrigin, hitNormal, g_PtAmbientAoRayCount);
    const float3 irradiance = EvaluateDiffuseIrradianceSh(hitNormal);
    return diffuseAlbedo * irradiance / kPi
        * aoVisibility
        * g_EnvironmentIntensity
        * g_PtAmbientStrength;
}

float TracePrimarySunVisibility(
    uint2 pixel,
    uint bounceIndex,
    float3 hitNormal,
    float3 shadowOrigin)
{
    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    if (ndotl <= 0.0)
    {
        return 0.0;
    }

    return (g_SunAngularTanRadius > 1e-6)
        ? TraceSoftSunVisibility(shadowOrigin, hitNormal, pixel, bounceIndex + kPtSoftSunRngSalt)
        : TraceVisibility(shadowOrigin, sunL, g_MaxTraceDistance);
}

float3 EvaluateDirectSun(
    uint2 pixel,
    uint bounceIndex,
    float3 diffuseAlbedo,
    float3 hitNormal,
    float3 shadowOrigin)
{
    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    if (ndotl <= 0.0)
    {
        return 0.0.xxx;
    }

    const float sunVis = TracePrimarySunVisibility(pixel, bounceIndex, hitNormal, shadowOrigin);
    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    return diffuseAlbedo * sunRadiance * ndotl / kPi * sunVis;
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

// Uniform sample on an axis-aligned box shell; pdfArea is 1 / total surface area.
void SampleUniformPointOnAabbSurface(
    float3 boundsMin,
    float3 boundsMax,
    float3 xi,
    out float3 surfacePoint,
    out float3 faceNormal,
    out float pdfArea)
{
    const float3 size = max(boundsMax - boundsMin, 0.0.xxx);
    const float areaPosX = size.y * size.z;
    const float areaNegX = areaPosX;
    const float areaPosY = size.x * size.z;
    const float areaNegY = areaPosY;
    const float areaPosZ = size.x * size.y;
    const float areaNegZ = areaPosZ;
    const float totalArea = 2.0 * (areaPosX + areaPosY + areaPosZ);

    if (totalArea <= 1e-8)
    {
        surfacePoint = 0.5 * (boundsMin + boundsMax);
        faceNormal = float3(0.0, 1.0, 0.0);
        pdfArea = 1.0;
        return;
    }

    pdfArea = 1.0 / totalArea;
    float facePick = xi.x * totalArea;

    if (facePick < areaPosX)
    {
        faceNormal = float3(1.0, 0.0, 0.0);
        surfacePoint = float3(
            boundsMax.x,
            lerp(boundsMin.y, boundsMax.y, xi.y),
            lerp(boundsMin.z, boundsMax.z, xi.z));
        return;
    }
    facePick -= areaPosX;

    if (facePick < areaNegX)
    {
        faceNormal = float3(-1.0, 0.0, 0.0);
        surfacePoint = float3(
            boundsMin.x,
            lerp(boundsMin.y, boundsMax.y, xi.y),
            lerp(boundsMin.z, boundsMax.z, xi.z));
        return;
    }
    facePick -= areaNegX;

    if (facePick < areaPosY)
    {
        faceNormal = float3(0.0, 1.0, 0.0);
        surfacePoint = float3(
            lerp(boundsMin.x, boundsMax.x, xi.y),
            boundsMax.y,
            lerp(boundsMin.z, boundsMax.z, xi.z));
        return;
    }
    facePick -= areaPosY;

    if (facePick < areaNegY)
    {
        faceNormal = float3(0.0, -1.0, 0.0);
        surfacePoint = float3(
            lerp(boundsMin.x, boundsMax.x, xi.y),
            boundsMin.y,
            lerp(boundsMin.z, boundsMax.z, xi.z));
        return;
    }
    facePick -= areaNegY;

    if (facePick < areaPosZ)
    {
        faceNormal = float3(0.0, 0.0, 1.0);
        surfacePoint = float3(
            lerp(boundsMin.x, boundsMax.x, xi.y),
            lerp(boundsMin.y, boundsMax.y, xi.z),
            boundsMax.z);
        return;
    }

    faceNormal = float3(0.0, 0.0, -1.0);
    surfacePoint = float3(
        lerp(boundsMin.x, boundsMax.x, xi.y),
        lerp(boundsMin.y, boundsMax.y, xi.z),
        boundsMin.z);
}

// One emissive area-light sample per bounce (AABB surface + shadow ray). MIS vs cosine hemisphere.
float3 EvaluateDirectEmissive(
    uint2 pixel,
    uint bounceIndex,
    float3 diffuseAlbedo,
    float3 hitNormal,
    float3 shadowOrigin)
{
    if (g_EmissiveLightCount == 0u || g_EmissiveLightPickWeightSum <= 0.0)
    {
        return 0.0.xxx;
    }

    const float4 xiPick = RandomXi4(pixel, g_FrameIndex, bounceIndex + kPtEmissiveNeeSalt);
    const float4 xiSurface = RandomXi4(pixel, g_FrameIndex, bounceIndex + kPtEmissiveNeeSalt + 1u);

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

    float3 lightPoint;
    float3 faceNormal;
    float pdfArea;
    SampleUniformPointOnAabbSurface(
        light.boundsMin,
        light.boundsMax,
        xiSurface.xyz,
        lightPoint,
        faceNormal,
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

    const float cosThetaEmitter = saturate(dot(faceNormal, -wi));
    if (cosThetaEmitter <= 0.0)
    {
        return 0.0.xxx;
    }

    const float visibility = TraceVisibility(shadowOrigin, wi, dist - 0.001);
    if (visibility <= 0.0)
    {
        return 0.0.xxx;
    }

    const float pickPdf = light.pickWeight / g_EmissiveLightPickWeightSum;
    const float pdfSolidAngle = pickPdf * pdfArea * dist2 / max(cosThetaEmitter, 1e-6);
    const float pdfBsdf = cosThetaReceiver / kPi;
    const float misWeight = BalanceHeuristic(pdfSolidAngle, pdfBsdf);

    const float3 bsdf = diffuseAlbedo / kPi;
    const float geometryTerm = cosThetaReceiver * cosThetaEmitter / dist2;

    return bsdf * EmissiveWithBloomHalo(light.emissive) * geometryTerm * visibility * misWeight
        / max(pickPdf * pdfArea, 1e-8);
}

// Continuous glass weight: transmission fades out as metallic rises (metals are opaque conductors).
float DielectricWeight(float transmission, float metallic)
{
    return saturate(transmission) * (1.0 - saturate(metallic));
}

// PT-A: unpolarized Fresnel reflectance for dielectric interfaces (air ↔ material).
float FresnelDielectric(float cosThetaI, float eta)
{
    const float cosI = abs(cosThetaI);
    const float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0)
    {
        return 1.0;
    }

    const float cosT = sqrt(max(1.0 - sin2T, 0.0));
    const float rPar = (eta * cosI - cosT) / max(eta * cosI + cosT, 1e-6);
    const float rPerp = (cosI - eta * cosT) / max(cosI + eta * cosT, 1e-6);
    return saturate(0.5 * (rPar * rPar + rPerp * rPerp));
}

bool RefractSnell(float3 wi, float3 n, float eta, out float3 wt)
{
    // wi points away from the surface along the incoming path; n faces the incident medium.
    float cosI = dot(wi, n);
    if (cosI < 0.0)
    {
        n = -n;
        cosI = -cosI;
    }

    const float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T > 1.0)
    {
        wt = 0.0.xxx;
        return false;
    }

    const float cosT = sqrt(max(1.0 - sin2T, 0.0));
    wt = eta * wi - (eta * cosI + cosT) * n;
    return dot(wt, wt) > 1e-8;
}

// Map material roughness to environment cube mip without crushing HDRIs to black at 1.0.
float EnvironmentMipRoughness(float roughness)
{
    const float r = saturate(roughness);
    return r * r * 0.35;
}

// Transmitted rays blur via direction perturbation; keep env LOD low so sky stays visible.
float TransmissionMissEnvRoughness(float roughness, float dielectricWeight)
{
    const float mip = EnvironmentMipRoughness(roughness);
    return lerp(mip, min(mip, 0.05), saturate(dielectricWeight));
}

// Build refracted direction for a dielectric interface (no throughput bookkeeping).
bool ComputeDielectricRefractDir(
    float3 hitNormal,
    float3 rayDir,
    float ior,
    bool pathInMedium,
    out float3 refractDir)
{
    const float3 wi = -rayDir;
    float3 n = hitNormal;
    if (dot(wi, n) < 0.0)
    {
        n = -n;
    }

    const float iorClamped = max(ior, 1.0);
    const float eta = pathInMedium ? iorClamped : (1.0 / iorClamped);
    float3 refracted;
    if (!RefractSnell(wi, n, eta, refracted))
    {
        return false;
    }

    refractDir = normalize(refracted);
    return true;
}

// One-bounce mirror trace for Fresnel reflection on dielectric interfaces (no stochastic split).
float3 TraceDielectricReflectContrib(
    float3 hitPos,
    float3 hitNormal,
    float3 reflectDir,
    float envRoughness,
    uint excludeInstanceId)
{
    const float nDotV = saturate(dot(hitNormal, -reflectDir));
    const float bias = max(0.01, 0.01 * (1.0 + 2.0 * (1.0 - nDotV)));
    RayDesc reflectRay;
    reflectRay.Origin = hitPos + hitNormal * bias;
    reflectRay.Direction = reflectDir;
    reflectRay.TMin = 0.001;
    reflectRay.TMax = g_MaxTraceDistance;

    Payload reflectPayload;
    ResetPayload(reflectPayload);
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, reflectRay, reflectPayload);
    if (reflectPayload.hit != 0)
    {
        // Self-hits on thin glass (front face reflects into back face) read as bogus gray albedo.
        if (reflectPayload.instanceId == excludeInstanceId)
        {
            return SampleEnvironment(reflectDir, envRoughness);
        }

        const MaterialEntry hitMaterial = g_Materials[reflectPayload.instanceId];
        const float3 emissive = hitMaterial.emissive;
        if (max(emissive.r, max(emissive.g, emissive.b)) > 1e-4)
        {
            return emissive;
        }
    }

    return SampleEnvironment(reflectDir, envRoughness);
}

// Fresnel reflect (traced) + Snell refract (path continuation). No stochastic Fresnel roulette —
// avoids per-pixel reflect/refract speckle that reads as double vision at 1 spp.
void SampleDielectricInterface(
    float3 hitPos,
    float3 hitNormal,
    float3 rayDir,
    float roughness,
    float ior,
    bool pathInMedium,
    uint instanceId,
    float2 roughXi,
    out float3 fresnelReflectRadiance,
    out float3 nextDir,
    out bool outPathInMedium,
    out float scatterPdf,
    inout float3 throughput)
{
    fresnelReflectRadiance = 0.0.xxx;

    const float3 wi = -rayDir;
    float3 n = hitNormal;
    if (dot(wi, n) < 0.0)
    {
        n = -n;
    }

    const float iorClamped = max(ior, 1.0);
    const float eta = pathInMedium ? iorClamped : (1.0 / iorClamped);
    const bool enteringMedium = !pathInMedium;
    const float cosThetaI = dot(wi, n);
    const float fresnel = FresnelDielectric(cosThetaI, eta);
    const float3 reflectDir = normalize(reflect(wi, n));
    const float envRough = TransmissionMissEnvRoughness(roughness, 1.0);

    outPathInMedium = pathInMedium;
    scatterPdf = 1.0;

    if (fresnel > 1e-6)
    {
        fresnelReflectRadiance = fresnel
            * TraceDielectricReflectContrib(hitPos, n, reflectDir, envRough, instanceId);
    }

    throughput *= max(1.0 - fresnel, 1e-6);

    if ((1.0 - fresnel) < 1e-4)
    {
        nextDir = reflectDir;
        return;
    }

    float3 refracted;
    if (!RefractSnell(wi, n, eta, refracted))
    {
        nextDir = reflectDir;
        throughput = 0.0.xxx;
        return;
    }

    nextDir = normalize(refracted);
    outPathInMedium = enteringMedium;

    const float rough2 = roughness * roughness;
    if (rough2 > 0.0)
    {
        const float3 perturb = CosineSampleHemisphere(nextDir, roughXi);
        nextDir = normalize(lerp(nextDir, perturb, rough2));
    }
}

// Opaque GGX + cosine hemisphere. No mirror fast-path — roughness 0 is handled by GGX delta limit.
void SampleOpaqueInterface(
    uint2 pixel,
    uint bounceIndex,
    float3 hitNormal,
    float3 viewDir,
    float3 f0,
    float3 diffuseAlbedo,
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
    const float3 specularEnergy =
        FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(ggxRoughness, 0.55));

    float specProb = saturate(max(specularEnergy.r, max(specularEnergy.g, specularEnergy.b)));
    specProb = lerp(specProb, 1.0, saturate(metallic));

    // Metals never use the diffuse lobe (diffuseAlbedo = 0 when metallic). Always specular GGX.
    const bool traceSpecular = (metallic > 0.5) || ((lobeXi < specProb) && (ggxRoughness < 0.95));

    if (traceSpecular)
    {
        const float3 halfVector = SampleGgxVndfHalfVector(hitNormal, viewDir, ggxRoughness, xi2d);
        nextDir = normalize(reflect(-viewDir, halfVector));
        if (dot(nextDir, hitNormal) <= 1e-4)
        {
            nextDir = normalize(reflect(-viewDir, hitNormal));
        }
        scatterPdf = max(specProb, 1e-6);
        throughput *= f0 / scatterPdf;
    }
    else
    {
        nextDir = CosineSampleHemisphere(hitNormal, xi2d);
        const float cosTheta = saturate(dot(hitNormal, nextDir));
        scatterPdf = max((1.0 - specProb) * cosTheta / kPi, 1e-6);
        throughput *= diffuseAlbedo / max(1.0 - specProb, 1e-6);
    }

    isSpecular = traceSpecular;
}

// Unified material bounce: one stochastic lobe pick weighted by transmission·(1−metallic), then
// dielectric Fresnel/Snell or opaque GGX. Sliders interpolate smoothly — no hard cutoffs.
bool SampleMaterialBounce(
    uint2 pixel,
    uint bounceIndex,
    float3 hitPos,
    float3 hitNormal,
    float3 rayDir,
    float3 viewDir,
    float3 f0,
    float3 diffuseAlbedo,
    float roughness,
    float metallic,
    float transmission,
    float ior,
    bool pathInMedium,
    uint instanceId,
    out float3 nextDir,
    out bool isSpecular,
    out bool outPathInMedium,
    out float scatterPdf,
    out float3 interfaceAddRadiance,
    inout float3 throughput)
{
    const float dielectricWeight = DielectricWeight(transmission, metallic);
    const float4 xi = RandomXi4(pixel, g_FrameIndex, bounceIndex + 1u);

    outPathInMedium = pathInMedium;
    isSpecular = false;
    scatterPdf = 1.0;
    interfaceAddRadiance = 0.0.xxx;

    if (dielectricWeight > 0.0 && xi.w < dielectricWeight)
    {
        throughput /= max(dielectricWeight, 1e-6);
        const float3 throughputAtInterface = throughput;
        float3 fresnelReflect = 0.0.xxx;
        SampleDielectricInterface(
            hitPos,
            hitNormal,
            rayDir,
            roughness,
            ior,
            pathInMedium,
            instanceId,
            xi.xy,
            fresnelReflect,
            nextDir,
            outPathInMedium,
            scatterPdf,
            throughput);
        interfaceAddRadiance = throughputAtInterface * fresnelReflect;
        isSpecular = true;
        return true;
    }

    if (dielectricWeight > 0.0)
    {
        throughput /= max(1.0 - dielectricWeight, 1e-6);
    }

    SampleOpaqueInterface(
        pixel,
        bounceIndex,
        hitNormal,
        viewDir,
        f0,
        diffuseAlbedo,
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
        return max(
            radiancePreClamp - termDirectSun - termDirectEmissive - termSurfaceEmissive
                - termAmbient,
            0.0.xxx);
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

    const float4 primaryXi = RandomXi4(pixel, g_FrameIndex, 0u);
    const float2 primaryOffset = kPtCenterPrimaryRays ? float2(0.5, 0.5) : primaryXi.zw;
    const float2 texCoord = (float2(pixel) + primaryOffset) / float2(g_OutputSize);
    const float2 clipXY = PixelToClipXY(texCoord);

    const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
    const float3 farWorld = farH.xyz / farH.w;
    const float3 cameraRayDir = normalize(farWorld - g_CameraPos);

    const uint maxBounces = clamp(g_SamplesPerPixel, 1u, 8u);

    float3 radiance = 0.0.xxx;
    float3 throughput = 1.0.xxx;

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
    float lastScatterPdf = 1.0;
    bool pathInMedium = false;

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

            if (addEnvOnMiss)
            {
                radiance += throughput
                    * SampleEnvironment(ray.Direction, missEnvRoughness);
            }
            break;
        }

        pathConeWidth += g_PtPixelSpreadAngle * payload.hitDistance;

        const MaterialEntry material = g_Materials[payload.instanceId];
        const float3 albedo = SampleSurfaceAlbedo(
            payload.instanceId,
            payload.primitiveIndex,
            payload.barycentrics,
            ComputeAlbedoLod(payload, pathConeWidth, ray.Direction));
        const float3 hitNormal = payload.normal;
        const float3 viewDir = -ray.Direction;
        const float3 hitPos = ray.Origin + ray.Direction * payload.hitDistance;
        const float3 shadowOrigin = hitPos + hitNormal * max(payload.hitDistance * 0.001, 0.002);

        const float3 f0 = lerp(0.04.xxx, albedo, material.metallic);
        const float dielectricWeight =
            DielectricWeight(material.transmission, material.metallic);
        const float3 specularEnergy =
            FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(material.roughness, 0.55));
        const float3 diffuseAlbedo =
            albedo * (1.0.xxx - specularEnergy) * (1.0 - material.metallic) * (1.0 - dielectricWeight);

        const float emissiveLuminance = max(material.emissive.r, max(material.emissive.g, material.emissive.b));
        if (emissiveLuminance > 1e-4)
        {
            const float pickPdf = EmissiveLightPickPdf(payload.instanceId);
            const float misHit = (pickPdf > 0.0 && lastScatterPdf > 0.0)
                ? BalanceHeuristic(lastScatterPdf, pickPdf)
                : 1.0;
            const float3 surfaceEmissive = EmissiveWithBloomHalo(material.emissive) * misHit;
            radiance += throughput * surfaceEmissive;
            termSurfaceEmissive += throughput * surfaceEmissive;
        }

        const float3 sunContrib = EvaluateDirectSun(
            pixel, bounce, diffuseAlbedo, hitNormal, shadowOrigin);
        radiance += throughput * sunContrib;
        termDirectSun += throughput * sunContrib;

        const float3 emissiveContrib = EvaluateDirectEmissive(
            pixel, bounce, diffuseAlbedo, hitNormal, shadowOrigin);
        radiance += throughput * emissiveContrib;
        termDirectEmissive += throughput * emissiveContrib;

        // Real-time v2: primary-hit AO-gated SH ambient (devdoc/dxr/pt/crevice-darkening.md). Fills
        // crevices without the v1 washout from unoccluded per-bounce SH. Reference omits this.
        if (kPtCenterPrimaryRays && bounce == 0u)
        {
            primaryAoVis =
                TracePrimaryAmbientOcclusion(pixel, shadowOrigin, hitNormal, g_PtAmbientAoRayCount);
            primarySunVis = TracePrimarySunVisibility(pixel, bounce, hitNormal, shadowOrigin);
            const float3 ambientContrib =
                EvaluateRealTimeDiffuseAmbient(pixel, diffuseAlbedo, hitNormal, shadowOrigin);
            radiance += throughput * ambientContrib;
            termAmbient += throughput * ambientContrib;
        }

        if (bounce == 0u)
        {
            primaryHit = true;
            primaryInstanceId = payload.instanceId;
            primaryPrimitiveIndex = payload.primitiveIndex;
            primaryMotion = payload.primaryMotionNdc;
            const float nDotVPrimary = saturate(dot(hitNormal, viewDir));
            // Glass: refracted content is at a different depth than the surface. Trace a deterministic
            // refract ray for DLSS depth; zero MV avoids translation smear from surface reprojection.
            if (dielectricWeight > 0.01)
            {
                primaryMotion = 0.0.xx;
                specHitDistGuide = g_MaxTraceDistance;

                float3 refractDir;
                if (ComputeDielectricRefractDir(
                        hitNormal,
                        ray.Direction,
                        material.indexOfRefraction,
                        false,
                        refractDir))
                {
                    const float guideOriginBias =
                        max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotVPrimary));
                    RayDesc depthRay;
                    depthRay.Origin = hitPos + refractDir * guideOriginBias;
                    depthRay.Direction = refractDir;
                    depthRay.TMin = 0.001;
                    depthRay.TMax = g_MaxTraceDistance;

                    Payload depthPayload;
                    ResetPayload(depthPayload);
                    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, depthRay, depthPayload);
                    if (depthPayload.hit != 0 && depthPayload.instanceId != payload.instanceId)
                    {
                        primaryDepth = depthPayload.primaryDepth;
                    }
                    else
                    {
                        primaryDepth = 1.0;
                    }
                }
            }
            else
            {
                primaryDepth = payload.primaryDepth;
            }

            // P4b RR material guides from the SAME hit that seeds the integrator. Encodings match
            // rr_guides.ps.hlsl modes 0-2, except specular albedo uses the NVIDIA-documented
            // preintegrated form (Integration Guide appendix) instead of raw F0.
            const float3 diffuseGuideAlbedo = albedo * (1.0 - material.metallic) * (1.0 - dielectricWeight);
            const float3 diffuseGuide = lerp(diffuseGuideAlbedo, float3(0.5, 0.5, 0.5), saturate(material.metallic));
            const float dielectricSpec = FresnelDielectric(
                nDotVPrimary,
                1.0 / max(material.indexOfRefraction, 1.0));
            const float3 opaqueSpecGuide = max(
                EnvBRDFApprox2(f0, material.roughness * material.roughness, nDotVPrimary),
                0.04.xxx);
            const float3 specGuide = lerp(
                opaqueSpecGuide,
                float3(dielectricSpec, dielectricSpec, dielectricSpec),
                dielectricWeight);
            g_DiffuseAlbedoGuide[pixel] = float4(diffuseGuide, 1.0);
            g_SpecularAlbedoGuide[pixel] = float4(specGuide, 1.0);
            g_NormalRoughnessGuide[pixel] = float4(normalize(hitNormal), material.roughness);

            // Stable RR4 spec hit-distance guide (devdoc/dxr/pt/rr4-spec-hitdist.md): trace ONE
            // DETERMINISTIC mirror ray from the primary hit (no RNG) so DLSS-RR can reproject
            // reflections at their virtual depth without wobble. Reflective surfaces only; rougher /
            // diffuse / miss report g_MaxTraceDistance ("no specular reprojection"). Independent of
            // the stochastic radiance bounce chosen below.
            // Scale out as dielectric weight rises — glass refraction must not use a mirror guide.
            const float mirrorGuideWeight =
                (1.0 - dielectricWeight)
                * (1.0 - smoothstep(0.45, 0.65, material.roughness));
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

        if (bounce >= maxBounces)
        {
            // Terminal specular tail: the mirror-direction environment, energy-weighted (a
            // hall-of-mirrors fade instead of black). The DIFFUSE tail is covered by primary-hit SH
            // in real-time, or added here in reference mode. Only fires for paths that did not escape.
            const float terminalNdotV = saturate(dot(hitNormal, viewDir));
            const float3 reflectTail =
                SampleEnvironment(reflect(-viewDir, hitNormal), TransmissionMissEnvRoughness(material.roughness, dielectricWeight))
                * EnvBrdfApprox(f0, material.roughness, terminalNdotV);
            const float3 transmitTail = SampleEnvironment(
                ray.Direction,
                TransmissionMissEnvRoughness(material.roughness, dielectricWeight));
            radiance += throughput * lerp(reflectTail, transmitTail, dielectricWeight);
            if (!kPtCenterPrimaryRays)
            {
                radiance += throughput * diffuseAlbedo * EvaluateDiffuseIrradianceSh(hitNormal) / kPi
                    * g_EnvironmentIntensity;
            }
            break;
        }

        float3 nextDir;
        bool isSpecular = false;
        float scatterPdf = 1.0;
        float3 interfaceAddRadiance = 0.0.xxx;
        const bool pathInMediumBefore = pathInMedium;
        SampleMaterialBounce(
            pixel,
            bounce,
            hitPos,
            hitNormal,
            ray.Direction,
            viewDir,
            f0,
            diffuseAlbedo,
            material.roughness,
            material.metallic,
            material.transmission,
            material.indexOfRefraction,
            pathInMedium,
            payload.instanceId,
            nextDir,
            isSpecular,
            pathInMedium,
            scatterPdf,
            interfaceAddRadiance,
            throughput);
        radiance += interfaceAddRadiance;

        lastScatterPdf = scatterPdf;

        // Real-time: specular bounces add env on miss; diffuse bounces use primary-hit SH only.
        // Reference: always add the true sky. Transmitted rays always see through on miss.
        addEnvOnMiss = kPtCenterPrimaryRays ? (isSpecular || pathInMedium) : true;

        missEnvRoughness = TransmissionMissEnvRoughness(material.roughness, dielectricWeight);

        if (kPtRussianRouletteEnabled && bounce >= kRussianRouletteStartBounce)
        {
            const float rrProb = min(
                max(throughput.r, max(throughput.g, throughput.b)),
                kRussianRouletteMaxProb);
            const float rrXi = RandomXi4(pixel, g_FrameIndex, bounce + 64u).w;
            if (rrProb <= 1e-4 || rrXi > rrProb)
            {
                break;
            }
            throughput /= rrProb;
        }

        const float nDotV = saturate(dot(hitNormal, viewDir));
        const float originBias = max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV));
        ray.Origin = hitPos + nextDir * originBias;
        // Thin glass: after exiting the medium, push origin farther to skip the back face.
        if (dielectricWeight > 0.01 && pathInMediumBefore && !pathInMedium)
        {
            ray.Origin = hitPos + nextDir * max(originBias, 0.02);
        }
        ray.Direction = nextDir;
    }

    const float3 radiancePreClamp = radiance;
    if (kPtFireflyClampEnabled && g_PtDebugIsolateMode != 8u)
    {
        radiance = ClampRadiance(radiance);
    }

    const float3 displayRadiance = SelectPtDebugRadiance(
        g_PtDebugIsolateMode,
        primaryHit,
        radiance,
        radiancePreClamp,
        termDirectSun,
        termDirectEmissive,
        termSurfaceEmissive,
        termAmbient,
        primaryAoVis,
        primarySunVis,
        specHitDistGuide);

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
    const MaterialEntry material = g_Materials[instanceId];
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

    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const GeometryLookupEntry geo = g_GeometryLookup[instanceId];

    const float3 rayDir = WorldRayDirection();
    const float hitT = RayTCurrent();

    float3 hitNormal = ComputeWorldShadingNormal(geo, primitiveIndex, attribs.barycentrics);
    if (HitKind() == kHitKindTriangleBackFace)
    {
        hitNormal = -hitNormal;
    }
    if (dot(hitNormal, rayDir) > 0.0)
    {
        hitNormal = -hitNormal;
    }

    payload.hit = 1;
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;
    payload.hitDistance = hitT;
    payload.normal = hitNormal;
    payload.barycentrics = attribs.barycentrics;
    payload.triangleLod = ComputeTriangleAlbedoLodConstant(instanceId, primitiveIndex);
    ComputeVertexInterpolatedPrimarySurface(
        payload.primaryMotionNdc,
        payload.primaryDepth,
        instanceId,
        primitiveIndex,
        attribs.barycentrics);
}
