float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float2 ClipXYToDepthUv(float2 clipXY)
{
    return float2(clipXY.x * 0.5 + 0.5, (1.0 - clipXY.y) * 0.5);
}

float3 ReconstructViewPos(Texture2D depthMap, SamplerState depthSampler, float4x4 invProjection, float2 texCoord)
{
    const float depth = depthMap.Sample(depthSampler, texCoord).r;
    const float2 clipXY = DepthUvToClipXY(texCoord);
    const float4 viewSpace = mul(invProjection, float4(clipXY, depth, 1.0));
    return viewSpace.xyz / viewSpace.w;
}

float ViewDepthAt(Texture2D depthMap, SamplerState depthSampler, float4x4 invProjection, float2 texCoord)
{
    return ReconstructViewPos(depthMap, depthSampler, invProjection, texCoord).z;
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}
