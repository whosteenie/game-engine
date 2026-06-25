Texture2D uInput : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uInputSampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
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

float main(PSInput input) : SV_Target
{
    const float2 texelSize = float2(uTexelSizeX, uTexelSizeY);
    const float centerDepth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
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
            const float sampleDepth = uDepthMap.Sample(uDepthSampler, sampleUv).r;
            const float depthWeight = 1.0 - smoothstep(
                uDepthThreshold * 0.5,
                uDepthThreshold,
                abs(sampleDepth - centerDepth));
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
