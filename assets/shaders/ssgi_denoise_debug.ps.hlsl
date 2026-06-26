Texture2D uRadianceMap : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uRadianceSampler : register(s0);
SamplerState uDepthSampler : register(s1);

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 radiance = uRadianceMap.Sample(uRadianceSampler, input.texCoord);
    return float4(max(radiance.rgb, 0.0.xxx), 1.0);
}
