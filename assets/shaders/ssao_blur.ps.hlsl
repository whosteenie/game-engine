Texture2D uInput : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uInputSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float4x4 uInvProjection;
    float uTexelSizeX;
    float uTexelSizeY;
    float uDepthThreshold;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float ViewDepth(float2 texCoord)
{
    const float depth = uDepthMap.Sample(uDepthSampler, texCoord).r;
    const float2 clipXY = float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
    const float4 viewSpace = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewSpace.z / viewSpace.w;
}

float main(PSInput input) : SV_Target
{
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float centerDepth = ViewDepth(input.texCoord);
    const float centerAo = uInput.Sample(uInputSampler, input.texCoord).r;

    float result = 0.0;
    float weightSum = 0.0;

    [loop]
    for (int x = -1; x <= 1; ++x)
    {
        [loop]
        for (int y = -1; y <= 1; ++y)
        {
            const float2 sampleUv = input.texCoord + float2((float)x, (float)y) * texelSize;
            const float sampleDepth = ViewDepth(sampleUv);
            const float relativeDepthDelta =
                abs(sampleDepth - centerDepth) / max(centerDepth, 1e-3);
            const float depthWeight = 1.0 - smoothstep(
                uDepthThreshold * 0.5,
                uDepthThreshold,
                relativeDepthDelta);
            const float weight = depthWeight;
            result += uInput.Sample(uInputSampler, sampleUv).r * weight;
            weightSum += weight;
        }
    }

    if (weightSum <= 1e-5)
    {
        return centerAo;
    }

    return result / weightSum;
}
