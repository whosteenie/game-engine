Texture2D uDepthMap : register(t0);
Texture2D uNormalMap : register(t1);
Texture2D uNoiseMap : register(t2);

SamplerState uDepthSampler : register(s0);
SamplerState uNormalSampler : register(s1);
SamplerState uNoiseSampler : register(s2);

cbuffer PerPixel : register(b0)
{
    float4x4 uProjection;
    float4x4 uInvProjection;
    float4 uSamples[32];
    float uRadius;
    float uBias;
    int uKernelSize;
    float uNoiseScaleX;
    float uNoiseScaleY;
    int uUseGeometryNormals;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 ReconstructViewPos(float2 texCoord, float depth)
{
    float4 clipSpace = float4(texCoord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    float4 viewSpace = mul(uInvProjection, clipSpace);
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
        float3 storedNormal = uNormalMap.Sample(uNormalSampler, texCoord).xyz;
        if (dot(storedNormal, storedNormal) > 1e-4)
        {
            return normalize(storedNormal);
        }
    }

    return ReconstructViewNormal(texCoord, depth);
}

float main(PSInput input) : SV_Target
{
    float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 1.0)
    {
        return 1.0;
    }

    float3 fragPos = ReconstructViewPos(input.texCoord, depth);
    float3 normal = SampleViewNormal(input.texCoord, depth);
    float3 randomVec = uNoiseMap.Sample(uNoiseSampler, input.texCoord * float2(uNoiseScaleX, uNoiseScaleY)).xyz;
    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 tbn = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0;
    int validSamples = 0;

    [loop]
    for (int i = 0; i < uKernelSize; ++i)
    {
        float3 sampleOffset = mul(tbn, uSamples[i].xyz);
        float3 samplePos = fragPos + sampleOffset * uRadius;

        float4 offset = mul(uProjection, float4(samplePos, 1.0));
        offset.xyz /= offset.w;
        offset.xyz = offset.xyz * 0.5 + 0.5;

        if (offset.x < 0.0 || offset.x > 1.0 || offset.y < 0.0 || offset.y > 1.0)
        {
            continue;
        }

        float sampleDepth = uDepthMap.Sample(uDepthSampler, offset.xy).r;
        if (sampleDepth >= 1.0)
        {
            continue;
        }

        float3 sampleViewPos = ReconstructViewPos(offset.xy, sampleDepth);
        float dist3D = length(sampleViewPos - samplePos);

        if (dist3D > uRadius)
        {
            continue;
        }

        if (abs(fragPos.z - sampleViewPos.z) > uRadius)
        {
            continue;
        }

        validSamples++;
        if (sampleViewPos.z >= samplePos.z + uBias)
        {
            occlusion += 1.0;
        }
    }

    if (validSamples == 0)
    {
        return 1.0;
    }

    return 1.0 - (occlusion / (float)validSamples);
}
