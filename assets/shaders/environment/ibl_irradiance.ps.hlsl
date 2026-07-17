TextureCube uEnvironmentMap : register(t0);
SamplerState uEnvironmentSampler : register(s0);

static const float PI = 3.14159265359;

struct PSInput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0;
};

float RadicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10;
}

float2 Hammersley(uint i, uint N)
{
    return float2((float)i / (float)N, RadicalInverseVdC(i));
}

float3 BuildTangentBasis(float3 normal, out float3 right, out float3 up)
{
    up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    right = normalize(cross(up, normal));
    up = cross(normal, right);
    return normal;
}

float3 SampleCosineHemisphere(float2 xi, float3 normal, float3 right, float3 up)
{
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt(1.0 - xi.y);
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float3 tangentSample = float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return tangentSample.x * right + tangentSample.y * up + tangentSample.z * normal;
}

float4 main(PSInput input) : SV_Target
{
    float3 normal = normalize(input.localPos);
    float3 right;
    float3 up;
    BuildTangentBasis(normal, right, up);

    float3 irradiance = 0.0.xxx;
    const uint sampleCount = 512u;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)
    {
        float2 xi = Hammersley(sampleIndex, sampleCount);
        float3 sampleDir = SampleCosineHemisphere(xi, normal, right, up);
        irradiance += uEnvironmentMap.Sample(uEnvironmentSampler, sampleDir).rgb;
    }

    // Cosine-weighted hemisphere integral: E = pi * average(L) for this sampling scheme.
    irradiance = PI * irradiance / (float)sampleCount;
    return float4(irradiance, 1.0);
}
