Texture2D uHdrColor : register(t0);
SamplerState uHdrColorSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uThreshold;
    float uSoftKnee;
    float uExposure;
    float uFullTexelSizeX;
    float uFullTexelSizeY;
    float2 _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Karis-weighted 4-tap downsample reduces single-pixel specular fireflies before thresholding.
float3 KarisDownsample(float2 uv, float2 fullTexelSize)
{
    const float2 offsets[4] = {
        float2(-0.5, -0.5),
        float2(0.5, -0.5),
        float2(-0.5, 0.5),
        float2(0.5, 0.5),
    };

    float3 sum = 0.0.xxx;
    float weightSum = 0.0;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float3 sampleRgb = uHdrColor.Sample(uHdrColorSampler, uv + offsets[i] * fullTexelSize).rgb;
        const float weight = 1.0 / (1.0 + Luminance(sampleRgb));
        sum += sampleRgb * weight;
        weightSum += weight;
    }

    return sum / max(weightSum, 1e-5);
}

float3 ExtractBrightColor(float3 color, float threshold, float knee)
{
    const float brightness = max(color.r, max(color.g, color.b));
    const float kneeRange = max(threshold * knee, 1e-4);
    float soft = brightness - threshold + kneeRange;
    soft = clamp(soft, 0.0, 2.0 * kneeRange);
    soft = (soft * soft) / (4.0 * kneeRange + 0.00001);
    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 0.00001);
    return color * contribution;
}

float3 main(PSInput input) : SV_Target
{
    const float2 fullTexelSize = float2(uFullTexelSizeX, uFullTexelSizeY);
    float3 hdr = KarisDownsample(input.texCoord, fullTexelSize) * exp2(uExposure);
    return ExtractBrightColor(hdr, uThreshold, uSoftKnee);
}
