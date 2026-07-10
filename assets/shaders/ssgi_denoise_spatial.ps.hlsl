Texture2D uInput : register(t0);
Texture2D uDepthMap : register(t1);
Texture2D uNormalMap : register(t2);
Texture2D uMaterial0Map : register(t3);

SamplerState uInputSampler : register(s0);
SamplerState uDepthSampler : register(s1);
SamplerState uNormalSampler : register(s2);
SamplerState uMaterial0Sampler : register(s3);

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
    const float2 clipXY = float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
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

    const float4 centerSample = uInput.Sample(uInputSampler, uv);
    const float3 centerNormal = normalize(uNormalMap.Sample(uNormalSampler, uv).rgb);
    const float roughness = uMaterial0Map.Sample(uMaterial0Sampler, uv).a;
    const float roughnessSpread = lerp(uRoughnessSpreadMin, uRoughnessSpreadMax, roughness);
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
                uDepthThreshold * 0.5,
                uDepthThreshold,
                relativeDepthDelta);

            const float3 sampleNormal = normalize(uNormalMap.Sample(uNormalSampler, sampleUv).rgb);
            const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), uNormalPower);

            const float weight = kernelWeight * depthWeight * normalWeight;
            const float4 sampleRadiance = uInput.Sample(uInputSampler, sampleUv);
            result += sampleRadiance.rgb * weight;
            confidenceSum += saturate(sampleRadiance.a) * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= 1e-5)
    {
        return centerSample;
    }

    return float4(result / weightSum, saturate(confidenceSum / weightSum));
}
