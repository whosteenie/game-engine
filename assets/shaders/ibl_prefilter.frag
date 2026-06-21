#version 330 core
out vec4 FragColor;

in vec3 vLocalPos;

uniform samplerCube uEnvironmentMap;
uniform float uRoughness;

const float PI = 3.14159265359;

float DistributionGGX(vec3 normal, vec3 halfDir, float roughness)
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
    return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint N)
{
    return vec2(float(i) / float(N), RadicalInverseVdC(i));
}

vec3 ImportanceSampleGGX(vec2 xi, vec3 normal, float roughness)
{
    float alpha = roughness * roughness;
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (alpha * alpha - 1.0) * xi.y));
    float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

    vec3 h;
    h.x = cos(phi) * sinTheta;
    h.y = sin(phi) * sinTheta;
    h.z = cosTheta;

    vec3 up = abs(normal.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, normal));
    vec3 bitangent = cross(normal, tangent);

    return normalize(tangent * h.x + bitangent * h.y + normal * h.z);
}

void main()
{
    vec3 normal = normalize(vLocalPos);
    vec3 reflection = normal;
    vec3 viewDirection = reflection;

    vec3 prefilteredColor = vec3(0.0);
    float totalWeight = 0.0;

    const uint sampleCount = 1024u;
    for (uint i = 0u; i < sampleCount; ++i)
    {
        vec2 xi = Hammersley(i, sampleCount);
        vec3 halfDir = ImportanceSampleGGX(xi, normal, uRoughness);
        vec3 sampleDirection = reflect(-viewDirection, halfDir);

        float normalDotSample = max(dot(normal, sampleDirection), 0.0);
        if (normalDotSample > 0.0)
        {
            float d = DistributionGGX(normal, halfDir, uRoughness);
            float normalDotHalf = max(dot(normal, halfDir), 0.0);
            float normalDotView = max(dot(normal, viewDirection), 0.0);
            float pdf = (d * normalDotHalf / max(4.0 * normalDotView * normalDotHalf + 0.0001, 0.0001)) + 0.0001;
            float resolution = 128.0;
            float saTexel = 4.0 * PI / (6.0 * resolution * resolution);
            float saSample = 1.0 / (float(sampleCount) * pdf + 0.0001);
            float mipLevel = uRoughness == 0.0 ? 0.0 : max(0.5 * log2(saSample / saTexel), 0.0);

            prefilteredColor += textureLod(uEnvironmentMap, sampleDirection, mipLevel).rgb * normalDotSample;
            totalWeight += normalDotSample;
        }
    }

    prefilteredColor /= totalWeight;
    FragColor = vec4(prefilteredColor, 1.0);
}
