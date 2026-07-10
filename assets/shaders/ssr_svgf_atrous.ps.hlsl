// SVGF pass 3 — variance-guided à-trous wavelet filter for SSR.
// Multiple passes with increasing step (1, 2, 4, 8 px). Higher variance → stronger filter.

#include "screen_space_common.hlsl"
#include "ssr_svgf_common.hlsl"

Texture2D uColor : register(t0);
Texture2D uVariance : register(t1);
Texture2D uDepthMap : register(t2);
Texture2D uNormalMap : register(t3);
Texture2D uMaterial0Map : register(t4);

SamplerState uColorSampler : register(s0);
SamplerState uVarianceSampler : register(s1);
SamplerState uDepthSampler : register(s2);
SamplerState uNormalSampler : register(s3);
SamplerState uMaterial0Sampler : register(s4);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float2 uTexelSize;
    float uDepthThreshold;
    float uBlurSpread;
    float uRoughnessSpreadMin;
    float uRoughnessSpreadMax;
    float uNormalPower;
    float uStepScale;
    float uPhiEpsilon;
    float uFilterStrength;
    float2 _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

static const float kAtrous1D[5] = {0.0625, 0.25, 0.375, 0.25, 0.0625};

float ViewDepth(float2 texCoord)
{
    const float depth = uDepthMap.Sample(uDepthSampler, texCoord).r;
    const float2 clipXY = DepthUvToClipXY(texCoord);
    const float4 viewSpace = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewSpace.z / viewSpace.w;
}

float4 main(PSInput input) : SV_Target
{
    const float2 uv = input.texCoord;
    const float depth = uDepthMap.Sample(uDepthSampler, uv).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 centerSample = uColor.Sample(uColorSampler, uv);
    if (centerSample.a <= 1e-4)
    {
        return centerSample;
    }

    const float variance = max(uVariance.Sample(uVarianceSampler, uv).r, 0.0);
    const float phi = variance / (variance + uPhiEpsilon);
    const float filterWeight = saturate(phi * uFilterStrength);
    if (filterWeight <= 1e-4)
    {
        return centerSample;
    }

    const float3 centerNormal = normalize(uNormalMap.Sample(uNormalSampler, uv).rgb);
    const float roughness = uMaterial0Map.Sample(uMaterial0Sampler, uv).a;
    const float smoothness = 1.0 - roughness;
    const float roughnessSpread = lerp(uRoughnessSpreadMax, uRoughnessSpreadMin, smoothness);
    const float stepPixels = max(uBlurSpread * uStepScale * roughnessSpread, 0.25);
    const float2 step = uTexelSize * stepPixels;
    const float centerDepth = ViewDepth(uv);

    float3 result = 0.0.xxx;
    float confidenceSum = 0.0;
    float weightSum = 0.0;

    [loop]
    for (int j = -2; j <= 2; ++j)
    {
        [loop]
        for (int i = -2; i <= 2; ++i)
        {
            const float kernelWeight = kAtrous1D[i + 2] * kAtrous1D[j + 2];
            const float2 sampleUv = uv + float2((float)i, (float)j) * step;
            const float sampleDepth = ViewDepth(sampleUv);
            const float relativeDepthDelta =
                abs(sampleDepth - centerDepth) / max(abs(centerDepth), 1e-3);
            const float depthWeight = 1.0 - smoothstep(
                uDepthThreshold * 0.35,
                uDepthThreshold,
                relativeDepthDelta);

            const float3 sampleNormal = normalize(uNormalMap.Sample(uNormalSampler, sampleUv).rgb);
            const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), uNormalPower);

            const float weight = kernelWeight * depthWeight * normalWeight;
            const float4 sampleRadiance = uColor.Sample(uColorSampler, sampleUv);
            result += sampleRadiance.rgb * weight;
            confidenceSum += sampleRadiance.a * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= 1e-5)
    {
        return centerSample;
    }

    const float4 filtered = float4(
        result / max(weightSum, 1e-5),
        saturate(confidenceSum / weightSum));
    return lerp(centerSample, filtered, filterWeight);
}
