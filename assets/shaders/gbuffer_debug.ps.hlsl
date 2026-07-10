Texture2D uMaterial0Map : register(t0);
Texture2D uMaterial1Map : register(t1);
Texture2D uDepthMap : register(t2);

SamplerState uMaterial0Sampler : register(s0);
SamplerState uMaterial1Sampler : register(s1);
SamplerState uDepthSampler : register(s2);

cbuffer PerPixel : register(b0)
{
    int uGBufferDebugMode;
    float3 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

// uGBufferDebugMode: 0 = albedo rgb, 1 = roughness, 2 = metallic, 3 = emissive rgb
float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 material0 = uMaterial0Map.Sample(uMaterial0Sampler, input.texCoord);
    const float4 material1 = uMaterial1Map.Sample(uMaterial1Sampler, input.texCoord);

    if (uGBufferDebugMode == 0)
    {
        return float4(material0.rgb, 1.0);
    }
    if (uGBufferDebugMode == 1)
    {
        const float roughness = material0.a;
        return float4(roughness, roughness, roughness, 1.0);
    }
    if (uGBufferDebugMode == 2)
    {
        const float metallic = material1.r;
        return float4(metallic, metallic, metallic, 1.0);
    }

    return float4(material1.gba, 1.0);
}
