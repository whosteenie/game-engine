Texture2D uDepthMap : register(t0);
Texture2D uNormalMap : register(t1);
Texture2D uNoiseMap : register(t2);

SamplerState uDepthSampler : register(s0);
SamplerState uNormalSampler : register(s1);
SamplerState uNoiseSampler : register(s2);

#define SSAO_KERNEL_SIZE 32

// 0 = AO output, 1 = usedSamples/kernel, 2 = occluded/used, 3 = view-depth delta heatmap,
// 4 = force 0.5, 5 = view-Z preview, 6 = projection vs buffer depth error
cbuffer PerPixel : register(b0)
{
    float4x4 uProjection;
    float4x4 uInvProjection;
    float4x4 uView;
    float4 uSamples[SSAO_KERNEL_SIZE];
    float uRadius;
    float uBias;
    float uNearPlane;
    float uFarPlane;
    int uKernelSize;
    float uNoiseScaleX;
    float uNoiseScaleY;
    int uUseGeometryNormals;
    int uDebugMode;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float2 DepthUvToClipXY(float2 texCoord)
{
    return float2(texCoord.x * 2.0 - 1.0, 1.0 - texCoord.y * 2.0);
}

float2 ClipXYToDepthUv(float2 clipXY)
{
    return float2(clipXY.x * 0.5 + 0.5, (1.0 - clipXY.y) * 0.5);
}

float ViewDepthAt(float2 texCoord, float depth)
{
    float2 clipXY = DepthUvToClipXY(texCoord);
    float4 viewSpace = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewSpace.z / viewSpace.w;
}

float3 ReconstructViewPos(float2 texCoord, float depth)
{
    float2 clipXY = DepthUvToClipXY(texCoord);
    float4 viewSpace = mul(uInvProjection, float4(clipXY, depth, 1.0));
    return viewSpace.xyz / viewSpace.w;
}

float3 ReconstructViewNormal(float2 texCoord, float depth)
{
    uint width;
    uint height;
    uDepthMap.GetDimensions(width, height);
    float2 texelSize = 1.0 / float2(width, height);
    float3 posCenter = ReconstructViewPos(texCoord, depth);
    float3 posX = ReconstructViewPos(
        texCoord + float2(texelSize.x, 0.0),
        uDepthMap.Sample(uDepthSampler, texCoord + float2(texelSize.x, 0.0)).r);
    float3 posY = ReconstructViewPos(
        texCoord + float2(0.0, texelSize.y),
        uDepthMap.Sample(uDepthSampler, texCoord + float2(0.0, texelSize.y)).r);
    float3 normal = normalize(cross(posX - posCenter, posY - posCenter));
    float3 viewDir = normalize(-posCenter);
    return faceforward(normal, viewDir, normal);
}

float3 SampleViewNormal(float2 texCoord, float depth)
{
    if (uUseGeometryNormals != 0)
    {
        float3 worldNormal = uNormalMap.Sample(uNormalSampler, texCoord).xyz;
        if (dot(worldNormal, worldNormal) > 1e-4)
        {
            return normalize(mul((float3x3)uView, normalize(worldNormal)));
        }
    }

    return ReconstructViewNormal(texCoord, depth);
}

float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float3x3 BuildRotatedTbn(float3 normal, float2 screenPixel)
{
    // Per-pixel interleaved gradient noise avoids the visible 4x4 tiling from a small noise texture.
    const float angle = 6.28318530718 * InterleavedGradientNoise(screenPixel + 0.5);
    float3 randomVector = float3(cos(angle), sin(angle), 0.0);

    float3 tangent = randomVector - normal * dot(randomVector, normal);
    if (dot(tangent, tangent) < 1e-5)
    {
        const float3 helper = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
        tangent = cross(helper, normal);
    }
    tangent = normalize(tangent);
    float3 bitangent = cross(normal, tangent);

    return float3x3(tangent, bitangent, normal);
}

float main(PSInput input) : SV_Target
{
    if (uDebugMode == 4)
    {
        return 0.5;
    }

    float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 1.0)
    {
        return 1.0;
    }

    float3 fragPos = ReconstructViewPos(input.texCoord, depth);
    float centerViewZ = ViewDepthAt(input.texCoord, depth);

    if (uDebugMode == 5)
    {
        return saturate(centerViewZ / max(uFarPlane * 0.25, uNearPlane));
    }

    if (uDebugMode == 6)
    {
        float4 fragClip = mul(uProjection, float4(fragPos, 1.0));
        fragClip.xyz /= fragClip.w;
        return saturate(abs(fragClip.z - depth) * 200.0);
    }

    float3 normal = SampleViewNormal(input.texCoord, depth);
    float3x3 tbn = BuildRotatedTbn(normal, input.position.xy);

    const float radius = max(uRadius, 1e-4);
    const float bias = max(uBias, 0.0);
    const float thicknessLimit = max(radius * 1.05, bias * 6.0);
    const int kernelSize = clamp(uKernelSize, 1, SSAO_KERNEL_SIZE);

    float occlusion = 0.0;
    float deltaSum = 0.0;
    int usedSamples = 0;
    int occludedSamples = 0;

    [loop]
    for (int i = 0; i < SSAO_KERNEL_SIZE; ++i)
    {
        if (i >= kernelSize)
        {
            break;
        }

        float3 sampleOffset = mul(uSamples[i].xyz, tbn);
        if (dot(sampleOffset, normal) < 0.0)
        {
            sampleOffset = -sampleOffset;
        }

        float3 samplePos = fragPos + sampleOffset * radius;

        float4 sampleClip = mul(uProjection, float4(samplePos, 1.0));
        if (sampleClip.w <= 1e-6)
        {
            continue;
        }

        sampleClip.xyz /= sampleClip.w;
        float2 sampleUv = ClipXYToDepthUv(sampleClip.xy);
        if (sampleUv.x < 0.0 || sampleUv.x > 1.0 || sampleUv.y < 0.0 || sampleUv.y > 1.0)
        {
            continue;
        }

        float sceneDepth = uDepthMap.Sample(uDepthSampler, sampleUv).r;
        if (sceneDepth >= 1.0)
        {
            continue;
        }

        // LH view space (+Z forward): smaller Z is closer. Projection is only used for sampleUv.
        float sceneViewZ = ViewDepthAt(sampleUv, sceneDepth);
        const float viewDelta = samplePos.z - sceneViewZ;
        const float centerDistance = abs(centerViewZ - sceneViewZ);
        const float rangeCheck = 1.0 - smoothstep(radius * 0.2, radius, centerDistance);
        const float thicknessWeight = 1.0 - smoothstep(bias, thicknessLimit, viewDelta);
        const bool occluded = viewDelta > bias && viewDelta < thicknessLimit;

        usedSamples++;
        deltaSum += viewDelta;
        if (occluded)
        {
            occludedSamples++;
            occlusion += rangeCheck * thicknessWeight;
        }
    }

    if (uDebugMode == 1)
    {
        return saturate((float)usedSamples / (float)kernelSize);
    }

    if (uDebugMode == 2)
    {
        return saturate((float)occludedSamples / (float)max(usedSamples, 1));
    }

    if (uDebugMode == 3)
    {
        const float avgDelta = deltaSum / (float)max(usedSamples, 1);
        return saturate(avgDelta * 2.0);
    }

    if (usedSamples == 0)
    {
        return 1.0;
    }

    const float visibility = 1.0 - saturate(occlusion / (float)usedSamples);
    return visibility;
}
