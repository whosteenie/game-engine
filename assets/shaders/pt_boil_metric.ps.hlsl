// Approximate full-frame boil metric. A fixed grid keeps the diagnostic cheap and stable.

Texture2D<float4> uStatsMap : register(t0);
SamplerState uStatsSampler : register(s0);

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    static const int kSampleSide = 64;
    float sumDelta = 0.0;

    [loop]
    for (int y = 0; y < kSampleSide; ++y)
    {
        [loop]
        for (int x = 0; x < kSampleSide; ++x)
        {
            const float2 uv = (float2(x, y) + 0.5) / float(kSampleSide);
            sumDelta += uStatsMap.SampleLevel(uStatsSampler, uv, 0.0).b;
        }
    }

    const float meanDelta = sumDelta / float(kSampleSide * kSampleSide);
    return float4(meanDelta, 0.0, 0.0, 1.0);
}
