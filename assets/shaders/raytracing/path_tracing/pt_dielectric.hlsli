// PT-A v2 — production dielectric / glass shading for the path tracer.
// Included from path_tracer.hlsl after SampleGgxVndfHalfVector is defined.

#ifndef PT_DIELECTRIC_HLSLI
#define PT_DIELECTRIC_HLSLI

static const uint kMaxTransmissiveShadowSegments = 6u;
// Beer-Lambert scale: albedo tints transmitted light; higher = stronger absorption per unit.
static const float kBeerLambertScale = 1.0;
// After thin-slab refraction, push the path origin past physical shell thickness (scaled-cube panes).
static const float kThinShellMinExitBias = 0.06;

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
    // Transmitted PROPAGATION direction (PBRT eq. 9.34 / GLSL refract with incident d = -wi). The
    // previous form (eta*wi - (eta*cosI + cosT)*n) flipped the tangential component, mirroring every
    // refraction about the normal — invisible for parallel-face slabs (entry/exit cancel) but wrong
    // for lenses, internal paths, glass shadows, and transmission guides.
    wt = -eta * wi + (eta * cosI - cosT) * n;
    return dot(wt, wt) > 1e-8;
}

// Map material roughness to environment cube mip without crushing HDRIs to black at 1.0.
float EnvironmentMipRoughness(float roughness)
{
    const float r = saturate(roughness);
    return r * r * 0.35;
}

// Transmitted rays blur via microfacet normal spread; keep env LOD low so sky stays visible.
float TransmissionMissEnvRoughness(float roughness, float dielectricWeight)
{
    const float mip = EnvironmentMipRoughness(roughness);
    return lerp(mip, min(mip, 0.05), saturate(dielectricWeight));
}

// Beer-Lambert absorption: albedo acts as transmission tint (green bottle, amber glass).
float3 BeerLambertMediumAttenuation(float3 tint, float distance)
{
    const float3 sigma = -log(max(tint, 0.001)) * kBeerLambertScale;
    return exp(-sigma * max(distance, 0.0));
}

// GGX microfacet normal for rough dielectric interfaces (reuses path_tracer VNDF sampler).
float3 SampleDielectricMicroNormal(float3 n, float3 wi, float roughness, float2 xi)
{
    if (roughness <= 1e-4)
    {
        return n;
    }

    float3 microN = SampleGgxVndfHalfVector(n, wi, roughness, xi);
    if (dot(microN, wi) < 0.0)
    {
        microN = -microN;
    }
    return microN;
}

// Thin dielectric slab (zero thickness, parallel faces): the enter and exit refractions are equal
// and opposite, so the transmitted ray continues along the original propagation direction — PBRT's
// ThinDielectricBxDF transmits along the unmodified incident direction. wi points back along the
// incoming path, so the forward transmit direction is -wi. A parallel slab always transmits the
// non-reflected portion (no TIR), so this never fails. (n / ior unused: kept for call-site parity.)
bool RefractThinSlab(float3 wi, float3 n, float ior, out float3 wo)
{
    wo = -wi;
    return true;
}

// Build refracted direction for DLSS depth guides and shadow continuation.
bool ComputeDielectricRefractDir(
    float3 hitNormal,
    float3 rayDir,
    float ior,
    bool thinWalled,
    bool pathInMedium,
    out float3 refractDir)
{
    const float3 wi = -rayDir;
    float3 n = hitNormal;
    if (dot(wi, n) < 0.0)
    {
        n = -n;
    }

    if (thinWalled)
    {
        return RefractThinSlab(wi, n, ior, refractDir);
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

// Dielectric interface scatter: stochastic reflect OR refract with Fresnel-proportional selection
// (G2 / ReSTIR R0). Real-time and reference share this path — one sample, one pdf. Throughput is
// unchanged on either branch because weight = f/pdf = 1 for Fresnel-proportional picks (C1).
void SampleDielectricInterface(
    float3 hitNormal,
    float3 rayDir,
    float roughness,
    float ior,
    bool thinWalled,
    bool pathInMedium,
    float fresnelXi,
    float2 roughXi,
    out float3 nextDir,
    out bool outPathInMedium,
    out float scatterPdf)
{
    const float3 wi = -rayDir;
    float3 n = hitNormal;
    if (dot(wi, n) < 0.0)
    {
        n = -n;
    }

    const float iorClamped = max(ior, 1.0);
    const float3 geoN = n;
    // Thin slab: geometric normal for Fresnel/Snell slab model; microfacet tilt on reflection only
    // (tilting the slab breaks enter+exit cancellation on transmission). Solid volumes tilt both lobes.
    const float3 reflectN = thinWalled
        ? SampleDielectricMicroNormal(geoN, wi, roughness, roughXi)
        : SampleDielectricMicroNormal(n, wi, roughness, roughXi);
    const float3 snellN = thinWalled ? geoN : reflectN;
    const float eta = thinWalled ? (1.0 / iorClamped) : (pathInMedium ? iorClamped : (1.0 / iorClamped));
    const bool enteringMedium = !pathInMedium && !thinWalled;
    const float cosThetaI = dot(wi, snellN);
    const float singleFaceFresnel = FresnelDielectric(cosThetaI, eta);
    const float fresnel = thinWalled
        ? (2.0 * singleFaceFresnel / (1.0 + singleFaceFresnel))
        : singleFaceFresnel;
    // Mirror reflection of the INCOMING ray about the interface normal. reflect() takes the incident
    // direction (rayDir, into the surface); reflect(wi, ...) with wi = -rayDir returns the NEGATED
    // reflection (pointing into the surface) — a long-standing sign bug that sent the glass reflection
    // ray the wrong way (and internal TIR out through the wall instead of bouncing back in).
    const float3 reflectDir = normalize(reflect(rayDir, reflectN));

    outPathInMedium = thinWalled ? false : pathInMedium;
    scatterPdf = kDeltaScatterPdf;

    float3 refracted;
    bool refractOk = false;
    if (thinWalled)
    {
        refractOk = RefractThinSlab(wi, geoN, iorClamped, refracted);
    }
    else
    {
        refractOk = RefractSnell(wi, snellN, eta, refracted);
    }

    const bool mustReflect = !refractOk;
    const bool chooseReflect = mustReflect || (fresnelXi < fresnel);

    if (chooseReflect)
    {
        // Fresnel-proportional selection: the estimator weight is reflectance / P(reflect) = F/F = 1
        // (and 1/1 = 1 for TIR). Throughput passes through unchanged.
        nextDir = reflectDir;
        outPathInMedium = thinWalled ? false : pathInMedium;
        return;
    }

    // Transmit weight = (1 - F) / P(transmit) = (1 - F)/(1 - F) = 1. No throughput scaling.
    nextDir = normalize(refracted);
    if (thinWalled)
    {
        outPathInMedium = false;
    }
    else
    {
        outPathInMedium = enteringMedium;
    }
}

// Result of a deterministic refracted primary-ray trace for DLSS-RR transmission guides (PSR).
struct TransmissionGuideHit
{
    bool valid;
    float depth;
    float2 motion;
    uint instanceId;
    uint primitiveIndex;
    float2 barycentrics;
    float3 normal;
    float3 shadingNormal;
    float triangleLod;
    float3 refractDir;          // direction of the FINAL segment that reached the background
    float refractedHitDistance; // length of that final segment
    float3 backgroundWorldPos;  // world position of the background hit (accounts for the bent path)
};

// DLSS-RR transmission guide: trace the refracted primary ray and return depth, motion, and the
// background hit payload needed for PSR-style material guides (Omniverse virtual motion; NVIDIA PSR).
TransmissionGuideHit TraceTransmissionGuide(
    float3 hitPos,
    float3 hitNormal,
    float3 rayDir,
    float ior,
    bool thinWalled,
    float originBias,
    uint excludeInstanceId)
{
    TransmissionGuideHit result;
    result.valid = false;
    result.depth = 1.0;
    result.motion = 0.0.xx;
    result.instanceId = 0u;
    result.primitiveIndex = 0u;
    result.barycentrics = 0.0.xx;
    result.normal = float3(0.0, 0.0, 1.0);
    result.shadingNormal = float3(0.0, 0.0, 1.0);
    result.triangleLod = 0.0;
    result.refractDir = rayDir;
    result.refractedHitDistance = 0.0;
    result.backgroundWorldPos = hitPos;

    float3 refractDir;
    if (!ComputeDielectricRefractDir(hitNormal, rayDir, ior, thinWalled, false, refractDir))
    {
        return result;
    }

    result.refractDir = refractDir;
    float3 guideOrigin = hitPos + refractDir * originBias;
    // Solid volumes: the radiance path refracts at BOTH the entry and exit interfaces, so the guide
    // must too. Previously it continued straight through the glass after the front-face refraction,
    // landing on a DIFFERENT background point than the color — DLSS-RR then ghosts/doubles the
    // through-glass image (worst on solid cubes/spheres; thin panes single-refract and are fine).
    [loop]
    for (uint attempt = 0u; attempt < 4u; ++attempt)
    {
        RayDesc guideRay;
        guideRay.Origin = guideOrigin;
        guideRay.Direction = refractDir;
        guideRay.TMin = 0.001;
        guideRay.TMax = g_MaxTraceDistance;

        Payload guidePayload;
        ResetPayload(guidePayload);
        // Background hit feeds RR guides — need shading + primary motion/depth.
        guidePayload.hit = kPayloadReqShadingData | kPayloadReqPrimarySurface;
        TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, guideRay, guidePayload);
        if (guidePayload.hit == 0)
        {
            result.refractDir = refractDir;
            return result;
        }

        const float3 interfaceHitPos = guideOrigin + refractDir * guidePayload.hitDistance;

        if (guidePayload.instanceId != excludeInstanceId)
        {
            result.valid = true;
            result.depth = guidePayload.primaryDepth;
            result.motion = PayloadPrimaryMotion(guidePayload);
            result.instanceId = guidePayload.instanceId;
            result.primitiveIndex = guidePayload.primitiveIndex;
            result.barycentrics = PayloadBarycentrics(guidePayload);
            result.normal = PayloadGeomNormal(guidePayload);
            result.shadingNormal = PayloadShadingNormal(guidePayload);
            result.triangleLod = PayloadTriangleLod(guidePayload);
            result.refractDir = refractDir;
            result.refractedHitDistance = guidePayload.hitDistance;
            result.backgroundWorldPos = interfaceHitPos;
            return result;
        }

        // Hit the same glass again — an internal interface (exit face of a solid volume, or the far
        // side of a thin shell). Refract out (glass→air, eta = ior) so the continuation matches the
        // radiance path's exit; on total internal reflection, bounce internally. Thin walls keep the
        // straight-through direction (their radiance model transmits straight).
        if (!thinWalled)
        {
            const float3 guideGeomNormal = PayloadGeomNormal(guidePayload);
            float3 exitDir;
            if (RefractSnell(-refractDir, guideGeomNormal, max(ior, 1.0), exitDir))
            {
                refractDir = normalize(exitDir);
            }
            else
            {
                refractDir = normalize(reflect(refractDir, guideGeomNormal));
            }
        }
        guideOrigin = interfaceHitPos + refractDir * kThinShellMinExitBias;
    }

    result.refractDir = refractDir;
    return result;
}

// Sun / shadow rays that refract through glass instead of treating it as opaque.
float TraceTransmissiveVisibility(float3 origin, float3 direction, float tMax)
{
    float transmittance = 1.0;
    float3 rayOrigin = origin;
    float3 rayDir = direction;
    float distRemaining = tMax;
    bool pathInMedium = false;
    const float stepBias = 0.002;

    [loop]
    for (uint seg = 0u; seg < kMaxTransmissiveShadowSegments; ++seg)
    {
        RayDesc ray;
        ray.Origin = rayOrigin;
        ray.Direction = rayDir;
        ray.TMin = stepBias;
        ray.TMax = distRemaining;

        Payload probe;
        ResetPayload(probe);
        TraceRay(g_SceneTlas, kPrimaryRayFlags, 0xFF, 0, 0, 0, ray, probe);
        if (probe.hit == 0)
        {
            return transmittance;
        }

        const MaterialEntry mat = LoadMaterialForInstance(probe.instanceId);
        const float glassWeight = DielectricWeight(mat.transmission, mat.metallic);
        // Fully opaque hit — block the shadow ray. No 0.01 cutoff: partial transmission must fade
        // smoothly (gw * (1−F)), otherwise t=0 vs t≈0.01 snaps from full shadow to nearly clear.
        if (glassWeight <= 0.0)
        {
            return 0.0;
        }

        if (pathInMedium)
        {
            const float3 mediumAttenuation =
                BeerLambertMediumAttenuation(mat.albedo, probe.hitDistance);
            transmittance *= (mediumAttenuation.r + mediumAttenuation.g + mediumAttenuation.b) / 3.0;
        }

        const float3 hitPos = rayOrigin + rayDir * probe.hitDistance;
        const float3 wi = -rayDir;
        float3 n = PayloadGeomNormal(probe);
        if (dot(wi, n) < 0.0)
        {
            n = -n;
        }

        const float ior = max(mat.indexOfRefraction, 1.0);
        const bool thin = mat.thinWalled > 0.5;
        const float eta = thin ? (1.0 / ior) : (pathInMedium ? ior : (1.0 / ior));
        const float singleFaceFresnel = FresnelDielectric(dot(wi, n), eta);
        // Match SampleDielectricInterface: two-face slab reflectance for panes (R_slab = 2R/(1+R)).
        const float fresnel = thin
            ? (2.0 * singleFaceFresnel / (1.0 + singleFaceFresnel))
            : singleFaceFresnel;
        // Scale by glassWeight so t=0.3 blocks ~70% and transmits ~30%·(1−F), matching radiance path.
        transmittance *= glassWeight * max(1.0 - fresnel, 0.0);
        if (transmittance < 1e-4)
        {
            return 0.0;
        }

        float3 transDir;
        bool refractOk;
        if (thin)
        {
            refractOk = RefractThinSlab(wi, n, ior, transDir);
            pathInMedium = false;
        }
        else
        {
            float3 wt;
            refractOk = RefractSnell(wi, n, eta, wt);
            transDir = normalize(wt);
            pathInMedium = !pathInMedium;
        }

        if (!refractOk)
        {
            return 0.0;
        }

        const float nDotV = saturate(dot(n, wi));
        const float bias = max(0.02, 0.01 * (1.0 + 2.0 * (1.0 - nDotV)));
        rayOrigin = hitPos + transDir * bias;
        rayDir = transDir;
        distRemaining -= probe.hitDistance;
        if (distRemaining <= stepBias)
        {
            return 0.0;
        }
    }

    return 0.0;
}

#endif // PT_DIELECTRIC_HLSLI
