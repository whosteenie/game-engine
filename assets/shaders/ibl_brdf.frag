#version 330 core
layout (location = 0) out vec2 FragColor;

in vec2 vTexCoord;

const float PI = 3.14159265359;

float GeometrySchlickGGX(float normalDotView, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;
    return normalDotView / (normalDotView * (1.0 - k) + k + 0.0001);
}

float GeometrySmith(float normalDotView, float normalDotLight, float roughness)
{
    return GeometrySchlickGGX(normalDotView, roughness) * GeometrySchlickGGX(normalDotLight, roughness);
}

vec2 IntegrateBRDF(float normalDotView, float roughness)
{
    vec3 viewDirection;
    viewDirection.x = sqrt(1.0 - normalDotView * normalDotView);
    viewDirection.y = 0.0;
    viewDirection.z = normalDotView;

    float a = 0.0;
    float b = 0.0;
    vec3 normal = vec3(0.0, 0.0, 1.0);

    const uint sampleCount = 1024u;
    for (uint i = 0u; i < sampleCount; ++i)
    {
        float xi1 = float(i) / float(sampleCount);
        float xi2 = fract(sin(float(i) * 12.9898) * 43758.5453);

        float phi = 2.0 * PI * xi1;
        float cosTheta = sqrt((1.0 - xi2) / (1.0 + (roughness * roughness - 1.0) * xi2));
        float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

        vec3 halfDir = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
        vec3 lightDirection = normalize(2.0 * dot(viewDirection, halfDir) * halfDir - viewDirection);

        float normalDotLight = max(lightDirection.z, 0.0);
        float normalDotHalf = max(halfDir.z, 0.0);
        float viewDotHalf = max(dot(viewDirection, halfDir), 0.0);

        if (normalDotLight > 0.0)
        {
            float geometry = GeometrySmith(normalDotView, normalDotLight, roughness);
            float fresnel = pow(1.0 - viewDotHalf, 5.0);
            float visible = geometry * viewDotHalf / (normalDotHalf * normalDotView + 0.0001);
            a += (1.0 - fresnel) * visible;
            b += fresnel * visible;
        }
    }

    return vec2(a, b) / float(sampleCount);
}

void main()
{
    vec2 integratedBrdf = IntegrateBRDF(vTexCoord.x, vTexCoord.y);
    FragColor = integratedBrdf;
}
