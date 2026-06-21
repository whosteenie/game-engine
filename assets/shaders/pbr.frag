#version 330 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;
in vec4 vFragPosLightSpace;

#define MAX_LIGHTS 8

const float PI = 3.14159265359;

const int LIGHT_TYPE_DIRECTIONAL = 0;
const int LIGHT_TYPE_POINT = 1;
const int LIGHT_TYPE_SPOT = 2;

uniform vec3 uViewPos;
uniform vec3 uAlbedo;
uniform float uRoughness;
uniform float uMetallic;

uniform int uLightCount;
uniform int uLightTypes[MAX_LIGHTS];
uniform vec3 uLightPositions[MAX_LIGHTS];
uniform vec3 uLightDirections[MAX_LIGHTS];
uniform vec3 uLightColors[MAX_LIGHTS];
uniform float uLightIntensities[MAX_LIGHTS];
uniform float uLightAttenConstant[MAX_LIGHTS];
uniform float uLightAttenLinear[MAX_LIGHTS];
uniform float uLightAttenQuadratic[MAX_LIGHTS];
uniform float uLightRange[MAX_LIGHTS];
uniform float uLightInnerCutoffCos[MAX_LIGHTS];
uniform float uLightOuterCutoffCos[MAX_LIGHTS];

uniform float uAmbientStrength;
uniform float uIndirectStrength;
uniform vec3 uIndirectBounceDirection;
uniform vec3 uIndirectBounceColor;

uniform sampler2D uShadowMap;
uniform int uShadowLightIndex;
uniform int uReceiveShadow;

vec3 SrgbToLinear(vec3 srgb)
{
    return pow(srgb, vec3(2.2));
}

vec3 LinearToSrgb(vec3 linear)
{
    return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2));
}

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

float GeometrySchlickGGX(float normalDotView, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;

    float numerator = normalDotView;
    float denominator = normalDotView * (1.0 - k) + k;

    return numerator / max(denominator, 0.0001);
}

float GeometrySmith(vec3 normal, vec3 viewDir, vec3 lightDir, float roughness)
{
    float normalDotView = max(dot(normal, viewDir), 0.0);
    float normalDotLight = max(dot(normal, lightDir), 0.0);
    float geometryView = GeometrySchlickGGX(normalDotView, roughness);
    float geometryLight = GeometrySchlickGGX(normalDotLight, roughness);
    return geometryView * geometryLight;
}

vec3 FresnelSchlick(float cosineTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosineTheta, 0.0, 1.0), 5.0);
}

float CalcAttenuation(
    float constant,
    float linear,
    float quadratic,
    float range,
    float distance)
{
    float attenuation = 1.0 / (constant + linear * distance + quadratic * distance * distance);

    if (range > 0.0)
    {
        float rangeFactor = clamp(1.0 - pow(distance / range, 4.0), 0.0, 1.0);
        attenuation *= rangeFactor * rangeFactor;
    }

    return attenuation;
}

float CalcSpotIntensity(
    vec3 lightDir,
    vec3 spotDirection,
    float innerCutoffCos,
    float outerCutoffCos)
{
    float theta = dot(lightDir, normalize(spotDirection));
    return clamp((theta - outerCutoffCos) / (innerCutoffCos - outerCutoffCos + 0.0001), 0.0, 1.0);
}

void CalcLightDirectionAndAttenuation(
    int lightType,
    vec3 lightPosition,
    vec3 lightDirection,
    float constant,
    float linear,
    float quadratic,
    float range,
    float innerCutoffCos,
    float outerCutoffCos,
    out vec3 lightDir,
    out float attenuation,
    out float spotIntensity)
{
    if (lightType == LIGHT_TYPE_DIRECTIONAL)
    {
        lightDir = normalize(lightDirection);
        attenuation = 1.0;
        spotIntensity = 1.0;
        return;
    }

    vec3 toLight = lightPosition - vFragPos;
    float distance = length(toLight);
    lightDir = toLight / max(distance, 0.0001);
    attenuation = CalcAttenuation(constant, linear, quadratic, range, distance);
    spotIntensity = 1.0;

    if (lightType == LIGHT_TYPE_SPOT)
    {
        spotIntensity = CalcSpotIntensity(lightDir, lightDirection, innerCutoffCos, outerCutoffCos);
    }
}

vec3 CalcCookTorranceContribution(
    vec3 normal,
    vec3 viewDir,
    vec3 lightDir,
    vec3 albedo,
    float roughness,
    float metallic,
    vec3 radiance)
{
    vec3 halfDir = normalize(viewDir + lightDir);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    float normalDistribution = DistributionGGX(normal, halfDir, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    vec3 fresnel = FresnelSchlick(max(dot(halfDir, viewDir), 0.0), f0);

    vec3 numerator = normalDistribution * geometry * fresnel;
    float denominator = 4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, lightDir), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 specularEnergy = fresnel;
    vec3 diffuseEnergy = (vec3(1.0) - specularEnergy) * (1.0 - metallic);
    float normalDotLight = max(dot(normal, lightDir), 0.0);

    return (diffuseEnergy * albedo / PI + specular) * radiance * normalDotLight;
}

float CalcShadow(vec3 normal, vec3 lightDir)
{
    if (uReceiveShadow == 0)
    {
        return 1.0;
    }

    vec3 projCoords = vFragPosLightSpace.xyz / vFragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
    {
        return 1.0;
    }

    float bias = max(0.0025 * (1.0 - dot(normal, lightDir)), 0.0005);
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0));
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float closestDepth = texture(uShadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += projCoords.z - bias > closestDepth ? 0.0 : 1.0;
        }
    }

    return shadow / 9.0;
}

void main()
{
    vec3 normal = normalize(vNormal);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    vec3 albedo = SrgbToLinear(uAlbedo);
    float roughness = clamp(uRoughness, 0.04, 1.0);
    float metallic = clamp(uMetallic, 0.0, 1.0);

    vec3 skyAmbient = vec3(0.06, 0.06, 0.09);
    vec3 groundAmbient = vec3(0.02, 0.02, 0.025);
    float hemisphere = normal.y * 0.5 + 0.5;
    vec3 ambient = mix(groundAmbient, skyAmbient, hemisphere) * albedo * (1.0 - metallic) * uAmbientStrength;

    vec3 bounceDir = normalize(uIndirectBounceDirection);
    float bounce = max(dot(normal, bounceDir), 0.0);
    vec3 indirect = bounce * uIndirectStrength * albedo * (1.0 - metallic) * SrgbToLinear(uIndirectBounceColor);

    vec3 directLighting = vec3(0.0);

    for (int i = 0; i < uLightCount; ++i)
    {
        vec3 lightDir;
        float attenuation;
        float spotIntensity;

        CalcLightDirectionAndAttenuation(
            uLightTypes[i],
            uLightPositions[i],
            uLightDirections[i],
            uLightAttenConstant[i],
            uLightAttenLinear[i],
            uLightAttenQuadratic[i],
            uLightRange[i],
            uLightInnerCutoffCos[i],
            uLightOuterCutoffCos[i],
            lightDir,
            attenuation,
            spotIntensity);

        vec3 radiance = SrgbToLinear(uLightColors[i]) * uLightIntensities[i];
        vec3 contribution = CalcCookTorranceContribution(
            normal,
            viewDir,
            lightDir,
            albedo,
            roughness,
            metallic,
            radiance);

        float shadow = 1.0;
        if (i == uShadowLightIndex)
        {
            shadow = CalcShadow(normal, lightDir);
        }

        directLighting += contribution * attenuation * spotIntensity * shadow;
    }

    vec3 result = ambient + indirect + directLighting;
    FragColor = vec4(LinearToSrgb(result), 1.0);
}
