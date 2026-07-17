// Object-masked path-tracer GI quality signals. Temporal chroma motion measures color crawl;
// geometry-aware local residuals measure bright/color patches without treating normal shading
// gradients or neighboring geometry as defects.

Texture2D<float4> uCurrentRadiance : register(t0);
Texture2D<float4> uPrevRadiance : register(t1);
Texture2D<float4> uMotion : register(t2);
Texture2D<uint2> uMetadata : register(t3);
Texture2D<float4> uNormalRoughness : register(t4);

cbuffer PerPixel : register(b0)
{
    int uPrevFrameValid;
    int uMotionReproject;
    uint uSelectedInstanceIdPlusOne;
    float _Padding0;
    float2 uMotionScale;
    float2 _Padding1;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luminance(float3 color)
{
    return dot(max(color, 0.0.xxx), float3(0.2126, 0.7152, 0.0722));
}

float3 Chromaticity(float3 color)
{
    color = max(color, 0.0.xxx);
    return color / max(color.r + color.g + color.b, 0.05);
}

bool SameSelectedInstance(uint2 pixel)
{
    return uSelectedInstanceIdPlusOne == 0u
        || uMetadata.Load(int3(pixel, 0)).x == uSelectedInstanceIdPlusOne;
}

float4 main(PSInput input) : SV_Target
{
    const uint2 pixel = uint2(input.position.xy);
    uint width, height;
    uCurrentRadiance.GetDimensions(width, height);
    const float4 current = uCurrentRadiance.Load(int3(pixel, 0));
    if (current.a <= 0.0 || !SameSelectedInstance(pixel))
    {
        return float4(-1.0, -1.0, -1.0, 0.0);
    }

    float temporalChromaDelta = -1.0;
    if (uPrevFrameValid != 0)
    {
        int2 previousPixel = int2(pixel);
        bool previousValid = true;
        if (uMotionReproject != 0)
        {
            const float2 currentUv = (float2(pixel) + 0.5) / float2(width, height);
            const float4 motion = uMotion.Load(int3(pixel, 0));
            const float2 previousUv = currentUv + motion.xy * uMotionScale;
            previousValid = all(previousUv >= 0.0.xx) && all(previousUv <= 1.0.xx);
            previousPixel = clamp(
                int2(previousUv * float2(width, height)),
                int2(0, 0),
                int2(width, height) - 1);
            if (previousValid)
            {
                const float4 previous = uPrevRadiance.Load(int3(previousPixel, 0));
                const float expectedPreviousDepth = current.a + motion.z;
                previousValid = previous.a > 0.0 && expectedPreviousDepth > 0.0
                    && abs(previous.a - expectedPreviousDepth)
                        <= 0.02 * max(expectedPreviousDepth, 1.0e-3);
            }
        }
        if (previousValid)
        {
            const float3 previousColor = uPrevRadiance.Load(int3(previousPixel, 0)).rgb;
            temporalChromaDelta = length(Chromaticity(current.rgb) - Chromaticity(previousColor));
        }
    }

    const float3 centerNormal = normalize(uNormalRoughness.Load(int3(pixel, 0)).xyz);
    const float centerLuma = Luminance(current.rgb);
    const int2 offsets[4] = { int2(-4, 0), int2(4, 0), int2(0, -4), int2(0, 4) };
    float localLumaResidual = 0.0;
    float localChromaResidual = 0.0;
    float localCount = 0.0;
    [unroll]
    for (int neighborIndex = 0; neighborIndex < 4; ++neighborIndex)
    {
        const int2 neighborPixel = int2(pixel) + offsets[neighborIndex];
        if (any(neighborPixel < 0) || any(neighborPixel >= int2(width, height))) continue;
        if (!SameSelectedInstance(uint2(neighborPixel))) continue;
        const float4 neighbor = uCurrentRadiance.Load(int3(neighborPixel, 0));
        if (neighbor.a <= 0.0
            || abs(neighbor.a - current.a) > 0.02 * max(current.a, 1.0e-3)) continue;
        const float3 neighborNormal = normalize(
            uNormalRoughness.Load(int3(neighborPixel, 0)).xyz);
        if (dot(centerNormal, neighborNormal) < 0.95) continue;

        localLumaResidual += abs(Luminance(neighbor.rgb) - centerLuma)
            / max(abs(centerLuma), 0.05);
        localChromaResidual += length(Chromaticity(neighbor.rgb) - Chromaticity(current.rgb));
        localCount += 1.0;
    }

    if (localCount <= 0.0)
    {
        return float4(temporalChromaDelta, -1.0, -1.0, 1.0);
    }
    return float4(
        temporalChromaDelta,
        localLumaResidual / localCount,
        localChromaResidual / localCount,
        1.0);
}
