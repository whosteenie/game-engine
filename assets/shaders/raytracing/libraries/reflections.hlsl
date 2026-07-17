// DXR Phase D4/D5 — specular reflection trace + NRD guide buffers
// (see devdoc/dxr/reflections.md and devdoc/dxr/nrd-integration.md).
// u0: RGBA16F radiance (rgb) + HIT DISTANCE (a) — RELAX_FrontEnd_PackRadianceAndHitDist
//     convention (raw world units; miss = maxTraceDistance).
// u1: R32F linear viewZ (sky = large value beyond the denoising range)
// u2: RGBA8 normal+roughness — MUST match the NRD compile options set in CMakeLists
//     (NRD_NORMAL_ENCODING=0: rgb = N*0.5+0.5; NRD_ROUGHNESS_ENCODING=1: a = linear roughness)
// u3: RG16F screen-space motion, NRD convention mv = uvPrev - uvCurr (motionVectorScale {1,1})
// Hit shading: shared material evaluation in hit_shading.hlsli; this file adds GGX sampling,
// visibility probes, and GI bounce traces for reflection hits.

#include "../common/hit_shading.hlsli"

RWTexture2D<float4> g_ReflectionOutput : register(u0);
RWTexture2D<float> g_ViewZOutput : register(u1);
RWTexture2D<float4> g_NormalRoughnessOutput : register(u2);
RWTexture2D<float2> g_MotionOutput : register(u3);

Texture2D<float4> g_DirectMap : register(t7);    // RT0 fill direct + emissive
Texture2D<float4> g_SunShadowMap : register(t8); // RT3 sun rgb + shadow factor a
Texture2D<float4> g_IndirectMap : register(t9);  // RT1 indirect/ambient
Texture2D<float4> g_GiDenoisedMap : register(t13); // RELAX_DIFFUSE output for screen-space GI lookup

static const uint kReflectionRayFlags = RAY_FLAG_FORCE_OPAQUE;
static const uint kPayloadFlagGiBounce = 2u; // payload.hit sentinel for GI bounce traces

struct Payload
{
    float3 radiance;
    float confidence;
    float hitDistance;
    float surfaceRoughness; // in: receiver roughness (for miss mip selection)
    uint hit;
    uint rngPixelPacked; // raygen dispatch pixel (x low 16, y high 16) for closest-hit AO RNG
};

// GGX VNDF half-vector sampling (Heitz 2018, "Sampling the GGX Distribution of Visible
// Normals"). Generates only half-vectors VISIBLE from the view direction — unlike plain NDF
// sampling this (a) almost never produces below-surface reflection directions (our previous
// mirror-direction fallback for those was a bias/variance source right at grazing edges) and
// (b) has substantially lower variance at grazing incidence, where the shimmer lived.
// viewWorld: direction from surface toward the camera (unit). Returns a world-space half-vector.
float3 SampleGgxVndfHalfVector(float3 normal, float3 viewWorld, float roughness, float2 xi)
{
    const float alpha = max(roughness * roughness, 1e-3);

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(normal, tangent, bitangent);

    // View direction in tangent space (z along the normal).
    const float3 viewTangent = float3(
        dot(viewWorld, tangent),
        dot(viewWorld, bitangent),
        dot(viewWorld, normal));

    // Stretch view by alpha (transforms GGX to the hemisphere configuration).
    const float3 stretchedView =
        normalize(float3(alpha * viewTangent.x, alpha * viewTangent.y, viewTangent.z));

    // Orthonormal basis around the stretched view.
    const float lenSq = stretchedView.x * stretchedView.x + stretchedView.y * stretchedView.y;
    const float3 basis1 = lenSq > 1e-7
        ? float3(-stretchedView.y, stretchedView.x, 0.0) * rsqrt(lenSq)
        : float3(1.0, 0.0, 0.0);
    const float3 basis2 = cross(stretchedView, basis1);

    // Uniform disk sample warped onto the visible hemisphere.
    const float r = sqrt(xi.x);
    const float phi = 2.0 * kPi * xi.y;
    const float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    const float s = 0.5 * (1.0 + stretchedView.z);
    t2 = (1.0 - s) * sqrt(saturate(1.0 - t1 * t1)) + s * t2;

    const float3 halfStretched = t1 * basis1 + t2 * basis2
        + sqrt(saturate(1.0 - t1 * t1 - t2 * t2)) * stretchedView;

    // Unstretch back to the GGX configuration.
    const float3 halfTangent = normalize(float3(
        alpha * halfStretched.x,
        alpha * halfStretched.y,
        max(halfStretched.z, 1e-6)));

    return normalize(
        tangent * halfTangent.x + bitangent * halfTangent.y + normal * halfTangent.z);
}

// Binary visibility along a ray. The probe SKIPS the closest-hit shader and ends at the first
// hit, so it never re-enters ReflectionClosestHit (recursion stays <= 2, matching the RTPSO's
// MaxTraceRecursionDepth) and costs only a traversal. Returns 1.0 when the ray reaches the miss
// shader (unoccluded), 0.0 when anything is hit. Requires MaxTraceRecursionDepth >= 2.
float TraceVisibility(float3 origin, float3 direction, float tMax)
{
    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = direction;
    ray.TMin = 0.001;
    ray.TMax = tMax;

    Payload probe;
    probe.radiance = 0.0.xxx;
    probe.confidence = 0.0;
    probe.hitDistance = 0.0;
    probe.surfaceRoughness = 0.0;
    probe.hit = 1; // assume occluded; ReflectionMiss clears this to 0 when the ray escapes
    probe.rngPixelPacked = 0;

    const uint occlusionFlags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH
        | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_OPAQUE;
    TraceRay(g_SceneTlas, occlusionFlags, 0xFF, 0, 0, 0, ray, probe);
    return probe.hit == 0 ? 1.0 : 0.0;
}

// Soft sun visibility at a world-space hit: cone-jittered shadow rays matching shadows.hlsl.
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

    const uint sampleCount = 4u;
    float visSum = 0.0;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        const float4 xi = RandomXi4(rngPixel, g_FrameIndex, salt + sampleIndex);
        const float diskRadius = sqrt(xi.x);
        const float diskPhi = 2.0 * kPi * xi.y;
        const float2 disk = float2(diskRadius * cos(diskPhi), diskRadius * sin(diskPhi));
        const float3 rayDir = normalize(
            sunDir + (tangent * disk.x + bitangent * disk.y) * g_SunAngularTanRadius);
        visSum += TraceVisibility(origin, rayDir, g_MaxTraceDistance);
    }
    return visSum / float(sampleCount);
}

float3 TraceDiffuseGiBounce(
    float3 origin, float3 normal, float3 viewVecTowardReceiver, uint2 rngPixel)
{
    const float2 giXi = RandomXi(rngPixel, g_FrameIndex, 24u);
    const float3 giDir = CosineSampleHemisphere(normal, giXi);

    Payload giProbe;
    giProbe.radiance = 0.0.xxx;
    giProbe.confidence = 0.0;
    giProbe.hitDistance = 0.0;
    giProbe.surfaceRoughness = 0.0;
    giProbe.hit = kPayloadFlagGiBounce;
    giProbe.rngPixelPacked = (rngPixel.x & 0xFFFFu) | (rngPixel.y << 16u);

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = giDir;
    ray.TMin = 0.001;
    ray.TMax = max(g_MaxTraceDistance, 0.1);

    TraceRay(g_SceneTlas, kReflectionRayFlags, 0xFF, 0, 0, 0, ray, giProbe);
    return giProbe.radiance;
}

[shader("raygeneration")]
void ReflectionRayGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    if (pixel.x >= g_OutputSize.x || pixel.y >= g_OutputSize.y)
    {
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(g_OutputSize);
    const int2 gbufferPixel = int2(uv * float2(g_GBufferSize));
    const float depth = g_DepthMap.Load(int3(gbufferPixel, 0)).r;

    // Sky: env in the raw view; viewZ pushed beyond the denoising range so NRD ignores it.
    const float2 clipXY = DepthUvToClipXY(uv);
    if (depth >= 0.9999)
    {
        const float4 farH = mul(g_InvViewProj, float4(clipXY, 1.0, 1.0));
        const float3 viewDir = normalize(farH.xyz / farH.w - g_CameraPos);
        g_ReflectionOutput[pixel] = float4(SampleEnvironment(viewDir, 0.0), g_MaxTraceDistance);
        g_ViewZOutput[pixel] = 1e6;
        g_NormalRoughnessOutput[pixel] = float4(0.5, 0.5, 1.0, 1.0);
        // Sky has no surface motion — zero MV avoids spurious history reprojection streaks.
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

    // MSAA-resolved silhouette rejection (RTQ-01): at partially covered pixels the resolved
    // G-buffer averages TWO surfaces — the depth reconstructs a position floating in space
    // and the normal is a blend of disagreeing directions (its length shrinks below 1).
    // Rays launched from such pixels hit random geometry (bright emissives especially) and
    // the bias is CONSISTENT, so temporal accumulation converges to speckle instead of
    // averaging it away. Hand these pixels to the IBL fallback (hitDist = miss).
    const float3 shadingNormal = rawNormal / max(rawNormalLength, 1e-4);
    if (rawNormalLength < 0.9)
    {
        const float3 mirrorFallback = reflect(normalize(worldPos - g_CameraPos), shadingNormal);
        g_ReflectionOutput[pixel] =
            float4(SampleEnvironment(mirrorFallback, roughness), g_MaxTraceDistance);
        g_ViewZOutput[pixel] = mul(g_WorldToView, float4(worldPos, 1.0)).z;
        g_NormalRoughnessOutput[pixel] = float4(shadingNormal * 0.5 + 0.5, roughness);
        return;
    }

    // NRD guides (encoding contract documented at the top of this file).
    g_ViewZOutput[pixel] = mul(g_WorldToView, float4(worldPos, 1.0)).z;
    g_NormalRoughnessOutput[pixel] = float4(shadingNormal * 0.5 + 0.5, roughness);

    const float3 viewVecCenter = normalize(worldPos - g_CameraPos); // camera -> surface (center)

    // Roughness cutoff (perf + quality): surfaces rougher than the cutoff produce a diffuse-wide,
    // noisy scatter that the denoiser just smears into a blurry blob — and diffuse GI/IBL already
    // covers that indirect. Skip the per-pixel TraceRay entirely and write the prefiltered-env
    // fallback along the mirror direction. The composite fades RT->IBL over the same cutoff, so
    // the handoff is seamless. Guides above were already written for RELAX.
    if (roughness >= g_RoughnessCutoff)
    {
        const float3 mirrorDir = reflect(viewVecCenter, shadingNormal);
        g_ReflectionOutput[pixel] =
            float4(SampleEnvironment(mirrorDir, roughness), g_MaxTraceDistance);
        return;
    }

    const uint sampleCount = clamp(g_SamplesPerPixel, 1u, 16u);
    float3 radianceSum = 0.0.xxx;
    float radianceWeightSum = 0.0;
    // Representative hit distance for the NRD guide: the CLOSEST actual hit. Averaging hits
    // with maxTrace misses fed NRD a value that wobbled frame-to-frame at edge pixels, and
    // RELAX drives specular reprojection/filter footprints from it — direct shimmer source.
    float closestHitDist = 1e30;
    bool anyHit = false;

    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        // IMPORTANT: the sequence MUST vary per frame. NRD's temporal accumulation converges
        // frame-varying noise toward the true mean; a static per-pixel sequence latches pixels
        // onto whatever their fixed directions hit (bright emissives!) every single frame, so
        // the denoiser converges TO the speckle instead of averaging it away.
        const float4 xi = RandomXi4(pixel, g_FrameIndex, sampleIndex);

        // Reflection anti-aliasing: jitter the RAY SETUP sub-pixel per sample (xi.zw).
        // Without this, mirror surfaces (roughness <= 0.03) fire N identical rays — zero
        // anti-aliasing of the reflected image no matter the sample count. Jittered setup
        // integrates both the reflected image and the receiver's own edge footprint.
        const float2 sampleUv = (float2(pixel) + xi.zw) / float2(g_OutputSize);
        const int2 sampleGbufferPixel = int2(sampleUv * float2(g_GBufferSize));
        const float sampleDepth = g_DepthMap.Load(int3(sampleGbufferPixel, 0)).r;
        const float3 sampleRawNormal = g_NormalMap.Load(int3(sampleGbufferPixel, 0)).xyz;
        const float sampleNormalLength = length(sampleRawNormal);
        const float sampleRoughness = g_Material0Map.Load(int3(sampleGbufferPixel, 0)).a;

        float3 sampleRadiance;

        if (sampleDepth >= 0.9999 || sampleNormalLength < 0.9)
        {
            // Sub-sample landed on sky or a broken (MSAA-averaged) texel: environment along
            // the center mirror direction; counts as a miss for the composite.
            sampleRadiance = ClampRadiance(
                SampleEnvironment(reflect(viewVecCenter, shadingNormal), roughness));
        }
        else
        {
            const float2 sampleClipXY = DepthUvToClipXY(sampleUv);
            const float4 sampleWorldH = mul(g_InvViewProj, float4(sampleClipXY, sampleDepth, 1.0));
            const float3 sampleWorldPos = sampleWorldH.xyz / sampleWorldH.w;
            const float3 sampleNormal = sampleRawNormal / sampleNormalLength;
            const float3 sampleViewVec = normalize(sampleWorldPos - g_CameraPos);
            const float3 sampleMirror = reflect(sampleViewVec, sampleNormal);

            float3 rayDir = sampleMirror;
            if (sampleRoughness > 0.03)
            {
                const float3 halfVector = SampleGgxVndfHalfVector(
                    sampleNormal, -sampleViewVec, sampleRoughness, xi.xy);
                const float3 sampled = reflect(sampleViewVec, halfVector);
                // VNDF makes below-surface directions vanishingly rare; keep the mirror
                // fallback purely as numerical insurance.
                if (dot(sampled, sampleNormal) > 1e-4)
                {
                    rayDir = normalize(sampled);
                }
            }

            // Grazing-aware origin bias: at glancing incidence rays creep along the surface
            // and intermittently self-hit (edge shimmer) — push the origin out further.
            const float sampleDistance = length(sampleWorldPos - g_CameraPos);
            const float nDotV = saturate(dot(sampleNormal, -sampleViewVec));
            const float3 rayOrigin = sampleWorldPos
                + sampleNormal * (max(sampleDistance * 0.0015, 0.01) * (1.0 + 2.0 * (1.0 - nDotV)));

            RayDesc ray;
            ray.Origin = rayOrigin;
            ray.Direction = rayDir;
            ray.TMin = 0.001;
            ray.TMax = max(g_MaxTraceDistance, 0.1);

            Payload payload;
            payload.radiance = 0.0.xxx;
            payload.confidence = 0.0;
            payload.hitDistance = 0.0;
            payload.surfaceRoughness = sampleRoughness;
            payload.hit = 0;
            payload.rngPixelPacked = (pixel.x & 0xFFFFu) | (pixel.y << 16u);

            TraceRay(g_SceneTlas, kReflectionRayFlags, 0xFF, 0, 0, 0, ray, payload);

            sampleRadiance = ClampRadiance(payload.radiance);
            if (payload.hit != 0)
            {
                closestHitDist = min(closestHitDist, payload.hitDistance);
                anyHit = true;
            }
        }

        // Karis firefly-weighted accumulation: a plain arithmetic mean lets a single very
        // bright sample (emissive hit) dominate the pixel forever — one 50-luminance hit in
        // 16 samples leaves a mean of ~3 on a 0.05-luminance surface, i.e. a permanent dot.
        // Weighting each sample by 1/(1+luma) forms a robust mean with negligible bias.
        const float sampleWeight = 1.0 / (1.0 + Luminance(sampleRadiance));
        radianceSum += sampleRadiance * sampleWeight;
        radianceWeightSum += sampleWeight;
    }

    // RELAX packing: radiance + raw hit distance in world units (miss = maxTraceDistance).
    g_ReflectionOutput[pixel] = float4(
        radianceSum / max(radianceWeightSum, 1e-4),
        anyHit ? closestHitDist : g_MaxTraceDistance);
}

[shader("miss")]
void ReflectionMiss(inout Payload payload)
{
    if (payload.hit == kPayloadFlagGiBounce)
    {
        payload.radiance = SampleEnvironment(WorldRayDirection(), 1.0);
        payload.hit = 0;
        return;
    }

    // Ray left the scene: prefiltered environment IS the correct radiance. Hit distance is
    // reported as the trace range ("hit at infinity") — NRD uses it for reprojection and the
    // D6 composite will treat far hits as IBL-equivalent.
    payload.radiance = SampleEnvironment(WorldRayDirection(), payload.surfaceRoughness);
    payload.confidence = 0.0;
    payload.hitDistance = g_MaxTraceDistance;
    payload.hit = 0;
}

[shader("closesthit")]
void ReflectionClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attribs)
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

    const uint payloadMode = payload.hit;
    payload.hit = 1;
    payload.hitDistance = hitT;

    const uint2 rngPixel =
        uint2(payload.rngPixelPacked & 0xFFFFu, payload.rngPixelPacked >> 16u);

    if (payloadMode == kPayloadFlagGiBounce)
    {
        const float3 viewVec = -rayDir;
        const float3 shadingNormal = ApplyWorldNormalMap(
            instanceId, primitiveIndex, attribs.barycentrics, hitNormal, viewVec, 0.0);
        payload.radiance = ShadeHit(
            instanceId, primitiveIndex, attribs.barycentrics, shadingNormal, viewVec);
        payload.hit = 1;
        return;
    }

    // In-hit analytic material shading. Screen-space reprojection of the primary G-buffer does
    // NOT work here (unlike SSR): a reflection hit projects to pixels that usually show unrelated
    // geometry, which caused misaligned/shimmery reflections. Evaluate lighting at the hit instead.
    const float3 viewVec = -rayDir;
    float3 albedo;
    float3 shadingNormal;
    float roughness;
    float metallic;
    float3 emissive;
    ResolveSurfaceMaterial(
        instanceId,
        primitiveIndex,
        attribs.barycentrics,
        hitNormal,
        viewVec,
        0.0,
        albedo,
        shadingNormal,
        roughness,
        metallic,
        emissive);

    const float3 f0 = lerp(0.04.xxx, albedo, metallic);
    const float nDotV = saturate(dot(shadingNormal, viewVec));
    const float3 specularEnergy =
        FresnelSchlickRoughnessReflection(nDotV, f0, max(roughness, 0.55));
    const float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - metallic);
    const float3 diffuseAlbedo = albedo * diffuseEnergy;

    const float3 hitPos = WorldRayOrigin() + rayDir * hitT;
    const float3 occlusionOrigin = hitPos + hitNormal * max(hitT * 0.001, 0.002);

    // Occlusion only modulates diffuse; skip on metals/near-black (perf).
    const float diffuseWeight = max(diffuseAlbedo.r, max(diffuseAlbedo.g, diffuseAlbedo.b));
    const bool needsOcclusion = diffuseWeight > 0.02;

    const float3 sunL = normalize(g_SunDirection);
    const float ndotl = saturate(dot(shadingNormal, sunL));
    const float3 sunRadiance = SrgbToLinear(g_SunColor) * g_SunIntensity;
    float softSunVis = 1.0;
    if (needsOcclusion)
    {
        softSunVis = (ndotl > 0.0)
            ? TraceSoftSunVisibility(occlusionOrigin, hitNormal, rngPixel, 4u)
            : 0.0;
    }
    const float3 direct = diffuseAlbedo * sunRadiance * ndotl / kPi * softSunVis;

    const float aoRadius = max(g_MaxTraceDistance * 0.05, 0.5);
    const uint aoRayCount = g_AoRayCount;
    const bool traceAo = needsOcclusion && aoRayCount > 0u;
    float aoVisibility = 1.0;
    if (traceAo)
    {
        float aoSum = 0.0;
        [loop]
        for (uint aoIndex = 0u; aoIndex < aoRayCount; ++aoIndex)
        {
            const float2 aoXi = RandomXi(rngPixel, g_FrameIndex, aoIndex + 8u);
            const float3 aoDir = CosineSampleHemisphere(hitNormal, aoXi);
            aoSum += TraceVisibility(occlusionOrigin, aoDir, aoRadius);
        }
        aoVisibility = aoSum / float(aoRayCount);
    }

    const float3 irradiance = EvaluateDiffuseIrradianceSh(shadingNormal);
    const float3 ambient = diffuseAlbedo * irradiance / kPi * aoVisibility;

    float3 giBounce = 0.0.xxx;
    if (g_HasGiTrace != 0u && needsOcclusion)
    {
        giBounce = TraceDiffuseGiBounce(occlusionOrigin, shadingNormal, viewVec, rngPixel);
    }

    const float3 reflectDir = reflect(rayDir, shadingNormal);
    float3 specular = SampleEnvironment(reflectDir, roughness)
        * EnvBrdfApprox(f0, roughness, nDotV);
    specular *= TraceVisibility(occlusionOrigin, reflectDir, g_MaxTraceDistance);

    payload.radiance = max(
        direct + ambient + diffuseAlbedo * giBounce * g_GiStrength + specular + emissive,
        0.0.xxx);

    const float distance01 = saturate(hitT / max(g_MaxTraceDistance, 1e-4));
    const float distanceWeight = 1.0 - distance01 * distance01;
    const float facingWeight = saturate(dot(hitNormal, -rayDir));
    payload.confidence = distanceWeight * facingWeight;
}
