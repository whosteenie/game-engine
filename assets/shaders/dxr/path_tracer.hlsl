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

static const uint kPtAmbientAoRngSalt = 128u;
static const uint kPtSoftSunRngSalt = 32u;
static const uint kPtSoftSunSampleCount = 4u;

static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const uint kPayloadFlagVisibility = 2u;
static const uint kRussianRouletteStartBounce = 3u;
static const float kRussianRouletteMaxProb = 0.95;
static const float kMirrorRoughnessCutoff = 0.03; // match reflections.hlsl mirror path
// RR4 stable spec hit-distance guide (devdoc/dxr/pt/rr4-spec-hitdist.md): only reflective-enough
// primary surfaces emit a finite hit distance; rougher/diffuse surfaces report "no reflection".
static const float kReflectionGuideRoughnessCutoff = 0.6; // match hybrid g_RoughnessCutoff default

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

    const float sunVis = (g_SunAngularTanRadius > 1e-6)
        ? TraceSoftSunVisibility(shadowOrigin, hitNormal, pixel, bounceIndex + kPtSoftSunRngSalt)
        : TraceVisibility(shadowOrigin, sunL, g_MaxTraceDistance);
    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    return diffuseAlbedo * sunRadiance * ndotl / kPi * sunVis;
}

bool SampleNextBounceDirection(
    uint2 pixel,
    uint bounceIndex,
    float3 hitNormal,
    float3 viewDir,
    float3 f0,
    float3 diffuseAlbedo,
    float roughness,
    float metallic,
    out float3 nextDir,
    out bool isSpecular,
    inout float3 throughput)
{
    const float4 xi = RandomXi4(pixel, g_FrameIndex, bounceIndex + 1u);
    const float3 specularEnergy =
        FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(roughness, 0.55));

    float specProb = saturate(max(specularEnergy.r, max(specularEnergy.g, specularEnergy.b)));
    specProb = lerp(specProb, 1.0, metallic);

    // Mirror surfaces: deterministic perfect reflection (same threshold as reflections.hlsl).
    // Stochastic lobe selection + VNDF at roughness 0 is a poor delta-BSDF approximation and
    // breaks sky/mirror-in-mirror paths that need a stable reflect(view, normal) direction.
    if (roughness <= kMirrorRoughnessCutoff)
    {
        isSpecular = true;
        nextDir = normalize(reflect(-viewDir, hitNormal));
        if (dot(nextDir, hitNormal) <= 1e-4)
        {
            nextDir = normalize(reflect(-viewDir, hitNormal));
        }
        throughput *= f0 / max(specProb, 1e-3);
        return true;
    }

    const float lobeXi = xi.z;
    const bool traceSpecular = (lobeXi < specProb) && (roughness < 0.95);

    if (traceSpecular)
    {
        const float3 halfVector = SampleGgxVndfHalfVector(hitNormal, viewDir, roughness, xi.xy);
        nextDir = normalize(reflect(-viewDir, halfVector));
        if (dot(nextDir, hitNormal) <= 1e-4)
        {
            nextDir = normalize(reflect(-viewDir, hitNormal));
        }
        throughput *= f0 / max(specProb, 1e-3);
    }
    else
    {
        nextDir = CosineSampleHemisphere(hitNormal, xi.xy);
        throughput *= diffuseAlbedo / max(1.0 - specProb, 1e-3);
    }

    isSpecular = traceSpecular;
    return traceSpecular;
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
                radiance += throughput * SampleEnvironment(ray.Direction, missEnvRoughness);
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
        const float3 specularEnergy =
            FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(material.roughness, 0.55));
        const float3 diffuseAlbedo = albedo * (1.0.xxx - specularEnergy) * (1.0 - material.metallic);

        radiance += throughput * material.emissive;
        radiance += throughput * EvaluateDirectSun(
            pixel, bounce, diffuseAlbedo, hitNormal, shadowOrigin);

        // Real-time v2: primary-hit AO-gated SH ambient (devdoc/dxr/pt/crevice-darkening.md). Fills
        // crevices without the v1 washout from unoccluded per-bounce SH. Reference omits this.
        if (kPtCenterPrimaryRays && bounce == 0u)
        {
            radiance += throughput
                * EvaluateRealTimeDiffuseAmbient(pixel, diffuseAlbedo, hitNormal, shadowOrigin);
        }

        if (bounce == 0u)
        {
            primaryHit = true;
            primaryInstanceId = payload.instanceId;
            primaryPrimitiveIndex = payload.primitiveIndex;
            primaryMotion = payload.primaryMotionNdc;
            primaryDepth = payload.primaryDepth;

            // P4b RR material guides from the SAME hit that seeds the integrator. Encodings match
            // rr_guides.ps.hlsl modes 0-2, except specular albedo uses the NVIDIA-documented
            // preintegrated form (Integration Guide appendix) instead of raw F0.
            const float nDotVPrimary = saturate(dot(hitNormal, viewDir));
            g_DiffuseAlbedoGuide[pixel] = float4(albedo * (1.0 - material.metallic), 1.0);
            g_SpecularAlbedoGuide[pixel] = float4(
                EnvBRDFApprox2(f0, material.roughness * material.roughness, nDotVPrimary), 1.0);
            g_NormalRoughnessGuide[pixel] = float4(normalize(hitNormal), material.roughness);

            // Stable RR4 spec hit-distance guide (devdoc/dxr/pt/rr4-spec-hitdist.md): trace ONE
            // DETERMINISTIC mirror ray from the primary hit (no RNG) so DLSS-RR can reproject
            // reflections at their virtual depth without wobble. Reflective surfaces only; rougher /
            // diffuse / miss report g_MaxTraceDistance ("no specular reprojection"). Independent of
            // the stochastic radiance bounce chosen below.
            if (material.roughness < kReflectionGuideRoughnessCutoff)
            {
                RayDesc guideRay;
                guideRay.Origin = shadowOrigin;
                guideRay.Direction = normalize(reflect(ray.Direction, hitNormal));
                guideRay.TMin = 0.001;
                guideRay.TMax = g_MaxTraceDistance;

                Payload guidePayload;
                ResetPayload(guidePayload);
                TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, guideRay, guidePayload);
                specHitDistGuide =
                    (guidePayload.hit != 0) ? guidePayload.hitDistance : g_MaxTraceDistance;
            }
        }

        if (bounce >= maxBounces)
        {
            // Terminal specular tail: the mirror-direction environment, energy-weighted (a
            // hall-of-mirrors fade instead of black). The DIFFUSE tail is covered by primary-hit SH
            // in real-time, or added here in reference mode. Only fires for paths that did not escape.
            const float terminalNdotV = saturate(dot(hitNormal, viewDir));
            radiance += throughput
                * SampleEnvironment(reflect(-viewDir, hitNormal), material.roughness)
                * EnvBrdfApprox(f0, material.roughness, terminalNdotV);
            if (!kPtCenterPrimaryRays)
            {
                radiance += throughput * diffuseAlbedo * EvaluateDiffuseIrradianceSh(hitNormal) / kPi
                    * g_EnvironmentIntensity;
            }
            break;
        }

        float3 nextDir;
        bool isSpecular = false;
        SampleNextBounceDirection(
            pixel,
            bounce,
            hitNormal,
            viewDir,
            f0,
            diffuseAlbedo,
            material.roughness,
            material.metallic,
            nextDir,
            isSpecular,
            throughput);

        // Real-time: specular bounces add env on miss; diffuse bounces use primary-hit SH only.
        // Reference: always add the true sky.
        addEnvOnMiss = kPtCenterPrimaryRays ? isSpecular : true;

        missEnvRoughness = material.roughness;

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
        ray.Origin = hitPos + hitNormal * max(payload.hitDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV));
        ray.Direction = nextDir;
    }

    if (kPtFireflyClampEnabled)
    {
        radiance = ClampRadiance(radiance);
    }
    g_Output[pixel] = float4(radiance, specHitDistGuide);
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
