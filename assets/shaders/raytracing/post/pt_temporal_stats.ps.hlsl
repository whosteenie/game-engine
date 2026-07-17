// Path-tracer temporal diagnostics: Welford luminance stats plus per-frame luminance delta.

Texture2D<float4> uCurrentRadiance : register(t0);
Texture2D<float4> uPrevRadiance : register(t1);
Texture2D<float4> uPrevStats : register(t2);
Texture2D<float4> uMotion : register(t3);
Texture2D<uint2> uMetadata : register(t4);

cbuffer PerPixel : register(b0)
{
    int uResetStats;
    int uPrevFrameValid;
    int uMotionReproject;
    int uGiSignal;
    float2 uMotionScale;
    uint uSelectedInstanceIdPlusOne;
    float _Padding0;
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
    uint width, height;
    uCurrentRadiance.GetDimensions(width, height);
    const float4 currentSignal = uCurrentRadiance.Load(int3(pixel, 0));
    if (uGiSignal != 0 && (currentSignal.a <= 0.0
        || (uSelectedInstanceIdPlusOne != 0u
            && uMetadata.Load(int3(pixel, 0)).x != uSelectedInstanceIdPlusOne)))
    {
        return float4(0.0, 0.0, -1.0, 0.0);
    }

    int2 previousPixel = int2(pixel);
    bool previousValid = uPrevFrameValid != 0;
    if (previousValid && uMotionReproject != 0)
    {
        const float2 currentUv = (float2(pixel) + 0.5) / float2(width, height);
        const float4 motion = uMotion.Load(int3(pixel, 0));
        const float2 previousUv = currentUv + motion.xy * uMotionScale;
        previousValid = all(previousUv >= 0.0.xx) && all(previousUv <= 1.0.xx);
        previousPixel = clamp(
            int2(previousUv * float2(width, height)),
            int2(0, 0),
            int2(width, height) - 1);
        if (previousValid && uGiSignal != 0)
        {
            const float4 previousSignal = uPrevRadiance.Load(int3(previousPixel, 0));
            const float expectedPreviousDepth = currentSignal.a + motion.z;
            previousValid = previousSignal.a > 0.0 && expectedPreviousDepth > 0.0
                && abs(previousSignal.a - expectedPreviousDepth)
                    <= 0.02 * max(expectedPreviousDepth, 1.0e-3);
        }
    }

    const float currentLuma = Luminance(currentSignal.rgb);
    const float prevLuma = previousValid
        ? Luminance(uPrevRadiance.Load(int3(previousPixel, 0)).rgb) : currentLuma;
    const float frameDelta = previousValid ? abs(currentLuma - prevLuma) : -1.0;

    if (uResetStats != 0 || !previousValid)
    {
        return float4(currentLuma, 0.0, frameDelta, 1.0);
    }

    const float4 previous = uPrevStats.Load(int3(previousPixel, 0));
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
