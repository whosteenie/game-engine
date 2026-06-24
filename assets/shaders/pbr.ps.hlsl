#define MAX_LIGHTS 8
#define MAX_CASCADES 4

static const float PI = 3.14159265359;

static const int LIGHT_TYPE_DIRECTIONAL = 0;
static const int LIGHT_TYPE_POINT = 1;
static const int LIGHT_TYPE_SPOT = 2;

TextureCube uIrradianceMap : register(t1);
TextureCube uPrefilterMap : register(t2);
Texture2D uBrdfLut : register(t3);
Texture2D uAlbedoMap : register(t4);
Texture2D uNormalMap : register(t5);
Texture2D uAoMap : register(t6);
Texture2D uRoughnessMap : register(t7);
Texture2DArray uShadowMap : register(t8);

SamplerState uIrradianceSampler : register(s1);
SamplerState uPrefilterSampler : register(s2);
SamplerState uBrdfLutSampler : register(s3);
SamplerState uAlbedoSampler : register(s4);
SamplerState uNormalSampler : register(s5);
SamplerState uAoSampler : register(s6);
SamplerState uRoughnessSampler : register(s7);
SamplerState uShadowMapSampler : register(s8);

cbuffer PerPixel : register(b0)
{
    float3 uViewPos;
    float _pad0;
    float3 uAlbedo;
    float uRoughness;
    float uMetallic;
    int uUseAlbedoMap;
    int uUseNormalMap;
    int uUseAoMap;
    int uUseRoughnessMap;
    int uUseMetallicRoughnessMap;
    int uAlbedoTexCoordSet;
    int uNormalTexCoordSet;
    int uAoTexCoordSet;
    int uRoughnessTexCoordSet;
    int uLightCount;
    int uLightTypes[MAX_LIGHTS];
    float4 uLightPositions[MAX_LIGHTS];
    float4 uLightDirections[MAX_LIGHTS];
    float4 uLightColors[MAX_LIGHTS];
    float uLightIntensities[MAX_LIGHTS];
    float uLightAttenConstant[MAX_LIGHTS];
    float uLightAttenLinear[MAX_LIGHTS];
    float uLightAttenQuadratic[MAX_LIGHTS];
    float uLightRange[MAX_LIGHTS];
    float uLightInnerCutoffCos[MAX_LIGHTS];
    float uLightOuterCutoffCos[MAX_LIGHTS];
    float4x4 uLightSpaceMatrices[MAX_CASCADES];
    float uCascadeEndSplits[MAX_CASCADES];
    float uCascadeTexelWorldSizes[MAX_CASCADES];
    float uCascadeBlendRatio;
    int uCascadeCount;
    float uCascadeNearPlane;
    int uShadowFilterMode;
    int uPcfKernelRadius;
    int uUsePoissonPcf;
    float uMinPenumbraTexels;
    int uPcssBlockerRadius;
    float uPcssLightAngularSize;
    float uPcssMinPenumbraTexels;
    float uPcssMaxPenumbraTexels;
    float uWorldBiasScale;
    float uDepthBiasScale;
    float uShadowMapResolution;
    float4x4 uLightSpaceMatrix;
    float4x4 uView;
    int uShadowLightIndex;
    int uReceiveShadow;
    int uOutputLinear;
    int uSplitLightingOutput;
    int uDebugMode;
    float uMaxReflectionLod;
    float uEnvironmentIntensity;
    float _pad1;
    float _pad2;
};

struct PSInput
{
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord0 : TEXCOORD2;
    float2 texCoord1 : TEXCOORD3;
    float4 tangent : TEXCOORD4;
    float4 fragPosLightSpace : TEXCOORD5;
    float viewDepth : TEXCOORD6;
};

struct PSOutput
{
    float4 oDirect : SV_Target0;
    float4 oIndirect : SV_Target1;
    float4 oNormal : SV_Target2;
    float oShadowFactor : SV_Target3;
};

float3 SrgbToLinear(float3 srgb)
{
    return pow(srgb, 2.2.xxx);
}

float3 LinearToSrgb(float3 linearColor)
{
    return pow(max(linearColor, 0.0.xxx), 1.0.xxx / 2.2);
}

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

float GeometrySchlickGGX(float normalDotView, float roughness)
{
    float remappedRoughness = roughness + 1.0;
    float k = (remappedRoughness * remappedRoughness) / 8.0;

    float numerator = normalDotView;
    float denominator = normalDotView * (1.0 - k) + k;

    return numerator / max(denominator, 0.0001);
}

float GeometrySmith(float3 normal, float3 viewDir, float3 lightDir, float roughness)
{
    float normalDotView = max(dot(normal, viewDir), 0.0);
    float normalDotLight = max(dot(normal, lightDir), 0.0);
    float geometryView = GeometrySchlickGGX(normalDotView, roughness);
    float geometryLight = GeometrySchlickGGX(normalDotLight, roughness);
    return geometryView * geometryLight;
}

float3 FresnelSchlick(float cosineTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosineTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosineTheta, float3 f0, float roughness)
{
    float3 maxReflection = max(1.0.xxx - roughness, f0);
    return f0 + (maxReflection - f0) * pow(saturate(1.0 - cosineTheta), 5.0);
}

float CalcAttenuation(
    float constant,
    float linearAttenuation,
    float quadratic,
    float lightRange,
    float distance)
{
    float attenuation = 1.0 / (constant + linearAttenuation * distance + quadratic * distance * distance);

    if (lightRange > 0.0)
    {
        float rangeFactor = saturate(1.0 - pow(distance / lightRange, 4.0));
        attenuation *= rangeFactor * rangeFactor;
    }

    return attenuation;
}

float CalcSpotIntensity(
    float3 lightDir,
    float3 spotDirection,
    float innerCutoffCos,
    float outerCutoffCos)
{
    float theta = dot(lightDir, normalize(spotDirection));
    return saturate((theta - outerCutoffCos) / (innerCutoffCos - outerCutoffCos + 0.0001));
}

void CalcLightDirectionAndAttenuation(
    int lightType,
    float3 lightPosition,
    float3 lightDirection,
    float constant,
    float linearAttenuation,
    float quadratic,
    float lightRange,
    float innerCutoffCos,
    float outerCutoffCos,
    float3 fragPos,
    out float3 lightDir,
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

    float3 toLight = lightPosition - fragPos;
    float distance = length(toLight);
    lightDir = toLight / max(distance, 0.0001);
    attenuation = CalcAttenuation(constant, linearAttenuation, quadratic, lightRange, distance);
    spotIntensity = 1.0;

    if (lightType == LIGHT_TYPE_SPOT)
    {
        spotIntensity = CalcSpotIntensity(lightDir, lightDirection, innerCutoffCos, outerCutoffCos);
    }
}

float3 CalcCookTorranceContribution(
    float3 normal,
    float3 viewDir,
    float3 lightDir,
    float3 albedo,
    float roughness,
    float metallic,
    float3 radiance)
{
    float3 halfDir = normalize(viewDir + lightDir);

    float3 f0 = lerp(0.04.xxx, albedo, metallic);

    float normalDistribution = DistributionGGX(normal, halfDir, roughness);
    float geometry = GeometrySmith(normal, viewDir, lightDir, roughness);
    float3 fresnel = FresnelSchlick(max(dot(halfDir, viewDir), 0.0), f0);

    float3 numerator = normalDistribution * geometry * fresnel;
    float denominator = 4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, lightDir), 0.0) + 0.0001;
    float3 specular = numerator / denominator;

    float3 specularEnergy = fresnel;
    float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - metallic);
    float normalDotLight = max(dot(normal, lightDir), 0.0);

    return (diffuseEnergy * albedo / PI + specular) * radiance * normalDotLight;
}

float FetchShadowDepth(int cascadeIndex, float2 shadowUv)
{
    return uShadowMap.Sample(uShadowMapSampler, float3(shadowUv, (float)cascadeIndex)).r;
}

bool IsShadowBlocked(int cascadeIndex, float2 shadowUv, float compareDepth)
{
    return FetchShadowDepth(cascadeIndex, shadowUv) < compareDepth;
}

float ShadowFilterRotation(int cascadeIndex)
{
    return (float)cascadeIndex * 0.78539816339 + 0.39269908169;
}

static const int kMaxRotatedGridRadius = 4;

float EffectiveMinPenumbraTexels()
{
    return clamp(uMinPenumbraTexels, 0.0, 8.0);
}

float ClampFilterRadiusTexels(float filterRadiusTexels)
{
    return clamp(filterRadiusTexels, 0.5, 12.0);
}

float FilterShadowRotatedGrid(
    int cascadeIndex,
    float2 shadowUv,
    float compareDepth,
    float2 texelSize,
    float filterRadiusTexels,
    int kernelRadius)
{
    int radius = min(kernelRadius, kMaxRotatedGridRadius);
    float phi = ShadowFilterRotation(cascadeIndex);
    float cosine = cos(phi);
    float sine = sin(phi);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    float2 texelStep = texelSize * (filterRadiusTexels / max((float)radius, 1.0));
    float radiusSquared = max((float)(radius * radius), 1.0);

    float shadow = 0.0;
    float weightSum = 0.0;
    [loop]
    for (int x = -kMaxRotatedGridRadius; x <= kMaxRotatedGridRadius; ++x)
    {
        [loop]
        for (int y = -kMaxRotatedGridRadius; y <= kMaxRotatedGridRadius; ++y)
        {
            if (abs(x) > radius || abs(y) > radius)
            {
                continue;
            }

            float distSquared = (float)(x * x + y * y);
            float weight = exp(-distSquared / (radiusSquared * 0.45));
            float2 offset = mul(rotation, float2((float)x, (float)y)) * texelStep;
            shadow += (IsShadowBlocked(cascadeIndex, shadowUv + offset, compareDepth) ? 0.0 : 1.0) * weight;
            weightSum += weight;
        }
    }

    return shadow / max(weightSum, 1e-5);
}

float FilterShadowGridPcf(int cascadeIndex, float2 shadowUv, float compareDepth, float2 texelSize, int kernelRadius)
{
    float shadow = 0.0;
    float sampleCount = 0.0;
    [loop]
    for (int x = -kernelRadius; x <= kernelRadius; ++x)
    {
        [loop]
        for (int y = -kernelRadius; y <= kernelRadius; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            shadow += IsShadowBlocked(cascadeIndex, shadowUv + offset, compareDepth) ? 0.0 : 1.0;
            sampleCount += 1.0;
        }
    }

    return shadow / max(sampleCount, 1.0);
}

float FilterShadowPcf(int cascadeIndex, float2 shadowUv, float compareDepth, float2 texelSize, int kernelRadius)
{
    if (uUsePoissonPcf != 0)
    {
        float filterRadiusTexels = ClampFilterRadiusTexels(
            max((float)kernelRadius, EffectiveMinPenumbraTexels()));
        return FilterShadowRotatedGrid(
            cascadeIndex, shadowUv, compareDepth, texelSize, filterRadiusTexels, kernelRadius);
    }

    return FilterShadowGridPcf(cascadeIndex, shadowUv, compareDepth, texelSize, kernelRadius);
}

float FilterShadowPcss(int cascadeIndex, float2 shadowUv, float compareDepth, float2 texelSize)
{
    float blockerDepthSum = 0.0;
    int blockerCount = 0;

    [loop]
    for (int x = -uPcssBlockerRadius; x <= uPcssBlockerRadius; ++x)
    {
        [loop]
        for (int y = -uPcssBlockerRadius; y <= uPcssBlockerRadius; ++y)
        {
            float2 offset = float2((float)x, (float)y) * texelSize;
            float blockerDepth = FetchShadowDepth(cascadeIndex, shadowUv + offset);
            if (blockerDepth < compareDepth)
            {
                blockerDepthSum += blockerDepth;
                blockerCount++;
            }
        }
    }

    if (blockerCount == 0)
    {
        return FilterShadowPcf(cascadeIndex, shadowUv, compareDepth, texelSize, uPcfKernelRadius);
    }

    float avgBlockerDepth = blockerDepthSum / (float)blockerCount;
    float penumbraRatio = (compareDepth - avgBlockerDepth) / max(avgBlockerDepth, 1e-5);
    float filterRadius = penumbraRatio * uPcssLightAngularSize;
    filterRadius = clamp(
        filterRadius,
        max(uPcssMinPenumbraTexels, EffectiveMinPenumbraTexels()),
        uPcssMaxPenumbraTexels);
    filterRadius = ClampFilterRadiusTexels(filterRadius);
    filterRadius = max(EffectiveMinPenumbraTexels(), ceil(filterRadius));

    if (uUsePoissonPcf != 0)
    {
        int kernelRadius = min((int)ceil(filterRadius), kMaxRotatedGridRadius);
        return FilterShadowRotatedGrid(
            cascadeIndex, shadowUv, compareDepth, texelSize, filterRadius, kernelRadius);
    }

    int kernelRadius = (int)ceil(filterRadius);
    kernelRadius = min(kernelRadius, kMaxRotatedGridRadius);
    return FilterShadowGridPcf(cascadeIndex, shadowUv, compareDepth, texelSize, kernelRadius);
}

float SampleCascadeShadow(int cascadeIndex, float3 worldPos, float3 geomNormal, float3 lightDir)
{
    float3 normal = normalize(geomNormal);
    float nDotL = saturate(dot(normal, lightDir));
    float sinTheta = sqrt(max(1.0 - nDotL * nDotL, 1e-5));

    float2 texelSize = 1.0 / float2(uShadowMapResolution, uShadowMapResolution);
    float texelUvSpan = max(texelSize.x, texelSize.y);
    float texelWorldSpan = uCascadeTexelWorldSizes[cascadeIndex];

    float worldBias = texelWorldSpan * (1.5 + 3.5 * sinTheta / max(nDotL, 0.15)) * uWorldBiasScale;
    float depthBias = texelUvSpan * (1.0 + 2.0 * sinTheta / max(nDotL, 0.15)) * uDepthBiasScale;

    float3 biasedWorldPos = worldPos + normal * worldBias;
    float4 lightSpace = mul(uLightSpaceMatrices[cascadeIndex], float4(biasedWorldPos, 1.0));
    float3 projCoords = lightSpace.xyz / lightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    if (projCoords.z < 0.0 || projCoords.z > 1.0)
    {
        return 1.0;
    }

    float2 shadowUv = clamp(projCoords.xy, 0.0.xx, 1.0.xx);
    float compareDepth = clamp(projCoords.z - depthBias, 0.0, 1.0);

    float shadow = uShadowFilterMode == 1
        ? FilterShadowPcss(cascadeIndex, shadowUv, compareDepth, texelSize)
        : FilterShadowPcf(cascadeIndex, shadowUv, compareDepth, texelSize, uPcfKernelRadius);

    return shadow;
}

int SelectCascadeIndex(float viewDepth)
{
    int cascadeIndex = 0;
    [loop]
    for (int i = 0; i < uCascadeCount - 1; ++i)
    {
        if (viewDepth > uCascadeEndSplits[i])
        {
            cascadeIndex = i + 1;
        }
    }
    return cascadeIndex;
}

float CalcShadow(float3 fragPos, float viewDepth, float3 geomNormal, float3 lightDir)
{
    if (uReceiveShadow == 0)
    {
        return 1.0;
    }

    float3 normal = normalize(geomNormal);

    [loop]
    for (int boundaryIndex = 0; boundaryIndex < uCascadeCount - 1; ++boundaryIndex)
    {
        float splitDistance = uCascadeEndSplits[boundaryIndex];
        float previousSplit = uCascadeNearPlane;
        if (boundaryIndex > 0)
        {
            previousSplit = uCascadeEndSplits[boundaryIndex - 1];
        }
        float cascadeSpan = max(splitDistance - previousSplit, 1e-4);
        float blendWidth = cascadeSpan * uCascadeBlendRatio;
        float blendStart = splitDistance - blendWidth;

        if (viewDepth > blendStart && viewDepth <= splitDistance)
        {
            float nearShadow = SampleCascadeShadow(boundaryIndex, fragPos, geomNormal, lightDir);
            float farShadow = SampleCascadeShadow(boundaryIndex + 1, fragPos, geomNormal, lightDir);
            float blendFactor = smoothstep(blendStart, splitDistance, viewDepth);
            return lerp(nearShadow, farShadow, blendFactor);
        }
    }

    int cascadeIndex = SelectCascadeIndex(viewDepth);
    return SampleCascadeShadow(cascadeIndex, fragPos, geomNormal, lightDir);
}

float2 SelectTexCoord(int texCoordSet, float2 texCoord0, float2 texCoord1)
{
    return texCoordSet == 1 ? texCoord1 : texCoord0;
}

float3 CalcNormalFromMap(float3 normal, float4 tangent, float2 texCoord)
{
    float3 tangentNormal = uNormalMap.Sample(uNormalSampler, texCoord).rgb * 2.0 - 1.0;

    float3 tangentVector = normalize(tangent.xyz);
    tangentVector = normalize(tangentVector - dot(tangentVector, normal) * normal);
    float3 bitangent = normalize(cross(normal, tangentVector) * tangent.w);
    float3x3 tbn = float3x3(tangentVector, bitangent, normal);

    return normalize(mul(tbn, tangentNormal));
}

float3 ToViewNormal(float3 worldNormal)
{
    return normalize(mul((float3x3)uView, worldNormal));
}

PSOutput main(PSInput input)
{
    PSOutput output;

    float3 normal = normalize(input.normal);
    float2 albedoTexCoord = SelectTexCoord(uAlbedoTexCoordSet, input.texCoord0, input.texCoord1);
    float2 normalTexCoord = SelectTexCoord(uNormalTexCoordSet, input.texCoord0, input.texCoord1);
    float2 aoTexCoord = SelectTexCoord(uAoTexCoordSet, input.texCoord0, input.texCoord1);
    float2 roughnessTexCoord = SelectTexCoord(uRoughnessTexCoordSet, input.texCoord0, input.texCoord1);
    if (uUseNormalMap != 0)
    {
        normal = CalcNormalFromMap(normal, input.tangent, normalTexCoord);
    }

    float3 viewDir = normalize(uViewPos - input.fragPos);

    float3 albedo = uAlbedo;
    if (uUseAlbedoMap != 0)
    {
        albedo *= uAlbedoMap.Sample(uAlbedoSampler, albedoTexCoord).rgb;
    }

    float roughness = uRoughness;
    float metallic = uMetallic;
    if (uUseMetallicRoughnessMap != 0)
    {
        float3 metallicRoughnessSample = uRoughnessMap.Sample(uRoughnessSampler, roughnessTexCoord).rgb;
        roughness *= metallicRoughnessSample.g;
        metallic *= metallicRoughnessSample.b;
    }
    else if (uUseRoughnessMap != 0)
    {
        roughness *= uRoughnessMap.Sample(uRoughnessSampler, roughnessTexCoord).r;
    }
    roughness = clamp(roughness, 0.04, 1.0);
    metallic = clamp(metallic, 0.0, 1.0);

    float3 f0 = lerp(0.04.xxx, albedo, metallic);

    float ambientOcclusion = 1.0;
    if (uUseAoMap != 0)
    {
        ambientOcclusion = uAoMap.Sample(uAoSampler, aoTexCoord).r;
    }

    float3 irradiance = uIrradianceMap.Sample(uIrradianceSampler, normal).rgb;
    float3 diffuseIbl = irradiance * albedo;

    float3 reflection = reflect(-viewDir, normal);
    float3 prefilteredColor = uPrefilterMap.SampleLevel(uPrefilterSampler, reflection, roughness * uMaxReflectionLod).rgb;
    float2 envBrdf = uBrdfLut.Sample(uBrdfLutSampler, float2(max(dot(normal, viewDir), 0.0), roughness)).rg;
    float3 specularIbl = prefilteredColor * (f0 * envBrdf.x + envBrdf.y);

    float3 specularEnergy = FresnelSchlickRoughness(max(dot(normal, viewDir), 0.0), f0, roughness);
    float3 diffuseEnergy = (1.0.xxx - specularEnergy) * (1.0 - metallic);
    float3 ambient = (diffuseEnergy * diffuseIbl + specularIbl) * uEnvironmentIntensity * ambientOcclusion;

    float3 directShadowed = 0.0.xxx;
    float3 directUnshadowed = 0.0.xxx;
    float shadowFactor = 1.0;

    [loop]
    for (int i = 0; i < uLightCount; ++i)
    {
        float3 lightDir;
        float attenuation;
        float spotIntensity;

        CalcLightDirectionAndAttenuation(
            uLightTypes[i],
            uLightPositions[i].xyz,
            uLightDirections[i].xyz,
            uLightAttenConstant[i],
            uLightAttenLinear[i],
            uLightAttenQuadratic[i],
            uLightRange[i],
            uLightInnerCutoffCos[i],
            uLightOuterCutoffCos[i],
            input.fragPos,
            lightDir,
            attenuation,
            spotIntensity);

        float3 radiance = SrgbToLinear(uLightColors[i].rgb) * uLightIntensities[i];
        float3 contribution = CalcCookTorranceContribution(
            normal,
            viewDir,
            lightDir,
            albedo,
            roughness,
            metallic,
            radiance);

        float lightShadow = 1.0;
        if (i == uShadowLightIndex)
        {
            lightShadow = CalcShadow(input.fragPos, input.viewDepth, normal, lightDir);
            shadowFactor = lightShadow;
        }

        float3 litContribution = contribution * attenuation * spotIntensity;
        directUnshadowed += litContribution;
        directShadowed += litContribution * lightShadow;
    }

    if (uShadowLightIndex < 0)
    {
        shadowFactor = 1.0;
    }

    float3 result = ambient + directShadowed;

    if (uDebugMode != 0)
    {
        float3 debugColor = result;
        if (uDebugMode == 1)
        {
            float shadow = 1.0;
            if (uShadowLightIndex >= 0)
            {
                shadow = CalcShadow(input.fragPos, input.viewDepth, normal, normalize(uLightDirections[uShadowLightIndex].xyz));
            }
            debugColor = shadow.xxx;
        }
        else if (uDebugMode == 2)
        {
            debugColor = directShadowed / (directShadowed + 0.25.xxx);
        }
        else if (uDebugMode == 3)
        {
            debugColor = ambient / (ambient + 0.25.xxx);
        }
        else if (uDebugMode == 4)
        {
            float3 projCoords = input.fragPosLightSpace.xyz / input.fragPosLightSpace.w;
            projCoords = projCoords * 0.5 + 0.5;
            debugColor = float3(projCoords.xy, 0.0);
        }
        else if (uDebugMode == 5)
        {
            float3 projCoords = input.fragPosLightSpace.xyz / input.fragPosLightSpace.w;
            projCoords = projCoords * 0.5 + 0.5;
            debugColor = projCoords.z.xxx;
        }
        else if (uDebugMode == 6)
        {
            int cascadeIndex = SelectCascadeIndex(input.viewDepth);
            if (cascadeIndex == 0)
            {
                debugColor = float3(1.0, 0.2, 0.2);
            }
            else if (cascadeIndex == 1)
            {
                debugColor = float3(0.2, 1.0, 0.2);
            }
            else if (cascadeIndex == 2)
            {
                debugColor = float3(0.2, 0.35, 1.0);
            }
            else
            {
                debugColor = float3(1.0, 0.85, 0.2);
            }
        }

        output.oDirect = float4(debugColor, 1.0);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
        output.oShadowFactor = 1.0;
        output.oNormal = float4(ToViewNormal(normal), 1.0);
        return output;
    }

    if (uSplitLightingOutput != 0)
    {
        output.oDirect = float4(directUnshadowed, 1.0);
        output.oIndirect = float4(ambient, 1.0);
        output.oShadowFactor = shadowFactor;
        output.oNormal = float4(ToViewNormal(normal), 1.0);
        return output;
    }

    if (uOutputLinear != 0)
    {
        output.oDirect = float4(result, 1.0);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
    }
    else
    {
        output.oDirect = float4(LinearToSrgb(result), 1.0);
        output.oIndirect = float4(0.0, 0.0, 0.0, 0.0);
    }
    output.oShadowFactor = 1.0;
    output.oNormal = float4(ToViewNormal(normal), 1.0);
    return output;
}
