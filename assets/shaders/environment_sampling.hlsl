#ifndef ENVIRONMENT_SAMPLING_HLSL
#define ENVIRONMENT_SAMPLING_HLSL

static const float2 kInvAtan = float2(0.15915494309, 0.31830988618);

float3 RotateEnvironmentY(float3 direction, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return float3(c * direction.x + s * direction.z, direction.y, -s * direction.x + c * direction.z);
}

float2 DirectionToEquirectUv(float3 direction)
{
    float2 uv = float2(atan2(direction.z, direction.x), asin(clamp(direction.y, -1.0, 1.0)));
    uv *= kInvAtan;
    uv += 0.5;
    return uv;
}

float3 SampleEquirectEnvironment(
    Texture2D equirectMap,
    SamplerState equirectSampler,
    float3 worldDirection,
    float rotationY,
    float exposure)
{
    float3 direction = RotateEnvironmentY(normalize(worldDirection), rotationY);
    float2 uv = DirectionToEquirectUv(direction);
    return equirectMap.Sample(equirectSampler, uv).rgb * exposure;
}

#endif
