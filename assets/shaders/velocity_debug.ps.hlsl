Texture2D uVelocityMap : register(t0);
Texture2D uDepthMap : register(t1);

SamplerState uVelocitySampler : register(s0);
SamplerState uDepthSampler : register(s1);

cbuffer PerPixel : register(b0)
{
    float uVelocityScale;
    float3 _pad;
};

struct PSInput
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
};

float3 HueToRgb(float hue)
{
    float3 rgb = abs(hue * 6.0 - float3(3.0, 2.0, 4.0)) * float3(1.0, -1.0, -1.0) + float3(-1.0, 2.0, 2.0);
    return saturate(rgb);
}

float4 main(PSInput input) : SV_Target
{
    const float depth = uDepthMap.Sample(uDepthSampler, input.texCoord).r;
    if (depth >= 0.9999)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float2 velocity = uVelocityMap.Sample(uVelocitySampler, input.texCoord).rg;
    const float magnitude = length(velocity);
    if (magnitude < 1e-5)
    {
        return float4(0.03, 0.03, 0.03, 1.0);
    }

    const float scaledMagnitude = magnitude * uVelocityScale;
    const float hue = atan2(velocity.y, velocity.x) / (2.0 * 3.14159265) + 0.5;
    const float3 directionColor = HueToRgb(hue);
    const float3 magnitudeColor = saturate(scaledMagnitude).xxx;
    return float4(lerp(directionColor * 0.35, directionColor, magnitudeColor), 1.0);
}
