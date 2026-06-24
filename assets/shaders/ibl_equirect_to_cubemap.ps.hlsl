Texture2D uEquirectangularMap : register(t0);
SamplerState uEquirectangularSampler : register(s0);

static const float2 InvAtan = float2(0.1591, 0.3183);

struct PSInput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0;
};

float2 SampleSphericalMap(float3 direction)
{
    float2 uv = float2(atan2(direction.z, direction.x), asin(direction.y));
    uv *= InvAtan;
    uv += 0.5;
    return uv;
}

float4 main(PSInput input) : SV_Target
{
    float2 uv = SampleSphericalMap(normalize(input.localPos));
    float3 color = uEquirectangularMap.Sample(uEquirectangularSampler, uv).rgb;
    return float4(color, 1.0);
}
