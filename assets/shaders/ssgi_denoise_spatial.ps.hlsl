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
    float2 uBlurDirection;
    float uDepthThreshold;
    float uBlurSpread;
    float uRoughnessSpreadMin;
    float uRoughnessSpreadMax;
    float uNormalPower;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

static const int kBlurRadius = 3;
static const float kBlurWeights[7] = {0.0312, 0.0878, 0.1719, 0.2182, 0.1719, 0.0878, 0.0312};

float ViewDepth(float2 texCoord)
{
    const float depth = uDepthMap.Sample(uDepthSampler, texCoord).r;
    const float2 clipXY = float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
    const float4 viewSpace = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewSpace.z / viewSpace.w;
}

float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float4 centerSample = uInput.Sample(uInputSampler, input.texCoord);
    const float3 centerNormal = normalize(uNormalMap.Sample(uNormalSampler, input.texCoord).rgb);
    const float roughness = uMaterial0Map.Sample(uMaterial0Sampler, input.texCoord).a;
    const float roughnessSpread = lerp(uRoughnessSpreadMin, uRoughnessSpreadMax, roughness);
    const float2 direction = uBlurDirection * uTexelSize * max(uBlurSpread * roughnessSpread, 0.25);
    const float centerDepth = ViewDepth(input.texCoord);

    float3 result = 0.0.xxx;
    float weightSum = 0.0;

    [loop]
    for (int tap = -kBlurRadius; tap <= kBlurRadius; ++tap)
    {
        const float2 sampleUv = input.texCoord + direction * (float)tap;
        const float sampleDepth = ViewDepth(sampleUv);
        const float relativeDepthDelta =
            abs(sampleDepth - centerDepth) / max(abs(centerDepth), 1e-3);
        const float depthWeight = 1.0 - smoothstep(
            uDepthThreshold * 0.5,
            uDepthThreshold,
            relativeDepthDelta);

        const float3 sampleNormal = normalize(uNormalMap.Sample(uNormalSampler, sampleUv).rgb);
        const float normalWeight = pow(saturate(dot(centerNormal, sampleNormal)), uNormalPower);

        const float weight = kBlurWeights[tap + kBlurRadius] * depthWeight * normalWeight;
        result += uInput.Sample(uInputSampler, sampleUv).rgb * weight;
        weightSum += weight;
    }

    if (weightSum <= 1e-5)
    {
        return centerSample;
    }

    return float4(result / weightSum, centerSample.a);
}
