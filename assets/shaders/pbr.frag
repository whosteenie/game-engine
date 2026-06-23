#version 330 core
layout (location = 0) out vec4 oDirect;
layout (location = 1) out vec4 oIndirect;
layout (location = 2) out vec4 oNormal;

in vec3 vFragPos;
in vec3 vNormal;
in vec2 vTexCoord0;
in vec2 vTexCoord1;
in vec4 vTangent;
in vec4 vFragPosLightSpace;
in float vViewDepth;

#define MAX_LIGHTS 8
#define MAX_CASCADES 4

const float PI = 3.14159265359;

const int LIGHT_TYPE_DIRECTIONAL = 0;
const int LIGHT_TYPE_POINT = 1;
const int LIGHT_TYPE_SPOT = 2;

uniform vec3 uViewPos;
uniform vec3 uAlbedo;
uniform float uRoughness;
uniform float uMetallic;

uniform int uUseAlbedoMap;
uniform int uUseNormalMap;
uniform int uUseAoMap;
uniform int uUseRoughnessMap;
uniform int uUseMetallicRoughnessMap;

uniform int uAlbedoTexCoordSet;
uniform int uNormalTexCoordSet;
uniform int uAoTexCoordSet;
uniform int uRoughnessTexCoordSet;

uniform sampler2D uAlbedoMap;
uniform sampler2D uNormalMap;
uniform sampler2D uAoMap;
uniform sampler2D uRoughnessMap;

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

uniform sampler2DArrayShadow uShadowMap;
uniform mat4 uLightSpaceMatrices[MAX_CASCADES];
uniform float uCascadeEndSplits[MAX_CASCADES];
uniform float uCascadeTexelWorldSizes[MAX_CASCADES];
uniform float uCascadeBlendRatio;
uniform int uCascadeCount;
uniform float uCascadeNearPlane;
uniform mat4 uLightSpaceMatrix;
uniform mat4 uView;
uniform int uShadowLightIndex;
uniform int uReceiveShadow;
uniform int uOutputLinear;
uniform int uSplitLightingOutput;
uniform int uDebugMode;

uniform samplerCube uIrradianceMap;
uniform samplerCube uPrefilterMap;
uniform sampler2D uBrdfLut;
uniform float uMaxReflectionLod;
uniform float uEnvironmentIntensity;

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

vec3 FresnelSchlickRoughness(float cosineTheta, vec3 f0, float roughness)
{
    vec3 maxReflection = max(vec3(1.0 - roughness), f0);
    return f0 + (maxReflection - f0) * pow(clamp(1.0 - cosineTheta, 0.0, 1.0), 5.0);
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

float SampleCascadeShadow(int cascadeIndex, vec3 worldPos, vec3 geomNormal, vec3 lightDir)
{
    vec3 normal = normalize(geomNormal);
    float nDotL = clamp(dot(normal, lightDir), 0.0, 1.0);
    float sinTheta = sqrt(max(1.0 - nDotL * nDotL, 1e-5));

    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMap, 0).xy);
    float texelUvSpan = max(texelSize.x, texelSize.y);
    float texelWorldSpan = uCascadeTexelWorldSizes[cascadeIndex];

    float worldBias = texelWorldSpan * (1.5 + 3.5 * sinTheta / max(nDotL, 0.15));
    float depthBias = texelUvSpan * (1.0 + 2.0 * sinTheta / max(nDotL, 0.15));

    vec3 biasedWorldPos = worldPos + normal * worldBias;
    vec4 lightSpace = uLightSpaceMatrices[cascadeIndex] * vec4(biasedWorldPos, 1.0);
    vec3 projCoords = lightSpace.xyz / lightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.x < 0.0 || projCoords.x > 1.0 ||
        projCoords.y < 0.0 || projCoords.y > 1.0 ||
        projCoords.z > 1.0)
    {
        return 1.0;
    }

    vec2 shadowUv = projCoords.xy;
    float compareDepth = projCoords.z - depthBias;

    float shadow = 0.0;
    for (int x = -2; x <= 2; ++x)
    {
        for (int y = -2; y <= 2; ++y)
        {
            vec2 offset = vec2(x, y) * texelSize;
            shadow += texture(
                uShadowMap,
                vec4(shadowUv + offset, float(cascadeIndex), compareDepth));
        }
    }

    shadow /= 25.0;
    return shadow * shadow * (3.0 - 2.0 * shadow);
}

int SelectCascadeIndex(float viewDepth)
{
    int cascadeIndex = 0;
    for (int i = 0; i < uCascadeCount - 1; ++i)
    {
        if (viewDepth > uCascadeEndSplits[i])
        {
            cascadeIndex = i + 1;
        }
    }
    return cascadeIndex;
}

float CalcShadow(vec3 lightDir)
{
    if (uReceiveShadow == 0)
    {
        return 1.0;
    }

    vec3 geomNormal = normalize(vNormal);

    for (int boundaryIndex = 0; boundaryIndex < uCascadeCount - 1; ++boundaryIndex)
    {
        float splitDistance = uCascadeEndSplits[boundaryIndex];
        float previousSplit = boundaryIndex == 0
            ? uCascadeNearPlane
            : uCascadeEndSplits[boundaryIndex - 1];
        float cascadeSpan = max(splitDistance - previousSplit, 1e-4);
        float blendWidth = cascadeSpan * uCascadeBlendRatio;
        float blendStart = splitDistance - blendWidth;

        if (vViewDepth > blendStart && vViewDepth <= splitDistance)
        {
            float nearShadow = SampleCascadeShadow(boundaryIndex, vFragPos, geomNormal, lightDir);
            float farShadow = SampleCascadeShadow(boundaryIndex + 1, vFragPos, geomNormal, lightDir);
            float blendFactor = smoothstep(blendStart, splitDistance, vViewDepth);
            return mix(nearShadow, farShadow, blendFactor);
        }
    }

    int cascadeIndex = SelectCascadeIndex(vViewDepth);
    return SampleCascadeShadow(cascadeIndex, vFragPos, geomNormal, lightDir);
}

vec2 SelectTexCoord(int texCoordSet)
{
    return texCoordSet == 1 ? vTexCoord1 : vTexCoord0;
}

vec3 CalcNormalFromMap(vec3 normal, vec4 tangent, vec2 texCoord)
{
    vec3 tangentNormal = texture(uNormalMap, texCoord).rgb * 2.0 - 1.0;

    vec3 tangentVector = normalize(tangent.xyz);
    tangentVector = normalize(tangentVector - dot(tangentVector, normal) * normal);
    vec3 bitangent = normalize(cross(normal, tangentVector) * tangent.w);
    mat3 tbn = mat3(tangentVector, bitangent, normal);

    return normalize(tbn * tangentNormal);
}

vec3 ToViewNormal(vec3 worldNormal)
{
    return normalize(mat3(uView) * worldNormal);
}

void WriteViewNormal(vec3 worldNormal)
{
    oNormal = vec4(ToViewNormal(worldNormal), 1.0);
}

void main()
{
    vec3 normal = normalize(vNormal);
    vec2 albedoTexCoord = SelectTexCoord(uAlbedoTexCoordSet);
    vec2 normalTexCoord = SelectTexCoord(uNormalTexCoordSet);
    vec2 aoTexCoord = SelectTexCoord(uAoTexCoordSet);
    vec2 roughnessTexCoord = SelectTexCoord(uRoughnessTexCoordSet);
    if (uUseNormalMap != 0)
    {
        normal = CalcNormalFromMap(normal, vTangent, normalTexCoord);
    }

    vec3 viewDir = normalize(uViewPos - vFragPos);

    vec3 albedo = uAlbedo;
    if (uUseAlbedoMap != 0)
    {
        albedo *= texture(uAlbedoMap, albedoTexCoord).rgb;
    }

    float roughness = uRoughness;
    float metallic = uMetallic;
    if (uUseMetallicRoughnessMap != 0)
    {
        vec3 metallicRoughnessSample = texture(uRoughnessMap, roughnessTexCoord).rgb;
        roughness *= metallicRoughnessSample.g;
        metallic *= metallicRoughnessSample.b;
    }
    else if (uUseRoughnessMap != 0)
    {
        roughness *= texture(uRoughnessMap, roughnessTexCoord).r;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    vec3 f0 = mix(vec3(0.04), albedo, metallic);

    float ambientOcclusion = 1.0;
    if (uUseAoMap != 0)
    {
        ambientOcclusion = texture(uAoMap, aoTexCoord).r;
    }

    vec3 irradiance = texture(uIrradianceMap, normal).rgb;
    vec3 diffuseIbl = irradiance * albedo;

    vec3 reflection = reflect(-viewDir, normal);
    vec3 prefilteredColor = textureLod(uPrefilterMap, reflection, roughness * uMaxReflectionLod).rgb;
    vec2 envBrdf = texture(uBrdfLut, vec2(max(dot(normal, viewDir), 0.0), roughness)).rg;
    vec3 specularIbl = prefilteredColor * (f0 * envBrdf.x + envBrdf.y);

    vec3 specularEnergy = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    vec3 diffuseEnergy = (vec3(1.0) - specularEnergy) * (1.0 - metallic);
    vec3 ambient = (diffuseEnergy * diffuseIbl + specularIbl) * uEnvironmentIntensity * ambientOcclusion;

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
            shadow = CalcShadow(lightDir);
        }

        directLighting += contribution * attenuation * spotIntensity * shadow;
    }

    vec3 result = ambient + directLighting;

    if (uDebugMode != 0)
    {
        vec3 debugColor = result;
        if (uDebugMode == 1)
        {
            float shadow = 1.0;
            if (uShadowLightIndex >= 0)
            {
                shadow = CalcShadow(normalize(uLightDirections[uShadowLightIndex]));
            }
            debugColor = vec3(shadow);
        }
        else if (uDebugMode == 2)
        {
            debugColor = directLighting / (directLighting + vec3(0.25));
        }
        else if (uDebugMode == 3)
        {
            debugColor = ambient / (ambient + vec3(0.25));
        }
        else if (uDebugMode == 4)
        {
            vec3 projCoords = vFragPosLightSpace.xyz / vFragPosLightSpace.w;
            projCoords = projCoords * 0.5 + 0.5;
            debugColor = vec3(projCoords.xy, 0.0);
        }
        else if (uDebugMode == 5)
        {
            vec3 projCoords = vFragPosLightSpace.xyz / vFragPosLightSpace.w;
            projCoords = projCoords * 0.5 + 0.5;
            debugColor = vec3(projCoords.z);
        }
        else if (uDebugMode == 6)
        {
            int cascadeIndex = SelectCascadeIndex(vViewDepth);
            if (cascadeIndex == 0)
            {
                debugColor = vec3(1.0, 0.2, 0.2);
            }
            else if (cascadeIndex == 1)
            {
                debugColor = vec3(0.2, 1.0, 0.2);
            }
            else if (cascadeIndex == 2)
            {
                debugColor = vec3(0.2, 0.35, 1.0);
            }
            else
            {
                debugColor = vec3(1.0, 0.85, 0.2);
            }
        }

        oDirect = vec4(debugColor, 1.0);
        oIndirect = vec4(0.0);
        WriteViewNormal(normal);
        return;
    }

    if (uSplitLightingOutput != 0)
    {
        oDirect = vec4(directLighting, 1.0);
        oIndirect = vec4(ambient, 1.0);
        WriteViewNormal(normal);
        return;
    }

    if (uOutputLinear != 0)
    {
        oDirect = vec4(result, 1.0);
        oIndirect = vec4(0.0);
    }
    else
    {
        oDirect = vec4(LinearToSrgb(result), 1.0);
        oIndirect = vec4(0.0);
    }
    WriteViewNormal(normal);
}
