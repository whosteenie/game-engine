#include "../../common/screen_space_common.hlsl"

Texture2D uDepthMap : register(t0);
Texture2D uNormalMap : register(t1);

SamplerState uDepthSampler : register(s0);
SamplerState uNormalSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float4x4 uProjection;
    float4x4 uInvProjection;
    float4x4 uView;
    float2 uProjectionScale;
    float uRadius;
    float uThickness;
    float uFalloff;
    float uNearPlane;
    float uFarPlane;
    int uDirections;
    int uSteps;
    int uUseGeometryNormals;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 ReconstructViewNormal(float2 texCoord, float depth)
{
    uint width;
    uint height;
    uDepthMap.GetDimensions(width, height);
    const float2 texelSize = 1.0 / float2(width, height);
    const float3 posCenter = ReconstructViewPos(uDepthMap, uDepthSampler, uInvProjection, texCoord);
    const float3 posX = ReconstructViewPos(
        uDepthMap,
        uDepthSampler,
        uInvProjection,
        texCoord + float2(texelSize.x, 0.0));
    const float3 posY = ReconstructViewPos(
        uDepthMap,
        uDepthSampler,
        uInvProjection,
        texCoord + float2(0.0, texelSize.y));
    float3 normal = normalize(cross(posX - posCenter, posY - posCenter));
    const float3 viewDir = normalize(-posCenter);
    return faceforward(normal, viewDir, normal);
}

float3 SampleViewNormal(float2 texCoord, float depth)
{
    if (uUseGeometryNormals != 0)
    {
        const float3 worldNormal = uNormalMap.Sample(uNormalSampler, texCoord).xyz;
        if (dot(worldNormal, worldNormal) > 1e-4)
        {
            return normalize(mul((float3x3)uView, normalize(worldNormal)));
        }
    }

    return ReconstructViewNormal(texCoord, depth);
}

float2 DirectionFromIndex(int directionIndex, int directionCount, float rotation)
{
    const float angle = (6.28318530718 * ((float)directionIndex + rotation)) / (float)directionCount;
    return float2(cos(angle), sin(angle));
}

float main(PSInput input) : SV_Target
{
    const float centerDepth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (centerDepth >= 0.9999)
    {
        return 1.0;
    }

    const float3 centerPos = ReconstructViewPos(uDepthMap, uDepthSampler, uInvProjection, input.texCoord);
    const float3 centerNormal = SampleViewNormal(input.texCoord, centerDepth);
    const float centerViewZ = max(centerPos.z, uNearPlane);
    const float radius = max(uRadius, 0.01);
    const float thickness = max(uThickness, 0.01);
    const int directionCount = clamp(uDirections, 2, 8);
    const int stepCount = clamp(uSteps, 2, 12);

    const float2 uvRadius = 0.5 * radius * uProjectionScale / max(centerViewZ, 1e-3);
    const float rotation = InterleavedGradientNoise(input.position.xy + 0.5);

    float occlusion = 0.0;
    float weightSum = 0.0;

    [loop]
    for (int directionIndex = 0; directionIndex < 8; ++directionIndex)
    {
        if (directionIndex >= directionCount)
        {
            break;
        }

        const float2 direction = DirectionFromIndex(directionIndex, directionCount, rotation);
        float directionOcclusion = 0.0;
        float directionWeight = 0.0;

        [loop]
        for (int stepIndex = 1; stepIndex <= 12; ++stepIndex)
        {
            if (stepIndex > stepCount)
            {
                break;
            }

            const float stepFraction = ((float)stepIndex + 0.35 * rotation) / (float)stepCount;
            const float2 sampleUv = input.texCoord + direction * uvRadius * stepFraction;
            if (sampleUv.x <= 0.0 || sampleUv.x >= 1.0 || sampleUv.y <= 0.0 || sampleUv.y >= 1.0)
            {
                continue;
            }

            const float sampleDepth = uDepthMap.Sample(uDepthSampler, sampleUv).r;
            if (sampleDepth >= 0.9999)
            {
                continue;
            }

            const float3 samplePos = ReconstructViewPos(uDepthMap, uDepthSampler, uInvProjection, sampleUv);
            const float3 sampleVector = samplePos - centerPos;
            const float distance = length(sampleVector);
            if (distance <= 1e-4 || distance > radius)
            {
                continue;
            }

            const float3 sampleDir = sampleVector / distance;
            const float normalHorizon = saturate(dot(centerNormal, sampleDir));
            const float distanceWeight = pow(saturate(1.0 - distance / radius), max(uFalloff, 0.25));
            const float depthDelta = abs(samplePos.z - centerPos.z);
            const float thicknessWeight = 1.0 - smoothstep(thickness, thickness * 3.0, depthDelta);
            const float sampleOcclusion = normalHorizon * distanceWeight * thicknessWeight;

            directionOcclusion = max(directionOcclusion, sampleOcclusion);
            directionWeight = max(directionWeight, distanceWeight);
        }

        occlusion += directionOcclusion;
        weightSum += max(directionWeight, 0.25);
    }

    if (weightSum <= 1e-4)
    {
        return 1.0;
    }

    float visibility = 1.0 - saturate(occlusion / weightSum);

    const float distanceFade = smoothstep(uFarPlane * 0.35, uFarPlane * 0.9, centerViewZ);
    visibility = lerp(visibility, 1.0, distanceFade);

    return visibility;
}
