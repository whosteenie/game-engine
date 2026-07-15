// Fixed-grid path-tracer instability reductions. The 4x1 output stores aggregate, tail,
// spatial-coherence, and upper/lower ROI measurements for asynchronous CPU readback.

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
    static const int kHistogramBins = 64;
    static const float kHistogramMax = 8.0;
    static const float kHotRelativeDelta = 1.0;
    const uint outputIndex = (uint)input.position.x;

    if (outputIndex == 1u)
    {
        uint histogram[kHistogramBins];
        [unroll]
        for (int bin = 0; bin < kHistogramBins; ++bin)
        {
            histogram[bin] = 0u;
        }

        uint deltaCount = 0u;
        uint hotCount = 0u;
        float maxRelativeDelta = 0.0;
        [loop]
        for (int y = 0; y < kSampleSide; ++y)
        {
            [loop]
            for (int x = 0; x < kSampleSide; ++x)
            {
                const float2 uv = (float2(x, y) + 0.5) / float(kSampleSide);
                if (any(uv < uRoiMin) || any(uv > uRoiMax)) continue;
                const float4 stats = uStatsMap.SampleLevel(uStatsSampler, uv, 0.0);
                if (stats.a <= 0.0 || stats.b < 0.0) continue;

                const float relativeDelta = stats.b / max(abs(stats.r), 0.05);
                const uint histogramIndex = min(
                    (uint)(saturate(relativeDelta / kHistogramMax) * float(kHistogramBins)),
                    (uint)(kHistogramBins - 1));
                histogram[histogramIndex]++;
                deltaCount++;
                hotCount += relativeDelta >= kHotRelativeDelta ? 1u : 0u;
                maxRelativeDelta = max(maxRelativeDelta, relativeDelta);
            }
        }

        float p95 = 0.0;
        float p99 = 0.0;
        if (deltaCount > 0u)
        {
            const uint p95Target = max((uint)ceil(float(deltaCount) * 0.95), 1u);
            const uint p99Target = max((uint)ceil(float(deltaCount) * 0.99), 1u);
            uint cumulative = 0u;
            [loop]
            for (int percentileBin = 0; percentileBin < kHistogramBins; ++percentileBin)
            {
                cumulative += histogram[percentileBin];
                const float binUpper =
                    float(percentileBin + 1) * kHistogramMax / float(kHistogramBins);
                if (p95 == 0.0 && cumulative >= p95Target) p95 = binUpper;
                if (p99 == 0.0 && cumulative >= p99Target)
                {
                    p99 = binUpper;
                    break;
                }
            }
        }
        const float hotFraction = deltaCount > 0u
            ? float(hotCount) / float(deltaCount) : 0.0;
        return float4(p95, p99, min(maxRelativeDelta, 65504.0), hotFraction);
    }

    if (outputIndex == 2u)
    {
        uint mapWidth, mapHeight;
        uStatsMap.GetDimensions(mapWidth, mapHeight);
        const float2 neighborStep = 4.0 / float2(mapWidth, mapHeight);
        float rawSum = 0.0;
        float rawSquareSum = 0.0;
        float blurredSum = 0.0;
        float blurredSquareSum = 0.0;
        float sampleCount = 0.0;
        float blurredHotCount = 0.0;
        float pairASum = 0.0;
        float pairBSum = 0.0;
        float pairASquareSum = 0.0;
        float pairBSquareSum = 0.0;
        float pairProductSum = 0.0;
        float pairCount = 0.0;

        [loop]
        for (int y = 0; y < kSampleSide; ++y)
        {
            [loop]
            for (int x = 0; x < kSampleSide; ++x)
            {
                const float2 uv = (float2(x, y) + 0.5) / float(kSampleSide);
                if (any(uv < uRoiMin) || any(uv > uRoiMax)) continue;
                const float4 centerStats = uStatsMap.SampleLevel(uStatsSampler, uv, 0.0);
                if (centerStats.a <= 0.0 || centerStats.b < 0.0) continue;
                const float centerValue = centerStats.b / max(abs(centerStats.r), 0.05);

                float localSum = 0.0;
                float localCount = 0.0;
                [unroll]
                for (int oy = -1; oy <= 1; ++oy)
                {
                    [unroll]
                    for (int ox = -1; ox <= 1; ++ox)
                    {
                        const float2 neighborUv = uv + float2(ox, oy) * neighborStep;
                        if (any(neighborUv < uRoiMin) || any(neighborUv > uRoiMax)) continue;
                        const float4 neighborStats =
                            uStatsMap.SampleLevel(uStatsSampler, neighborUv, 0.0);
                        if (neighborStats.a <= 0.0 || neighborStats.b < 0.0) continue;
                        localSum += neighborStats.b / max(abs(neighborStats.r), 0.05);
                        localCount += 1.0;
                    }
                }
                const float blurredValue = localCount > 0.0 ? localSum / localCount : centerValue;
                rawSum += centerValue;
                rawSquareSum += centerValue * centerValue;
                blurredSum += blurredValue;
                blurredSquareSum += blurredValue * blurredValue;
                blurredHotCount += blurredValue >= kHotRelativeDelta ? 1.0 : 0.0;
                sampleCount += 1.0;

                const float2 pairUvs[2] = {
                    uv + float2(neighborStep.x, 0.0),
                    uv + float2(0.0, neighborStep.y),
                };
                [unroll]
                for (int pairIndex = 0; pairIndex < 2; ++pairIndex)
                {
                    const float2 pairUv = pairUvs[pairIndex];
                    if (any(pairUv < uRoiMin) || any(pairUv > uRoiMax)) continue;
                    const float4 pairStats = uStatsMap.SampleLevel(uStatsSampler, pairUv, 0.0);
                    if (pairStats.a <= 0.0 || pairStats.b < 0.0) continue;
                    const float pairValue = pairStats.b / max(abs(pairStats.r), 0.05);
                    pairASum += centerValue;
                    pairBSum += pairValue;
                    pairASquareSum += centerValue * centerValue;
                    pairBSquareSum += pairValue * pairValue;
                    pairProductSum += centerValue * pairValue;
                    pairCount += 1.0;
                }
            }
        }

        float neighborCorrelation = 0.0;
        if (pairCount > 0.0)
        {
            const float meanA = pairASum / pairCount;
            const float meanB = pairBSum / pairCount;
            const float varianceA = max(pairASquareSum / pairCount - meanA * meanA, 0.0);
            const float varianceB = max(pairBSquareSum / pairCount - meanB * meanB, 0.0);
            const float covariance = pairProductSum / pairCount - meanA * meanB;
            neighborCorrelation = covariance / max(sqrt(varianceA * varianceB), 1.0e-6);
        }
        float lowFrequencyRatio = 0.0;
        if (sampleCount > 0.0)
        {
            const float rawMean = rawSum / sampleCount;
            const float blurredMean = blurredSum / sampleCount;
            const float rawVariance = max(rawSquareSum / sampleCount - rawMean * rawMean, 0.0);
            const float blurredVariance =
                max(blurredSquareSum / sampleCount - blurredMean * blurredMean, 0.0);
            lowFrequencyRatio = blurredVariance / max(rawVariance, 1.0e-6);
        }
        const float blurredHotFraction = sampleCount > 0.0
            ? blurredHotCount / sampleCount : 0.0;
        return float4(
            clamp(neighborCorrelation, -1.0, 1.0),
            min(lowFrequencyRatio, 8.0),
            blurredHotFraction,
            sampleCount);
    }

    if (outputIndex == 3u)
    {
        uint upperHistogram[kHistogramBins];
        uint lowerHistogram[kHistogramBins];
        [unroll]
        for (int splitBin = 0; splitBin < kHistogramBins; ++splitBin)
        {
            upperHistogram[splitBin] = 0u;
            lowerHistogram[splitBin] = 0u;
        }
        uint upperCount = 0u;
        uint lowerCount = 0u;
        uint upperHotCount = 0u;
        uint lowerHotCount = 0u;
        const float splitY = 0.5 * (uRoiMin.y + uRoiMax.y);
        [loop]
        for (int y = 0; y < kSampleSide; ++y)
        {
            [loop]
            for (int x = 0; x < kSampleSide; ++x)
            {
                const float2 uv = (float2(x, y) + 0.5) / float(kSampleSide);
                if (any(uv < uRoiMin) || any(uv > uRoiMax)) continue;
                const float4 stats = uStatsMap.SampleLevel(uStatsSampler, uv, 0.0);
                if (stats.a <= 0.0 || stats.b < 0.0) continue;
                const float relativeDelta = stats.b / max(abs(stats.r), 0.05);
                const uint histogramIndex = min(
                    (uint)(saturate(relativeDelta / kHistogramMax) * float(kHistogramBins)),
                    (uint)(kHistogramBins - 1));
                if (uv.y < splitY)
                {
                    upperHistogram[histogramIndex]++;
                    upperCount++;
                    upperHotCount += relativeDelta >= kHotRelativeDelta ? 1u : 0u;
                }
                else
                {
                    lowerHistogram[histogramIndex]++;
                    lowerCount++;
                    lowerHotCount += relativeDelta >= kHotRelativeDelta ? 1u : 0u;
                }
            }
        }

        float upperP99 = 0.0;
        float lowerP99 = 0.0;
        const uint upperTarget = max((uint)ceil(float(upperCount) * 0.99), 1u);
        const uint lowerTarget = max((uint)ceil(float(lowerCount) * 0.99), 1u);
        uint upperCumulative = 0u;
        uint lowerCumulative = 0u;
        [loop]
        for (int halfBin = 0; halfBin < kHistogramBins; ++halfBin)
        {
            const float binUpper = float(halfBin + 1) * kHistogramMax / float(kHistogramBins);
            upperCumulative += upperHistogram[halfBin];
            lowerCumulative += lowerHistogram[halfBin];
            if (upperP99 == 0.0 && upperCount > 0u && upperCumulative >= upperTarget)
                upperP99 = binUpper;
            if (lowerP99 == 0.0 && lowerCount > 0u && lowerCumulative >= lowerTarget)
                lowerP99 = binUpper;
        }
        return float4(
            upperP99,
            lowerP99,
            upperCount > 0u ? float(upperHotCount) / float(upperCount) : 0.0,
            lowerCount > 0u ? float(lowerHotCount) / float(lowerCount) : 0.0);
    }

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
