TextureCube uEnvironmentMap : register(t0);
SamplerState uEnvironmentSampler : register(s0);

static const float PI = 3.14159265359;

struct PSInput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
{
    float3 normal = normalize(input.localPos);
    float3 irradiance = 0.0.xxx;

    float3 up = float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(up, normal));
    up = normalize(cross(normal, right));

    float sampleDelta = 0.025;
    float sampleCount = 0.0;
    [loop]
    for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
    {
        [loop]
        for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
        {
            float3 tangentSample = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            float3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal;
            irradiance += uEnvironmentMap.Sample(uEnvironmentSampler, sampleVec).rgb * cos(theta) * sin(theta);
            sampleCount += 1.0;
        }
    }

    irradiance = PI * irradiance * (1.0 / sampleCount);
    return float4(irradiance, 1.0);
}
