// SSR trace — quadratic view-space march, stochastic jitter, multi-sample averaging.
// Miss returns 0; IBL fallback in composite (Phase S4).

#include "screen_space_common.hlsl"

Texture2D uDepthMap : register(t0);
Texture2D uNormalMap : register(t1);
Texture2D uMaterial0Map : register(t2);
Texture2D uSceneColorMap : register(t3);

SamplerState uDepthSampler : register(s0);
SamplerState uNormalSampler : register(s1);
SamplerState uMaterial0Sampler : register(s2);
SamplerState uSceneColorLinearSampler : register(s3);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float4x4 uProjection;
    float4x4 uView;
    float uMaxTraceDistance;
    int uStepCount;
    float uThickness;
    float uRoughnessCutoff;
    float uFrameIndex;
    float uStepExponent;
    int uSampleCount;
    float2 uTexelSize;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

static const int kMaxSamples = 4;
static const int kRefineSteps = 6;
static const float kMaxSsrRadiance = 32.0;
static const float kMaxReflectionSpread = 0.14;

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 34.345);
    return frac(p.x * p.y);
}

float2 Hash22(float2 p)
{
    return float2(Hash21(p), Hash21(p.yx + 19.19));
}

float3 ViewPosFromDepth(float2 texCoord, float depth)
{
    const float2 clipXY = DepthUvToClipXY(texCoord);
    float4 viewH = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewH.xyz / viewH.w;
}

float ViewDepthAt(float2 texCoord)
{
    return ViewPosFromDepth(texCoord, uDepthMap.Sample(uDepthSampler, texCoord).r).z;
}

float2 ViewPosToDepthUv(float3 viewPos)
{
    float4 clipH = mul(uProjection, float4(viewPos, 1.0));
    clipH.xyz /= clipH.w;
    return ClipXYToDepthUv(clipH.xy);
}

float3 ClampRadiance(float3 radiance)
{
    radiance = max(radiance, 0.0.xxx);
    const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= kMaxSsrRadiance)
    {
        return radiance;
    }

    return radiance * (kMaxSsrRadiance / max(luminance, 1e-4));
}

float SpecularTraceWeight(float roughness)
{
    return saturate((uRoughnessCutoff - roughness) / max(uRoughnessCutoff, 1e-4));
}

void BuildTangentFrame(float3 normal, out float3 tangent, out float3 bitangent)
{
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    tangent = normalize(cross(up, normal));
    bitangent = cross(normal, tangent);
}

float3 PerturbReflection(float3 reflectDir, float3 viewNormal, float roughness, float2 xi, float spreadScale)
{
    const float spread = kMaxReflectionSpread * roughness * roughness * spreadScale;
    if (spread <= 1e-5)
    {
        return reflectDir;
    }

    float3 tangent;
    float3 bitangent;
    BuildTangentFrame(viewNormal, tangent, bitangent);

    const float2 disk = (xi * 2.0 - 1.0) * spread;
    return normalize(reflectDir + tangent * disk.x + bitangent * disk.y);
}

float RayDistanceForBoundary(int stepBoundary, int stepCount, float maxDist, float exponent)
{
    const float step01 = saturate((float)stepBoundary / (float)stepCount);
    return maxDist * pow(step01, exponent);
}

bool SampleRayDepthDelta(
    float t,
    float3 viewPos,
    float3 rayDir,
    out float2 sampleUv,
    out float depthDelta)
{
    const float3 marchPos = viewPos + rayDir * t;
    sampleUv = ViewPosToDepthUv(marchPos);
    if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0)
    {
        depthDelta = 0.0;
        return false;
    }

    const float sampleDepth = uDepthMap.Sample(uDepthSampler, sampleUv).r;
    if (sampleDepth >= 0.9999)
    {
        depthDelta = 0.0;
        return false;
    }

    const float3 sampleViewPos = ViewPosFromDepth(sampleUv, sampleDepth);
    depthDelta = marchPos.z - sampleViewPos.z;
    return true;
}

float RefineHitDistance(float tLow, float tHigh, float3 viewPos, float3 rayDir)
{
    [loop]
    for (int refineIndex = 0; refineIndex < kRefineSteps; ++refineIndex)
    {
        const float tMid = 0.5 * (tLow + tHigh);
        float2 sampleUv;
        float depthDelta;
        if (!SampleRayDepthDelta(tMid, viewPos, rayDir, sampleUv, depthDelta))
        {
            tHigh = tMid;
            continue;
        }

        if (depthDelta > uThickness)
        {
            tHigh = tMid;
        }
        else if (depthDelta <= 0.0)
        {
            tLow = tMid;
        }
        else
        {
            tHigh = tMid;
        }
    }

    return 0.5 * (tLow + tHigh);
}

float HitScreenEdgePenalty(float2 sampleUv)
{
    const float centerDepth = ViewDepthAt(sampleUv);
    const float2 offsets[4] = {
        float2(uTexelSize.x, 0.0),
        float2(-uTexelSize.x, 0.0),
        float2(0.0, uTexelSize.y),
        float2(0.0, -uTexelSize.y),
    };

    float maxRelativeJump = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float2 neighborUv = sampleUv + offsets[i];
        if (neighborUv.x < 0.0 || neighborUv.x > 1.0 || neighborUv.y < 0.0 || neighborUv.y > 1.0)
        {
            continue;
        }

        const float neighborDepth = ViewDepthAt(neighborUv);
        maxRelativeJump = max(
            maxRelativeJump,
            abs(neighborDepth - centerDepth) / max(abs(centerDepth), 1e-3));
    }

    return 1.0 - smoothstep(0.04, 0.18, maxRelativeJump);
}

float4 SampleSceneColorBilinear(float2 uv)
{
    const float2 texel = max(uTexelSize, 1e-6);
    const float2 base = uv - texel * 0.5;
    float3 rgb = 0.0.xxx;
    float validity = 0.0;
    float weightSum = 0.0;

    [unroll]
    for (int j = 0; j <= 1; ++j)
    {
        [unroll]
        for (int i = 0; i <= 1; ++i)
        {
            const float2 sampleUv = base + float2((float)i, (float)j) * texel;
            const float4 sampleColor = uSceneColorMap.Sample(uSceneColorLinearSampler, sampleUv);
            const float weight = sampleColor.a;
            rgb += sampleColor.rgb * weight;
            validity += weight;
            weightSum += 1.0;
        }
    }

    if (validity <= 1e-4)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    return float4(rgb / validity, saturate(validity / weightSum));
}

bool TryAcceptHit(
    float tHit,
    float3 viewPos,
    float3 rayDir,
    out float3 hitRadiance,
    out float hitConfidence)
{
    hitRadiance = 0.0.xxx;
    hitConfidence = 0.0;

    float2 sampleUv;
    float depthDelta;
    if (!SampleRayDepthDelta(tHit, viewPos, rayDir, sampleUv, depthDelta))
    {
        return false;
    }

    if (depthDelta <= 0.0 || depthDelta >= uThickness)
    {
        return false;
    }

    const float4 sceneColor = SampleSceneColorBilinear(sampleUv);
    if (sceneColor.a <= 0.35)
    {
        return false;
    }

    const float3 sampleWorldNormal = normalize(uNormalMap.Sample(uNormalSampler, sampleUv).rgb);
    float3 sampleViewNormal = mul((float3x3)uView, sampleWorldNormal);
    sampleViewNormal = normalize(sampleViewNormal);

    const float distance01 = saturate(tHit / max(uMaxTraceDistance, 1e-4));
    const float distanceWeight = pow(saturate(1.0 - distance01), 2.0);
    const float thicknessWeight = 1.0 - smoothstep(0.0, uThickness, depthDelta);
    const float facingWeight = saturate(dot(sampleViewNormal, -rayDir));
    const float edgeWeight = HitScreenEdgePenalty(sampleUv);
    hitConfidence = distanceWeight * thicknessWeight * facingWeight * edgeWeight * sceneColor.a;
    hitRadiance = ClampRadiance(sceneColor.rgb);
    return hitConfidence > 0.0;
}

bool TraceReflectionRay(
    float2 uv,
    float3 viewPos,
    float3 rayDir,
    int stepCount,
    float maxDist,
    float stepExponent,
    float frameJitter,
    float sampleSeed,
    out float3 hitRadiance,
    out float hitConfidence)
{
    hitRadiance = 0.0.xxx;
    hitConfidence = 0.0;

    float tPrev = 0.0;
    float prevDelta = 0.0;
    bool hasPrevSample = false;

    [loop]
    for (int stepIndex = 0; stepIndex < stepCount; ++stepIndex)
    {
        const float tLow = RayDistanceForBoundary(stepIndex, stepCount, maxDist, stepExponent);
        const float tHigh = RayDistanceForBoundary(stepIndex + 1, stepCount, maxDist, stepExponent);
        const float stepJitter = Hash21(
            uv + float2((float)stepIndex * 1.37 + sampleSeed * 4.11, frameJitter * 0.91));
        const float t = lerp(tLow, tHigh, stepJitter);
        if (t > maxDist)
        {
            break;
        }

        float2 sampleUv;
        float depthDelta;
        if (!SampleRayDepthDelta(t, viewPos, rayDir, sampleUv, depthDelta))
        {
            break;
        }

        const bool crossedSurface = hasPrevSample && prevDelta <= 0.0 && depthDelta > 0.0;
        const bool insideThickness = depthDelta > 0.0 && depthDelta < uThickness;
        if (crossedSurface || insideThickness)
        {
            const float refineLow = hasPrevSample ? tPrev : tLow;
            const float tHit = RefineHitDistance(refineLow, t, viewPos, rayDir);
            if (TryAcceptHit(tHit, viewPos, rayDir, hitRadiance, hitConfidence))
            {
                return true;
            }
            break;
        }

        tPrev = t;
        prevDelta = depthDelta;
        hasPrevSample = true;
    }

    return false;
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uDepthSampler, uv).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float roughness = uMaterial0Map.Sample(uMaterial0Sampler, uv).a;
    const float specularWeight = SpecularTraceWeight(roughness);
    if (specularWeight <= 1e-4)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 worldNormal = normalize(uNormalMap.Sample(uNormalSampler, uv).rgb);
    float3 viewNormal = mul((float3x3)uView, worldNormal);
    viewNormal = normalize(viewNormal);

    float3 viewPos = ViewPosFromDepth(uv, depth);
    const float normalBias = max(abs(viewPos.z) * 0.00075, 0.015);
    viewPos += viewNormal * normalBias;

    const float3 viewDir = normalize(-viewPos);
    float3 baseRayDir = reflect(-viewDir, viewNormal);
    if (dot(baseRayDir, viewNormal) < 1e-4)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    baseRayDir = normalize(baseRayDir);

    const int stepCount = max(uStepCount, 1);
    const float stepExponent = max(uStepExponent, 1.05);
    const int sampleCount = clamp(uSampleCount, 1, kMaxSamples);
    const float frameJitter = uFrameIndex + InterleavedGradientNoise(input.position.xy + 0.5) * 13.0;
    const float spreadScale = rsqrt((float)sampleCount);

    float3 radianceSum = 0.0.xxx;
    float confidenceWeightSum = 0.0;
    float confidenceSum = 0.0;

    [loop]
    for (int sampleIndex = 0; sampleIndex < kMaxSamples; ++sampleIndex)
    {
        if (sampleIndex >= sampleCount)
        {
            break;
        }

        const float sampleSeed = (float)sampleIndex + 1.0;
        const float2 reflectXi = Hash22(
            uv + float2(frameJitter * 0.017 + sampleSeed * 2.71, frameJitter * 0.031 + sampleSeed * 5.13));
        const float3 rayDir = PerturbReflection(baseRayDir, viewNormal, roughness, reflectXi, spreadScale);

        float3 hitRadiance;
        float hitConfidence;
        if (TraceReflectionRay(
                uv,
                viewPos,
                rayDir,
                stepCount,
                uMaxTraceDistance,
                stepExponent,
                frameJitter,
                sampleSeed,
                hitRadiance,
                hitConfidence))
        {
            radianceSum += hitRadiance * hitConfidence;
            confidenceWeightSum += hitConfidence;
            confidenceSum += hitConfidence;
        }
    }

    if (confidenceWeightSum <= 1e-5)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float invSamples = 1.0 / (float)sampleCount;
    const float3 tracedRadiance = (radianceSum / confidenceWeightSum) * specularWeight;
    const float confidence = saturate(confidenceSum * invSamples * specularWeight);
    return float4(tracedRadiance, confidence);
}
