#version 330 core
out vec4 FragColor;

in vec3 vFragPos;
in vec3 vNormal;

#define MAX_LIGHTS 8

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
uniform float uSpecularStrength;
uniform vec3 uIndirectBounceDirection;
uniform vec3 uIndirectBounceColor;

vec3 SrgbToLinear(vec3 srgb)
{
    return pow(srgb, vec3(2.2));
}

vec3 LinearToSrgb(vec3 linear)
{
    return pow(max(linear, vec3(0.0)), vec3(1.0 / 2.2));
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

void main()
{
    vec3 norm = normalize(vNormal);
    vec3 viewDir = normalize(uViewPos - vFragPos);

    vec3 albedo = SrgbToLinear(uAlbedo);
    float shininess = mix(8.0, 128.0, 1.0 - clamp(uRoughness, 0.0, 1.0));
    vec3 diffuseColor = albedo * (1.0 - uMetallic);
    vec3 specularColor = mix(vec3(0.04), albedo, uMetallic);

    vec3 skyAmbient = vec3(0.06, 0.06, 0.09);
    vec3 groundAmbient = vec3(0.02, 0.02, 0.025);
    float hemisphere = norm.y * 0.5 + 0.5;
    vec3 ambient = mix(groundAmbient, skyAmbient, hemisphere) * diffuseColor * uAmbientStrength;

    vec3 bounceDir = normalize(uIndirectBounceDirection);
    float bounce = max(dot(norm, bounceDir), 0.0);
    vec3 indirect = bounce * uIndirectStrength * diffuseColor * SrgbToLinear(uIndirectBounceColor);

    vec3 directDiffuse = vec3(0.0);
    vec3 directSpecular = vec3(0.0);

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

        float diff = max(dot(norm, lightDir), 0.0);
        directDiffuse += diff * diffuseColor * radiance * attenuation * spotIntensity;

        vec3 halfDir = normalize(lightDir + viewDir);
        float spec = pow(max(dot(norm, halfDir), 0.0), shininess);
        directSpecular += uSpecularStrength * spec * specularColor * radiance * attenuation * spotIntensity;
    }

    vec3 result = ambient + indirect + directDiffuse + directSpecular;
    FragColor = vec4(LinearToSrgb(result), 1.0);
}
