Texture2D uHdrColor : register(t0);
Texture2D uMaterial0 : register(t1);
Texture2D uMaterial1 : register(t2);

SamplerState uHdrColorSampler : register(s0);
SamplerState uMaterial0Sampler : register(s1);
SamplerState uMaterial1Sampler : register(s2);

cbuffer PerPixel : register(b0)
{
    float uThreshold;
    float uSoftKnee;
    float uExposure;
    float uFullTexelSizeX;
    float uFullTexelSizeY;
    float uUseMaterialGbuffer;
    float _pad0;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 SampleHdr(float2 uv)
{
    return uHdrColor.Sample(uHdrColorSampler, uv).rgb;
}

float3 KarisGroupAverage(float2 uv, float2 o0, float2 o1, float2 o2, float2 o3)
{
    const float3 s0 = SampleHdr(uv + o0);
    const float3 s1 = SampleHdr(uv + o1);
    const float3 s2 = SampleHdr(uv + o2);
    const float3 s3 = SampleHdr(uv + o3);
    const float w0 = 1.0 / (1.0 + Luminance(s0));
    const float w1 = 1.0 / (1.0 + Luminance(s1));
    const float w2 = 1.0 / (1.0 + Luminance(s2));
    const float w3 = 1.0 / (1.0 + Luminance(s3));
    const float weightSum = w0 + w1 + w2 + w3;
    return (s0 * w0 + s1 * w1 + s2 * w2 + s3 * w3) / max(weightSum, 1e-5);
}

float3 KarisDownsample13(float2 uv, float2 fullTexelSize)
{
    const float2 dx = float2(fullTexelSize.x * 2.0, 0.0);
    const float2 dy = float2(0.0, fullTexelSize.y * 2.0);
    const float3 a = KarisGroupAverage(uv, -dx - dy, -dy, dx - dy, -dx);
    const float3 b = KarisGroupAverage(uv, -dx, float2(0.0, 0.0), dx, dy - dx);
    const float3 c = KarisGroupAverage(uv, dx - dy, dy, dx + dy, -dy);
    const float3 d = KarisGroupAverage(uv, float2(0.0, 0.0), dx, dy, dx + dy);
    return (a + b + c + d) * 0.25;
}

float SpecularBloomAttenuation(float roughness, float metallic)
{
    const float smoothMetal = saturate(metallic) * (1.0 - saturate(roughness * 2.5));
    return lerp(1.0, max(roughness, 0.2), smoothMetal);
}

float3 ExtractBrightColor(float3 color, float threshold, float knee)
{
    const float brightness = max(color.r, max(color.g, color.b));
    const float kneeRange = max(threshold * knee, 1e-4);
    float soft = brightness - threshold + kneeRange;
    soft = clamp(soft, 0.0, 2.0 * kneeRange);
    soft = (soft * soft) / (4.0 * kneeRange + 0.00001);
    float contribution = max(soft, brightness - threshold);
    contribution /= max(brightness, 0.00001);
    return color * contribution;
}

float4 main(PSInput input) : SV_Target
{
    const float2 fullTexelSize = float2(uFullTexelSizeX, uFullTexelSizeY);
    const float2 uv = input.texCoord;

    float3 hdr = KarisDownsample13(uv, fullTexelSize) * exp2(uExposure);

    if (uUseMaterialGbuffer > 0.5)
    {
        const float4 material0 = uMaterial0.Sample(uMaterial0Sampler, uv);
        const float4 material1 = uMaterial1.Sample(uMaterial1Sampler, uv);
        const float roughness = material0.a;
        const float metallic = material1.r;
        const float3 emissive = material1.gba;
        hdr *= SpecularBloomAttenuation(roughness, metallic);
        hdr = max(hdr, emissive * exp2(uExposure));
    }

    return float4(ExtractBrightColor(hdr, uThreshold, uSoftKnee), 1.0);
}
