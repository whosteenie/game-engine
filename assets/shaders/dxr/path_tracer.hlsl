// DXR path tracer — Phase P1 direct lighting (devdoc/dxr-path-tracing.md).
//
// P0 proved pure camera-ray tracing via a separate PT RTPSO/SBT. P1 replaces the normal debug
// output with a direct-only HDR image at the primary hit: sun NEE + binary shadow ray, emissive,
// and prefiltered environment on miss. No multi-bounce GI yet (P2).
//
// Reuses the reflection global root signature + DxrRootSignature::ReflectionDispatchConstants so
// material table (t12), bindless albedo (space1), and prefiltered env (t10) bind the same way as
// the hybrid RT passes. MaxTraceRecursionDepth = 2 for nested shadow rays.

#include "hit_shading.hlsli"

RWTexture2D<float4> g_Output : register(u0);   // rgb = HDR radiance, a = hit distance
RWTexture2D<uint2> g_Metadata : register(u1);  // (instanceId+1, primitiveIndex)

static const uint kPrimaryRayFlags = RAY_FLAG_FORCE_OPAQUE;

struct Payload
{
    float3 radiance;
    float hitDistance;
    uint hit;
    uint instanceId;
    uint primitiveIndex;
    uint _pad;
};

float2 PixelToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

void ResetPayload(inout Payload payload)
{
    payload.radiance = 0.0.xxx;
    payload.hitDistance = 0.0;
    payload.hit = 0;
    payload.instanceId = 0;
    payload.primitiveIndex = 0;
    payload._pad = 0;
}

// Binary visibility along a ray (same contract as reflections.hlsl TraceVisibility).
float TraceVisibility(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    Payload probe;
    ResetPayload(probe);
    probe.hit = 1; // assume occluded; PathTracerMiss clears to 0 when the ray escapes

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(g_SceneTlas, occlusionFlags, 0xFF, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
}

// P1 direct lighting at the primary hit: Lambert sun (NEE + shadow) + emissive. No SH ambient or
// specular env yet — those arrive with multi-bounce GI in P2.
float3 ShadePrimaryDirect(
    uint instanceId,
    uint primitiveIndex,
    float2 barycentrics,
    float3 hitNormal,
    float3 viewDir,
    float3 shadowOrigin)
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

    const float3 f0 = lerp(0.04.xxx, albedo, material.metallic);
    const float nDotV = saturate(dot(hitNormal, viewDir));
    const float3 specularEnergy =
        FresnelSchlickRoughnessGi(nDotV, f0, max(material.roughness, 0.55));
    const float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - material.metallic);
    const float3 diffuseAlbedo = albedo * diffuseEnergy;

    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(hitNormal, sunL));
    float sunVis = 0.0;
    if (ndotl > 0.0)
    {
        sunVis = TraceVisibility(shadowOrigin, sunL, g_MaxTraceDistance);
    }

    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    const float3 direct = diffuseAlbedo * sunRadiance * ndotl / kPi * sunVis;

    return ClampRadiance(max(direct + material.emissive, 0.0.xxx));
}

[shader("raygeneration")]
void PathTracerRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const float2 texCoord = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const float2 clipXY = PixelToClipXY(texCoord);

    const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
    const float3 farWorld = farH.xyz / farH.w;
    const float3 rayDir = normalize(farWorld - g_CameraPos);

    RayDesc ray;
    ray.Origin = g_CameraPos;
    ray.Direction = rayDir;
    ray.TMin = 0.001;
    ray.TMax = g_MaxTraceDistance;

    Payload payload;
    ResetPayload(payload);
    TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, payload);

    if (payload.hit != 0)
    {
        g_Output[pixel] = float4(payload.radiance, payload.hitDistance);
        g_Metadata[pixel] = uint2(payload.instanceId + 1u, payload.primitiveIndex);
    }
    else
    {
        g_Output[pixel] = float4(payload.radiance, g_MaxTraceDistance);
        g_Metadata[pixel] = uint2(0, 0);
    }
}

[shader("miss")]
void PathTracerMiss(inout Payload payload)
{
    // Visibility probes prime hit=1 (occluded) with zero radiance; reaching miss means unoccluded.
    const bool visibilityProbe = (payload.hit != 0u && all(payload.radiance == 0.0));
    payload.hit = 0;
    if (visibilityProbe)
    {
        return;
    }

    payload.radiance = SampleEnvironment(WorldRayDirection(), 0.0);
    payload.hitDistance = g_MaxTraceDistance;
}

[shader("closesthit")]
void PathTracerClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
{
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

    const float3 hitPos = WorldRayOrigin() + rayDir * hitT;
    const float3 shadowOrigin = hitPos + hitNormal * max(hitT * 0.001, 0.002);
    const float3 viewDir = -rayDir;

    payload.hit = 1;
    payload.instanceId = instanceId;
    payload.primitiveIndex = primitiveIndex;
    payload.hitDistance = hitT;
    payload.radiance = ShadePrimaryDirect(
        instanceId, primitiveIndex, attribs.barycentrics, hitNormal, viewDir, shadowOrigin);
}
