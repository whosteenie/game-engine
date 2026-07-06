// DXR Phase D9 — one-bounce diffuse GI trace + NRD RELAX_DIFFUSE guides
// (see devdoc/dxr-diffuse-gi.md). Cosine-weighted hemisphere rays from every G-buffer pixel,
// shaded at the hit with the SAME analytic material shading the reflection trace uses
// (hit_shading.hlsli), accumulated Karis-weighted, denoised by RELAX_DIFFUSE, then added into
// RT1 by dxr_gi_inject.ps.hlsl.
// u0: RGBA16F incoming diffuse radiance (rgb, cosine pdf already folds the cosine in) + HIT
//     DISTANCE (a) — RELAX packing (raw world units; miss = maxTraceDistance).
// u1: R32F linear viewZ. u2: RGBA16_UNORM normal+roughness (NRD_NORMAL_ENCODING=3). u3: RG16F MV.

#include "hit_shading.hlsli"

RWTexture2D<float4> g_GiOutput : register(u0);
RWTexture2D<float> g_ViewZOutput : register(u1);
RWTexture2D<float4> g_NormalRoughnessOutput : register(u2);
RWTexture2D<float2> g_MotionOutput : register(u3);

static const uint kGiRayFlags = RAY_FLAG_FORCE_OPAQUE;

struct GiPayload
{
    float3 radiance;
    float hitDistance;
    uint hit;
    uint _pad;
};

// Cosine-weighted hemisphere sample (pdf = cos/pi, so the radiance sum needs NO cos factor —
// the pi from the pdf cancels the 1/pi in the Lambertian BRDF, leaving albedo * radiance).
float3 SampleCosineHemisphere(float3 normal, float2 xi)
{
    float3 t;
    float3 b;
    BuildTangentFrame(normal, t, b);
    const float r = sqrt(xi.x);
    const float phi = 2.0 * kPi * xi.y;
    const float3 local = float3(r * cos(phi), r * sin(phi), sqrt(saturate(1.0 - xi.x)));
    return normalize(t * local.x + b * local.y + normal * local.z);
}

[shader("raygeneration")]
void GiRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const int2 gbufferPixel = int2(uv * float2(g_GBufferSize));
    const float depth = g_DepthMap.Load(int3(gbufferPixel, 0)).r;
    const float2 clipXY = DepthUvToClipXY(uv);

    // Sky: no surface bounces here. RELAX ignores it (viewZ beyond the denoising range), and the
    // inject pass passes RT1 through at sky depth, so radiance is irrelevant — keep it zero.
    if (depth >= 0.9999)
    {
        g_GiOutput[pixel] = float4(0.0.xxx, g_MaxTraceDistance);
        g_ViewZOutput[pixel] = 1e6;
        g_NormalRoughnessOutput[pixel] = float4(0.5, 0.5, 1.0, 1.0);
        g_MotionOutput[pixel] = 0.0.xx;
        return;
    }

    // NRD motion guide: engine velocity is NDC (curr - prev); NRD wants uvPrev - uvCurr.
    const float2 velocityNdc = g_VelocityMap.Load(int3(gbufferPixel, 0)).rg;
    g_MotionOutput[pixel] = float2(-0.5 * velocityNdc.x, 0.5 * velocityNdc.y);

    const float4 worldH = mul(g_InvViewProj, float4(clipXY, depth, 1.0));
    const float3 worldPos = worldH.xyz / worldH.w;
    const float3 rawNormal = g_NormalMap.Load(int3(gbufferPixel, 0)).xyz;
    const float rawNormalLength = length(rawNormal);
    const float roughness = g_Material0Map.Load(int3(gbufferPixel, 0)).a;
    const float3 shadingNormal = rawNormal / max(rawNormalLength, 1e-4);

    // NRD guides (encoding contract documented at the top).
    g_ViewZOutput[pixel] = mul(g_WorldToView, float4(worldPos, 1.0)).z;
    g_NormalRoughnessOutput[pixel] = float4(shadingNormal * 0.5 + 0.5, roughness);

    // MSAA-resolved silhouette texel (blended normal): the reconstructed position floats, so a
    // bounce trace from it is unreliable — contribute zero GI (additive, harmless) and let RELAX
    // fill the thin edge from neighbors.
    if (rawNormalLength < 0.9)
    {
        g_GiOutput[pixel] = float4(0.0.xxx, g_MaxTraceDistance);
        return;
    }

    const uint sampleCount = clamp(g_SamplesPerPixel, 1u, 16u);
    float3 radianceSum = 0.0.xxx;
    float radianceWeightSum = 0.0;
    float closestHitDist = 1e30;
    bool anyHit = false;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        // Frame-varying sequence (RTQ-01). xy = hemisphere sample, zw = sub-pixel ray-setup jitter.
        const float4 xi = RandomXi4(pixel, g_FrameIndex, sampleIndex);

        const float2 sampleUv = (float2(pixel) + xi.zw) / float2(g_OutputSize);
        const int2 sampleGbufferPixel = int2(sampleUv * float2(g_GBufferSize));
        const float sampleDepth = g_DepthMap.Load(int3(sampleGbufferPixel, 0)).r;
        const float3 sampleRawNormal = g_NormalMap.Load(int3(sampleGbufferPixel, 0)).xyz;
        const float sampleNormalLength = length(sampleRawNormal);

        float3 sampleRadiance;

        if (sampleDepth >= 0.9999 || sampleNormalLength < 0.9)
        {
            // Sub-sample landed on sky or a broken texel: the visible hemisphere sees the sky.
            sampleRadiance = ClampRadiance(SampleEnvironment(shadingNormal, 1.0));
        }
        else
        {
            const float2 sampleClipXY = DepthUvToClipXY(sampleUv);
            const float4 sampleWorldH = mul(g_InvViewProj, float4(sampleClipXY, sampleDepth, 1.0));
            const float3 sampleWorldPos = sampleWorldH.xyz / sampleWorldH.w;
            const float3 sampleNormal = sampleRawNormal / sampleNormalLength;

            const float3 rayDir = SampleCosineHemisphere(sampleNormal, xi.xy);

            // Grazing-aware origin bias (copied from reflections.hlsl): push the origin out at
            // glancing incidence so rays don't creep along the surface and self-hit.
            const float sampleDistance = length(sampleWorldPos - g_CameraPos);
            const float3 sampleViewVec = normalize(sampleWorldPos - g_CameraPos);
            const float nDotV = saturate(dot(sampleNormal, -sampleViewVec));
            const float3 rayOrigin = sampleWorldPos
                + sampleNormal * (max(sampleDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV)));

            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin = 0.001;
            ray.TMax = max(g_MaxTraceDistance, 0.1);

            GiPayload payload;
            payload.radiance = 0.0.xxx;
            payload.hitDistance = 0.0;
            payload.hit = 0;
            payload._pad = 0;

            TraceRay(g_SceneTlas, kGiRayFlags, 0xFF, 0, 0, 0, ray, payload);

            sampleRadiance = ClampRadiance(payload.radiance);
            if (payload.hit != 0)
            {
                closestHitDist = min(closestHitDist, payload.hitDistance);
                anyHit = true;
            }
        }

        // Karis firefly-weighted accumulation: a robust mean that a single very bright bounce
        // sample cannot dominate (see reflections.hlsl for the full rationale).
        const float sampleWeight = 1.0 / (1.0 + Luminance(sampleRadiance));
        radianceSum += sampleRadiance * sampleWeight;
        radianceWeightSum += sampleWeight;
    }

    // RELAX packing: radiance + raw closest hit distance in world units (miss = maxTraceDistance).
    g_GiOutput[pixel] = float4(
        radianceSum / max(radianceWeightSum, 1e-4),
        anyHit ? closestHitDist : g_MaxTraceDistance);
}

[shader("miss")]
void GiMiss(inout GiPayload payload)
{
    // Ray left the scene: the prefiltered environment (max roughness = diffuse-ish) is the
    // incoming sky radiance along the ray.
    payload.radiance = SampleEnvironment(WorldRayDirection(), 1.0);
    payload.hitDistance = g_MaxTraceDistance;
    payload.hit = 0;
}

[shader("closesthit")]
void GiClosestHit(inout GiPayload payload, BuiltInTriangleIntersectionAttributes attribs)
{
    const uint instanceId = InstanceID();
    const uint primitiveIndex = PrimitiveIndex();
    const float3 rayDir = WorldRayDirection();
    const float hitT = RayTCurrent();

    float3 hitNormal =
        ComputeWorldShadingNormal(g_GeometryLookup[instanceId], primitiveIndex, attribs.barycentrics);
    if (HitKind() == kHitKindTriangleBackFace)
    {
        hitNormal = -hitNormal;
    }
    if (dot(hitNormal, rayDir) > 0.0)
    {
        hitNormal = -hitNormal;
    }

    payload.hit = 1;
    payload.hitDistance = hitT;
    payload.radiance = ShadeHitDiffuse(instanceId, primitiveIndex, attribs.barycentrics, hitNormal);
}
