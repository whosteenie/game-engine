Texture2D uInput : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uInputSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float uDirectionX;
    float uDirectionY;
    float uBlurRadius;
    float uDepthThreshold;
    float uShadowThreshold;
};

static const float kKernelWeights[5] = {
    0.227027,
    0.1945946,
    0.1216216,
    0.054054,
    0.016216
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float ViewDepth(float2 texCoord)
{
    float depth = uDepthMap.Sample(uDepthSampler, texCoord).r;
    float4 clipSpace = float4(texCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    float4 viewSpace = mul(uInvProjection, clipSpace);
    return viewSpace.z / viewSpace.w;
}

float SampleWeight(float centerShadow, float centerViewDepth, float2 sampleUv, float kernelWeight)
{
    float sampleShadow = uInput.Sample(uInputSampler, sampleUv).r;
    float sampleViewDepth = ViewDepth(sampleUv);
    float depthWeight = 1.0 - smoothstep(
        uDepthThreshold * 0.5,
        uDepthThreshold,
        abs(sampleViewDepth - centerViewDepth));
    float shadowWeight = 1.0 - smoothstep(
        uShadowThreshold * 0.5,
        uShadowThreshold,
        abs(sampleShadow - centerShadow));
    return kernelWeight * depthWeight * shadowWeight;
}

float main(PSInput input) : SV_Target
{
    float2 direction = float2(uDirectionX, uDirectionY) * uBlurRadius;
    float centerShadow = uInput.Sample(uInputSampler, input.texCoord).r;
    float centerViewDepth = ViewDepth(input.texCoord);

    float result = 0.0;
    float weightSum = 0.0;

    [loop]
    for (int tap = 0; tap < 5; ++tap)
    {
        float kernelWeight = kKernelWeights[tap];
        if (tap == 0)
        {
            result += centerShadow * kernelWeight;
            weightSum += kernelWeight;
            continue;
        }

        float2 offset = direction * (float)tap;
        [loop]
        for (int sign = -1; sign <= 1; sign += 2)
        {
            float2 sampleUv = input.texCoord + offset * (float)sign;
            float weight = SampleWeight(centerShadow, centerViewDepth, sampleUv, kernelWeight);
            result += uInput.Sample(uInputSampler, sampleUv).r * weight;
            weightSum += weight;
        }
    }

    return result / max(weightSum, 1e-5);
}
