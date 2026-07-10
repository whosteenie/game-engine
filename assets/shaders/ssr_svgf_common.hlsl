// Shared helpers for SSR SVGF (Spatiotemporal Variance-Guided Filtering).

#ifndef SSR_SVGF_COMMON_HLSL
#define SSR_SVGF_COMMON_HLSL

float SsrTraceLuma(float4 trace)
{
    return dot(trace.rgb, float3(0.2126, 0.7152, 0.0722));
}

float SsrSpatialLumaVariance(
    Texture2D traceMap,
    SamplerState traceSampler,
    float2 uv,
    float2 texelSize)
{
    float lumaSum = 0.0;
    float lumaSqSum = 0.0;
    int count = 0;

    [loop]
    for (int j = -1; j <= 1; ++j)
    {
        [loop]
        for (int i = -1; i <= 1; ++i)
        {
            const float4 sampleTrace =
                traceMap.Sample(traceSampler, uv + float2((float)i, (float)j) * texelSize);
            if (sampleTrace.a <= 1e-4)
            {
                continue;
            }

            const float sampleLuma = SsrTraceLuma(sampleTrace);
            lumaSum += sampleLuma;
            lumaSqSum += sampleLuma * sampleLuma;
            ++count;
        }
    }

    if (count < 3)
    {
        return 0.0;
    }

    const float mean = lumaSum / (float)count;
    return max(lumaSqSum / (float)count - mean * mean, 0.0);
}

float2 SsrVelocityNdcToUvDelta(float2 velocityNdc)
{
    return float2(velocityNdc.x * 0.5, -velocityNdc.y * 0.5);
}

#endif
