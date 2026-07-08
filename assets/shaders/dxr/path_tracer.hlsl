// DXR path tracer — Phase P2 core integrator (devdoc/dxr-path-tracing.md).
//
// Megakernel: the raygen owns the bounce loop (throughput + NEE + BRDF sampling + Russian roulette).
// Closest-hit only extracts surface data; shadow and bounce traces originate from raygen so
// MaxTraceRecursionDepth = 1 suffices. P1 direct-only shading is subsumed by the loop.

#include "hit_shading.hlsli"

RWTexture2D<float4> g_Output : register(u0);   // rgb = HDR radiance, a = specular hit-distance guide (RR4)
RWTexture2D<float> g_DepthOutput : register(u1); // hyperbolic depth [0,1] at primary hit (DLSS)
RWTexture2D<uint2> g_Metadata : register(u2);  // (instanceId+1, primitiveIndex)
RWTexture2D<float4> g_MotionOutput : register(u3); // NDC motion (curr - prev) at primary hit, matches RT4

// Path-tracer-only packing in reflection cbuffer fields this pass does not otherwise use.
#define kPtFireflyClampEnabled (g_AoRayCount != 0u)
#define kPtRussianRouletteEnabled (g_HasGiTrace != 0u)
#define kPtCenterPrimaryRays (g_RoughnessCutoff > 0.5)

static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const uint kPayloadFlagVisibility = 2u;
static const uint kRussianRouletteStartBounce = 3u;
static const float kRussianRouletteMaxProb = 0.95;
static const float kMirrorRoughnessCutoff = 0.03; // match reflections.hlsl mirror path

struct Payload
{
    float3 normal;
    float hitDistance;
    uint instanceId;
    uint primitiveIndex;
    uint hit;
    float2 barycentrics;
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

void ResetPayload(inout Payload payload)
{
    payload.normal = float3(0.0, 0.0, 1.0);
    payload.hitDistance = 0.0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload.hit = 0;
    payload.barycentrics = 0.0.xx;
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

float3 CosineSampleHemisphere(float3 normal, float2 xi)
{
    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);
    const float radius = sqrt(saturate(xi.x));
    const float phi = 2.0 * kPi * xi.y;
    const float z = sqrt(max(1.0 - xi.x, 0.0));
    return normalize(tangent * (radius * cos(phi)) + bitangent * (radius * sin(phi)) + normal * z);
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

float3 SampleSurfaceAlbedo(uint instanceId, uint primitiveIndex, float2 barycentrics)
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
                .SampleLevel(g_LinearWrapSampler, hitUv, 0.0).rgb;
        albedo *= texel;
    }
    return albedo;
}

float3 EvaluateDirectSun(
    float3 diffuseAlbedo, float3 hitNormal, float3 shadowOrigin)
{
    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    if (ndotl <= 0.0)
    {
        return 0.0.xxx;
    }

    const float sunVis = TraceVisibility(shadowOrigin, sunL, g_MaxTraceDistance);
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
    bool specGuideCaptured = false;
    bool launchSpecularFromPrimary = false;
    // Roughness of the surface that launched the current ray — drives env mip on miss, matching
    // reflections.hlsl (payload.surfaceRoughness). Previously bounce>=1 always used 1.0, which
    // made mirror sky reflections black while primary camera misses (roughness 0) still worked.
    float missEnvRoughness = 0.0;

    [loop]
    for (uint bounce = 0u; bounce <= maxBounces; ++bounce)
    {
        Payload payload;
        ResetPayload(payload);
        TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);

        if (payload.hit == 0)
        {
            if (bounce == 1u && launchSpecularFromPrimary)
            {
                specHitDistGuide = g_MaxTraceDistance;
                specGuideCaptured = true;
            }

            radiance += throughput * SampleEnvironment(ray.Direction, missEnvRoughness);
            break;
        }

        if (bounce == 1u && launchSpecularFromPrimary)
        {
            specHitDistGuide = payload.hitDistance;
            specGuideCaptured = true;
        }

        const MaterialEntry material = g_Materials[payload.instanceId];
        const float3 albedo = SampleSurfaceAlbedo(
            payload.instanceId, payload.primitiveIndex, payload.barycentrics);
        const float3 hitNormal = payload.normal;
        const float3 viewDir = -ray.Direction;
        const float3 hitPos = ray.Origin + ray.Direction * payload.hitDistance;
        const float3 shadowOrigin = hitPos + hitNormal * max(payload.hitDistance * 0.001, 0.002);

        const float3 f0 = lerp(0.04.xxx, albedo, material.metallic);
        const float3 specularEnergy =
            FresnelSchlickRoughnessGi(saturate(dot(hitNormal, viewDir)), f0, max(material.roughness, 0.55));
        const float3 diffuseAlbedo = albedo * (1.0.xxx - specularEnergy) * (1.0 - material.metallic);

        radiance += throughput * material.emissive;
        radiance += throughput * EvaluateDirectSun(diffuseAlbedo, hitNormal, shadowOrigin);

        if (bounce == 0u)
        {
            primaryHit = true;
            primaryInstanceId = payload.instanceId;
            primaryPrimitiveIndex = payload.primitiveIndex;
            const float4 currClip = mul(g_UnjitteredViewProj, float4(hitPos, 1.0));
            const float4 prevClip = mul(g_PrevViewProj, float4(hitPos, 1.0));
            primaryMotion = ComputeMotionNdc(currClip, prevClip);
            primaryDepth = saturate(currClip.z / max(currClip.w, 1e-6));
        }

        if (bounce >= maxBounces)
        {
            // Terminal-bounce rule: fade to environment instead of zero (devdoc P2).
            const float3 terminalDir = normalize(reflect(-viewDir, hitNormal));
            radiance += throughput * SampleEnvironment(terminalDir, material.roughness);
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

        if (bounce == 0u)
        {
            launchSpecularFromPrimary = isSpecular;
        }

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
}
