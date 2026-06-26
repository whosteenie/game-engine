Texture2D uEquirectangularMap : register(t0);
SamplerState uEquirectangularSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uRotationY;
};

static const float2 InvAtan = float2(0.1591, 0.3183);

float3 RotateY(float3 direction, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return float3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}

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
    float3 direction = RotateY(normalize(input.localPos), uRotationY);
    float2 uv = SampleSphericalMap(direction);
    float3 color = uEquirectangularMap.Sample(uEquirectangularSampler, uv).rgb;
    return float4(color, 1.0);
}
