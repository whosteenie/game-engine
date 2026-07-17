TextureCube uEnvironmentMap : register(t0);
SamplerState uEnvironmentSampler : register(s0);

cbuffer PerPixel : register(b0)
{
    float uRoughness;
    float _pad0;
    float _pad1;
    float _pad2;
};

static const float PI = 3.14159265359;

struct PSInput
{
    float4 position : SV_Position;
    float3 localPos : TEXCOORD0;
};

float DistributionGGX(float3 normal, float3 halfDir, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float normalDotHalf = max(dot(normal, halfDir), 0.0);
    float normalDotHalfSquared = normalDotHalf * normalDotHalf;
    float numerator = alphaSquared;
    float denominator = normalDotHalfSquared * (alphaSquared - 1.0) + 1.0;
    denominator = PI * denominator * denominator;
    return numerator / max(denominator, 0.0001);
}

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

float3 ImportanceSampleGGX(float2 xi, float3 normal, float roughness)
{
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    float3 h;
    h.x = cos(phi) * sinTheta;
    h.y = sin(phi) * sinTheta;
    h.z = cosTheta;

    float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);

    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

float4 main(PSInput input) : SV_Target
{
    float3 normal = normalize(input.localPos);
    float3 reflection = normal;
    float3 viewDirection = reflection;

    float3 prefilteredColor = 0.0.xxx;
    float totalWeight = 0.0;

    const uint sampleCount = 1024u;
    [loop]
    for (uint i = 0u; i < sampleCount; ++i)
    {
        float2 xi = Hammersley(i, sampleCount);
        float3 halfDir = ImportanceSampleGGX(xi, normal, uRoughness);
        float3 sampleDirection = reflect(-viewDirection, halfDir);

        float normalDotSample = max(dot(normal, sampleDirection), 0.0);
        if (normalDotSample > 0.0)
        {
            float d = DistributionGGX(normal, halfDir, uRoughness);
            float normalDotHalf = max(dot(normal, halfDir), 0.0);
            float normalDotView = max(dot(normal, viewDirection), 0.0);
            float pdf = (d * normalDotHalf / max(4.0 * normalDotView * normalDotHalf + 0.0001, 0.0001)) + 0.0001;
            float resolution = 128.0;
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / ((float)sampleCount * pdf + 0.0001);
            float mipLevel = uRoughness == 0.0 ? 0.0 : max(0.5 * log2(saSample / saTexel), 0.0);

            prefilteredColor += uEnvironmentMap.SampleLevel(uEnvironmentSampler, sampleDirection, mipLevel).rgb * normalDotSample;
            totalWeight += normalDotSample;
        }
    }

    prefilteredColor /= totalWeight;
    return float4(prefilteredColor, 1.0);
}
