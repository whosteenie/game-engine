// Approximate full-frame boil metric. A fixed grid keeps the diagnostic cheap and stable.

Texture2D<float4> uStatsMap : register(t0);
SamplerState uStatsSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float2 uRoiMin;
    float2 uRoiMax;
    int uStaticVarianceMetric;
    float3 _Padding0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    static const int kSampleSide = 64;
    float sumDelta = 0.0;
    float sumMeanLuminance = 0.0;
    float sumRelativeDelta = 0.0;
    float sumRelativeSigma = 0.0;
    float eligibleCount = 0.0;
    float deltaCount = 0.0;

    [loop]
    for (int y = 0; y < kSampleSide; ++y)
    {
        [loop]
        for (int x = 0; x < kSampleSide; ++x)
        {
            const float2 uv = (float2(x, y) + 0.5) / float(kSampleSide);
            if (any(uv < uRoiMin) || any(uv > uRoiMax))
            {
                continue;
            }
            const float4 stats = uStatsMap.SampleLevel(uStatsSampler, uv, 0.0);
            if (stats.a <= 0.0)
            {
                continue;
            }
            eligibleCount += 1.0;
            sumMeanLuminance += stats.r;
            const float variance = stats.a > 1.0
                ? max(stats.g / (stats.a - 1.0), 0.0) : 0.0;
            sumRelativeSigma += sqrt(variance) / max(abs(stats.r), 0.05);
            if (stats.b >= 0.0)
            {
                sumDelta += stats.b;
                sumRelativeDelta += stats.b / max(abs(stats.r), 0.05);
                deltaCount += 1.0;
            }
        }
    }

    const float meanDelta = deltaCount > 0.0 ? sumDelta / deltaCount : 0.0;
    const float meanLuminance = eligibleCount > 0.0 ? sumMeanLuminance / eligibleCount : 0.0;
    const float meanRelativeDelta = deltaCount > 0.0 ? sumRelativeDelta / deltaCount : 0.0;
    const float validFraction = eligibleCount > 0.0 ? deltaCount / eligibleCount : 0.0;
    const float meanRelativeSigma = eligibleCount > 0.0
        ? sumRelativeSigma / eligibleCount : 0.0;
    return float4(
        meanDelta,
        meanLuminance,
        meanRelativeDelta,
        uStaticVarianceMetric != 0 ? meanRelativeSigma : validFraction);
}
