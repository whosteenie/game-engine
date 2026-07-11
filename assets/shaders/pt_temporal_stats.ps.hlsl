// Path-tracer temporal diagnostics: Welford luminance stats plus per-frame luminance delta.

Texture2D<float4> uCurrentRadiance : register(t0);
Texture2D<float4> uPrevRadiance : register(t1);
Texture2D<float4> uPrevStats : register(t2);

cbuffer PerPixel : register(b0)
{
    int uResetStats;
    int uPrevFrameValid;
    float2 _Padding0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luminance(float3 color)
{
    return dot(max(color, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

float4 main(PSInput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    const float currentLuma = Luminance(uCurrentRadiance.Load(int3(pixel, 0)).rgb);
    const float prevLuma = Luminance(uPrevRadiance.Load(int3(pixel, 0)).rgb);
    const float frameDelta = uPrevFrameValid != 0 ? abs(currentLuma - prevLuma) : 0.0;

    if (uResetStats != 0)
    {
        return float4(currentLuma, 0.0, frameDelta, 1.0);
    }

    const float4 previous = uPrevStats.Load(int3(pixel, 0));
    const float previousMean = previous.r;
    const float previousM2 = previous.g;
    const float previousCount = max(previous.a, 0.0);
    const float count = previousCount + 1.0;
    const float delta = currentLuma - previousMean;
    const float mean = previousMean + delta / count;
    const float delta2 = currentLuma - mean;
    const float m2 = previousM2 + delta * delta2;

    return float4(mean, m2, frameDelta, count);
}
